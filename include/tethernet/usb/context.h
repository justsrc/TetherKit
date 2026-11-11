// libusb context and event loop thread.
//
//  libusb's threading model on macOS (measured + confirmed in the source;
//   contrary to intuition, this must be understood)
//
//   libusb's darwin backend starts an internal thread of its own
//   (org.libusb.device-hotplug) running a CFRunLoop, and IOKit's completion
//   notifications arrive on that thread. But **the user's transfer callback
//   does not run on that thread** -- darwin_async_io_callback merely puts the
//   transfer into ctx->completed_transfers and writes the event pipe; the
//   thread that actually invokes the callback is **whichever thread calls
//   libusb_handle_events*()**.
//
//   Three ironclad rules follow:
//
//   1. Somebody must keep calling libusb_handle_events*(), otherwise the
//      callback is never invoked. That is what this class is for: one
//      dedicated thread looping on handle_events.
//
//   2. **Never call the synchronous API from inside a callback**
//      (libusb_control_transfer / libusb_bulk_transfer). They open with
//      `if (usbi_handling_events(ctx)) return LIBUSB_ERROR_BUSY;`,
//      a TLS check -- calling them from the event thread (including from
//      inside any callback) always fails.
//      -> The RNDIS control channel must therefore run on a **separate
//         thread**, see rndis/state_machine.
//
//   3. **Never do blocking I/O inside a callback.** usbi_handle_transfer_completion
//      holds ctx->event_waiters_lock while it invokes the callback; another
//      thread waiting for a synchronous transfer to complete needs that very
//      lock, so a callback that blocks holds it up for exactly as long.
//      -> The RX callback only does "decode the RNDIS packet + memcpy into the
//         lock-free queue + resubmit immediately"; the real BPF write() is
//         handed to another thread.
//
//   4. **Resubmitting the same transfer directly from inside the callback is
//      officially supported** (usbi_handle_transfer_completion has already
//      removed it from the flying list and cleared the IN_FLIGHT flag before
//      invoking the callback, and does not hold itransfer->lock). This is the
//      key trick for keeping a USB pipe saturated.
#pragma once

#include "tethernet/common/error.h"

#include <libusb.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace tethernet::usb {

/// A libusb context plus a dedicated event loop thread.
///
/// Lifetime constraint: every device handle and transfer must be released
/// **before** this object is destroyed. Destructor order = stop the event
/// thread -> libusb_exit.
class Context {
 public:
  /// Initializes libusb and starts the event thread.
  [[nodiscard]] static Result<std::unique_ptr<Context>> Create();

  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  Context(Context&&) = delete;
  Context& operator=(Context&&) = delete;

  ~Context();

  [[nodiscard]] ::libusb_context* Raw() const noexcept { return context_; }

  /// The libusb version string, for logging.
  [[nodiscard]] static std::string VersionString();

  /// Whether hotplug is supported (always true on darwin).
  [[nodiscard]] static bool SupportsHotplug() noexcept;

  /// Asks the event thread to exit. Callable from any thread, idempotent.
  void RequestStop() noexcept;

  /// Whether the event thread is still running.
  [[nodiscard]] bool Running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

 private:
  Context() = default;

  /// The event thread body.
  void RunEventLoop() noexcept;

  ::libusb_context* context_ = nullptr;
  std::thread event_thread_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
};

}  // namespace tethernet::usb
