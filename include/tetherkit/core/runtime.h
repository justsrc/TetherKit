// Runtime orchestration: assembles USB, the RNDIS state machine, feth, BPF,
// and the bridge into a working driver.
//
//  Start sequence (each step depends on the previous step's products; the
//   order cannot be swapped)
//
//   1. Permission check     -- give a plain-language error early instead of
//                              letting a pile of EPERM surface later
//   2. libusb context + event thread
//   3. Find and open the RNDIS device (claim two interfaces, parse endpoints)
//   4. RNDIS control channel + state machine Start()
//        -- this is the step that yields: device MAC, negotiated MTU,
//           aggregation limits, alignment requirements
//   5. Create the feth interface pair from step 4's results
//      (system-side MAC = the MAC the device reported)
//   6. Open BPF on the driver-side interface
//   7. Create the USB data channel with step 4's negotiated parameters
//   8. Start the bridge
//   9. Control thread enters its loop: periodic Poll() (keep-alive + drain
//      device-pushed notifications)
//
//   Why feth must be created **after** RNDIS negotiation: the system-side
//   interface's MAC has to be set to the device-reported
//   OID_802_3_PERMANENT_ADDRESS and its MTU has to use the negotiated value --
//   neither is available before step 4. And the MAC must be set before the
//   interface goes IFF_UP.
//
//  Teardown order is the strict reverse of the start sequence
//
//   bridge -> data channel (wait for in-flight transfers to reap) -> BPF ->
//   feth -> state machine HALT -> device -> libusb
//
//   In particular, "data channel waits for in-flight transfers to reap" must
//   complete **before** the device is destroyed, otherwise it is a
//   use-after-free (see usb/data_channel.h for details).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "tetherkit/common/error.h"
#include "tetherkit/core/bridge.h"
#include "tetherkit/net/bpf_link.h"
#include "tetherkit/net/feth_device.h"
#include "tetherkit/rndis/state_machine.h"
#include "tetherkit/usb/context.h"
#include "tetherkit/usb/data_channel.h"
#include "tetherkit/usb/device.h"

namespace tetherkit::core {

/// Lifecycle state of the runtime. The only state dimension the host
/// (command line / GUI) needs to care about.
enum class RunState : std::uint8_t {
  kIdle,      ///< Created but Start() not yet called.
  kStarting,  ///< Control thread running the start sequence (enumerate ->
              ///< handshake -> create interface -> open bridge).
  kRunning,   ///< Data paths are up.
  kStopping,  ///< Tearing down in reverse order.
  kStopped,   ///< Shut down normally.
  kFailed,    ///< Start failed or an unrecoverable runtime error; see
              ///< RuntimeSnapshot::fatal_message.
};

[[nodiscard]] std::string_view RunStateName(RunState state) noexcept;

/// One event reported to the host by the runtime.
///
/// Uses "events + host queues them itself" rather than letting the host
/// implement StateMachineObserver directly: that interface's five callbacks
/// have unrelated semantics and C++ reference parameters, which cross-language
/// hosts cannot use; and in a callback it is all too easy to call back into
/// the runtime by accident (e.g. "stop on fatal error"), which deadlocks
/// waiting for itself.
struct RuntimeEvent {
  enum class Kind : std::uint8_t {
    kRndisState,   ///< a = rndis::State before the transition, b = after.
    kNegotiated,   ///< a = final MTU, b = link speed (Mbps).
    kLink,         ///< a = 1 means the link is connected.
    kDeviceReset,  ///< a = 1 means addressing information was lost and replayed.
    kFatal,        ///< text = the reason for an unrecoverable error.
    kRunState,     ///< a = RunState before the transition, b = after.
  };

  Kind kind = Kind::kRunState;
  std::int64_t a = 0;
  std::int64_t b = 0;
  std::string text;
};

/// Event sink: implemented by the host to receive runtime events.
///
///  Two hard constraints
///   1. Callbacks fire **on the control thread** (all event sources live on
///      that thread), while the host's own threads may be calling Snapshot()
///      at the same time -- every lock the implementation needs must be there.
///   2. **An implementation must never call any Runtime method.** Stop()
///      joins the control thread; calling it from an event is a self-deadlock.
///      The right approach is to queue events and process them on a host
///      thread.
class RuntimeEventSink {
 public:
  RuntimeEventSink() = default;
  RuntimeEventSink(const RuntimeEventSink&) = delete;
  RuntimeEventSink& operator=(const RuntimeEventSink&) = delete;
  RuntimeEventSink(RuntimeEventSink&&) = delete;
  RuntimeEventSink& operator=(RuntimeEventSink&&) = delete;
  virtual ~RuntimeEventSink() = default;

  virtual void OnRuntimeEvent(const RuntimeEvent& event) = 0;
};

/// The runtime's fully observable snapshot.
///
/// Why a single whole-struct copy instead of a pile of per-field accessors:
/// the host needs a **consistent** set of values (a UI whose state and
/// interface names do not match is hard to understand), and per-field reads
/// can observe torn combinations. Copying a few hundred bytes at once is
/// no problem at a 2 Hz refresh rate.
struct RuntimeSnapshot {
  RunState run_state = RunState::kIdle;
  rndis::State rndis_state = rndis::State::kUninitialized;
  bool link_up = false;
  /// Whether data movement is paused (link down or during a device soft reset).
  bool paused = false;

  /// System-side interface name (the one the host configures IP on). Empty
  /// until created.
  std::string system_interface;
  /// Driver-side interface name (BPF attaches to this one). For diagnostics
  /// display only.
  std::string driver_interface;
  /// Like "Bus 020 Device 003: 18d1:4ee4". Empty when no device is open.
  std::string device_description;

  rndis::DeviceInfo device_info;
  rndis::NegotiatedParameters parameters;
  BridgeStats bridge;

  /// The reason when run_state == kFailed; empty otherwise.
  std::string fatal_message;
};

/// Runtime configuration. Each field's comment explains how its default was
/// derived.
struct RuntimeConfig {
  /// Device filter. All-zero means use the first RNDIS device found.
  usb::DeviceFilter device_filter;

  /// Desired MTU. Negotiated down if the device cannot handle it.
  ///
  /// The cap is bounded by feth's sysctl net.link.fake.max_mtu (2048 on this
  /// machine).
  std::uint32_t mtu = rndis::kDefaultMtu;

  /// Explicit feth interface names (e.g. "feth7"); empty means let the kernel
  /// pick the numbers.
  std::string system_interface_name;
  std::string driver_interface_name;

  /// Whether to set the system-side interface's MAC to the device-reported
  /// address.
  ///
  /// On by default: in RNDIS semantics the device *is* this interface, and the
  /// peer's ARP table and DHCP leases are established against that MAC.
  /// Turning it off is only useful when debugging MAC conflicts.
  bool adopt_device_mac = true;

  rndis::StateMachineConfig rndis;
  usb::DataChannelConfig data_channel;
  net::BpfConfig bpf;
  BridgeConfig bridge;

  /// Statistics reporting period (milliseconds); 0 means do not report.
  std::uint32_t stats_interval_millis = 5'000;

  /// Event sink. nullptr means event reporting is not needed (the command
  /// line does not need it; it reads the logs).
  ///
  /// Its lifetime is the caller's responsibility and must outlive the
  /// Runtime.
  RuntimeEventSink* event_sink = nullptr;
};

/// Assemble and run the whole driver.
///
///  Threading model: Start() is **non-blocking**
///
///   The Runtime owns one control thread, and the start sequence, keep-alive
///   loop, and teardown **all** run on it. The host thread only gives orders
///   and reads snapshots.
///
///   Why it must be this way:
///     * The start sequence contains a USB handshake which can take hundreds
///       of milliseconds to seconds on a slow device; blocking the GUI main
///       thread is unacceptable.
///     * The control channel uses libusb's synchronous API, and the
///       synchronous API returns LIBUSB_ERROR_BUSY on the libusb event
///       thread -- with Runtime owning the thread, we avoid demanding a "must
///       call from this particular thread" convention that is trivial to
///       violate.
///     * Teardown embeds RNDIS's graceful shutdown (which sends control
///       messages); putting it on the same thread satisfies `StateMachine`'s
///       "all methods called from the same thread" constraint naturally.
///
/// Usage:
/// ```
/// TETHERKIT_ASSIGN_OR_RETURN(auto runtime, Runtime::Create(config));
/// TETHERKIT_RETURN_IF_ERROR(runtime->Start());   // returns immediately
/// runtime->WaitUntilStopped();                   // command line: park the
///                                                // main thread
/// runtime->Stop();                               // idempotent
/// ```
/// A GUI does not call WaitUntilStopped; it calls Snapshot() periodically to
/// refresh the UI.
class Runtime final : public rndis::StateMachineObserver {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Runtime>> Create(const RuntimeConfig& config);

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  ~Runtime() override;

  /// Starts the control thread and returns immediately.
  ///
  /// A successful return only means the "start request was accepted"; the real
  /// outcome must be observed via Snapshot().run_state. The only failure that
  /// returns **synchronously** is the root check -- it needs no I/O, and
  /// reporting it early lets the command line print an immediate
  /// plain-language hint.
  [[nodiscard]] Status Start();

  /// Blocks until the runtime stops (graceful shutdown or fatal error). The
  /// command line uses it to park the main thread.
  void WaitUntilStopped();

  /// Requests shutdown. Callable from a signal handler or any thread;
  /// async-signal-safe (writes a single atomic).
  ///
  /// The cost is that the control thread may need up to one loop period
  /// (<=250 ms) to notice. To stop immediately use Stop(), which also wakes
  /// the control thread -- but Stop() takes a lock and is **not**
  /// async-signal-safe.
  void RequestStop() noexcept { stop_requested_.store(true, std::memory_order_release); }

  /// Requests shutdown and waits for the control thread to finish the full
  /// teardown. Idempotent.
  ///
  /// Must NOT be called from a RuntimeEventSink -- that is the control thread
  /// itself; it would deadlock waiting for itself.
  void Stop();

  /// Takes a consistent state snapshot. **Callable from any thread.**
  [[nodiscard]] RuntimeSnapshot Snapshot() const;

  /// System-side interface name, for printing "what to do next" hints to the
  /// user.
  [[nodiscard]] std::string SystemInterfaceName() const;

  // ---- StateMachineObserver (all called on the control thread) ----
  void OnStateChanged(rndis::State from, rndis::State to) override;
  void OnNegotiated(const rndis::NegotiatedParameters& parameters,
                    const rndis::DeviceInfo& info) override;
  void OnLinkStateChanged(bool connected) override;
  void OnDeviceReset(bool addressing_lost) override;
  void OnFatalError(const Error& error) override;

 private:
  Runtime() = default;

  /// Control thread body: start sequence -> control loop -> teardown.
  void RunControlThread() noexcept;

  /// The complete start sequence (formerly the body of Start()). Runs on the
  /// control thread only.
  [[nodiscard]] Status RunStartSequence();

  /// The keep-alive + statistics + stop-check loop. Runs on the control
  /// thread only.
  void RunControlLoop();

  /// Teardown in the strict reverse of the start order. Runs on the control
  /// thread only.
  void Teardown();

  /// Transitions the lifecycle state and reports the event.
  void SetRunState(RunState next);

  /// Records a fatal error: write it into the snapshot, set the kFailed flag,
  /// report the event.
  void RecordFatal(const Error& error);

  /// Refreshes the snapshot with the current state of every component. Called
  /// on the control thread only.
  void RefreshSnapshot();

  /// Hands one event to the host. A no-op when the sink is null.
  void Emit(const RuntimeEvent& event) const;

  /// Prints the "interface is ready, here is how to configure the IP" hint.
  void PrintNextSteps() const;

  RuntimeConfig config_;

  // ---- Every component below is created, accessed and destroyed by the
  // ---- control thread alone ----
  //
  // The host thread reads state exclusively through snapshot_ and never
  // touches these pointers -- otherwise the reset() in Stop() and the host's
  // reads form a data race, and such a race shows up on a real device as a
  // random crash.
  std::unique_ptr<usb::Context> usb_context_;
  std::unique_ptr<usb::Device> device_;
  std::unique_ptr<usb::UsbControlChannel> control_channel_;
  std::unique_ptr<rndis::StateMachine> state_machine_;
  std::unique_ptr<net::FethPair> feth_pair_;
  std::unique_ptr<net::BpfLink> bpf_link_;
  std::unique_ptr<usb::UsbDataChannel> data_channel_;
  std::unique_ptr<Bridge> bridge_;

  std::thread control_thread_;

  /// Guards snapshot_. The reader is the host thread (the GUI every 500 ms),
  /// the writer is the control thread; the contention is light, so a plain
  /// mutex is enough.
  mutable std::mutex snapshot_mutex_;
  RuntimeSnapshot snapshot_;

  /// Lets Stop() wake the sleeping control thread immediately instead of
  /// waiting out a full loop period.
  std::mutex stop_mutex_;
  std::condition_variable stop_condition_;

  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> fatal_error_{false};

  /// Read and written on the host thread only (Start / Stop / destructor), so
  /// no synchronization is needed.
  bool started_ = false;
  bool stopped_ = false;
};

}  // namespace tetherkit::core
