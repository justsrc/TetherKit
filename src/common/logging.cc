#include "tethernet/common/logging.h"

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace tethernet {
namespace {

// The log configuration is process-wide singleton state, naturally a mutable
// global; once it is made thread-safe with atomics, splitting it into a
// singleton class would only add indirection with no real benefit, so the
// related checks are exempted wholesale here.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)

/// The runtime log floor. An atomic rather than a mutex: reads happen on the
/// branch of every log line.
std::atomic<LogLevel> g_runtime_level{LogLevel::kInfo};

/// Whether ANSI colors are emitted. Lazily initialized to `isatty(STDERR_FILENO)`.
std::atomic<int> g_color_enabled{-1};

/// Protects stderr's whole-line output so that logs from multiple threads do
/// not interleave. Logging is not on the data hot path, so lock contention is
/// negligible.
std::mutex& OutputMutex() {
  static std::mutex mutex;
  return mutex;
}

/// The host-installed log sink. The sink and its user must be updated **as a
/// pair**, so this is protected by OutputMutex rather than two independent
/// atomics -- two atomics cannot be updated together atomically, and a reader
/// could observe a mismatched combination.
LogSink g_log_sink = nullptr;
void* g_log_sink_user = nullptr;

/// The thread name. thread_local rather than querying pthread_getname_np:
/// the latter is a syscall.
constexpr std::size_t kThreadNameCapacity = 16;
thread_local std::array<char, kThreadNameCapacity> t_thread_name{};

std::string_view CurrentThreadName() {
  if (t_thread_name[0] == '\0') {
    return "main";
  }
  return {t_thread_name.data(), std::strlen(t_thread_name.data())};
}

/// Level labels and ANSI color codes.
///
/// Deliberately `const char*` instead of `std::string_view`: these values are
/// fed straight to `%s`, and the `data()` of an empty string_view is nullptr,
/// which is undefined behavior when passed to printf.
struct LevelStyle {
  const char* label;
  const char* color;
};

LevelStyle StyleFor(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace:
      return {"TRACE", "\033[90m"};  // bright black (gray)
    case LogLevel::kDebug:
      return {"DEBUG", "\033[36m"};  // cyan
    case LogLevel::kInfo:
      return {"INFO ", "\033[32m"};  // green
    case LogLevel::kWarn:
      return {"WARN ", "\033[33m"};  // yellow
    case LogLevel::kError:
      return {"ERROR", "\033[31m"};  // red
    case LogLevel::kOff:
      break;
  }
  return {"?????", ""};
}

bool ColorEnabled() {
  int cached = g_color_enabled.load(std::memory_order_relaxed);
  if (cached < 0) {
    cached = ::isatty(STDERR_FILENO) != 0 ? 1 : 0;
    g_color_enabled.store(cached, std::memory_order_relaxed);
  }
  return cached != 0;
}

/// Formats the current wall-clock time as `HH:MM:SS.mmm`.
///
/// Wall time rather than monotonic time: logs are read by humans and need to
/// align with other system logs.
void FormatTimestamp(std::array<char, 16>& out) {
  ::timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  ::tm local{};
  ::localtime_r(&ts.tv_sec, &local);
  const long millis = ts.tv_nsec / 1'000'000;
  std::snprintf(out.data(), out.size(), "%02d:%02d:%02d.%03ld", local.tm_hour, local.tm_min,
                local.tm_sec, millis);
}

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace

void SetLogLevel(LogLevel level) noexcept {
  g_runtime_level.store(level, std::memory_order_relaxed);
}

LogLevel GetLogLevel() noexcept {
  return g_runtime_level.load(std::memory_order_relaxed);
}

void SetLogColorEnabled(bool enabled) noexcept {
  g_color_enabled.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

void SetLogSink(LogSink sink, void* user) noexcept {
  const std::lock_guard<std::mutex> guard(OutputMutex());
  g_log_sink = sink;
  g_log_sink_user = user;
}

void SetCurrentThreadName(std::string_view name) noexcept {
  const std::size_t copy_len = std::min(name.size(), kThreadNameCapacity - 1);
  std::memcpy(t_thread_name.data(), name.data(), copy_len);
  t_thread_name[copy_len] = '\0';
  // Push the name to the kernel too, so that lldb / Instruments / `sample` also
  // see a meaningful thread name.
  ::pthread_setname_np(t_thread_name.data());
}

namespace detail {

bool IsLogLevelEnabled(LogLevel level) noexcept {
  return level >= g_runtime_level.load(std::memory_order_relaxed);
}

void EmitLogLine(LogLevel level, std::string_view file, unsigned line,
                 std::string_view message) noexcept {
  const LevelStyle style = StyleFor(level);
  std::array<char, 16> timestamp{};
  FormatTimestamp(timestamp);

  const bool color = ColorEnabled();
  const char* color_on = color ? style.color : "";
  const char* color_off = color ? "\033[0m" : "";
  const std::string_view thread_name = CurrentThreadName();

  // One fprintf for the whole line, plus the mutex, guarantees lines do not
  // interleave. Not std::format so that this cannot throw either -- the logging
  // path must be noexcept. file / thread_name / message are string_views and
  // may be unterminated, so `%.*s` always passes the length explicitly.
  const std::lock_guard<std::mutex> guard(OutputMutex());
  std::fprintf(stderr, "%s%s%s %s [%-10.*s] %.*s:%u  %.*s\n", color_on, style.label, color_off,
               timestamp.data(), static_cast<int>(thread_name.size()), thread_name.data(),
               static_cast<int>(file.size()), file.data(), line, static_cast<int>(message.size()),
               message.data());

  // Forward to the host. Calling inside the lock is deliberate: the host then
  // sees exactly the same line order as stderr. The cost is that the sink must
  // never log again (it would wait on itself, deadlocking); the constraint is
  // documented on logging.h.
  if (g_log_sink != nullptr) {
    g_log_sink(level, thread_name, message, g_log_sink_user);
  }
}

}  // namespace detail
}  // namespace tethernet
