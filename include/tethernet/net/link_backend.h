// Link-layer backend abstraction.
//
// Why the abstraction exists: BPF needs root and a real feth interface, which
// a developer machine cannot provide. Once "sending and receiving raw Ethernet
// frames" sits behind a narrow interface, end-to-end tests and throughput
// benchmarks can run anywhere on the in-memory loopback backend.
//
// Performance tradeoff: the interface uses **virtual functions**, but the
// virtual dispatch is **not per frame** -- ReadFrames / WriteFrames are batch
// interfaces; one virtual call handles dozens to hundreds of frames, and the
// per-frame amortized cost is far below 1 ns, entirely negligible.
// (A per-frame virtual call would have been unacceptable; that is the design
// deliberately avoided here.)
#pragma once

#include "tethernet/common/error.h"
#include "tethernet/common/frame_ring.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace tethernet::net {

/// A batch of frames to send.
using FrameBatch = std::span<const FrameView>;

/// The result of one batched read.
struct ReadBatch {
  /// Frames decoded in this batch; the views point into the backend's internal
  /// buffer and stay valid until the next ReadFrames call.
  std::span<const FrameView> frames;
  /// Cumulative kernel-side drop count (BPF's bs_drop). Used for backpressure
  /// alerts.
  std::uint64_t kernel_drops = 0;
};

/// The result of one batched write.
struct WriteResult {
  std::uint32_t frames_written = 0;
  std::uint64_t bytes_written = 0;
  /// Frames skipped, for example because a single frame exceeded the length
  /// cap.
  std::uint32_t frames_skipped = 0;
};

/// A backend that sends and receives raw Ethernet frames.
class LinkBackend {
 public:
  LinkBackend() = default;
  LinkBackend(const LinkBackend&) = delete;
  LinkBackend& operator=(const LinkBackend&) = delete;
  LinkBackend(LinkBackend&&) = delete;
  LinkBackend& operator=(LinkBackend&&) = delete;
  virtual ~LinkBackend() = default;

  /// Blocks until a batch of frames has been read.
  ///
  /// An empty batch is a legitimate return (for example when interrupted by a
  /// signal); the caller should keep looping. Only a genuine error returns an
  /// Error.
  [[nodiscard]] virtual Result<ReadBatch> ReadFrames() = 0;

  /// Writes a batch of frames. Best effort; over-length frames are skipped and
  /// counted.
  [[nodiscard]] virtual Result<WriteResult> WriteFrames(FrameBatch frames) = 0;

  /// Max single-frame length (14-byte Ethernet header included).
  [[nodiscard]] virtual std::uint32_t MaxFrameBytes() const noexcept = 0;

  /// Whether one syscall can write multiple frames. Used for logging and
  /// benchmark reports.
  [[nodiscard]] virtual bool SupportsBatchWrite() const noexcept = 0;

  /// Wakes a thread blocked inside ReadFrames, for graceful shutdown.
  ///
  /// Must be safe to call from **another thread**.
  virtual void Interrupt() noexcept = 0;
};

}  // namespace tethernet::net
