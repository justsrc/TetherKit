// Unit tests for SpscRing / SpscCursor.
//
// Multi-threaded cases only have full meaning under a ThreadSanitizer build:
//   cmake -B build-tsan -DTETHERNET_ENABLE_TSAN=ON && ctest --test-dir build-tsan -R spsc
#include "tethernet/common/cache.h"
#include "tethernet/common/spsc_ring.h"

#include <doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>

using tethernet::kCacheLineSize;
using tethernet::SpscRing;

namespace {

/// Used to verify that "element contents are transferred completely" rather than just indices.
struct Payload {
  std::uint64_t sequence;
  std::uint32_t checksum;
};

constexpr std::uint32_t Checksum(std::uint64_t sequence) {
  return static_cast<std::uint32_t>((sequence * 2654435761ULL) >> 16U);
}

}  // namespace

TEST_SUITE("common.spsc_ring") {
  TEST_CASE("capacity rounds up to a power of two") {
    CHECK(SpscRing<std::uint32_t>(1).Capacity() == 1);
    CHECK(SpscRing<std::uint32_t>(3).Capacity() == 4);
    CHECK(SpscRing<std::uint32_t>(4).Capacity() == 4);
    CHECK(SpscRing<std::uint32_t>(1000).Capacity() == 1024);
  }

  TEST_CASE("dequeue from an empty queue fails") {
    SpscRing<std::uint32_t> ring(4);
    std::uint32_t value = 0xFFFFFFFFU;
    CHECK_FALSE(ring.TryPop(value));
    CHECK(value == 0xFFFFFFFFU);  // the output parameter must not be modified on failure
    CHECK(ring.SizeSnapshot() == 0);
  }

  TEST_CASE("enqueue into a full queue fails and capacity is fully utilized") {
    // One design goal of the freely incrementing counter: do not waste slots. A capacity of 4
    // should hold 4 elements.
    SpscRing<std::uint32_t> ring(4);
    for (std::uint32_t i = 0; i < 4; ++i) {
      CHECK(ring.TryPush(i));
    }
    CHECK(ring.SizeSnapshot() == 4);
    CHECK_FALSE(ring.TryPush(99));
  }

  TEST_CASE("FIFO order and content integrity") {
    SpscRing<Payload> ring(8);
    for (std::uint64_t i = 0; i < 8; ++i) {
      CHECK(ring.TryPush(Payload{.sequence = i, .checksum = Checksum(i)}));
    }
    for (std::uint64_t i = 0; i < 8; ++i) {
      Payload out{};
      REQUIRE(ring.TryPop(out));
      CHECK(out.sequence == i);
      CHECK(out.checksum == Checksum(i));
    }
    CHECK(ring.SizeSnapshot() == 0);
  }

  TEST_CASE("repeated wrap-around does not lose data") {
    // Capacity 4, push 1000 elements, pop each one immediately, forcing the indices to wrap
    // around repeatedly.
    SpscRing<std::uint64_t> ring(4);
    for (std::uint64_t i = 0; i < 1000; ++i) {
      REQUIRE(ring.TryPush(i));
      std::uint64_t out = 0;
      REQUIRE(ring.TryPop(out));
      CHECK(out == i);
    }
  }

  TEST_CASE("index cursor fields land on different cache lines") {
    // This is a performance-correctness assertion: if false sharing regresses, this case fails.
    SpscRing<std::uint32_t> ring(8);
    const auto& cursor = ring.Cursor();
    const auto base = reinterpret_cast<std::uintptr_t>(&cursor);
    // The four indices in SpscCursor are each alignas(kCacheLineSize), so the whole object
    // must occupy at least 4 cache lines (plus the one for read-only fields).
    CHECK(sizeof(tethernet::SpscCursor) >= 4 * kCacheLineSize);
    CHECK(base % kCacheLineSize == 0);
  }

  TEST_CASE(
      "single-producer single-consumer concurrent transfer without loss, duplication, or "
      "reordering") {
    constexpr std::uint64_t kTotal = 200'000;
    SpscRing<Payload> ring(1024);

    std::atomic<std::uint64_t> produced{0};
    std::atomic<bool> corrupted{false};

    std::thread producer([&] {
      std::uint64_t sequence = 0;
      while (sequence < kTotal) {
        if (ring.TryPush(Payload{.sequence = sequence, .checksum = Checksum(sequence)})) {
          ++sequence;
          produced.store(sequence, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });

    std::uint64_t expected = 0;
    while (expected < kTotal) {
      Payload out{};
      if (ring.TryPop(out)) {
        // Order must be strictly increasing (FIFO) and content must be consistent with the
        // sequence number (no tearing).
        if (out.sequence != expected || out.checksum != Checksum(out.sequence)) {
          corrupted.store(true, std::memory_order_relaxed);
          break;
        }
        ++expected;
      } else {
        std::this_thread::yield();
      }
    }

    producer.join();
    CHECK_FALSE(corrupted.load());
    CHECK(expected == kTotal);
    CHECK(ring.SizeSnapshot() == 0);
  }

}  // TEST_SUITE("common.spsc_ring")
