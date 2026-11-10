// In-memory loopback link backend -- for offline tests and throughput
// benchmarks.
//
// Why it must exist: BpfLink needs root privileges plus a real feth interface,
// and a developer machine has neither root nor a USB device. Once the link
// layer sits behind the LinkBackend abstraction, the end-to-end logic (the
// bridge's threading model, batching, backpressure, statistics) can be tested
// and benchmarked in full on any machine.
//
// Semantics:
//   * Frames written by WriteFrames land in the `sent` queue, so tests can
//     assert "what the driver sent to the host side";
//   * ReadFrames takes frames from the `inbound` queue, so tests can inject
//     "what the host side sent";
//   * Both queues are bounded; a full queue drops the frame and counts it --
//     the same behavior as real BPF.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "tetherkit/common/frame_ring.h"
#include "tetherkit/net/link_backend.h"

namespace tetherkit::net {

/// Configuration of the loopback backend.
struct LoopbackConfig {
  std::uint32_t max_frame_bytes = 1518;
  std::size_t inbound_capacity = 4096;  ///< Cap on frames waiting to be taken by ReadFrames.
  std::size_t sent_capacity = 4096;     ///< Cap on frames already written by WriteFrames.
  std::size_t max_frames_per_batch = 256;
  /// Whether to advertise batch write support. Tests cover both paths.
  bool report_batch_write = true;
};

/// A purely in-memory link backend.
///
/// Thread safety: ReadFrames may be called from one thread only, WriteFrames
/// may be called from one thread only (matching the LinkBackend contract);
/// PushInbound / DrainSent / Interrupt may be called from any thread and are
/// protected internally by a mutex.
///
/// A mutex is used deliberately instead of a lock-free queue: this class only
/// serves the **injection side** of tests and benchmarks and is not on the
/// data path under test; making it lock-free would add implementation
/// complexity without testing anything more.
class LoopbackLink final : public LinkBackend {
 public:
  explicit LoopbackLink(const LoopbackConfig& config = {});

  // Copy and move are already deleted in the base class LinkBackend; they are
  // restated here to satisfy the static checks.
  LoopbackLink(const LoopbackLink&) = delete;
  LoopbackLink& operator=(const LoopbackLink&) = delete;
  LoopbackLink(LoopbackLink&&) = delete;
  LoopbackLink& operator=(LoopbackLink&&) = delete;
  ~LoopbackLink() override = default;

  [[nodiscard]] Result<ReadBatch> ReadFrames() override;
  [[nodiscard]] Result<WriteResult> WriteFrames(FrameBatch frames) override;

  [[nodiscard]] std::uint32_t MaxFrameBytes() const noexcept override {
    return config_.max_frame_bytes;
  }

  [[nodiscard]] bool SupportsBatchWrite() const noexcept override {
    return config_.report_batch_write;
  }

  void Interrupt() noexcept override;

  // ---------------------------------------------------------------------------
  // Test injection and observation interface
  // ---------------------------------------------------------------------------

  /// Injects one frame "from the host side" for ReadFrames to take. Returns
  /// false when the queue is full.
  [[nodiscard]] bool PushInbound(std::span<const std::byte> frame);

  /// Takes and clears every frame "the driver wrote to the host side".
  [[nodiscard]] std::vector<std::vector<std::byte>> DrainSent();

  /// Frames and bytes written (cumulative, unaffected by DrainSent).
  [[nodiscard]] std::uint64_t TotalSentFrames() const noexcept {
    return total_sent_frames_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t TotalSentBytes() const noexcept {
    return total_sent_bytes_.load(std::memory_order_relaxed);
  }

  /// Number of injected frames dropped because the queue was full.
  [[nodiscard]] std::uint64_t InboundDrops() const noexcept {
    return inbound_drops_.load(std::memory_order_relaxed);
  }

  /// Whether Interrupt has been called.
  [[nodiscard]] bool Interrupted() const noexcept {
    return interrupted_.load(std::memory_order_acquire);
  }

  /// Makes WriteFrames return an error from the Nth call on, to exercise the
  /// error propagation path.
  void FailWritesAfter(std::uint32_t successful_calls) noexcept {
    fail_writes_after_.store(successful_calls, std::memory_order_relaxed);
  }

 private:
  LoopbackConfig config_;

  mutable std::mutex mutex_;
  /// Frames waiting to be read (FIFO).
  std::vector<std::vector<std::byte>> inbound_;
  /// Frames already written.
  std::vector<std::vector<std::byte>> sent_;

  /// The views returned by ReadFrames must stay valid until the next call, so
  /// this batch's data is kept here.
  std::vector<std::vector<std::byte>> read_storage_;
  std::vector<FrameView> read_views_;

  std::atomic<bool> interrupted_{false};
  std::atomic<std::uint64_t> total_sent_frames_{0};
  std::atomic<std::uint64_t> total_sent_bytes_{0};
  std::atomic<std::uint64_t> inbound_drops_{0};
  std::atomic<std::uint32_t> write_calls_{0};
  std::atomic<std::uint32_t> fail_writes_after_{0};
};

}  // namespace tetherkit::net
