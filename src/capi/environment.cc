// The three root-free capabilities: version, environment preflight, device
// enumeration.
//
// The GUI itself (uid 501) calls only this group; the session and the
// interface configuration that need root are handed to tethernet-helper. This
// split is one of the core conclusions of docs/GUI-SPIKE.md.
#include "tethernet/capi/tethernet_c.h"
#include "tethernet/common/i18n.h"
#include "tethernet/net/feth_device.h"
#include "tethernet/usb/context.h"
#include "tethernet/usb/device.h"
#include "tethernet/version.h"

#include <libusb.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "capi_support.h"

namespace {

using tethernet::capi::ClearError;
using tethernet::capi::CopyText;
using tethernet::capi::FillError;

/// Reads a USB string descriptor into a fixed-size buffer. An index of 0 means
/// the device provides no such string.
///
/// Uses libusb's `_ascii` variant: it replaces code points beyond ASCII in the
/// UTF-16LE with '?'. That is good enough for us -- these strings only let the
/// user tell two devices of the same model apart, and most manufacturer names
/// and serial numbers are ASCII to begin with. A full UTF-16 -> UTF-8
/// conversion of our own would gain too little to be worth it.
void ReadStringDescriptor(::libusb_device_handle* handle, std::uint8_t index, char* destination,
                          std::size_t capacity) noexcept {
  if (index == 0) {
    return;
  }
  std::array<unsigned char, TN_USB_STRING_CAPACITY> buffer{};
  const int length = ::libusb_get_string_descriptor_ascii(
      handle, index, buffer.data(), static_cast<int>(buffer.size()));
  if (length <= 0) {
    return;
  }
  CopyText(destination, capacity,
           std::string_view{reinterpret_cast<const char*>(buffer.data()),
                            static_cast<std::size_t>(length)});
}

/// Best-effort fill-in of the manufacturer name / product name / serial number.
///
/// Why "best effort": reading a string descriptor requires libusb_open first,
/// and the device may already be held exclusively by another process on this
/// machine (a running tethernet-helper, say), in which case the darwin backend
/// returns LIBUSB_ERROR_ACCESS. That is not an error, it is just an
/// unavailable name -- at that point the string memory back-fills the value
/// read last time (see the end of tn_list_devices), and only when there is no
/// memory either does it fall back to VID:PID. It must never make the whole
/// enumeration fail.
void TryReadStrings(::libusb_context* context, tn_device_info_t& info) noexcept {
  ::libusb_device** list = nullptr;
  const ssize_t count = ::libusb_get_device_list(context, &list);
  if (count < 0) {
    return;
  }

  for (ssize_t i = 0; i < count; ++i) {
    if (::libusb_get_bus_number(list[i]) != info.bus_number ||
        ::libusb_get_device_address(list[i]) != info.device_address) {
      continue;
    }

    ::libusb_device_descriptor descriptor{};
    if (::libusb_get_device_descriptor(list[i], &descriptor) != LIBUSB_SUCCESS) {
      break;
    }
    ::libusb_device_handle* handle = nullptr;
    if (::libusb_open(list[i], &handle) != LIBUSB_SUCCESS) {
      break;
    }
    ReadStringDescriptor(handle, descriptor.iManufacturer, info.manufacturer,
                         sizeof(info.manufacturer));
    ReadStringDescriptor(handle, descriptor.iProduct, info.product, sizeof(info.product));
    ReadStringDescriptor(handle, descriptor.iSerialNumber, info.serial, sizeof(info.serial));
    ::libusb_close(handle);
    break;
  }

  // The second argument is 1: release the list together with the reference
  // count of every device in it.
  ::libusb_free_device_list(list, 1);
}

/// The process-wide shared libusb context, **for enumeration only**.
///
///  Why it must be shared
///   Creating a new context for every enumeration means running the whole
///   "initialize libusb -> start the event thread -> start the IOKit runloop
///   thread -> tear everything down" sequence every time. The GUI refreshes
///   the device list every few hundred milliseconds, so that overhead repeats
///   at the same rate, and the log fills up with "libusb initialized".
///
///   Sharing it means the whole process initializes once. A long-lived context
///   still sees newly plugged devices -- libusb's darwin backend has its own
///   hotplug thread maintaining the device list, which is the regular
///   mechanism every libusb program depends on.
///
/// The session (core::Runtime) has a context of its own and does not reuse
/// this one: its lifetime is managed by the session, and mixing it in would
/// only complicate the shutdown ordering. libusb supports multiple contexts
/// per process.
///
/// On initialization failure this returns nullptr and fills in the error, and
/// it does **not** cache the failure -- the next call retries.
tethernet::usb::Context* SharedEnumerationContext(tn_error_t* out_error) {
  static std::mutex mutex;
  static std::unique_ptr<tethernet::usb::Context> context;

  const std::lock_guard<std::mutex> guard(mutex);
  if (context == nullptr) {
    auto created = tethernet::usb::Context::Create();
    if (!created) {
      FillError(out_error, created.error());
      return nullptr;
    }
    context = std::move(*created);
  }
  return context.get();
}

/// The process-wide memory of string descriptors (lock + entries), the same
/// function-local static pattern as the shared enumeration context.
///
/// It lives at process level rather than in the caller: the helper is a
/// long-lived process, so the very first enumeration after the GUI restarts
/// already gets the back-filled names. The pure logic is described in
/// ReconcileDeviceStrings.
struct StringMemory {
  std::mutex mutex;
  std::vector<tethernet::capi::RememberedDeviceStrings> entries;
};

StringMemory& SharedStringMemory() {
  static StringMemory memory;
  return memory;
}

void FillDeviceInfo(const tethernet::usb::DeviceCandidate& candidate,
                    tn_device_info_t& info) noexcept {
  info = tn_device_info_t{};
  info.vendor_id = candidate.vendor_id;
  info.product_id = candidate.product_id;
  info.bus_number = candidate.bus_number;
  info.device_address = candidate.device_address;
  info.control_interface = candidate.control_interface;
  info.data_interface = candidate.data_interface;
  info.interface_class = candidate.signature.interface_class;
  info.interface_subclass = candidate.signature.interface_subclass;
  info.interface_protocol = candidate.signature.interface_protocol;
  info.used_android_quirk = candidate.used_android_quirk;
  CopyText(info.description, candidate.Describe());
}

}  // namespace

namespace {

/// tn_language_t > C++ Language. Unknown values return nullopt, which makes
/// the set call a no-op: a caller still passing a removed language id (the
/// old Chinese value 1) keeps working and simply stays in English.
[[nodiscard]] std::optional<tethernet::Language> ToLanguage(std::int32_t value) noexcept {
  switch (value) {
    case TN_LANGUAGE_ENGLISH:
      return tethernet::Language::kEnglish;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::int32_t FromLanguage(tethernet::Language /*language*/) noexcept {
  // English is the only language shipped today; the parameter is kept so the
  // signature survives a second language unchanged.
  return TN_LANGUAGE_ENGLISH;
}

}  // namespace

void tn_set_language(int32_t language) {
  if (const std::optional<tethernet::Language> parsed = ToLanguage(language); parsed.has_value()) {
    tethernet::SetLanguage(*parsed);
  }
}

int32_t tn_get_language(void) {
  return FromLanguage(tethernet::GetLanguage());
}

int32_t tn_detect_system_language(void) {
  return FromLanguage(tethernet::DetectLanguageFromEnvironment());
}

void tn_version(tn_version_info_t* out_version) {
  if (out_version == nullptr) {
    return;
  }
  *out_version = tn_version_info_t{};
  const tethernet::Version version = tethernet::GetVersion();
  out_version->major = version.major;
  out_version->minor = version.minor;
  out_version->patch = version.patch;
  CopyText(out_version->text, tethernet::GetVersionString());
  CopyText(out_version->build, tethernet::GetBuildDescription());
  CopyText(out_version->libusb, tethernet::usb::Context::VersionString());
}

tn_result_t tn_check_environment(tn_environment_t* out_environment) {
  if (out_environment == nullptr) {
    return TN_ERR_INVALID_ARGUMENT;
  }
  *out_environment = tn_environment_t{};

  out_environment->is_root = tethernet::net::IsRunningAsRoot();

  // These sysctls are a **creation-time snapshot** of feth; changing them
  // after the interface exists has no effect, so they must be queried up
  // front. When they do not qualify, the reason is handed to the GUI as-is --
  // the user has to see which switch is turned on to know what to change.
  if (const auto status = tethernet::net::VerifyFethSysctls(); status) {
    out_environment->sysctls_ok = true;
  } else {
    out_environment->sysctls_ok = false;
    CopyText(out_environment->sysctl_detail, status.error().ToString());
  }

  if (const auto max_mtu = tethernet::net::QueryFethMaxMtu(); max_mtu) {
    out_environment->feth_max_mtu = *max_mtu;
  }

  // Deliberately always returns success: "the environment does not qualify" is
  // a **result** to be shown to the user, not a call failure.
  return TN_OK;
}

tn_result_t tn_list_devices(tn_device_info_t* out_devices, size_t capacity, size_t* out_count,
                            bool read_strings, tn_error_t* out_error) {
  if (out_count == nullptr) {
    return TN_ERR_INVALID_ARGUMENT;
  }
  ClearError(out_error);
  *out_count = 0;

  tethernet::usb::Context* context = SharedEnumerationContext(out_error);
  if (context == nullptr) {
    return TN_ERR_FAILED;
  }

  auto candidates = tethernet::usb::FindRndisDevices(*context, {});
  if (!candidates) {
    FillError(out_error, candidates.error());
    return TN_ERR_FAILED;
  }

  *out_count = candidates->size();
  if (out_devices == nullptr) {
    return TN_OK;
  }

  const std::size_t writable = std::min(capacity, candidates->size());
  for (std::size_t i = 0; i < writable; ++i) {
    FillDeviceInfo((*candidates)[i], out_devices[i]);
    if (read_strings) {
      TryReadStrings(context->Raw(), out_devices[i]);
    }
  }

  // Remember what was read, back-fill what was not -- so that while a session
  // is running (read_strings=false, or the device is busy) the name does not
  // vanish from the UI. `present` uses the full candidate set rather than the
  // writable prefix: when the capacity is too small the devices after that
  // are still present, and their memory must not be dropped as "unplugged".
  {
    std::vector<tethernet::capi::DeviceIdentity> present;
    present.reserve(candidates->size());
    for (const tethernet::usb::DeviceCandidate& candidate : *candidates) {
      present.push_back({.bus_number = candidate.bus_number,
                         .device_address = candidate.device_address,
                         .vendor_id = candidate.vendor_id,
                         .product_id = candidate.product_id});
    }
    StringMemory& memory = SharedStringMemory();
    const std::lock_guard<std::mutex> guard(memory.mutex);
    tethernet::capi::ReconcileDeviceStrings(memory.entries, present, {out_devices, writable});
  }
  return TN_OK;
}
