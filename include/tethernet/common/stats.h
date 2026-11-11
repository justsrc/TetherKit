// Data-path statistics counters.
//
// Design notes:
//   * Counters on hot paths **must not** be shared std::atomic -- two threads doing
//     fetch_add on the same atomic at 25k-80k pps turn that cache line into a ping-pong
//     ball.
//   * So the structure is "one private counter set per direction + snapshot on read": the
//     writer is single-threaded and updates with relaxed stores (a plain str instruction
//     after compilation); the observer reads snapshots with relaxed loads, tolerating a
//     slightly inconsistent set of values -- statistics do not need strong consistency.
//   * std::atomic<uint64_t> + relaxed is used instead of a plain uint64_t so that
//     ThreadSanitizer does not flag "observer reads, worker writes" as a data race.
//     Relaxed atomics on arm64 are ordinary ldr/str instructions with no extra cost.
#pragma once

#include "tethernet/common/cache.h"
#include "tethernet/common/time.h"

#include <atomic>
#include <cstdint>

namespace tethernet {

/// Data-path counters for one direction (RX or TX).
///
/// Only **one** thread may call the Add* methods; any thread may call Snapshot().
struct alignas(kCacheLineSize) DirectionCounters {
  std::atomic<std::uint64_t> frames{0};             ///< Number of frames moved successfully.
  std::atomic<std::uint64_t> bytes{0};              ///< Bytes moved successfully (Ethernet
                                                    ///< frame payload).
  std::atomic<std::uint64_t> dropped_full{0};       ///< Frames dropped because the downstream
                                                    ///< queue was full.
  std::atomic<std::uint64_t> dropped_oversize{0};   ///< Frames dropped for exceeding the
                                                    ///< per-frame limit.
  std::atomic<std::uint64_t> dropped_malformed{0};  ///< Frames dropped for being
                                                    ///< malformed.
  std::atomic<std::uint64_t> io_errors{0};          ///< Underlying I/O failure count (write /
                                                    ///< transfer errors).
  std::atomic<std::uint64_t> batches{0};            ///< Batch count, used to compute average
                                                    ///< batch size.

  /// Record one successfully moved frame. This is the hottest line of code and is
  /// deliberately kept to just two relaxed increments.
  void AddFrame(std::uint32_t frame_bytes) noexcept {
    Bump(frames, 1);
    Bump(bytes, frame_bytes);
  }

  void AddBatch(std::uint64_t frame_count, std::uint64_t byte_count) noexcept {
    Bump(frames, frame_count);
    Bump(bytes, byte_count);
    Bump(batches, 1);
  }

  void AddDroppedFull(std::uint64_t count = 1) noexcept { Bump(dropped_full, count); }

  void AddDroppedOversize(std::uint64_t count = 1) noexcept { Bump(dropped_oversize, count); }

  void AddDroppedMalformed(std::uint64_t count = 1) noexcept { Bump(dropped_malformed, count); }

  void AddIoError(std::uint64_t count = 1) noexcept { Bump(io_errors, count); }

 private:
  /// Relaxed read-modify-write. Because there is only one writer thread, fetch_add
  /// atomicity is not needed here; load+store is enough and avoids the LSE atomic
  /// instruction (ldadd) on arm64.
  static void Bump(std::atomic<std::uint64_t>& counter, std::uint64_t delta) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + delta, std::memory_order_relaxed);
  }
};

/// Plain (non-atomic) snapshot of DirectionCounters, convenient for deltas and formatting.
struct DirectionSnapshot {
  std::uint64_t frames = 0;
  std::uint64_t bytes = 0;
  std::uint64_t dropped_full = 0;
  std::uint64_t dropped_oversize = 0;
  std::uint64_t dropped_malformed = 0;
  std::uint64_t io_errors = 0;
  std::uint64_t batches = 0;

  [[nodiscard]] std::uint64_t TotalDropped() const noexcept {
    return dropped_full + dropped_oversize + dropped_malformed;
  }

  /// Field-wise subtraction to get the delta between two samples.
  [[nodiscard]] DirectionSnapshot operator-(const DirectionSnapshot& earlier) const noexcept {
    return DirectionSnapshot{
        .frames = frames - earlier.frames,
        .bytes = bytes - earlier.bytes,
        .dropped_full = dropped_full - earlier.dropped_full,
        .dropped_oversize = dropped_oversize - earlier.dropped_oversize,
        .dropped_malformed = dropped_malformed - earlier.dropped_malformed,
        .io_errors = io_errors - earlier.io_errors,
        .batches = batches - earlier.batches,
    };
  }
};

[[nodiscard]] inline DirectionSnapshot Snapshot(const DirectionCounters& counters) noexcept {
  return DirectionSnapshot{
      .frames = counters.frames.load(std::memory_order_relaxed),
      .bytes = counters.bytes.load(std::memory_order_relaxed),
      .dropped_full = counters.dropped_full.load(std::memory_order_relaxed),
      .dropped_oversize = counters.dropped_oversize.load(std::memory_order_relaxed),
      .dropped_malformed = counters.dropped_malformed.load(std::memory_order_relaxed),
      .io_errors = counters.io_errors.load(std::memory_order_relaxed),
      .batches = counters.batches.load(std::memory_order_relaxed),
  };
}

/// All counters for the bidirectional data path.
///
/// RX = device -> host (USB bulk IN -> BPF write)
/// TX = host -> device (BPF read -> USB bulk OUT)
struct PathCounters {
  DirectionCounters rx;
  DirectionCounters tx;
};

/// Rates over a time window, used for periodic reporting.
struct RateReport {
  double seconds = 0.0;
  double rx_pps = 0.0;
  double rx_mbps = 0.0;
  double tx_pps = 0.0;
  double tx_mbps = 0.0;
  std::uint64_t rx_dropped = 0;
  std::uint64_t tx_dropped = 0;
  double rx_avg_batch = 0.0;
  double tx_avg_batch = 0.0;
};

/// Convert counter deltas into rates over a fixed period.
class RateSampler {
 public:
  RateSampler() : last_nanos_(MonotonicNanos()) {}

  /// Sample once and return the rates since the previous sample.
  [[nodiscard]] RateReport Sample(const PathCounters& counters) noexcept {
    const Nanos now = MonotonicNanos();
    const DirectionSnapshot rx_now = Snapshot(counters.rx);
    const DirectionSnapshot tx_now = Snapshot(counters.tx);

    const double seconds =
        static_cast<double>(now - last_nanos_) / static_cast<double>(kNanosPerSecond);
    const DirectionSnapshot rx_delta = rx_now - last_rx_;
    const DirectionSnapshot tx_delta = tx_now - last_tx_;

    last_nanos_ = now;
    last_rx_ = rx_now;
    last_tx_ = tx_now;

    if (seconds <= 0.0) {
      return RateReport{};
    }
    return RateReport{
        .seconds = seconds,
        .rx_pps = static_cast<double>(rx_delta.frames) / seconds,
        .rx_mbps = BytesToMegabitsPerSecond(rx_delta.bytes, seconds),
        .tx_pps = static_cast<double>(tx_delta.frames) / seconds,
        .tx_mbps = BytesToMegabitsPerSecond(tx_delta.bytes, seconds),
        .rx_dropped = rx_delta.TotalDropped(),
        .tx_dropped = tx_delta.TotalDropped(),
        .rx_avg_batch = AverageBatch(rx_delta),
        .tx_avg_batch = AverageBatch(tx_delta),
    };
  }

 private:
  static double BytesToMegabitsPerSecond(std::uint64_t bytes, double seconds) noexcept {
    return static_cast<double>(bytes) * 8.0 / 1'000'000.0 / seconds;
  }

  static double AverageBatch(const DirectionSnapshot& delta) noexcept {
    return delta.batches == 0 ? 0.0
                              : static_cast<double>(delta.frames) / static_cast<double>(delta.batches);
  }

  Nanos last_nanos_;
  DirectionSnapshot last_rx_;
  DirectionSnapshot last_tx_;
};

}  // namespace tethernet
