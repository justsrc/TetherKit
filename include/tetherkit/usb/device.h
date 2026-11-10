// Discovery, claiming and endpoint parsing for RNDIS USB devices.
//
//  Can a RNDIS interface be claimed on macOS? -- Yes, and it needs no root
//
//   This is the most critical feasibility premise of the project. The
//   conclusion comes from checking the IOKitPersonalities of every
//   CDC-family kext under /System/Library/Extensions one by one:
//
//     AppleUSBACMControl0 = {class 2, subclass 2, protocol 0}
//     AppleUSBACMControl1 = {class 2, subclass 2, protocol 1}
//     AppleUSBECMControl  = {class 2, subclass 6, protocol *}
//     AppleUSBNCMControl  = {class 2, subclass 13, protocol *}
//     AppleUSBWCMControl  = {class 2, subclass 8, protocol *}
//
//   The RNDIS communication-class interface is {class 0x02, subclass 0x02,
//   protocol **0xFF**} -- the protocol is neither 0 nor 1, so **no personality
//   matches at all**. The {0xE0, 0x01, 0x03} variant commonly used by Android
//   does not even have a personality for class 0xE0.
//   In other words, **the macOS kernel has no RNDIS driver whatsoever** (which
//   is exactly why third-party kexts such as HoRNDIS exist, and exactly why
//   this project exists).
//
//   The data-class interface {0x0A, 0x00, 0x00} really is probed by
//   AppleUSBECMData0 / AppleUSBACMData0, but their start() needs to find the
//   matching Control driver on the same device (through the bMasterInterface
//   of the CDC Union descriptor); in the RNDIS scenario the communication
//   interface is not taken over by any Control driver, so the pairing lookup
//   fails -> start() returns false -> the driver detaches, the IORegistry node
//   of that interface ends up without children, and libusb's
//   darwin_kernel_driver_active() returns 0.
//
//   At the device level AppleUSBCDCCompositeDevice does attach (its
//   IOProviderClass is IOUSBHostDevice, so it matches broadly), but it only
//   does ConfigureDevice / publishes the interface and **opens no interface**
//   at all, so it does not affect USBInterfaceOpen.
//
//   -> Conclusion: an ordinary non-sandboxed command line program **needs
//      neither root nor an entitlement**; libusb_open + libusb_claim_interface
//      succeed.
//      (The project as a whole still needs root, but that is a requirement of
//      feth and BPF, not of libusb.)
//
//  Never enable libusb_set_auto_detach_kernel_driver
//
//   On macOS it makes the claim go through darwin_capture_claim_interface,
//   which "detaches" as soon as darwin_kernel_driver_active reports true --
//   and darwin's detach implementation is **a full re-enumeration of the
//   device** (USBDeviceReEnumerate + CaptureDeviceMask), and it needs the
//   com.apple.vm.device-access entitlement or root, otherwise it returns
//   LIBUSB_ERROR_ACCESS. In the RNDIS scenario nobody holds the interface to
//   begin with, so enabling it only introduces a destructive operation.
#pragma once

#include <libusb.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tetherkit/common/error.h"
#include "tetherkit/rndis/protocol.h"
#include "tetherkit/usb/context.h"
#include "tetherkit/rndis/control_channel.h"

namespace tetherkit::usb {

/// A candidate device recognized as RNDIS.
struct DeviceCandidate {
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
  std::uint8_t bus_number = 0;
  std::uint8_t device_address = 0;

  /// The communication-class interface (carries the control channel and the
  /// interrupt notifications).
  std::uint8_t control_interface = 0;
  /// The data-class interface (carries bulk IN/OUT).
  std::uint8_t data_interface = 0;
  /// The matched interface signature, used in the log to say "which RNDIS
  /// shape this was recognized as".
  rndis::InterfaceSignature signature{};
  /// Whether the Android quirk fallback path was taken (see the comments of
  /// DeviceFinder).
  bool used_android_quirk = false;

  /// Of the form "Bus 020 Device 003: 18d1:4ee4".
  [[nodiscard]] std::string Describe() const;
};

/// Device filter criteria. All-zero / empty means no restriction.
struct DeviceFilter {
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
  /// Match only the given address on the given bus (both must be given).
  std::uint8_t bus_number = 0;
  std::uint8_t device_address = 0;
};

/// Enumerates the connected devices that look like RNDIS.
///
/// Recognition logic (the order is the priority):
///   1. Walk every interface of every configuration and find the interfaces
///      whose signature hits kControlSignature*;
///   2. Rule out fake RNDIS: a class == 0x02 interface carrying nonzero CDC
///      ACM bmCapabilities is a real cdc-acm modem, not RNDIS. **This check
///      only applies to class 0x02** -- the wireless-class (0xE0) RNDIS
///      function repurposes bmCapabilities for its own use.
///   3. Find the paired data interface: prefer the bSlaveInterface of the CDC
///      Union functional descriptor;
///   4. **Android quirk**: on many Android devices the CDC Union descriptor
///      points at a nonexistent interface number, or the CDC functional
///      descriptor is missing altogether. Fall back to the hardcoded
///      assumption "communication interface = 0, data interface = 1" (this is
///      what Linux's android_rndis_quirk does), and require the communication
///      interface to really be 0.
[[nodiscard]] Result<std::vector<DeviceCandidate>> FindRndisDevices(const Context& context,
                                                                   const DeviceFilter& filter = {});

/// An RNDIS device that has been opened and whose interfaces are claimed.
///
/// RAII: the destructor tears down in the order "release interfaces -> close
/// the handle".
/// **The caller must guarantee that every asynchronous transfer has already
/// been reaped before this object is destroyed** -- libusb_close does not
/// reap in-flight transfers for you (it merely nulls transfer->dev_handle and
/// logs a usbi_err), and the later IOKit abort still lets the callback run on
/// an internal libusb thread; if the transfer has been freed by then, that is
/// a use-after-free. See the teardown algorithm of TransferPool.
class Device {
 public:
  /// Opens the device and claims both interfaces.
  [[nodiscard]] static Result<std::unique_ptr<Device>> Open(const Context& context,
                                                            const DeviceCandidate& candidate);

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  Device(Device&&) = delete;
  Device& operator=(Device&&) = delete;

  ~Device();

  [[nodiscard]] ::libusb_device_handle* Handle() const noexcept { return handle_; }

  [[nodiscard]] const DeviceCandidate& Candidate() const noexcept { return candidate_; }

  [[nodiscard]] std::string_view Describe() const noexcept { return description_; }

  // ---------------------------------------------------------------------------
  // Endpoints
  // ---------------------------------------------------------------------------

  /// The bulk IN endpoint address (device -> host).
  [[nodiscard]] std::uint8_t BulkInEndpoint() const noexcept { return bulk_in_endpoint_; }

  /// The bulk OUT endpoint address (host -> device).
  [[nodiscard]] std::uint8_t BulkOutEndpoint() const noexcept { return bulk_out_endpoint_; }

  /// The interrupt IN endpoint address; 0 means the device has no interrupt
  /// endpoint (legal, degrades to polling).
  [[nodiscard]] std::uint8_t InterruptInEndpoint() const noexcept {
    return interrupt_in_endpoint_;
  }

  /// The wMaxPacketSize of the bulk endpoints.
  ///
  /// It has two uses: (1) derive the MaxTransferSize to advertise in
  /// INITIALIZE_MSG; (2) decide whether a bulk OUT transfer length is an
  /// exact multiple of it, which in turn decides whether a 1-byte padding is
  /// needed to dodge the ZLP.
  [[nodiscard]] std::uint16_t BulkMaxPacketSize() const noexcept { return bulk_max_packet_size_; }

  /// The wMaxPacketSize of the interrupt endpoint (RNDIS notifications are
  /// fixed at 8 bytes).
  [[nodiscard]] std::uint16_t InterruptMaxPacketSize() const noexcept {
    return interrupt_max_packet_size_;
  }

  /// The USB speed of the device, used for logging and throughput
  /// expectations.
  [[nodiscard]] int Speed() const noexcept { return speed_; }

  [[nodiscard]] std::string_view SpeedName() const noexcept;

  /// Clears the halt state of an endpoint.
  ///
  /// Recovering from a STALL can be done right inside the transfer callback:
  /// libusb_clear_halt goes through darwin_clear_halt -> ClearPipeStallBothEnds,
  /// which is a synchronous IOKit call; it does not run through the event loop
  /// and has no usbi_handling_events guard. The cost is that it blocks the
  /// event thread for tens of microseconds up to milliseconds, so it is only
  /// called on a real STALL.
  [[nodiscard]] Status ClearHalt(std::uint8_t endpoint);

 private:
  Device() = default;

  /// Parses the three endpoints out of the interface descriptors.
  [[nodiscard]] Status ResolveEndpoints(const ::libusb_config_descriptor& config);

  ::libusb_device_handle* handle_ = nullptr;
  DeviceCandidate candidate_{};
  std::string description_;

  bool control_interface_claimed_ = false;
  bool data_interface_claimed_ = false;

  std::uint8_t bulk_in_endpoint_ = 0;
  std::uint8_t bulk_out_endpoint_ = 0;
  std::uint8_t interrupt_in_endpoint_ = 0;
  std::uint16_t bulk_max_packet_size_ = 0;
  std::uint16_t interrupt_max_packet_size_ = 0;
  int speed_ = 0;
};

/// A control channel implementation based on libusb.
///
/// **Must be used from a dedicated thread that is not a libusb event
/// thread** -- the synchronous API (libusb_control_transfer) returns
/// LIBUSB_ERROR_BUSY on the event thread (usbi_handling_events is a TLS
/// check).
///
///  Interrupt notifications go through an **asynchronous** transfer; this is
///   not an optimization but a necessity
///
///   If WaitForNotification were switched to the synchronous
///   libusb_interrupt_transfer, the **control thread would hang forever**,
///   with this stack:
///       Poll -> WaitForNotification -> do_sync_bulk_transfer
///            -> sync_transfer_wait_for_completion -> handle_events -> poll(infinity)
///
///   The chain of causes:
///     1. The darwin backend tags every transfer with
///        USBI_TRANSFER_OS_HANDLES_TIMEOUT, meaning "the timeout is IOKit's
///        business, libusb does not time it itself"; so libusb_get_next_timeout
///        skips them and reports "no timeout", and the poll() inside
///        sync_transfer_wait_for_completion blocks indefinitely;
///     2. On the IOKit side, darwin's submit_interrupt_transfer uses
///        **ReadPipeAsync (the variant without a timeout)**, not the
///        ReadPipeAsyncTO used for bulk -- **the timeout parameter of an
///        interrupt transfer is simply not honored**;
///     3. So when the endpoint has no data, this synchronous transfer never
///        completes.
///
///   Changing the timeout from 0 to 1 ms is **useless** (point 2 decides that
///   it does not apply to interrupt endpoints).
///   The only correct solution is:
///   submit a permanent **asynchronous** interrupt transfer, let it complete
///   on the libusb event thread, and have WaitForNotification only check an
///   atomic flag, never entering libusb's waiting path.
class UsbControlChannel final : public rndis::ControlChannel {
 public:
  UsbControlChannel(Device& device, std::uint32_t timeout_millis);

  UsbControlChannel(const UsbControlChannel&) = delete;
  UsbControlChannel& operator=(const UsbControlChannel&) = delete;
  UsbControlChannel(UsbControlChannel&&) = delete;
  UsbControlChannel& operator=(UsbControlChannel&&) = delete;

  ~UsbControlChannel() override;

  /// Submits the permanent asynchronous interrupt transfer. A no-op when the
  /// device has no interrupt endpoint.
  [[nodiscard]] Status StartNotificationListener();

  /// Cancels the interrupt transfer and waits for the callback to be
  /// reaped. **Must not be called from the libusb event thread.**
  void StopNotificationListener();

  [[nodiscard]] Status SendMessage(std::span<const std::byte> message) override;
  [[nodiscard]] Result<std::span<const std::byte>> ReceiveMessage() override;
  [[nodiscard]] rndis::NotificationResult WaitForNotification(std::uint32_t timeout_millis) override;

  [[nodiscard]] std::uint32_t TimeoutMillis() const noexcept override { return timeout_millis_; }

  [[nodiscard]] std::string_view Describe() const noexcept override {
    return device_->Describe();
  }

 private:
  static void NotificationCallbackTrampoline(::libusb_transfer* transfer);
  void OnNotificationComplete() noexcept;

  Device* device_;
  std::uint32_t timeout_millis_;

  /// The receive buffer for GET_ENCAPSULATED_RESPONSE.
  std::vector<std::byte> response_buffer_;
  /// The notification buffer of the interrupt IN (8 bytes).
  std::vector<std::byte> notification_buffer_;

  /// The permanent asynchronous interrupt transfer.
  ::libusb_transfer* notification_transfer_ = nullptr;
  /// Whether the device has announced "there is a response to take". Set by
  /// the event thread, consumed by the control thread.
  std::atomic<bool> notification_pending_{false};
  /// Whether the interrupt transfer is in flight. Used to wait for the
  /// callback to be reaped during shutdown (avoiding a use-after-free).
  std::atomic<bool> notification_in_flight_{false};
  std::atomic<bool> notification_stopping_{false};
};

}  // namespace tetherkit::usb
