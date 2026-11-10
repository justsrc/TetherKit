// Lightweight leveled logging.
//
// Design constraints:
//   1. **Must not appear on data hot paths.** Trace/Debug levels are removed entirely by
//      the preprocessor in Release builds (even argument evaluation does not happen), so
//      writing TETHERKIT_TRACE on a hot path is safe; but Info and above really do
//      formatting and a locked stderr write, so hot paths must not use them.
//   2. Thread-safe: concurrent writes to stderr from multiple threads would interleave,
//      so each full line is written under a mutex. Logging is not on hot paths, so lock
//      contention does not matter.
//   3. No third-party logging library: "time + level + thread name + location + message"
//      is all that is needed, and std::format is enough.
#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

#include "tetherkit/common/i18n.h"

namespace tetherkit {

enum class LogLevel : std::uint8_t {
  kTrace = 0,  ///< Per-frame details, removed at compile time in Release builds.
  kDebug = 1,  ///< Protocol interaction details (every RNDIS control message), removed at
               ///< compile time in Release builds.
  kInfo = 2,   ///< Lifecycle events (device connect, state machine transition, statistics
               ///< report).
  kWarn = 3,   ///< Recoverable anomalies (USB stall, BPF frame drop, keep-alive timeout
               ///< retry).
  kError = 4,  ///< Unrecoverable error.
  kOff = 5,    ///< Fully off.
};

/// Compile-time log floor. Log calls below this level are removed entirely by the
/// preprocessor.
///
/// In Release builds (NDEBUG), Trace/Debug are removed so that TETHERKIT_TRACE on hot
/// paths does not even evaluate its arguments.
#ifndef TETHERKIT_COMPILED_MIN_LOG_LEVEL
#ifdef NDEBUG
#define TETHERKIT_COMPILED_MIN_LOG_LEVEL 2  // kInfo
#else
#define TETHERKIT_COMPILED_MIN_LOG_LEVEL 0  // kTrace
#endif
#endif

/// Runtime log floor, kInfo by default.
void SetLogLevel(LogLevel level) noexcept;

LogLevel GetLogLevel() noexcept;

/// Whether to enable colored output. Enabled by default only when stderr is a tty.
void SetLogColorEnabled(bool enabled) noexcept;

/// Give the current thread a short name that appears in log lines, making it easy to
/// distinguish usb-event / bpf-rx / bpf-tx.
/// Also calls pthread_setname_np so the name is visible in Instruments / lldb.
void SetCurrentThreadName(std::string_view name) noexcept;

/// Log sink: **additionally** hand each formatted log line to the host (the GUI needs to
/// show logs in the interface, while logging itself only writes to stderr). stderr output
/// is unaffected; the two run in parallel.
///
/// Three hard constraints when implementing a log sink:
///   1. It will be called on **any thread** (including the libusb event thread);
///   2. It will be called **inside** the logging mutex -- so the implementation must
///      **never log** (std::mutex is not reentrant and would deadlock immediately);
///   3. It must never do blocking I/O -- that would stall every thread that is logging.
///
/// The only reasonable implementation that satisfies all three is "copy into a fixed-size
/// ring buffer and let the host poll it out".
using LogSink = void (*)(LogLevel level, std::string_view thread_name, std::string_view message,
                         void* user) noexcept;

/// Install / uninstall the log sink. Pass nullptr to uninstall. Thread-safe.
void SetLogSink(LogSink sink, void* user) noexcept;

namespace detail {

[[nodiscard]] bool IsLogLevelEnabled(LogLevel level) noexcept;

/// Emit a full log line. `message` is already formatted.
void EmitLogLine(LogLevel level, std::string_view file, unsigned line,
                 std::string_view message) noexcept;

/// Extract the file name from __FILE__ to avoid long paths in log lines.
constexpr std::string_view BaseName(std::string_view path) noexcept {
  const auto pos = path.find_last_of('/');
  return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

/// Format and emit. Deliberately a function template rather than inlined in the macro
/// to keep macro expansion small.
template <typename... Args>
void LogFormatted(LogLevel level, std::string_view file, unsigned line,
                  std::format_string<Args...> fmt, Args&&... args) noexcept {
  // std::format may throw on out-of-memory; a logging failure must never take down the
  // driver.
  try {
    EmitLogLine(level, file, line, std::format(fmt, std::forward<Args>(args)...));
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Text() only looks up and returns a string_view, no allocation, no throw -- safe
    // in this catch.
    EmitLogLine(LogLevel::kError, file, line, Text(Msg::kCommonLogFormatFailed));
  }
}

}  // namespace detail

/// Logging macros. Do compile-time level trimming first, then the runtime level check
/// (branch-prediction friendly).
#define TETHERKIT_LOG(level, ...)                                                          \
  do {                                                                                     \
    if constexpr (static_cast<int>(level) >= TETHERKIT_COMPILED_MIN_LOG_LEVEL) {            \
      if (::tetherkit::detail::IsLogLevelEnabled(level)) [[unlikely]] {                     \
        ::tetherkit::detail::LogFormatted((level), ::tetherkit::detail::BaseName(__FILE__), \
                                          __LINE__, __VA_ARGS__);                           \
      }                                                                                     \
    }                                                                                       \
  } while (false)

#define TETHERKIT_TRACE(...) TETHERKIT_LOG(::tetherkit::LogLevel::kTrace, __VA_ARGS__)
#define TETHERKIT_DEBUG(...) TETHERKIT_LOG(::tetherkit::LogLevel::kDebug, __VA_ARGS__)
#define TETHERKIT_INFO(...) TETHERKIT_LOG(::tetherkit::LogLevel::kInfo, __VA_ARGS__)
#define TETHERKIT_WARN(...) TETHERKIT_LOG(::tetherkit::LogLevel::kWarn, __VA_ARGS__)
#define TETHERKIT_ERROR(...) TETHERKIT_LOG(::tetherkit::LogLevel::kError, __VA_ARGS__)

/// Log a **translatable** message. `id` is a `Msg` enum value; the remaining arguments
/// match std::format.
///
/// Why this family exists instead of writing `TETHERKIT_INFO("{}", Tr(id, ...))`
/// directly: as macros, Tr() stays **inside** the level check, so when the level is
/// disabled both the table lookup and formatting are skipped -- the handwritten form
/// above makes it easy to accidentally put Tr() outside the macro and pay for it on
/// every log line.
#define TETHERKIT_LOG_TR(level, id, ...) \
  TETHERKIT_LOG(level, "{}", ::tetherkit::Tr((id)__VA_OPT__(, ) __VA_ARGS__))

#define TETHERKIT_TRACE_TR(id, ...) \
  TETHERKIT_LOG_TR(::tetherkit::LogLevel::kTrace, id __VA_OPT__(, ) __VA_ARGS__)
#define TETHERKIT_DEBUG_TR(id, ...) \
  TETHERKIT_LOG_TR(::tetherkit::LogLevel::kDebug, id __VA_OPT__(, ) __VA_ARGS__)
#define TETHERKIT_INFO_TR(id, ...) \
  TETHERKIT_LOG_TR(::tetherkit::LogLevel::kInfo, id __VA_OPT__(, ) __VA_ARGS__)
#define TETHERKIT_WARN_TR(id, ...) \
  TETHERKIT_LOG_TR(::tetherkit::LogLevel::kWarn, id __VA_OPT__(, ) __VA_ARGS__)
#define TETHERKIT_ERROR_TR(id, ...) \
  TETHERKIT_LOG_TR(::tetherkit::LogLevel::kError, id __VA_OPT__(, ) __VA_ARGS__)

}  // namespace tetherkit
