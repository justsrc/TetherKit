#include "tetherkit/common/error.h"

#include <libusb.h>

#include <array>
#include <cstring>
#include <format>

namespace tetherkit {
namespace {

/// Thread-safe strerror.
///
/// std::strerror cannot be used: it returns a pointer to a static buffer, and
/// concurrent calls from multiple threads overwrite each other (clang-tidy's
/// concurrency-mt-unsafe exists precisely to flag this).
std::string SafeStrerror(int err) {
  std::array<char, 128> buffer{};
  // Darwin provides the XSI variant of strerror_r, which returns int:
  // 0 on success, ERANGE when the buffer is too small.
  if (::strerror_r(err, buffer.data(), buffer.size()) != 0) {
    return std::format("errno {}", err);
  }
  return buffer.data();
}

}  // namespace

std::string Error::ToString() const {
  switch (domain_) {
    case ErrorDomain::kGeneric:
      return context_;

    case ErrorDomain::kErrno: {
      const int err = static_cast<int>(code_);
      return std::format("{} [errno: {}({})]", context_, SafeStrerror(err), err);
    }

    case ErrorDomain::kLibUsb: {
      const int rc = static_cast<int>(code_);
      return std::format("{} [libusb: {}({})]", context_, libusb_error_name(rc), rc);
    }

    case ErrorDomain::kRndis: {
      // The RNDIS_STATUS_* to name mapping only lives in tk_rndis
      // (rndis/protocol.cc); tk_common is not allowed to depend on upper
      // modules, so this just prints the raw numeric value --
      // the rndis layer bakes the status code's symbolic name into the
      // context string when it constructs the Error.
      return std::format("{} [RNDIS_STATUS: {:#010x}]", context_,
                         static_cast<std::uint32_t>(code_));
    }
  }
  return context_;
}

}  // namespace tetherkit
