// Single-producer single-consumer (SPSC) lock-free bounded ring queue.
//
// Why write this instead of using std::queue + mutex:
//   The data path moves 25k-80k frames per second, one queue per direction. Lock/unlock
//   on a contended mutex goes through a kernel futex and can cost microseconds per
//   operation; enqueue/dequeue on an SPSC lock-free queue only needs one release store /
//   acquire load, about a few nanoseconds.
//
// This file provides two layers:
//   * SpscCursor -- only manages index advancement and memory ordering, agnostic to
//     storage layout.
//   * SpscRing<T> -- fixed-size element queue, suitable for small PODs (completion
//     events, indices, statistics snapshots).
// Variable-length Ethernet frames go through FrameRing (frame_ring.h), which reuses the
// same SpscCursor.
//
// Correctness is verified with ThreadSanitizer:
//   cmake -B build-tsan -DTETHERNET_ENABLE_TSAN=ON && ctest --test-dir build-tsan
#pragma once

#include "tethernet/common/cache.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace tethernet {

/// Index cursor for an SPSC queue: manages "where written / where read" and
/// cross-thread memory ordering.
///
/// Uses freely incrementing 64-bit counters instead of the classic "leave one slot
/// empty" scheme:
///   * Queue length is simply `write_pos - read_pos`, no wraparound comparison needed;
///   * Capacity can be fully utilized (the classic scheme wastes one slot);
///   * A 64-bit counter takes 5800 years to wrap at 100 Mpps, so it can be treated as
///     never wrapping.
///
/// The layout deliberately gives each of the four indices its own cache line:
///   * `write_pos_` is written by the producer and read by the consumer;
///   * `cached_read_pos_` is the producer's private copy of `read_pos_`, used to avoid
///     reading the consumer's cache line on every enqueue (this is the most important
///     optimization in this class -- it only synchronizes for real when it "looks full");
///   * `read_pos_` / `cached_write_pos_` are symmetric.
/// If `write_pos_` and `cached_read_pos_` shared a cache line, the consumer reading
/// `write_pos_` would downgrade that line to Shared, and the producer writing
/// `cached_read_pos_` would then have to take it back to Exclusive, wasting an extra
/// cache-coherence round trip.
class SpscCursor {
 public:
  /// `capacity` must be a power of two and greater than 0 (indexed by bitmask).
  explicit SpscCursor(std::size_t capacity) noexcept : capacity_(capacity), mask_(capacity - 1) {}

  [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

  // ---------------------------------------------------------------------------
  // Producer side (only the producer thread may call these)
  // ---------------------------------------------------------------------------

  /// Try to acquire the index of the slot at "current write position + offset". Returns
  /// false when the queue is full.
  ///
  /// `offset` is relative to the **not-yet-published** write position, used for batch
  /// writes: acquire slots with offset = 0,1,2,..., write to them, then call
  /// PublishWrite(n) once.
  ///
  /// This deliberately reads only its own write_pos_ (relaxed, this cache line is
  /// producer-exclusive), so acquiring a slot itself is nearly zero-cost -- the real cost
  /// is publishing; see PublishWrite.
  [[nodiscard]] bool TryAcquireWriteAt(std::uint64_t offset, std::size_t& index) noexcept {
    const std::uint64_t position = write_pos_.load(std::memory_order_relaxed) + offset;
    if (position - cached_read_pos_ >= capacity_) [[unlikely]] {
      // The cached read position is stale; synchronize the real value once and check
      // again. acquire guarantees: once we see the consumer's advanced read_pos_, the
      // consumer's reads of that slot are complete and it is safe to overwrite.
      cached_read_pos_ = read_pos_.load(std::memory_order_acquire);
      if (position - cached_read_pos_ >= capacity_) {
        return false;
      }
    }
    index = position & mask_;
    return true;
  }

  /// Try to acquire the next writable slot (equivalent to TryAcquireWriteAt(0, index)).
  [[nodiscard]] bool TryAcquireWrite(std::size_t& index) noexcept {
    return TryAcquireWriteAt(0, index);
  }

  /// Publish `count` newly written slots, making them visible to the consumer.
  ///
  /// release guarantees: after the consumer reads the new write_pos_ with acquire, it is
  /// guaranteed to see all of our writes to those slot contents.
  ///
  /// **Batch publishing is the most important optimization in this class.**
  /// This release store bounces the write_pos_ cache line across cores once (measured at
  /// about 60 ns). With per-item publishing that is the cost per item; publishing N at
  /// once amortizes it to 60/N:
  ///
  ///     batch=1  -> about 63 ns/item
  ///     batch=8  -> about 5.5 ns/item   (about 11x)
  ///     batch=32 -> about 1.9 ns/item   (about 33x)
  ///
  /// So data paths **must** publish in batches -- use FrameRing's BatchWriter /
  /// BatchReader, which separate "acquire slot + write" from "publish" and do only one
  /// atomic operation at the end of a batch.
  void PublishWrite(std::uint64_t count = 1) noexcept {
    write_pos_.store(write_pos_.load(std::memory_order_relaxed) + count,
                     std::memory_order_release);
  }

  /// Remaining writable slots from the producer's perspective (may be conservative
  /// because it uses the cached read position).
  [[nodiscard]] std::size_t WritableApprox() noexcept {
    const std::uint64_t write_pos = write_pos_.load(std::memory_order_relaxed);
    cached_read_pos_ = read_pos_.load(std::memory_order_acquire);
    return capacity_ - static_cast<std::size_t>(write_pos - cached_read_pos_);
  }

  // ---------------------------------------------------------------------------
  // Consumer side (only the consumer thread may call these)
  // ---------------------------------------------------------------------------

  /// Try to acquire the index of the slot at "current read position + offset". Returns
  /// false when the queue is empty.
  ///
  /// Symmetric with the write side: acquire in batches with offset = 0,1,2,..., then
  /// call PublishRead(n) once.
  [[nodiscard]] bool TryAcquireReadAt(std::uint64_t offset, std::size_t& index) noexcept {
    const std::uint64_t position = read_pos_.load(std::memory_order_relaxed) + offset;
    if (position >= cached_write_pos_) [[unlikely]] {
      cached_write_pos_ = write_pos_.load(std::memory_order_acquire);
      if (position >= cached_write_pos_) {
        return false;
      }
    }
    index = position & mask_;
    return true;
  }

  /// Try to acquire the next readable slot (equivalent to TryAcquireReadAt(0, index)).
  [[nodiscard]] bool TryAcquireRead(std::size_t& index) noexcept {
    return TryAcquireReadAt(0, index);
  }

  /// Release `count` slots that have just been read, making them reusable by the
  /// producer.
  ///
  /// Same as PublishWrite: batch releasing amortizes the cross-core cache-line bounce
  /// cost by a factor of N.
  void PublishRead(std::uint64_t count = 1) noexcept {
    read_pos_.store(read_pos_.load(std::memory_order_relaxed) + count,
                    std::memory_order_release);
  }

  /// Readable slots from the consumer's perspective. When dequeuing in batches, use this
  /// to get the batch size once and reduce synchronization.
  [[nodiscard]] std::size_t ReadableApprox() noexcept {
    const std::uint64_t read_pos = read_pos_.load(std::memory_order_relaxed);
    cached_write_pos_ = write_pos_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(cached_write_pos_ - read_pos);
  }

  // ---------------------------------------------------------------------------
  // Callable from any thread (statistics/observation only; results are point-in-time
  // snapshots)
  // ---------------------------------------------------------------------------

  /// Approximate number of elements currently in the queue.
  [[nodiscard]] std::size_t SizeSnapshot() const noexcept {
    const std::uint64_t write_pos = write_pos_.load(std::memory_order_acquire);
    const std::uint64_t read_pos = read_pos_.load(std::memory_order_acquire);
    return write_pos >= read_pos ? static_cast<std::size_t>(write_pos - read_pos) : 0;
  }

  /// Cumulative number of enqueued items (producer), for statistics.
  [[nodiscard]] std::uint64_t TotalEnqueued() const noexcept {
    return write_pos_.load(std::memory_order_relaxed);
  }

  /// Cumulative number of dequeued items (consumer), for statistics.
  [[nodiscard]] std::uint64_t TotalDequeued() const noexcept {
    return read_pos_.load(std::memory_order_relaxed);
  }

 private:
  // Read-only shared data gets its own cache line to avoid sharing with frequently
  // modified indices.
  TETHERNET_CACHE_ALIGNED const std::size_t capacity_;
  const std::size_t mask_;

  TETHERNET_CACHE_ALIGNED std::atomic<std::uint64_t> write_pos_{0};
  TETHERNET_CACHE_ALIGNED std::uint64_t cached_read_pos_{0};
  TETHERNET_CACHE_ALIGNED std::atomic<std::uint64_t> read_pos_{0};
  TETHERNET_CACHE_ALIGNED std::uint64_t cached_write_pos_{0};
};

/// SPSC lock-free bounded queue of fixed-size elements.
///
/// The element type must be trivially copyable (POD): types that may allocate memory or
/// throw are not accepted on data paths.
template <typename T>
  requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
class SpscRing {
 public:
  /// `capacity` is rounded up to a power of two.
  explicit SpscRing(std::size_t capacity)
      : cursor_(RoundUpCapacity(capacity)),
        slots_(std::make_unique<T[]>(RoundUpCapacity(capacity))) {}

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;
  ~SpscRing() = default;

  [[nodiscard]] std::size_t Capacity() const noexcept { return cursor_.Capacity(); }

  /// Enqueue. Returns false when the queue is full (the caller decides whether to drop
  /// or retry).
  [[nodiscard]] bool TryPush(const T& value) noexcept {
    std::size_t index = 0;
    if (!cursor_.TryAcquireWrite(index)) {
      return false;
    }
    slots_[index] = value;
    cursor_.PublishWrite();
    return true;
  }

  /// Dequeue. Returns false when the queue is empty.
  [[nodiscard]] bool TryPop(T& out) noexcept {
    std::size_t index = 0;
    if (!cursor_.TryAcquireRead(index)) {
      return false;
    }
    out = slots_[index];
    cursor_.PublishRead();
    return true;
  }

  [[nodiscard]] std::size_t SizeSnapshot() const noexcept { return cursor_.SizeSnapshot(); }

  [[nodiscard]] SpscCursor& Cursor() noexcept { return cursor_; }

 private:
  static std::size_t RoundUpCapacity(std::size_t requested) noexcept {
    std::size_t capacity = 1;
    while (capacity < requested) {
      capacity <<= 1U;
    }
    return capacity;
  }

  SpscCursor cursor_;
  std::unique_ptr<T[]> slots_;
};

}  // namespace tethernet
