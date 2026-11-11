// The C ABI of the session: lifecycle, status snapshot, event polling.
//
// This layer holds almost no logic; it does three things: POD config ->
// RuntimeConfig, RuntimeSnapshot -> POD status, RuntimeEvent -> ring queue.
// The real orchestration lives in core::Runtime.
#include "tethernet/capi/tethernet_c.h"
#include "tethernet/common/i18n.h"
#include "tethernet/common/time.h"
#include "tethernet/core/runtime.h"
#include "tethernet/net/feth_device.h"
#include "tethernet/rndis/state_machine.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <tuple>
#include <utility>

#include "capi_support.h"

namespace {

using tethernet::capi::ClearError;
using tethernet::capi::CopyText;
using tethernet::capi::FillError;
using tethernet::capi::WallNanos;
using tethernet::core::RunState;
using tethernet::core::RuntimeEvent;

// ---------------------------------------------------------------------------
// Enum value alignment check
//
// The C-side enums copy the numeric values of the C++ side and rely on a
// direct cast plus "the order happens to match". That coupling goes silently
// wrong the moment someone inserts an enum value in the middle (the UI shows
// the wrong status, and nothing reports an error), so it is nailed down at
// compile time.
// ---------------------------------------------------------------------------
static_assert(static_cast<int>(RunState::kIdle) == TN_RUN_IDLE);
static_assert(static_cast<int>(RunState::kStarting) == TN_RUN_STARTING);
static_assert(static_cast<int>(RunState::kRunning) == TN_RUN_RUNNING);
static_assert(static_cast<int>(RunState::kStopping) == TN_RUN_STOPPING);
static_assert(static_cast<int>(RunState::kStopped) == TN_RUN_STOPPED);
static_assert(static_cast<int>(RunState::kFailed) == TN_RUN_FAILED);

static_assert(static_cast<int>(tethernet::rndis::State::kUninitialized) == TN_RNDIS_UNINITIALIZED);
static_assert(static_cast<int>(tethernet::rndis::State::kInitializing) == TN_RNDIS_INITIALIZING);
static_assert(static_cast<int>(tethernet::rndis::State::kInitialized) == TN_RNDIS_INITIALIZED);
static_assert(static_cast<int>(tethernet::rndis::State::kDataInitialized) ==
              TN_RNDIS_DATA_INITIALIZED);
static_assert(static_cast<int>(tethernet::rndis::State::kHalting) == TN_RNDIS_HALTING);

static_assert(static_cast<int>(RuntimeEvent::Kind::kRndisState) == TN_EVENT_RNDIS_STATE);
static_assert(static_cast<int>(RuntimeEvent::Kind::kNegotiated) == TN_EVENT_NEGOTIATED);
static_assert(static_cast<int>(RuntimeEvent::Kind::kLink) == TN_EVENT_LINK);
static_assert(static_cast<int>(RuntimeEvent::Kind::kDeviceReset) == TN_EVENT_DEVICE_RESET);
static_assert(static_cast<int>(RuntimeEvent::Kind::kFatal) == TN_EVENT_FATAL);
static_assert(static_cast<int>(RuntimeEvent::Kind::kRunState) == TN_EVENT_RUN_STATE);

/// The event ring buffer.
///
/// Why a capacity of 128: events are only produced on state transitions, and a
/// full startup sequence is about 10 of them. 128 is enough to cover bursts
/// like "the device is plugged and unplugged repeatedly plus link flapping",
/// and the GUI drains it every 500 ms. The oldest is dropped when it fills --
/// events only drive animations and notifications, the authoritative state is
/// in the snapshot, and missing a few does not affect correctness.
constexpr std::size_t kEventRingCapacity = 128;

class EventRing final : public tethernet::core::RuntimeEventSink {
 public:
  /// Called by the control thread.
  void OnRuntimeEvent(const RuntimeEvent& event) noexcept override {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (count_ == kEventRingCapacity) {
      head_ = (head_ + 1) % kEventRingCapacity;
      --count_;
    }
    tn_event_t& slot = events_[(head_ + count_) % kEventRingCapacity];
    slot.kind = static_cast<std::int32_t>(event.kind);
    slot.wall_nanos = WallNanos();
    slot.a = event.a;
    slot.b = event.b;
    CopyText(slot.text, event.text);
    ++count_;
  }

  /// Called by the host thread.
  std::size_t Drain(tn_event_t* out_events, std::size_t capacity) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::size_t taken = 0;
    while (taken < capacity && count_ > 0) {
      if (out_events != nullptr) {
        out_events[taken] = events_[head_];
      }
      head_ = (head_ + 1) % kEventRingCapacity;
      --count_;
      ++taken;
    }
    return taken;
  }

 private:
  std::mutex mutex_;
  std::array<tn_event_t, kEventRingCapacity> events_{};
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

/// Reads a value, treating 0 as "use the library default".
///
/// Being lenient about the tuning knobs instead of erroring out: the GUI may
/// only care about the MTU and leave the rest at zero, and demanding non-zero
/// values would only force the caller to copy the defaults -- and copied
/// constants drift out of sync with the library sooner or later.
[[nodiscard]] std::uint32_t OrDefault(std::uint32_t value, std::uint32_t fallback) noexcept {
  return value != 0 ? value : fallback;
}

[[nodiscard]] tethernet::core::RuntimeConfig ToRuntimeConfig(const tn_session_config_t& config,
                                                             EventRing& sink) {
  const tethernet::core::RuntimeConfig defaults;
  tethernet::core::RuntimeConfig result;

  result.device_filter.vendor_id = config.vendor_id;
  result.device_filter.product_id = config.product_id;
  result.device_filter.bus_number = config.bus_number;
  result.device_filter.device_address = config.device_address;

  result.mtu = OrDefault(config.mtu, defaults.mtu);
  result.adopt_device_mac = config.adopt_device_mac;

  result.data_channel.rx_transfer_count =
      OrDefault(config.rx_transfer_count, defaults.data_channel.rx_transfer_count);
  result.data_channel.tx_transfer_count =
      OrDefault(config.tx_transfer_count, defaults.data_channel.tx_transfer_count);
  result.data_channel.rx_transfer_bytes =
      OrDefault(config.rx_transfer_kib * 1024, defaults.data_channel.rx_transfer_bytes);
  result.rndis.host_max_transfer_size =
      OrDefault(config.max_transfer_kib * 1024, defaults.rndis.host_max_transfer_size);
  result.bpf.kernel_buffer_bytes =
      OrDefault(config.bpf_buffer_kib * 1024, defaults.bpf.kernel_buffer_bytes);

  result.event_sink = &sink;
  return result;
}

}  // namespace

// The session object.
//
//  The declaration order of the members is the reverse of the destruction
//   order, and it must not be rearranged
//   events has to be declared **before** runtime, so that runtime is destroyed
//   first -- its destructor joins the control thread, and the control thread
//   may push events into events until the very last moment.
//   The other way around is a use-after-free.
struct tn_session {
  EventRing events;
  std::unique_ptr<tethernet::core::Runtime> runtime;
};

void tn_session_config_init(tn_session_config_t* out_config) {
  if (out_config == nullptr) {
    return;
  }
  const tethernet::core::RuntimeConfig defaults;
  *out_config = tn_session_config_t{};
  out_config->mtu = defaults.mtu;
  out_config->adopt_device_mac = defaults.adopt_device_mac;
  out_config->rx_transfer_count = defaults.data_channel.rx_transfer_count;
  out_config->tx_transfer_count = defaults.data_channel.tx_transfer_count;
  out_config->rx_transfer_kib = defaults.data_channel.rx_transfer_bytes / 1024;
  out_config->max_transfer_kib = defaults.rndis.host_max_transfer_size / 1024;
  out_config->bpf_buffer_kib = defaults.bpf.kernel_buffer_bytes / 1024;
}

tn_session_t* tn_session_create(const tn_session_config_t* config, tn_error_t* out_error) {
  ClearError(out_error);
  if (config == nullptr) {
    tethernet::capi::FillGenericError(out_error,
                                      tethernet::Tr(tethernet::Msg::kCapiSessionConfigNull));
    return nullptr;
  }

  // The session is the only entry point that creates an feth, so installing
  // the registry callback here is enough -- the root-free group of entry
  // points (version, enumeration, preflight) therefore stays free of side
  // effects and never touches /var/run.
  tethernet::capi::InstallInterfaceRegistry();

  auto session = std::unique_ptr<tn_session>(new (std::nothrow) tn_session());
  if (session == nullptr) {
    tethernet::capi::FillGenericError(out_error,
                                      tethernet::Tr(tethernet::Msg::kCapiSessionOutOfMemory));
    return nullptr;
  }

  auto runtime = tethernet::core::Runtime::Create(ToRuntimeConfig(*config, session->events));
  if (!runtime) {
    FillError(out_error, runtime.error());
    return nullptr;
  }
  session->runtime = std::move(*runtime);
  return session.release();
}

tn_result_t tn_session_start(tn_session_t* session, tn_error_t* out_error) {
  ClearError(out_error);
  if (session == nullptr) {
    return TN_ERR_INVALID_ARGUMENT;
  }

  if (const auto status = session->runtime->Start(); !status) {
    FillError(out_error, status.error());
    // The only synchronous failure is the root check (see Runtime::Start), so
    // it gets a dedicated code -- that lets the GUI pop up "authorization
    // required" instead of a generic "start failed".
    return tethernet::net::IsRunningAsRoot() ? TN_ERR_FAILED : TN_ERR_PERMISSION;
  }
  return TN_OK;
}

tn_result_t tn_session_stop(tn_session_t* session) {
  if (session == nullptr) {
    return TN_ERR_INVALID_ARGUMENT;
  }
  session->runtime->Stop();
  return TN_OK;
}

void tn_session_destroy(tn_session_t* session) {
  // The Runtime destructor calls Stop() (idempotent), so a plain delete is
  // enough here.
  delete session;  // NOLINT(cppcoreguidelines-owning-memory)
}

tn_result_t tn_session_status_get(tn_session_t* session, tn_session_status_t* out_status) {
  if (session == nullptr || out_status == nullptr) {
    return TN_ERR_INVALID_ARGUMENT;
  }

  const tethernet::core::RuntimeSnapshot snapshot = session->runtime->Snapshot();
  *out_status = tn_session_status_t{};

  out_status->run_state = static_cast<std::int32_t>(snapshot.run_state);
  out_status->rndis_state = static_cast<std::int32_t>(snapshot.rndis_state);
  out_status->link_up = snapshot.link_up;
  out_status->paused = snapshot.paused;

  CopyText(out_status->system_interface, snapshot.system_interface);
  CopyText(out_status->driver_interface, snapshot.driver_interface);
  CopyText(out_status->vendor_description, snapshot.device_info.vendor_description);
  CopyText(out_status->device_description, snapshot.device_description);

  // The permanent address rather than the current one: under RNDIS semantics
  // the device *is* this interface, and the peer's ARP table and DHCP lease
  // are both built from the permanent address, so showing it in the UI is
  // what matches what the user sees on the router.
  static_assert(sizeof(out_status->device_mac) == std::tuple_size_v<tethernet::rndis::MacAddress>);
  std::ranges::copy(snapshot.device_info.permanent_address, std::begin(out_status->device_mac));

  out_status->mtu = snapshot.parameters.mtu;
  out_status->link_speed_mbps = static_cast<std::uint32_t>(snapshot.device_info.LinkSpeedMbps());

  out_status->rx_frames = snapshot.bridge.rx.frames;
  out_status->rx_bytes = snapshot.bridge.rx.bytes;
  out_status->rx_dropped = snapshot.bridge.rx.TotalDropped();
  out_status->tx_frames = snapshot.bridge.tx.frames;
  out_status->tx_bytes = snapshot.bridge.tx.bytes;
  out_status->tx_dropped = snapshot.bridge.tx.TotalDropped();
  out_status->rx_queue_depth = snapshot.bridge.rx_queue_depth;
  out_status->link_kernel_drops = snapshot.bridge.link_kernel_drops;
  out_status->tx_backpressure = snapshot.bridge.tx_backpressure_events;

  // The snapshot is timestamped with the library's monotonic clock rather
  // than letting the host difference its own clock readings: scheduling
  // stretches the real interval between two fetches, and using the host's
  // timer period as the denominator overstates the rate.
  out_status->monotonic_nanos = static_cast<std::int64_t>(tethernet::MonotonicNanos());

  CopyText(out_status->fatal, snapshot.fatal_message);
  return TN_OK;
}

size_t tn_session_poll_events(tn_session_t* session, tn_event_t* out_events, size_t capacity) {
  if (session == nullptr || capacity == 0) {
    return 0;
  }
  return session->events.Drain(out_events, capacity);
}
