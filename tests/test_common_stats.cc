// Unit tests for stats.h and time.h.
#include <chrono>
#include <thread>

#include <doctest.h>

#include "tetherkit/common/stats.h"
#include "tetherkit/common/time.h"

using tetherkit::DirectionCounters;
using tetherkit::kNanosPerMilli;
using tetherkit::MonotonicNanos;
using tetherkit::PathCounters;
using tetherkit::PeriodicTimer;
using tetherkit::RateSampler;
using tetherkit::Snapshot;
using tetherkit::Stopwatch;

TEST_SUITE("common.stats") {
  TEST_CASE("counter accumulation and snapshot") {
    DirectionCounters counters;
    counters.AddFrame(1514);
    counters.AddFrame(64);
    counters.AddDroppedFull(3);
    counters.AddDroppedOversize();
    counters.AddDroppedMalformed(2);
    counters.AddIoError();

    const auto snapshot = Snapshot(counters);
    CHECK(snapshot.frames == 2);
    CHECK(snapshot.bytes == 1578);
    CHECK(snapshot.dropped_full == 3);
    CHECK(snapshot.dropped_oversize == 1);
    CHECK(snapshot.dropped_malformed == 2);
    CHECK(snapshot.io_errors == 1);
    CHECK(snapshot.TotalDropped() == 6);
  }

  TEST_CASE("batch count is used to compute average batch size") {
    DirectionCounters counters;
    counters.AddBatch(10, 15140);
    counters.AddBatch(6, 9084);

    const auto snapshot = Snapshot(counters);
    CHECK(snapshot.frames == 16);
    CHECK(snapshot.bytes == 24224);
    CHECK(snapshot.batches == 2);
  }

  TEST_CASE("subtracting snapshots yields the window delta") {
    DirectionCounters counters;
    counters.AddFrame(100);
    const auto first = Snapshot(counters);
    counters.AddFrame(200);
    counters.AddDroppedFull();
    const auto second = Snapshot(counters);

    const auto delta = second - first;
    CHECK(delta.frames == 1);
    CHECK(delta.bytes == 200);
    CHECK(delta.dropped_full == 1);
  }

  TEST_CASE("rate sampling converts byte counts to Mbps") {
    PathCounters counters;
    RateSampler sampler;

    // Create 12.5 MB of traffic over 1 second = 100 Mbps.
    counters.rx.AddBatch(8000, 12'500'000);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto report = sampler.Sample(counters);

    CHECK(report.seconds > 0.0);
    // Only about 20 ms elapsed, so the rate is amplified ~50x; only verify the conversion is
    // self-consistent here.
    const double expected_mbps = 12'500'000.0 * 8.0 / 1'000'000.0 / report.seconds;
    CHECK(report.rx_mbps == doctest::Approx(expected_mbps).epsilon(0.01));
    CHECK(report.rx_avg_batch == doctest::Approx(8000.0));
    CHECK(report.tx_pps == doctest::Approx(0.0));
  }

  TEST_CASE("stopwatch readings are monotonically non-decreasing") {
    const Stopwatch watch;
    const auto first = watch.Elapsed();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto second = watch.Elapsed();
    CHECK(second >= first);
    CHECK(watch.ElapsedMillis() >= 1.0);
  }

  TEST_CASE("PeriodicTimer default construction still means timing starts at construction") {
    PeriodicTimer timer(50 * kNanosPerMilli);
    // It should not be expired immediately after construction.
    CHECK_FALSE(timer.Expired());
    CHECK(timer.RemainingNanos(MonotonicNanos()) > 0);
    CHECK(timer.Period() == 50 * kNanosPerMilli);
  }

  TEST_CASE(
      "PeriodicTimer advances by accumulated deadlines without drifting on scheduling delay") {
    const tetherkit::Nanos now = MonotonicNanos();
    // `now` must be passed explicitly as the timing origin. If the constructor reads the clock
    // itself, the time it reads will be slightly later than `now` here, so "now + period" would
    // not yet be due and the assertion would fail randomly.
    PeriodicTimer timer(10 * kNanosPerMilli, now);

    CHECK_FALSE(timer.Expired(now));
    CHECK(timer.RemainingNanos(now) > 0);

    // Exactly due.
    CHECK(timer.Expired(now + 10 * kNanosPerMilli));
    // Just fired, so asking immediately should report not expired.
    CHECK_FALSE(timer.Expired(now + 10 * kNanosPerMilli));
    // Due again after one more period.
    CHECK(timer.Expired(now + 20 * kNanosPerMilli));
  }

  TEST_CASE("PeriodicTimer does not emit a burst of triggers when more than one period behind") {
    const tetherkit::Nanos now = MonotonicNanos();
    PeriodicTimer timer(10 * kNanosPerMilli, now);

    // The thread wakes up 1 second later after being suspended: it should fire only once, then
    // align to after the current time.
    const tetherkit::Nanos late = now + 1000 * kNanosPerMilli;
    CHECK(timer.Expired(late));
    CHECK_FALSE(timer.Expired(late));
    CHECK_FALSE(timer.Expired(late + 9 * kNanosPerMilli));
    CHECK(timer.Expired(late + 10 * kNanosPerMilli));
  }

}  // TEST_SUITE("common.stats")
