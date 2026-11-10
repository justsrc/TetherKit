#include "tetherkit/core/runtime.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <utility>

#include "tetherkit/common/i18n.h"
#include "tetherkit/common/logging.h"
#include "tetherkit/common/scheduling.h"
#include "tetherkit/common/time.h"

namespace tetherkit::core {

std::string_view RunStateName(RunState state) noexcept {
  switch (state) {
    case RunState::kIdle:
      return Text(Msg::kCoreRunStateIdle);
    case RunState::kStarting:
      return Text(Msg::kCoreRunStateStarting);
    case RunState::kRunning:
      return Text(Msg::kCoreRunStateRunning);
    case RunState::kStopping:
      return Text(Msg::kCoreRunStateStopping);
    case RunState::kStopped:
      return Text(Msg::kCoreRunStateStopped);
    case RunState::kFailed:
      return Text(Msg::kCoreRunStateFailed);
  }
  return Text(Msg::kCoreRunStateUnknown);
}

Result<std::unique_ptr<Runtime>> Runtime::Create(const RuntimeConfig& config) {
  auto runtime = std::unique_ptr<Runtime>(new Runtime());
  runtime->config_ = config;
  return runtime;
}

Runtime::~Runtime() {
  Stop();
}

// =============================================================================
// Host thread interface
// =============================================================================

Status Runtime::Start() {
  if (started_) {
    return std::unexpected(Error::Generic(Tr(Msg::kCoreRuntimeAlreadyStarted)));
  }

  // The root check is deliberately left on the **synchronous** path.
  //
  // It needs no I/O, and "forgot to sudo" is the most common failure; reporting
  // it synchronously up front lets the CLI reflect it in the exit code instead
  // of making the user watch a spinner while waiting for an async event.
  // Note that claiming an RNDIS interface over libusb does **not** need root
  // (macOS has no RNDIS kernel driver); root is needed for creating the feth
  // pair and opening /dev/bpf*.
  if (!net::IsRunningAsRoot()) {
    return std::unexpected(Error::Generic(Tr(Msg::kCoreNeedsRoot)));
  }

  started_ = true;
  SetRunState(RunState::kStarting);
  control_thread_ = std::thread([this] { RunControlThread(); });
  return Ok();
}

void Runtime::WaitUntilStopped() {
  if (control_thread_.joinable()) {
    control_thread_.join();
  }
}

void Runtime::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  if (!started_) {
    return;
  }

  // Set the stop flag and wake the control thread in case it is sleeping.
  //
  // The flag must be written while holding the lock: the control thread's
  // wait_for uses it as its predicate, and writing it without the lock leaves
  // a lost-wakeup window between "predicate checked" and "actually asleep".
  // RequestStop() not taking the lock is a deliberate trade-off (it must stay
  // async-signal-safe); the cost is that the flag may take up to one loop
  // period to be observed.
  {
    const std::lock_guard<std::mutex> guard(stop_mutex_);
    stop_requested_.store(true, std::memory_order_release);
  }
  stop_condition_.notify_all();

  if (control_thread_.joinable()) {
    control_thread_.join();
  }
}

RuntimeSnapshot Runtime::Snapshot() const {
  const std::lock_guard<std::mutex> guard(snapshot_mutex_);
  return snapshot_;
}

std::string Runtime::SystemInterfaceName() const {
  const std::lock_guard<std::mutex> guard(snapshot_mutex_);
  return snapshot_.system_interface;
}

// =============================================================================
// Control thread
// =============================================================================

void Runtime::RunControlThread() noexcept {
  // The control thread uses kControl rather than kDataPath QoS: it does a bit
  // of work every few seconds, so fighting for performance cores is pointless
  // and could crowd out the three real data-path threads.
  ConfigureCurrentThread("rndis-ctl", ThreadRole::kControl);

  if (const auto status = RunStartSequence(); !status) {
    RecordFatal(status.error());
  } else {
    SetRunState(RunState::kRunning);
    RunControlLoop();
    SetRunState(RunState::kStopping);
  }

  // Tear down whether or not startup succeeded: the start sequence may have
  // built half a stack already (e.g. feth created but BPF will not open), and
  // skipping this would leave the interface behind in the kernel.
  Teardown();

  SetRunState(fatal_error_.load(std::memory_order_acquire) ? RunState::kFailed
                                                           : RunState::kStopped);
}

Status Runtime::RunStartSequence() {
  // ---- Step 1: libusb context and event thread ----
  //
  // (The root check was already done synchronously in Start().)
  TETHERKIT_ASSIGN_OR_RETURN(usb_context_, usb::Context::Create());

  // ---- Step 2: discover and open the device ----
  TETHERKIT_ASSIGN_OR_RETURN(const auto candidates,
                             usb::FindRndisDevices(*usb_context_, config_.device_filter));
  if (candidates.empty()) {
    return std::unexpected(Error::Generic(Tr(Msg::kCoreNoDeviceFound)));
  }
  if (candidates.size() > 1) {
    TETHERKIT_INFO_TR(Msg::kCoreMultipleDevices, candidates.size());
  }
  TETHERKIT_ASSIGN_OR_RETURN(device_, usb::Device::Open(*usb_context_, candidates.front()));
  RefreshSnapshot();

  // ---- Step 3: RNDIS control channel and state machine ----
  //
  // The control channel uses the synchronous libusb API, so it must run on a
  // **non-event thread** -- this function runs on Runtime's own control
  // thread, which satisfies that.
  control_channel_ =
      std::make_unique<usb::UsbControlChannel>(*device_, config_.rndis.control_timeout_millis);

  // Interrupt notifications must go over an async transfer or the control
  // thread would wedge forever -- see the UsbControlChannel notes in
  // include/tetherkit/usb/device.h. It must be started **before** the state
  // machine's Start(), because the initialization handshake itself waits for
  // notifications.
  TETHERKIT_RETURN_IF_ERROR(control_channel_->StartNotificationListener());

  // Let the RNDIS negotiation use the real endpoint max packet size (it
  // affects how MaxTransferSize is derived).
  rndis::StateMachineConfig rndis_config = config_.rndis;
  rndis_config.requested_mtu = config_.mtu;
  state_machine_ = std::make_unique<rndis::StateMachine>(*control_channel_, *this, rndis_config);

  TETHERKIT_RETURN_IF_ERROR(state_machine_->Start());

  // Only now do we have the negotiation results: device MAC, final MTU,
  // aggregation caps, alignment requirements.
  const rndis::NegotiatedParameters& parameters = state_machine_->Parameters();
  const rndis::DeviceInfo& info = state_machine_->Info();

  // ---- Step 4: create the feth interface pair ----
  //
  // Must happen after negotiation: the system-side MAC has to be set to the
  // address the device reports, the MTU must come from the negotiation, and
  // the MAC must be set before the interface goes IFF_UP.
  {
    const net::MacAddress* system_mac = nullptr;
    net::MacAddress adopted{};
    if (config_.adopt_device_mac && info.has_permanent_address) {
      std::ranges::copy(info.permanent_address, adopted.begin());
      system_mac = &adopted;
    }
    TETHERKIT_ASSIGN_OR_RETURN(auto pair, net::FethPair::Create(parameters.mtu, system_mac));
    feth_pair_ = std::make_unique<net::FethPair>(std::move(pair));
  }
  // Flush the interfaces into the snapshot as soon as they exist: if any later
  // step fails, the GUI should still be able to show the interface names,
  // otherwise the user cannot even tell which card was just created.
  RefreshSnapshot();

  // ---- Step 5: open BPF on the **driver-side** interface ----
  //
  // We attach on the driver side rather than the system side because frames
  // the host sends from the system side appear as input on the driver side,
  // while frames we write() into the driver side arrive as input on the system
  // side -- one descriptor handles both directions.
  {
    net::BpfConfig bpf_config = config_.bpf;
    // Max frame = MTU + Ethernet header. Note the kernel bpfwrite hard cap is
    // MTU + 18, so this must never exceed it.
    bpf_config.max_frame_bytes = parameters.mtu + rndis::kEthernetHeaderBytes;
    TETHERKIT_ASSIGN_OR_RETURN(bpf_link_,
                               net::BpfLink::Open(feth_pair_->DriverSide().Name(), bpf_config));
  }

  // ---- Step 6: create the USB data channel ----
  {
    usb::DataChannelConfig data_config = config_.data_channel;
    // The bulk IN buffer must be >= the MaxTransferSize we advertise in
    // INITIALIZE_MSG, or large transfers the device aggregates will overflow
    // or be truncated.
    data_config.rx_transfer_bytes =
        std::max(data_config.rx_transfer_bytes, config_.rndis.host_max_transfer_size);
    TETHERKIT_ASSIGN_OR_RETURN(data_channel_,
                               usb::UsbDataChannel::Create(*device_, parameters, data_config));
  }

  // ---- Step 7: start the bridge ----
  {
    BridgeConfig bridge_config = config_.bridge;
    bridge_config.max_frame_bytes = parameters.mtu + rndis::kEthernetHeaderBytes;
    bridge_ = std::make_unique<Bridge>(*data_channel_, *bpf_link_, bridge_config);
    TETHERKIT_RETURN_IF_ERROR(bridge_->Start());
  }

  RefreshSnapshot();
  PrintNextSteps();
  return Ok();
}

void Runtime::RunControlLoop() {
  BridgeStats previous = bridge_->Snapshot();
  Nanos last_stats_nanos = MonotonicNanos();

  const Nanos stats_interval_nanos =
      static_cast<Nanos>(config_.stats_interval_millis) * kNanosPerMilli;

  while (!stop_requested_.load(std::memory_order_acquire) &&
         !fatal_error_.load(std::memory_order_acquire)) {
    // ---- State machine periodic work: keep-alive + draining device pushes ----
    if (const auto status = state_machine_->Poll(); !status) {
      RecordFatal(std::move(status).error());
      break;
    }

    // ---- Refresh the snapshot ----
    //
    // Done every loop iteration rather than only on events: the statistics
    // counters change continuously, and the GUI subtracts two snapshots to
    // compute rates. The loop period is capped at 250 ms, plenty for a 2 Hz
    // UI refresh.
    RefreshSnapshot();

    // ---- Periodic statistics ----
    const Nanos now = MonotonicNanos();
    if (stats_interval_nanos != 0 && now - last_stats_nanos >= stats_interval_nanos) {
      const BridgeStats current = bridge_->Snapshot();
      const double seconds =
          static_cast<double>(now - last_stats_nanos) / static_cast<double>(kNanosPerSecond);
      TETHERKIT_INFO("{}", FormatStatsLine(previous, current, seconds));
      previous = current;
      last_stats_nanos = now;
    }

    // ---- Sleep until the next moment we should wake ----
    //
    // Take the nearer of "next keep-alive" and "next statistics" deadlines to
    // avoid pointless wakeups. The 250 ms cap exists so that RequestStop(),
    // which only writes an atomic, is observed promptly.
    std::uint32_t sleep_millis =
        std::min<std::uint32_t>(state_machine_->MillisUntilNextPoll(), 250);
    if (stats_interval_nanos != 0) {
      const Nanos elapsed = now - last_stats_nanos;
      const Nanos remaining = elapsed >= stats_interval_nanos ? 0 : stats_interval_nanos - elapsed;
      sleep_millis = std::min<std::uint32_t>(sleep_millis,
                                             static_cast<std::uint32_t>(remaining / kNanosPerMilli));
    }
    // Always sleep at least 10 ms: avoids busy-spinning right at the
    // keep-alive deadline boundary.
    sleep_millis = std::max<std::uint32_t>(sleep_millis, 10);

    // A condition variable instead of sleep_for: Stop() wakes us immediately,
    // so pressing "Stop" responds right away instead of after up to 250 ms.
    std::unique_lock<std::mutex> lock(stop_mutex_);
    stop_condition_.wait_for(lock, std::chrono::milliseconds(sleep_millis),
                             [this] { return stop_requested_.load(std::memory_order_acquire); });
  }

  if (fatal_error_.load(std::memory_order_acquire)) {
    TETHERKIT_ERROR_TR(Msg::kCoreExitingOnFatal);
  }
}

void Runtime::PrintNextSteps() const {
  const std::string_view name = feth_pair_->SystemSide().Name();
  TETHERKIT_INFO("");
  TETHERKIT_INFO_TR(Msg::kCoreInterfaceReady, name);
  TETHERKIT_INFO_TR(Msg::kCoreNextStepsAssignIp);
  TETHERKIT_INFO("    sudo ipconfig set {} DHCP", name);
  TETHERKIT_INFO_TR(Msg::kCoreNextStepsVerify);
  TETHERKIT_INFO("    ipconfig getifaddr {}", name);
  TETHERKIT_INFO("    ipconfig getsummary {}", name);
  TETHERKIT_INFO_TR(Msg::kCoreNextStepsDefaultRoute);
  TETHERKIT_INFO("    sudo route -n change default $(ipconfig getoption {} router)", name);
  TETHERKIT_INFO_TR(Msg::kCoreNextStepsTemporaryNote1);
  TETHERKIT_INFO_TR(Msg::kCoreNextStepsTemporaryNote2);
  TETHERKIT_INFO("");
}

// =============================================================================
// Teardown
// =============================================================================

void Runtime::Teardown() {
  TETHERKIT_INFO_TR(Msg::kCoreStopping);

  // Teardown in the exact **reverse** of the start-up order.
  //
  // 1. Stop the bridge first: it joins its two data-path threads internally,
  //    then calls DataChannel::Shutdown() to wait for all in-flight USB
  //    transfers to be reclaimed. This must finish before device_ is
  //    destroyed -- libusb_close will not reclaim in-flight transfers for us,
  //    so closing the handle before freeing them is a use-after-free.
  if (bridge_ != nullptr) {
    bridge_->Stop();
    bridge_.reset();
  }
  data_channel_.reset();

  // 2. Close BPF (nobody is reading or writing it by now).
  bpf_link_.reset();

  // 3. Destroy the feth interface pair. The kernel's feth_clone_destroy
  //    unbinds the peer first, automatically.
  feth_pair_.reset();

  // 4. Take the device out of RNDIS: first SET filter = 0 so it stops sending
  //    data, then send HALT. This must happen before the USB handle is closed
  //    -- otherwise the device keeps believing the host is still there and the
  //    next plug-in starts unclean.
  //
  //    This step sends a synchronous control message, so it must run on the
  //    control thread -- which is where we are.
  if (state_machine_ != nullptr) {
    state_machine_->Stop();
    state_machine_.reset();
  }
  // Stop the interrupt listener first (it waits for in-flight async transfers
  // to be reclaimed), then destroy the channel object.
  if (control_channel_ != nullptr) {
    control_channel_->StopNotificationListener();
    control_channel_.reset();
  }

  // 5. Release the interfaces and close the device handle.
  device_.reset();

  // 6. Finally stop the libusb event thread and release the context.
  usb_context_.reset();

  // Refresh once more after teardown: interface names and statistics should be
  // zeroed, and the GUI should not keep showing an interface that no longer
  // exists. link_up is maintained by events and RefreshSnapshot does not touch
  // it, so clear it explicitly here -- leaving "link connected" lit after
  // shutdown would be misleading.
  {
    const std::lock_guard<std::mutex> guard(snapshot_mutex_);
    snapshot_.link_up = false;
  }
  RefreshSnapshot();
  TETHERKIT_INFO_TR(Msg::kCoreStopped);
}

// =============================================================================
// Snapshot and events
// =============================================================================

void Runtime::RefreshSnapshot() {
  // Read each component's state outside the lock first -- those reads go
  // through ioctl / atomic loads and should not hold snapshot_mutex_.
  RuntimeSnapshot fresh;
  if (device_ != nullptr) {
    fresh.device_description = std::string{device_->Describe()};
  }
  if (state_machine_ != nullptr) {
    fresh.rndis_state = state_machine_->CurrentState();
    fresh.device_info = state_machine_->Info();
    fresh.parameters = state_machine_->Parameters();
  }
  if (feth_pair_ != nullptr) {
    fresh.system_interface = std::string{feth_pair_->SystemSide().Name()};
    fresh.driver_interface = std::string{feth_pair_->DriverSide().Name()};
  }
  if (bridge_ != nullptr) {
    fresh.bridge = bridge_->Snapshot();
    fresh.paused = bridge_->Paused();
  }

  const std::lock_guard<std::mutex> guard(snapshot_mutex_);
  // run_state, link_up and fatal_message are maintained by their own
  // transition points; do not overwrite them here.
  fresh.run_state = snapshot_.run_state;
  fresh.link_up = snapshot_.link_up;
  fresh.fatal_message = snapshot_.fatal_message;
  snapshot_ = std::move(fresh);
}

void Runtime::SetRunState(RunState next) {
  RunState previous = RunState::kIdle;
  {
    const std::lock_guard<std::mutex> guard(snapshot_mutex_);
    previous = snapshot_.run_state;
    if (previous == next) {
      return;
    }
    snapshot_.run_state = next;
  }
  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kRunState,
                    .a = static_cast<std::int64_t>(previous),
                    .b = static_cast<std::int64_t>(next),
                    .text = std::string{RunStateName(next)}});
}

void Runtime::RecordFatal(const Error& error) {
  const std::string message = error.ToString();
  TETHERKIT_ERROR("{}", message);

  {
    const std::lock_guard<std::mutex> guard(snapshot_mutex_);
    // Record only the **first** fatal error: later ones are mostly knock-on
    // effects of it, and overwriting would lose the root cause.
    if (snapshot_.fatal_message.empty()) {
      snapshot_.fatal_message = message;
    }
  }
  fatal_error_.store(true, std::memory_order_release);
  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kFatal, .text = message});
}

void Runtime::Emit(const RuntimeEvent& event) const {
  if (config_.event_sink != nullptr) {
    config_.event_sink->OnRuntimeEvent(event);
  }
}

// =============================================================================
// StateMachineObserver (all called on the control thread)
// =============================================================================

void Runtime::OnStateChanged(rndis::State from, rndis::State to) {
  // The state machine has already logged this; do not repeat it here, just
  // forward the transition to the host.
  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kRndisState,
                    .a = static_cast<std::int64_t>(from),
                    .b = static_cast<std::int64_t>(to),
                    .text = std::string{rndis::StateName(to)}});
}

void Runtime::OnNegotiated(const rndis::NegotiatedParameters& parameters,
                           const rndis::DeviceInfo& info) {
  TETHERKIT_INFO_TR(Msg::kCoreRndisReady, rndis::FormatMac(info.permanent_address).data(),
                    parameters.mtu, info.LinkSpeedMbps());

  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kNegotiated,
                    .a = static_cast<std::int64_t>(parameters.mtu),
                    .b = static_cast<std::int64_t>(info.LinkSpeedMbps()),
                    .text = info.vendor_description});
}

void Runtime::OnLinkStateChanged(bool connected) {
  TETHERKIT_INFO_TR(Msg::kCoreLinkState,
                    Text(connected ? Msg::kCoreLinkConnected : Msg::kCoreLinkDisconnected));

  {
    const std::lock_guard<std::mutex> guard(snapshot_mutex_);
    snapshot_.link_up = connected;
  }

  // Pause data movement while the link is down. Pushing frames onto a broken
  // link is pure waste and would fill the queue, so on recovery a burst of
  // stale frames goes out first.
  if (bridge_ != nullptr) {
    bridge_->SetPaused(!connected);
  }

  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kLink, .a = connected ? 1 : 0});
}

void Runtime::OnDeviceReset(bool addressing_lost) {
  TETHERKIT_WARN_TR(Msg::kCoreDeviceReset,
                    Text(addressing_lost ? Msg::kCoreAddressingLost : Msg::kCoreAddressingKept));

  // The device discarded all outstanding packets during the reset. Pause
  // briefly so the state machine can finish replaying the packet filter,
  // avoiding pumping data into the window where the device rebuilds its
  // internal state.
  if (bridge_ != nullptr) {
    bridge_->SetPaused(true);
    bridge_->SetPaused(false);
  }

  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kDeviceReset, .a = addressing_lost ? 1 : 0});
}

void Runtime::OnFatalError(const Error& error) {
  // WithContext is &&-qualified and what we hold here is a const reference,
  // so we must copy first.
  Error annotated = error;
  RecordFatal(std::move(annotated).WithContext(Tr(Msg::kCoreLinkUnrecoverable)));
}

}  // namespace tetherkit::core
