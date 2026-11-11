// RNDIS control channel abstraction.
//
// Why this interface lives in the rndis layer rather than the usb layer: it describes the
// **RNDIS protocol** control-channel semantics (send a message, fetch a message, wait
// for a "response available" notification), not USB semantics. The state machine
// (tn_rndis) depends on it and the libusb implementation (tn_usb) provides it -- so the
// dependency direction is the correct rndis <- usb, with no reverse dependency from
// tn_rndis on tn_usb.
//
// RNDIS control messages do not go through bulk endpoints, but through two class
// requests on the USB control endpoint (EP0):
//   SEND_ENCAPSULATED_COMMAND  (bmRequestType=0x21, bRequest=0x00)
//   GET_ENCAPSULATED_RESPONSE  (bmRequestType=0xA1, bRequest=0x01)
// plus the interrupt IN endpoint on the communication-class interface, which notifies
// that "a response is available".
//
// Why abstract this as an interface: the state machine is the most logically complex and
// most test-demanding part of the RNDIS implementation, while the development machine has
// no USB devices at all. With the control channel abstracted away, the state machine can
// be driven entirely in memory, including paths that are extremely hard to reproduce on
// a real device (device-initiated KEEPALIVE, INDICATE_STATUS jumping the queue,
// re-sending required after RESET, out-of-order responses, timeout retries).
//
// Virtual dispatch here is no performance concern: the control channel does one
// keep-alive round trip every 5 seconds, and on Apple Silicon libusb control transfers
// themselves are **millisecond**-scale
// (libusb issue #1288: control transfers on M2 are about 10x slower than on x64).
#pragma once

#include "tethernet/common/error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tethernet::rndis {

/// Timeout value to pass when "just probing, do not really wait".
///
/// **Must not be 0.** On darwin, libusb passes timeout to IOKit as both noDataTimeout
/// and completionTimeout, and **0 means wait forever** -- passing 0 blocks the calling
/// thread in `libusb_wait_for_event` forever, the entire control loop hangs, and even
/// shutdown signals cannot be handled.
/// (See AGENTS.md section 7, pitfalls 12 and 13.)
/// libusb's synchronous API has no true non-blocking mode; the shortest wait possible is
/// 1 ms.
inline constexpr std::uint32_t kProbeOnlyTimeoutMillis = 1;

/// Result of waiting for a notification.
enum class NotificationResult : std::uint8_t {
  kResponseAvailable,  ///< The device explicitly notified that a response is available.
  kTimeout,            ///< Wait timed out. **Not necessarily an error** -- see below.
  kNotSupported,       ///< The device has no interrupt endpoint; the caller should poll
                       ///< directly.
};

/// RNDIS control channel.
///
/// Implementations must guarantee: all methods are called from **the same thread** (the
/// state-machine thread). This is not to simplify the implementation but a hard libusb
/// constraint -- calling the synchronous API from the event thread returns
/// LIBUSB_ERROR_BUSY, so the control channel must own a non-event thread exclusively.
class ControlChannel {
 public:
  ControlChannel() = default;
  ControlChannel(const ControlChannel&) = delete;
  ControlChannel& operator=(const ControlChannel&) = delete;
  ControlChannel(ControlChannel&&) = delete;
  ControlChannel& operator=(ControlChannel&&) = delete;
  virtual ~ControlChannel() = default;

  /// Send an RNDIS control message (SEND_ENCAPSULATED_COMMAND).
  [[nodiscard]] virtual Status SendMessage(std::span<const std::byte> message) = 0;

  /// Retrieve an RNDIS control message (GET_ENCAPSULATED_RESPONSE).
  ///
  /// The returned view points into the implementation's internal buffer and remains valid
  /// until the next call to this method.
  ///
  /// **An empty view is not an error**: the specification says that when no valid
  /// response is ready yet the device should return **1 byte 0x00** rather than STALL the
  /// control endpoint. Therefore a result shorter than 8 bytes must be treated by the
  /// caller as "not ready yet, retry later", not as a failure.
  [[nodiscard]] virtual Result<std::span<const std::byte>> ReceiveMessage() = 0;

  /// Wait for the device's RESPONSE_AVAILABLE notification.
  ///
  /// Interrupt endpoint handling must be compatible with two device behaviors:
  ///   * The specification behavior: host waits for an 8-byte notification on interrupt
  ///     IN (two LE32s: 0x00000001 = RESPONSE_AVAILABLE, 0);
  ///   * The Linux behavior: completely ignore the interrupt endpoint and poll
  ///     GET_ENCAPSULATED_RESPONSE directly up to 10 times with 40 ms intervals.
  /// And conversely, there are **devices that must have the interrupt endpoint read once
  /// before they will answer on the control endpoint**. So the correct strategy is: wait
  /// for the notification, but still poll once after timeout. kTimeout is therefore not
  /// an error.
  ///
  /// @param timeout_millis Wait limit. **0 means wait forever** (consistent with libusb
  ///        semantics) -- to "just probe" pass kProbeOnlyTimeoutMillis, not 0.
  ///        Implementations should clamp 0 defensively; see UsbControlChannel.
  [[nodiscard]] virtual NotificationResult WaitForNotification(std::uint32_t timeout_millis) = 0;

  /// Control transfer timeout (milliseconds).
  [[nodiscard]] virtual std::uint32_t TimeoutMillis() const noexcept = 0;

  /// Human-readable identifier for logging (e.g. "Bus 020 Device 003: 18d1:4ee4").
  [[nodiscard]] virtual std::string_view Describe() const noexcept = 0;
};

}  // namespace tethernet::rndis
