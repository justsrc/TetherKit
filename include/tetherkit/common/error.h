// Unified error representation and propagation utilities.
//
// Layering rules (consistent across the project):
//   * Initialization / control path -- propagate errors with Result<T>. The error carries
//     the source domain, the raw code, and a human-readable context string, so "libusb
//     returned -3" can be translated into
//     "failed to claim RNDIS data interface: LIBUSB_ERROR_ACCESS (root required)".
//   * Data hot path -- **never** use this header. The std::string inside Result<T>
//     allocates heap memory, which is unacceptable at 25k-80k pps. Hot paths express
//     failures as return counts plus atomic statistics counters.
#pragma once

#include <cerrno>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace tetherkit {

namespace detail {

/// The separator in "outer cause <separator> inner cause", which varies with the current
/// language.
///
/// Forward-declared here (implemented in common/i18n.cc) instead of including i18n.h
/// directly: error.h is included across the project, and pulling in <format> and the
/// whole message table would be expensive.
[[nodiscard]] std::string_view ContextSeparator() noexcept;

}  // namespace detail

/// Source domain of an error code. Determines how the `code` field is translated to text.
enum class ErrorDomain : std::uint8_t {
  kGeneric,  ///< Pure logic error; `code` is meaningless.
  kErrno,    ///< POSIX errno (open / ioctl / read / write and other system calls).
  kLibUsb,   ///< libusb enum libusb_error (negative values).
  kRndis,    ///< RNDIS_STATUS_* status codes (32-bit unsigned).
};

/// Complete description of a failure.
///
/// Deliberately a value type rather than an exception: most errors in this project are
/// "expected environment problems" (no device plugged in, not running as root, interface
/// held by a kernel driver). Callers always have to handle them, so they should not
/// propagate via exceptions.
class Error {
 public:
  Error() = default;

  /// Pure logic error with only a description.
  static Error Generic(std::string context) {
    return Error{ErrorDomain::kGeneric, 0, std::move(context)};
  }

  /// Wrap a POSIX errno. When `err` is 0, the global errno is read automatically.
  static Error FromErrno(int err, std::string context) {
    return Error{ErrorDomain::kErrno, err != 0 ? err : errno, std::move(context)};
  }

  /// Wrap a libusb error code (libusb error codes are negative and kept as-is).
  static Error FromLibUsb(int rc, std::string context) {
    return Error{ErrorDomain::kLibUsb, rc, std::move(context)};
  }

  /// Wrap RNDIS_STATUS_*.
  static Error FromRndisStatus(std::uint32_t status, std::string context) {
    return Error{ErrorDomain::kRndis, static_cast<std::int64_t>(status), std::move(context)};
  }

  [[nodiscard]] ErrorDomain Domain() const noexcept { return domain_; }

  [[nodiscard]] std::int64_t Code() const noexcept { return code_; }

  [[nodiscard]] std::string_view Context() const noexcept { return context_; }

  /// Append a layer of context to an existing error, forming an
  /// "outer cause: inner cause" chain.
  /// Usage: `return std::unexpected(std::move(e).WithContext(Tr(Msg::kNetOpenBpfFailed)));`
  ///
  /// The separator comes from detail::ContextSeparator() rather than a literal: it is
  /// ": " today, and a future language picks its own punctuation there.
  [[nodiscard]] Error WithContext(std::string_view outer) && {
    context_ = std::string{outer} + std::string{detail::ContextSeparator()} + context_;
    return std::move(*this);
  }

  /// Render as a human-readable string such as
  /// `failed to claim data interface [libusb: LIBUSB_ERROR_ACCESS(-3)]`.
  [[nodiscard]] std::string ToString() const;

 private:
  Error(ErrorDomain domain, std::int64_t code, std::string context)
      : code_(code), context_(std::move(context)), domain_(domain) {}

  std::int64_t code_ = 0;
  std::string context_;
  ErrorDomain domain_ = ErrorDomain::kGeneric;
};

/// An operation that may fail and return a value.
template <typename T>
using Result = std::expected<T, Error>;

/// An operation that may fail but has no return value.
using Status = std::expected<void, Error>;

/// Literal for a successful Status.
inline Status Ok() noexcept {
  return Status{};
}

namespace detail {

// Generate unique temporary variable names to avoid shadowing during macro expansion.
#define TETHERKIT_CONCAT_INNER(a, b) a##b
#define TETHERKIT_CONCAT(a, b) TETHERKIT_CONCAT_INNER(a, b)
#define TETHERKIT_UNIQUE(base) TETHERKIT_CONCAT(base, __LINE__)

}  // namespace detail

/// Propagate the error upward immediately if the expression fails.
///
/// Usage: `TETHERKIT_RETURN_IF_ERROR(link.Configure(mtu));`
#define TETHERKIT_RETURN_IF_ERROR(expr)                                      \
  do {                                                                       \
    auto TETHERKIT_UNIQUE(tk_status_) = (expr);                              \
    if (!TETHERKIT_UNIQUE(tk_status_)) {                                     \
      return std::unexpected(std::move(TETHERKIT_UNIQUE(tk_status_)).error()); \
    }                                                                        \
  } while (false)

/// Assign the success value to a new variable, or propagate the error upward on failure.
///
/// Usage: `TETHERKIT_ASSIGN_OR_RETURN(auto fd, OpenBpfDevice());`
///
/// `decl` is a declaration rather than an expression and cannot be parenthesized, so the
/// macro-parentheses check is suppressed here.
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define TETHERKIT_ASSIGN_OR_RETURN(decl, expr)                              \
  auto TETHERKIT_UNIQUE(tk_result_) = (expr);                               \
  if (!TETHERKIT_UNIQUE(tk_result_)) {                                      \
    return std::unexpected(std::move(TETHERKIT_UNIQUE(tk_result_)).error()); \
  }                                                                         \
  decl = std::move(TETHERKIT_UNIQUE(tk_result_)).value()

}  // namespace tetherkit
