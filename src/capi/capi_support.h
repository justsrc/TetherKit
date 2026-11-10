// Internal helpers for the C ABI implementation. **Not installed**; used only
// inside src/capi.
//
// The three recurring chores of the "C++ world <-> C world" bridge live here:
// copying a std::string_view into a fixed-size buffer, translating a
// tetherkit::Error into a tk_error_t, and reading the wall clock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "tetherkit/capi/tetherkit_c.h"
#include "tetherkit/common/error.h"

namespace tetherkit::capi {

/// Copies text into a fixed-size buffer, guarantees NUL termination, and
/// truncates when it is too long.
///
///  The truncation must land on a UTF-8 character boundary
///   The user-facing strings of this project are not necessarily ASCII: one
///   CJK character takes 3 bytes. A hard byte cut leaves half a character at
///   the end of the buffer, and on the Swift side `String(cString:)` turns the
///   **whole string** into replacement characters when it meets an invalid
///   sequence -- which destroys the entire error message. So after truncating
///   we walk back to the nearest character start.
inline void CopyText(char* destination, std::size_t capacity, std::string_view text) noexcept {
  if (destination == nullptr || capacity == 0) {
    return;
  }
  std::size_t length = text.size() < capacity - 1 ? text.size() : capacity - 1;

  // length < text.size() means truncation happened: if the cut point landed in
  // the middle of a multibyte sequence (text[length] is a 0b10xxxxxx
  // continuation byte), walk back to the start of that sequence.
  while (length > 0 && length < text.size() &&
         (static_cast<unsigned char>(text[length]) & 0xC0U) == 0x80U) {
    --length;
  }

  std::memcpy(destination, text.data(), length);
  destination[length] = '\0';
}

/// Array version, to avoid writing sizeof at every call site.
template <std::size_t N>
inline void CopyText(char (&destination)[N], std::string_view text) noexcept {
  CopyText(destination, N, text);
}

/// Translates a C++-side error into the C struct. `out_error` may be nullptr
/// (the caller does not care about the reason).
inline void FillError(tk_error_t* out_error, const Error& error) noexcept {
  if (out_error == nullptr) {
    return;
  }
  out_error->domain = static_cast<std::int32_t>(error.Domain());
  out_error->code = error.Code();
  // ToString() rather than Context(): the former carries the domain and code
  // (such as "[libusb: LIBUSB_ERROR_ACCESS(-3)]"), which the user can paste
  // straight to us when reporting a problem.
  CopyText(out_error->message, error.ToString());
}

/// Fills in a purely logical error that carries no underlying error code.
inline void FillGenericError(tk_error_t* out_error, std::string_view message) noexcept {
  if (out_error == nullptr) {
    return;
  }
  out_error->domain = TK_ERROR_DOMAIN_GENERIC;
  out_error->code = 0;
  CopyText(out_error->message, message);
}

/// Clears the error struct to "no error". Called on the success path so the
/// caller never reads a leftover from the previous call.
inline void ClearError(tk_error_t* out_error) noexcept {
  if (out_error == nullptr) {
    return;
  }
  out_error->domain = TK_ERROR_DOMAIN_GENERIC;
  out_error->code = 0;
  out_error->message[0] = '\0';
}

/// The tuple that uniquely identifies "a device plugged into the bus".
///
/// A bus address can be reused by the system after an unplug, so adding
/// VID:PID is what keeps one device's memory from being attached to a
/// different device that took over the same address.
struct DeviceIdentity {
  std::uint8_t bus_number = 0;
  std::uint8_t device_address = 0;
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;

  [[nodiscard]] bool operator==(const DeviceIdentity&) const noexcept = default;
};

/// Extracts the device identity from an enumeration result.
[[nodiscard]] inline DeviceIdentity IdentityOf(const tk_device_info_t& info) noexcept {
  return DeviceIdentity{.bus_number = info.bus_number,
                        .device_address = info.device_address,
                        .vendor_id = info.vendor_id,
                        .product_id = info.product_id};
}

/// The string descriptors **successfully read** from a device most recently.
struct RememberedDeviceStrings {
  DeviceIdentity identity;
  char manufacturer[TK_USB_STRING_CAPACITY]{};
  char product[TK_USB_STRING_CAPACITY]{};
  char serial[TK_USB_STRING_CAPACITY]{};
};

/// Maintains the memory of string descriptors: remember what was read, and
/// back-fill from memory what could not be read.
///
///  Why it is needed
///   A string descriptor can only be read after libusb_open, and while a
///   session is running the device is held exclusively by this process, so
///   the read is either skipped by the caller or fails outright -- but the
///   device itself has not changed, and "not available this time" does not
///   mean "the device has no name". Without the back-fill, one enumeration at
///   the moment of connection overwrites the "vivo iQOO Z10x" in the UI with
///   "USB device 2d95:600b", and that stays there for the whole session.
///
/// The rules (`infos` are the devices of this enumeration whose basic fields
/// are already filled in):
///   - **any** of the three strings of `infos[i]` is non-empty -> it was read
///     this time, record it into `memory` overwriting whatever was there;
///   - all empty -> it was not read (skipped, or the device was busy); if
///     `memory` holds an entry with the same identity, back-fill from it;
///   - entries in `memory` whose identity is not in `present` (all devices
///     currently on the bus) are dropped -- the device has been unplugged and
///     the memory is stale; if another device that yields no name is plugged
///     into the same address, showing VID:PID is better than handing it the
///     previous device's name.
///
/// `present` can be longer than `infos`: when the caller's array capacity is
/// too small only the first few are filled in, and the ones after that are
/// still present -- their memory must not be mistaken for "unplugged".
/// Pure logic, does not touch libusb, and the lock is the caller's
/// responsibility -- that is what makes it testable offline.
void ReconcileDeviceStrings(std::vector<RememberedDeviceStrings>& memory,
                            std::span<const DeviceIdentity> present,
                            std::span<tk_device_info_t> infos);

/// Wall clock time since the Unix epoch (nanoseconds).
///
/// Events and logs use the wall clock rather than the monotonic clock: they
/// are shown to people and have to line up with the system log in Console.app.
/// Rate calculation uses the monotonic clock instead; see
/// tk_session_status.monotonic_nanos.
[[nodiscard]] std::int64_t WallNanos() noexcept;

/// Validates the interface name: only `feth<digits>` is accepted.
///
/// This is not fussiness -- the interface name ends up in the argv of
/// `ipconfig` / `route`. We do pass an array to posix_spawn instead of going
/// through a shell (which already rules out injection), but restricting the
/// interface name to the shape we ourselves create also stops the more
/// realistic accidents, such as "en0 got passed in and the user's Wi-Fi
/// configuration got wiped".
[[nodiscard]] bool IsValidFethName(std::string_view name) noexcept;

/// Installs the on-disk registry of feth interfaces (see orphan_cleanup.cc).
/// Idempotent.
///
/// Called by tk_session_create -- only a session that really creates an
/// interface needs to register, which keeps the root-free group of entry
/// points (version, enumeration, preflight) free of side effects.
void InstallInterfaceRegistry();

}  // namespace tetherkit::capi
