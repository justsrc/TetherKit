#include "core_foundation_support.h"

#include <vector>

namespace tetherkit::capi {

ScopedCFRef<CFStringRef> MakeCFString(std::string_view text) {
  return ScopedCFRef<CFStringRef>{::CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),  // NOLINT
      static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8,
      /*isExternalRepresentation=*/static_cast<Boolean>(false))};
}

std::string CopyToStdString(CFStringRef text) {
  if (text == nullptr) {
    return {};
  }

  // First try the zero-copy fast path: CFStringGetCStringPtr hands back the
  // pointer directly when the string is already UTF-8 internally, which hits
  // in the vast majority of cases (what we read are things like interface
  // names and IPs, i.e. ASCII).
  if (const char* direct = ::CFStringGetCStringPtr(text, kCFStringEncodingUTF8);
      direct != nullptr) {
    return std::string{direct};
  }

  const CFIndex length = ::CFStringGetLength(text);
  const CFIndex capacity = ::CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::vector<char> buffer(static_cast<std::size_t>(capacity), '\0');
  if (::CFStringGetCString(text, buffer.data(), capacity, kCFStringEncodingUTF8) == 0) {
    return {};
  }
  return std::string{buffer.data()};
}

SCDynamicStoreRef SharedDynamicStore() {
  // A function-local static: created on the first call, never released before
  // the process exits. See the header for the reason.
  //
  // No lock is needed -- since C++11 the initialization of a function-local
  // static is thread-safe (the magic static), so concurrent first calls let
  // only one thread actually run the initialization.
  static SCDynamicStoreRef store = ::SCDynamicStoreCreate(
      kCFAllocatorDefault, CFSTR("TetherKit"), /*callout=*/nullptr, /*context=*/nullptr);
  return store;
}

}  // namespace tetherkit::capi
