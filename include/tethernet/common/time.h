// Monotonic clock and timing utilities.
//
// Why not use std::chrono::steady_clock directly:
//   libc++'s steady_clock on Darwin goes through clock_gettime(CLOCK_MONOTONIC),
//   while clock_gettime_nsec_np(CLOCK_UPTIME_RAW) is cheaper (reads mach absolute time
//   directly, with no timespec struct round-trip and no NTP adjustment). Benchmarks and
//   keep-alive timers call this tens of thousands of times per second, so the difference
//   matters.
#pragma once

#include <cstdint>
#include <ctime>

namespace tethernet {

/// Nanosecond timestamp type alias, making interface signatures self-explanatory.
using Nanos = std::uint64_t;

inline constexpr Nanos kNanosPerMicro = 1'000;
inline constexpr Nanos kNanosPerMilli = 1'000'000;
inline constexpr Nanos kNanosPerSecond = 1'000'000'000;

/// Monotonic nanoseconds since system boot. Not affected by system time adjustments and
/// does not stop during sleep.
///
/// CLOCK_UPTIME_RAW is the cheapest time source on Darwin: no NTP correction, no struct
/// conversion.
[[nodiscard]] inline Nanos MonotonicNanos() noexcept {
  return ::clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/// Stopwatch: starts timing on construction.
class Stopwatch {
 public:
  Stopwatch() noexcept : start_(MonotonicNanos()) {}

  /// Restart timing.
  void Reset() noexcept { start_ = MonotonicNanos(); }

  [[nodiscard]] Nanos Elapsed() const noexcept { return MonotonicNanos() - start_; }

  [[nodiscard]] double ElapsedMillis() const noexcept {
    return static_cast<double>(Elapsed()) / static_cast<double>(kNanosPerMilli);
  }

  [[nodiscard]] double ElapsedSeconds() const noexcept {
    return static_cast<double>(Elapsed()) / static_cast<double>(kNanosPerSecond);
  }

 private:
  Nanos start_;
};

/// Monotonically increasing periodic trigger, used for "do keep-alive / statistics
/// reporting every N milliseconds".
///
/// Deliberately avoids a timer thread or kqueue timer: the caller is already waking up
/// periodically in an event loop and only needs a cheap "is it time yet?" check.
class PeriodicTimer {
 public:
  /// @param period Trigger period.
  /// @param start  Time base. **Passed in explicitly instead of reading the clock
  ///               internally**, so that:
  ///               1. Unit tests can inject a deterministic time base (otherwise the
  ///                  constructor reads a time slightly later than the caller's `now`,
  ///                  so "now + period" is not yet expired and test results are not
  ///                  reproducible);
  ///               2. Callers that create multiple timers in one loop can share the
  ///                  same time base.
  ///               The default preserves the convenience of "start timing on
  ///               construction".
  explicit PeriodicTimer(Nanos period, Nanos start = MonotonicNanos()) noexcept
      : period_(period), next_deadline_(start + period) {}

  /// Return true if expired and advance the next deadline by one period.
  ///
  /// Uses "accumulate the deadline" rather than "reset to now + period" to avoid overall
  /// timer drift when the caller is delayed by scheduling. If it has fallen behind by
  /// more than one full period, align to just after the current time to avoid firing a
  /// burst of overdue triggers on wakeup.
  [[nodiscard]] bool Expired(Nanos now) noexcept {
    if (now < next_deadline_) {
      return false;
    }
    next_deadline_ += period_;
    if (next_deadline_ <= now) {
      next_deadline_ = now + period_;
    }
    return true;
  }

  [[nodiscard]] bool Expired() noexcept { return Expired(MonotonicNanos()); }

  /// Nanoseconds remaining until the next expiry; returns 0 if already expired. For event
  /// loops to compute select/kevent timeouts.
  [[nodiscard]] Nanos RemainingNanos(Nanos now) const noexcept {
    return now >= next_deadline_ ? 0 : next_deadline_ - now;
  }

  [[nodiscard]] Nanos Period() const noexcept { return period_; }

 private:
  Nanos period_;
  Nanos next_deadline_;
};

}  // namespace tethernet
