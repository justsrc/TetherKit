// A minimal RAII wrapper for CoreFoundation / SystemConfiguration.
//
// Wrapped only as far as "enough to get by": the one place in this project
// that touches CF is reading and writing the SCDynamicStore, pulling in a
// complete CF smart-pointer library is not worth it, and hand-written
// CFRelease calls are too easy to forget on an early-return branch.
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <string>
#include <string_view>
#include <utility>

namespace tetherkit::capi {

/// Owns a CF object and CFReleases it on destruction.
///
/// Only for references we **own** under the Create/Copy rules. A reference
/// returned by a Get rule such as CFDictionaryGetValue is not owned and must
/// never be put in here -- that would cause an over-release.
template <typename T>
class ScopedCFRef {
 public:
  ScopedCFRef() = default;
  explicit ScopedCFRef(T reference) noexcept : reference_(reference) {}

  ScopedCFRef(const ScopedCFRef&) = delete;
  ScopedCFRef& operator=(const ScopedCFRef&) = delete;

  ScopedCFRef(ScopedCFRef&& other) noexcept
      : reference_(std::exchange(other.reference_, nullptr)) {}

  ScopedCFRef& operator=(ScopedCFRef&& other) noexcept {
    if (this != &other) {
      Reset();
      reference_ = std::exchange(other.reference_, nullptr);
    }
    return *this;
  }

  ~ScopedCFRef() { Reset(); }

  [[nodiscard]] T Get() const noexcept { return reference_; }

  explicit operator bool() const noexcept { return reference_ != nullptr; }

 private:
  void Reset() noexcept {
    if (reference_ != nullptr) {
      ::CFRelease(reference_);
      reference_ = nullptr;
    }
  }

  T reference_ = nullptr;
};

/// UTF-8 string -> CFString.
[[nodiscard]] ScopedCFRef<CFStringRef> MakeCFString(std::string_view text);

/// CFString -> UTF-8 std::string. Passing nullptr yields an empty string.
[[nodiscard]] std::string CopyToStdString(CFStringRef text);

/// The process-wide shared SCDynamicStore handle. Created on the first call
/// and reused from then on; returns nullptr on failure.
///
///  Why it must be a process-lifetime handle
///   A value **set** in the SCDynamicStore by a session is deleted along with
///   that session when it is released. We have to write DNS keys into the
///   dynamic store, and with a store that is created temporarily and released
///   right after use the value we just wrote is gone a moment later -- and
///   the symptom is "the write returns success, the read-back comes back
///   empty", which is very hard to track down.
///
///   The price is that the handle is never released before the process exits,
///   and that is exactly what we want: the dynamic store entries written by
///   the helper are cleaned up automatically when it exits, leaving no litter.
[[nodiscard]] SCDynamicStoreRef SharedDynamicStore();

}  // namespace tetherkit::capi
