// Lightweight performance benchmark harness.
//
// Why not google/benchmark:
//   This project only cares about three kinds of metrics -- nanoseconds per operation, frames
//   per second (pps), and bits per second (Gbps). google/benchmark's automatic iteration-count
//   inference, statistical model, and JSON output are all unnecessary, and it would introduce
//   a networked FetchContent dependency and significantly slow the build. This harness is
//   only ~200 lines, yet it can directly output Markdown tables that can be pasted into
//   docs/BENCHMARKS.md, saving a parsing layer.
//
// Measurement method (per benchmark):
//   1. Warm up for several rounds to let the branch predictor, caches, and CPU frequency
//      reach steady state;
//   2. Run N measured rounds, each with a fixed number of inner-loop iterations;
//   3. Take the median across rounds rather than the mean -- background processes and
//      big.LITTLE scheduling on macOS cause occasional long tails, and the median is more
//      robust; also report the best value and relative spread to judge whether this
//      measurement is trustworthy.
#pragma once

#include <algorithm>
#include <ranges>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tetherkit/common/time.h"

namespace tetherkit::bench {

/// Prevents the compiler from optimizing away the code under measurement.
///
/// The inline assembly declares the value as "read and memory may be modified", so the
/// compiler cannot eliminate the computation that produced it. This is equivalent to
/// google/benchmark's DoNotOptimize.
template <typename T>
inline void DoNotOptimize(T const& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

/// Tells the compiler that all memory may be read or written, used to separate two sections
/// of code that must not be merged.
inline void ClobberMemory() {
  asm volatile("" : : : "memory");
}

/// Measurement result for a single benchmark.
struct Result {
  std::string name;
  std::string group;

  std::uint64_t operations = 0;    ///< Operations per round (frames / enqueues / ...).
  std::uint64_t bytes_per_op = 0;  ///< Bytes transferred per operation; 0 means not applicable.

  double median_nanos_per_op = 0.0;
  double best_nanos_per_op = 0.0;
  double worst_nanos_per_op = 0.0;

  /// Relative spread = (worst - best) / median. Above 0.3 means the measurement environment
  /// was noisy.
  [[nodiscard]] double Spread() const {
    return median_nanos_per_op > 0.0 ? (worst_nanos_per_op - best_nanos_per_op) / median_nanos_per_op
                                     : 0.0;
  }

  /// Operations per second (median).
  [[nodiscard]] double OpsPerSecond() const {
    return median_nanos_per_op > 0.0 ? 1e9 / median_nanos_per_op : 0.0;
  }

  /// Throughput (Gbps). Meaningful only when bytes_per_op > 0.
  [[nodiscard]] double Gigabitsps() const {
    return bytes_per_op == 0 ? 0.0
                             : OpsPerSecond() * static_cast<double>(bytes_per_op) * 8.0 / 1e9;
  }
};

/// Configuration for a single benchmark run.
struct Config {
  std::uint32_t warmup_rounds = 3;   ///< Warm-up rounds, results not counted.
  std::uint32_t measure_rounds = 9;  ///< Measured rounds, take the median (odd for easy midpoint).
  std::uint64_t ops_per_round = 0;   ///< Operations per round; decided by each benchmark.
  std::uint64_t bytes_per_op = 0;    ///< Bytes per operation, used to compute Gbps.
};

/// Benchmark registry and executor.
class Runner {
 public:
  /// Signature of the function under measurement: given the number of operations to run in
  /// this round, returns the number actually executed.
  ///
  /// Returning a value instead of always using the argument lets benchmarks like "stop when
  /// the queue is full" report honestly.
  using Body = std::function<std::uint64_t(std::uint64_t)>;

  /// Registers a benchmark. `group` is used to group entries in the summary table.
  void Add(std::string group, std::string name, Config config, Body body) {
    entries_.push_back(Entry{std::move(group), std::move(name), config, std::move(body)});
  }

  /// Runs all benchmarks in order, printing progress as it goes.
  [[nodiscard]] std::vector<Result> RunAll() {
    std::vector<Result> results;
    results.reserve(entries_.size());
    for (const Entry& entry : entries_) {
      std::fprintf(stderr, "  measuring %s / %s ...\n", entry.group.c_str(), entry.name.c_str());
      results.push_back(RunOne(entry));
    }
    return results;
  }

 private:
  struct Entry {
    std::string group;
    std::string name;
    Config config;
    Body body;
  };

  static Result RunOne(const Entry& entry) {
    const Config& config = entry.config;

    // Warm-up: not recorded, only to let caches and frequency reach steady state.
    for (std::uint32_t i = 0; i < config.warmup_rounds; ++i) {
      DoNotOptimize(entry.body(config.ops_per_round));
    }

    std::vector<double> nanos_per_op;
    nanos_per_op.reserve(config.measure_rounds);
    for (std::uint32_t i = 0; i < config.measure_rounds; ++i) {
      ClobberMemory();
      const Nanos start = MonotonicNanos();
      const std::uint64_t executed = entry.body(config.ops_per_round);
      const Nanos elapsed = MonotonicNanos() - start;
      ClobberMemory();

      if (executed == 0) {
        continue;
      }
      nanos_per_op.push_back(static_cast<double>(elapsed) / static_cast<double>(executed));
    }

    Result result;
    result.group = entry.group;
    result.name = entry.name;
    result.operations = config.ops_per_round;
    result.bytes_per_op = config.bytes_per_op;
    if (nanos_per_op.empty()) {
      return result;
    }

    std::ranges::sort(nanos_per_op);
    result.best_nanos_per_op = nanos_per_op.front();
    result.worst_nanos_per_op = nanos_per_op.back();
    result.median_nanos_per_op = nanos_per_op[nanos_per_op.size() / 2];
    return result;
  }

  std::vector<Entry> entries_;
};

/// Renders the result set as a Markdown table that can be pasted directly into docs/BENCHMARKS.md.
void PrintMarkdownReport(const std::vector<Result>& results, std::string_view title);

/// Prints local environment information (CPU, core counts, cache line, build configuration);
/// benchmark results are only meaningful with context.
void PrintEnvironment();

}  // namespace tetherkit::bench
