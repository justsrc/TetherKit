// SPSC lock-free ring queue for variable-length Ethernet frames.
//
// Difference from SpscRing<T>: Ethernet frames are variable-length (14-2048
// bytes), and we want **exactly one memory copy on the whole data path**. So
// instead of "assemble elsewhere, then enqueue", this offers a
// "reserve slot -> write in place -> commit" interface:
//
//   Producer (libusb callback thread)         Consumer (BPF write thread)
//   ------------------------------------      -----------------------------------
//   std::byte* dst = ring.BeginWrite();       auto view = ring.BeginRead();
//   memcpy(dst, usb_payload, len);            ::write(bpf_fd, view.data(), ...);
//   ring.CommitWrite(len);                    ring.CommitRead();
//   (the memcpy is the single copy)
//
// Why this one copy is mandatory (instead of zero-copy):
//   libusb's bulk IN transfer buffers must be resubmitted quickly to keep the
//   USB pipe saturated; handing buffers to the downstream would require far
//   more buffers than "in-flight transfers" plus a return mechanism. A measured
//   1500-byte memcpy on Apple Silicon costs about 40-60 ns, while a single BPF
//   write() syscall costs 30-50x that -- the copy is not the bottleneck, and
//   zero-copy past it is the wrong optimization.
//
// Storage layout: capacity fixed-size slots, each slot = a cache-line-aligned
// header (length) + frame data area.
// Why fixed slots instead of a compact arena: an arena must splice frames
// across the ring tail on wraparound, which turns "one write() per frame" into
// two and is slower.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

#include "tetherkit/common/cache.h"
#include "tetherkit/common/spsc_ring.h"

namespace tetherkit {

/// Maximum bytes of an Ethernet frame (excluding FCS).
///
/// Why 2048: feth's `net.link.fake.max_mtu` is 2048, so the MTU ceiling is
/// 2048, add the 14-byte Ethernet header and it should be 2062; but an RNDIS
/// device may negotiate a larger `OID_GEN_MAXIMUM_FRAME_SIZE`, and power-of-two
/// slot sizes favor address math, so 2048 is taken directly as the frame data
/// capacity; oversize frames are dropped and counted before enqueue.
inline constexpr std::uint32_t kMaxEthernetFrameBytes = 2048;

/// Minimum legal Ethernet frame (destination MAC 6 + source MAC 6 + EtherType 2).
inline constexpr std::uint32_t kMinEthernetFrameBytes = 14;

/// Read-only view of a frame pointing into the ring's internal storage, valid
/// until CommitRead.
struct FrameView {
  const std::byte* data = nullptr;
  std::uint32_t length = 0;

  [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return {data, length}; }

  [[nodiscard]] bool Empty() const noexcept { return length == 0; }
};

/// SPSC lock-free bounded queue of variable-length frames.
class FrameRing {
 public:
  /// `capacity_frames` is rounded up to a power of two.
  /// `max_frame_bytes` is the per-frame limit, defaulting to kMaxEthernetFrameBytes.
  explicit FrameRing(std::size_t capacity_frames,
                     std::uint32_t max_frame_bytes = kMaxEthernetFrameBytes)
      : cursor_(RoundUpToPowerOfTwo(capacity_frames)),
        max_frame_bytes_(max_frame_bytes),
        slot_stride_(ComputeSlotStride(max_frame_bytes)),
        storage_(AllocateStorage(RoundUpToPowerOfTwo(capacity_frames),
                                 ComputeSlotStride(max_frame_bytes))) {
    // Cache the storage start aligned to a cache line to avoid recomputing alignment on
    // every slot access. Use std::align rather than integer modulo: the latter requires
    // uintptr_t <-> pointer conversions which weaken the compiler's alias analysis
    // (clang-tidy performance-no-int-to-ptr flags this).
    void* base = storage_.get();
    std::size_t space = cursor_.Capacity() * slot_stride_ + kCacheLineSize;
    void* aligned = std::align(kCacheLineSize, cursor_.Capacity() * slot_stride_, base, space);
    assert(aligned != nullptr);
    aligned_base_ = static_cast<std::byte*>(aligned);
  }

  FrameRing(const FrameRing&) = delete;
  FrameRing& operator=(const FrameRing&) = delete;
  FrameRing(FrameRing&&) = delete;
  FrameRing& operator=(FrameRing&&) = delete;
  ~FrameRing() = default;

  [[nodiscard]] std::size_t Capacity() const noexcept { return cursor_.Capacity(); }

  [[nodiscard]] std::uint32_t MaxFrameBytes() const noexcept { return max_frame_bytes_; }

  /// Total bytes occupied by the queue, used to print the memory budget at startup.
  [[nodiscard]] std::size_t StorageBytes() const noexcept {
    return cursor_.Capacity() * slot_stride_;
  }

  // ---------------------------------------------------------------------------
  // Producer side
  // ---------------------------------------------------------------------------

  /// Reserve a slot and return a buffer writable for `MaxFrameBytes()` bytes; returns an
  /// empty span when the queue is full.
  ///
  /// Must be paired with CommitWrite; BeginWrite must not be called again between the
  /// two.
  [[nodiscard]] std::span<std::byte> BeginWrite() noexcept {
    if (!cursor_.TryAcquireWrite(pending_write_index_)) [[unlikely]] {
      return {};
    }
    return {FrameDataAt(pending_write_index_), max_frame_bytes_};
  }

  /// Commit the frame that was just written. `length` must be <= MaxFrameBytes().
  void CommitWrite(std::uint32_t length) noexcept {
    assert(length <= max_frame_bytes_);
    StoreLength(pending_write_index_, length);
    cursor_.PublishWrite();
  }

  /// Convenience entry point: copy a whole frame into the queue. Returns true on success,
  /// or false when the queue is full or the frame exceeds the limit.
  [[nodiscard]] bool TryPush(std::span<const std::byte> frame) noexcept {
    if (frame.size() > max_frame_bytes_) [[unlikely]] {
      return false;
    }
    const std::span<std::byte> dst = BeginWrite();
    if (dst.empty()) [[unlikely]] {
      return false;
    }
    std::memcpy(dst.data(), frame.data(), frame.size());
    CommitWrite(static_cast<std::uint32_t>(frame.size()));
    return true;
  }

  /// Free slots from the producer's perspective. Used before unpacking to check whether
  /// the whole batch fits and avoid dropping half a batch.
  [[nodiscard]] std::size_t WritableApprox() noexcept { return cursor_.WritableApprox(); }

  /// Batch write session. **Data paths should always use this instead of per-frame
  /// TryPush.**
  ///
  /// One release store bounces the write-position cache line across cores once (measured
  /// at about 60 ns). With per-frame publishing that is the fixed cost per frame; batch
  /// publishing amortizes it to 60/N:
  ///     batch=1 about 63 ns/frame, batch=8 about 5.5 ns/frame, batch=32 about 1.9
  ///     ns/frame.
  ///
  /// Usage (unpacking multiple RNDIS packets from one transfer in a libusb callback):
  /// ```
  /// auto batch = ring.BeginBatchWrite();
  /// while (reader.Next(frame) == ReadOutcome::kFrame) {
  ///   const std::span<std::byte> dst = batch.Begin();
  ///   if (dst.empty()) { break; }          // queue full
  ///   std::memcpy(dst.data(), frame.data(), frame.size());
  ///   batch.Commit(frame.size());
  /// }
  /// // batch destructor publishes all frames at once
  /// ```
  class BatchWrite {
   public:
    explicit BatchWrite(FrameRing& ring) noexcept : ring_(&ring) {}

    BatchWrite(const BatchWrite&) = delete;
    BatchWrite& operator=(const BatchWrite&) = delete;
    BatchWrite(BatchWrite&&) = delete;
    BatchWrite& operator=(BatchWrite&&) = delete;

    /// Auto-publish on destruction. Forgetting an explicit Publish does not lose data.
    ~BatchWrite() { Publish(); }

    /// Reserve the next slot and return a writable buffer; returns an empty span when the
    /// queue is full.
    [[nodiscard]] std::span<std::byte> Begin() noexcept {
      if (!ring_->cursor_.TryAcquireWriteAt(staged_, staged_index_)) [[unlikely]] {
        return {};
      }
      return {ring_->FrameDataAt(staged_index_), ring_->max_frame_bytes_};
    }

    /// Record the length of the frame that was just written. **No atomic operations** --
    /// this is the key to batching.
    void Commit(std::uint32_t length) noexcept {
      assert(length <= ring_->max_frame_bytes_);
      ring_->StoreLength(staged_index_, length);
      ++staged_;
    }

    /// Convenience entry point: copy a whole frame. Returns true on success.
    [[nodiscard]] bool Push(std::span<const std::byte> frame) noexcept {
      if (frame.size() > ring_->max_frame_bytes_) [[unlikely]] {
        return false;
      }
      const std::span<std::byte> dst = Begin();
      if (dst.empty()) [[unlikely]] {
        return false;
      }
      std::memcpy(dst.data(), frame.data(), frame.size());
      Commit(static_cast<std::uint32_t>(frame.size()));
      return true;
    }

    /// Publish all staged frames immediately. May be called early; this session can
    /// continue staging new frames afterward.
    void Publish() noexcept {
      if (staged_ != 0) {
        ring_->cursor_.PublishWrite(staged_);
        published_ += staged_;
        staged_ = 0;
      }
    }

    /// Number of frames staged but not yet published in this session.
    [[nodiscard]] std::uint32_t Staged() const noexcept { return staged_; }

    /// Cumulative number of frames published in this session.
    [[nodiscard]] std::uint32_t Published() const noexcept { return published_; }

   private:
    FrameRing* ring_;
    std::uint32_t staged_ = 0;
    std::uint32_t published_ = 0;
    std::size_t staged_index_ = 0;
  };

  [[nodiscard]] BatchWrite BeginBatchWrite() noexcept { return BatchWrite{*this}; }

  // ---------------------------------------------------------------------------
  // Consumer side
  // ---------------------------------------------------------------------------

  /// Take a read-only view of the head frame; returns a view with length == 0 when the
  /// queue is empty.
  ///
  /// The view is valid until the next CommitRead. Must be paired with CommitRead.
  [[nodiscard]] FrameView BeginRead() noexcept {
    if (!cursor_.TryAcquireRead(pending_read_index_)) [[unlikely]] {
      return FrameView{.data = nullptr, .length = 0};
    }
    return FrameView{.data = FrameDataAt(pending_read_index_),
                     .length = LoadLength(pending_read_index_)};
  }

  /// Release the head frame.
  void CommitRead() noexcept { cursor_.PublishRead(); }

  /// Readable frames from the consumer's perspective. When consuming in batches, use this
  /// to get the batch size once.
  [[nodiscard]] std::size_t ReadableApprox() noexcept { return cursor_.ReadableApprox(); }

  /// Batch read session. Symmetric with BatchWrite; does only one release store at the
  /// end of the batch.
  ///
  /// This is exactly the shape the BPF write thread needs: take out dozens of frames at
  /// once, collect them into one BIOCSBATCHWRITE batch, then release all slots at once.
  ///
  /// Usage:
  /// ```
  /// auto batch = ring.BeginBatchRead();
  /// std::vector<FrameView> views;
  /// while (views.size() < kMaxBatch) {
  ///   const FrameView view = batch.Next();
  ///   if (view.Empty()) { break; }
  ///   views.push_back(view);              // view valid until batch is destroyed
  /// }
  /// link.WriteFrames(views);
  /// // batch destructor releases all slots at once
  /// ```
  class BatchRead {
   public:
    explicit BatchRead(FrameRing& ring) noexcept : ring_(&ring) {}

    BatchRead(const BatchRead&) = delete;
    BatchRead& operator=(const BatchRead&) = delete;
    BatchRead(BatchRead&&) = delete;
    BatchRead& operator=(BatchRead&&) = delete;

    /// Auto-release taken slots on destruction.
    ~BatchRead() { Release(); }

    /// Take a read-only view of the next frame; returns a view with length == 0 when
    /// there are no more frames.
    ///
    /// The returned view is valid **until** this session's Release() (or destruction) --
    /// because the slot has not been returned to the producer, its contents cannot be
    /// overwritten. This is the precondition for zero-copy batch writes.
    [[nodiscard]] FrameView Next() noexcept {
      std::size_t index = 0;
      if (!ring_->cursor_.TryAcquireReadAt(staged_, index)) [[unlikely]] {
        return FrameView{};
      }
      ++staged_;
      return FrameView{.data = ring_->FrameDataAt(index),
                       .length = ring_->LoadLength(index)};
    }

    /// Release all taken slots immediately. All previously returned views become invalid
    /// after this call.
    void Release() noexcept {
      if (staged_ != 0) {
        ring_->cursor_.PublishRead(staged_);
        released_ += staged_;
        staged_ = 0;
      }
    }

    /// Number of frames taken but not yet released.
    [[nodiscard]] std::uint32_t Staged() const noexcept { return staged_; }

    /// Cumulative number of frames released in this session.
    [[nodiscard]] std::uint32_t Released() const noexcept { return released_; }

   private:
    FrameRing* ring_;
    std::uint32_t staged_ = 0;
    std::uint32_t released_ = 0;
  };

  [[nodiscard]] BatchRead BeginBatchRead() noexcept { return BatchRead{*this}; }

  // ---------------------------------------------------------------------------
  // Observation
  // ---------------------------------------------------------------------------

  [[nodiscard]] std::size_t SizeSnapshot() const noexcept { return cursor_.SizeSnapshot(); }

  [[nodiscard]] std::uint64_t TotalEnqueued() const noexcept { return cursor_.TotalEnqueued(); }

  [[nodiscard]] std::uint64_t TotalDequeued() const noexcept { return cursor_.TotalDequeued(); }

 private:
  // The two batch session classes need direct access to the cursor and slot addressing to
  // avoid an extra forwarding layer on hot paths.
  friend class BatchWrite;
  friend class BatchRead;

  /// Slot layout: [0, 4) stores the length, [kSlotHeaderBytes, ...) stores frame data.
  ///
  /// Frame data starts at offset kCacheLineSize so that every frame's start address is
  /// cache-line aligned -- memcpy and subsequent write() can take the optimal path.
  static constexpr std::size_t kSlotHeaderBytes = kCacheLineSize;

  static std::size_t RoundUpToPowerOfTwo(std::size_t requested) noexcept {
    std::size_t value = 1;
    while (value < requested) {
      value <<= 1U;
    }
    return value;
  }

  /// Slot stride: header + frame data area, rounded up to a cache line so each slot
  /// occupies a whole number of lines exclusively.
  static std::size_t ComputeSlotStride(std::uint32_t max_frame_bytes) noexcept {
    const std::size_t raw = kSlotHeaderBytes + max_frame_bytes;
    return ((raw + kCacheLineSize - 1) / kCacheLineSize) * kCacheLineSize;
  }

  static std::unique_ptr<std::byte[]> AllocateStorage(std::size_t capacity, std::size_t stride) {
    // Use new[] rather than aligned_alloc: alignment of the block's start address is
    // handled by AlignedBase below, so destruction stays a plain unique_ptr without a
    // custom deleter.
    return std::make_unique<std::byte[]>(capacity * stride + kCacheLineSize);
  }

  [[nodiscard]] std::byte* SlotAt(std::size_t index) const noexcept {
    return aligned_base_ + index * slot_stride_;
  }

  [[nodiscard]] std::byte* FrameDataAt(std::size_t index) const noexcept {
    return SlotAt(index) + kSlotHeaderBytes;
  }

  void StoreLength(std::size_t index, std::uint32_t length) noexcept {
    std::memcpy(SlotAt(index), &length, sizeof(length));
  }

  [[nodiscard]] std::uint32_t LoadLength(std::size_t index) const noexcept {
    std::uint32_t length = 0;
    std::memcpy(&length, SlotAt(index), sizeof(length));
    return length;
  }

  SpscCursor cursor_;

  // Read-only after construction; both threads only read them, so they can safely share
  // the same cache line.
  std::uint32_t max_frame_bytes_;
  std::size_t slot_stride_;
  std::unique_ptr<std::byte[]> storage_;
  std::byte* aligned_base_ = nullptr;

  // "Reserved but not yet committed" slot indices: private to the producer and consumer
  // respectively and **must** live on different cache lines -- otherwise every frame's
  // BeginWrite/BeginRead would invalidate each other, and all the cache-line isolation
  // done for SpscCursor would be wasted.
  TETHERKIT_CACHE_ALIGNED std::size_t pending_write_index_ = 0;
  TETHERKIT_CACHE_ALIGNED std::size_t pending_read_index_ = 0;
};

}  // namespace tetherkit
