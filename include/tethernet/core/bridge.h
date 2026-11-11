// Data path bridge: connects the RNDIS data channel to the feth/BPF link.
//
//  Threading model (the most important design decision in this file)
//
//  +---------------------------------------------------------------------+
//  | RX direction (device -> host)                                       |
//  |                                                                     |
//  |  libusb event thread                     RX injector thread         |
//  |  ---------------                        -----------                 |
//  |  handle_events()                        BatchRead takes a batch     |
//  |   +> bulk IN callback             +-->  +> LinkBackend::WriteFrames |
//  |       + decode RNDIS packet        |        (one syscall per batch) |
//  |       + BatchWrite into FrameRing -+                                |
//  |       + resubmit immediately                                        |
//  +---------------------------------------------------------------------+
//  +---------------------------------------------------------------------+
//  | TX direction (host -> device)                                       |
//  |                                                                     |
//  |  TX extractor thread                                                |
//  |  -----------                                                        |
//  |  LinkBackend::ReadFrames() (blocks, the kernel aggregates a batch)  |
//  |   +> DataChannel::SendFrames (aggregates into RNDIS multi-packets,  |
//  |      submits bulk OUT asynchronously)                               |
//  +---------------------------------------------------------------------+
//
//   Three data path threads in total (plus libusb's own IOKit runloop thread
//   and the state machine's control thread). This machine has 4 performance
//   cores and 6 efficiency cores, so the three hot threads plus libusb's
//   runloop thread just fill the performance core cluster without
//   oversubscribing it.
//
//   Why must RX be split into two threads?
//     libusb's transfer callback holds ctx->event_waiters_lock, so blocking
//     I/O inside the callback holds up every thread waiting for a synchronous
//     transfer (including the state machine thread that runs the control
//     channel). And the BPF write() is a system call (about 660 ns on macOS,
//     longer for a batch write). So the callback only does "decode the packet
//     + memcpy into the lock-free queue + resubmit immediately"; the actual
//     write-out is handed to the injector thread.
//
//   Why does TX need only one thread?
//     BPF's read() is itself blocking and self-aggregating (with
//     BIOCIMMEDIATE=1 it wakes on every arriving packet and delivers all the
//     packets accumulated in the meantime in one go), and SendFrames submits
//     **asynchronously**, so it does not block. Therefore "read a batch ->
//     submit a batch" in a single thread is the optimal shape; one more thread
//     would only add one more cross-core queue transfer.
//
//   Why not merge all three into a single kqueue event loop?
//     Measured on macOS, one empty kevent() (timeout={0,0}, no events) costs
//     13.4~13.7 us, 20 times more than an ordinary system call. At 25 kpps
//     the polling alone would eat 34% of a core. kqueue is only for real
//     blocking waits, never for polling -- and each of our three paths
//     already has a natural blocking point, so merging gains nothing.
#pragma once

#include "tethernet/common/error.h"
#include "tethernet/common/frame_ring.h"
#include "tethernet/common/stats.h"
#include "tethernet/net/link_backend.h"
#include "tethernet/usb/data_channel.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace tethernet::core {

/// Bridge layer configuration.
struct BridgeConfig {
  /// RX queue capacity (in frames).
  ///
  /// Sizing rationale: it has to absorb the mismatch between "a burst of
  /// libusb callbacks" and "the BPF writer being delayed by the scheduler".
  /// At a worst-case 10 ms scheduling delay and full 1 Gbps speed (82 k pps)
  /// that needs about 820 frames; 2048 leaves a 2.5x margin. Each slot is
  /// 2048+128 bytes, so 2048 slots are about 4.25 MiB.
  std::size_t rx_ring_frames = 2048;

  /// Max single-frame length (Ethernet header included). Should match the
  /// feth MTU + 14.
  std::uint32_t max_frame_bytes = 1518;

  /// How many frames the RX injector thread accumulates before writing them
  /// out.
  ///
  /// A batch write amortizes "the system call cost of one write()" over the
  /// frames; measured, the gain basically saturates at batch >= 8, but a
  /// larger batch further cuts the number of system calls during a burst. 64
  /// balances both: batches stay small when idle (low latency) and grow on
  /// their own during a burst (high throughput).
  std::uint32_t rx_write_batch = 64;

  /// How many times to spin before yielding the CPU when the RX queue is
  /// empty.
  ///
  /// Pure spinning burns a core for nothing; yielding outright adds latency
  /// at high throughput. A short spin covers very brief gaps such as "the
  /// producer is writing the next frame".
  std::uint32_t rx_spin_before_yield = 64;

  /// How many frames a single TX batch submits to the USB.
  std::uint32_t tx_submit_batch = 256;

  /// Statistics reporting period (milliseconds); 0 means do not report
  /// periodically.
  std::uint32_t stats_interval_millis = 0;
};

/// An observable snapshot of the bridge layer at runtime.
struct BridgeStats {
  DirectionSnapshot rx;
  DirectionSnapshot tx;
  /// Current RX queue depth, used to tell which side is the bottleneck.
  std::size_t rx_queue_depth = 0;
  /// Cumulative frame drops on the kernel BPF side (bs_drop).
  std::uint64_t link_kernel_drops = 0;
  /// How many times TX had to wait because the transfer pool had no free
  /// slot.
  ///
  /// This counts **events**, not frames: it goes up by 1 every time
  /// SendFrames manages to submit not a single frame and the TX thread
  /// switches to waiting (under sustained pressure at most one more every
  /// ~50 ms). It measures "how often the TX side hits the pool ceiling"; its
  /// presence does not mean frames were dropped -- during the wait the frames
  /// stay safely in the batch, upstream bursts are absorbed by the kernel BPF
  /// buffer, and a real overflow shows up in link_kernel_drops.
  std::uint64_t tx_backpressure_events = 0;
};

/// The data path bridge.
///
/// Lifecycle: Create -> Start -> (running) -> Stop. Stop is idempotent and
/// the destructor calls it as a backstop.
class Bridge {
 public:
  Bridge(usb::DataChannel& data_channel, net::LinkBackend& link, const BridgeConfig& config);

  Bridge(const Bridge&) = delete;
  Bridge& operator=(const Bridge&) = delete;
  Bridge(Bridge&&) = delete;
  Bridge& operator=(Bridge&&) = delete;

  ~Bridge();

  /// Starts the data path.
  [[nodiscard]] Status Start();

  /// Stops the data path.
  ///
  /// The teardown order is deliberate; see the comments in the implementation
  /// -- a wrong order leaks frames or deadlocks.
  void Stop();

  [[nodiscard]] bool Running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  [[nodiscard]] BridgeStats Snapshot() const;

  /// The RX direction counters. The data channel's receive callback writes
  /// them directly.
  [[nodiscard]] DirectionCounters& RxCounters() noexcept { return counters_.rx; }

  /// Pauses / resumes the data movement.
  ///
  /// Used during an RNDIS soft reset: the device drops every outstanding
  /// packet, so keeping frames coming at it during that window only wastes
  /// effort and may collide with the device rebuilding its internal state.
  ///
  /// `SetPaused(true)` is **synchronous**: on return the RX injector thread is
  /// guaranteed to be parked in the paused branch, no longer touching rx_ring_
  /// and no longer writing frames to the link.
  ///
  /// That synchronization is required; a flag alone gives no such guarantee:
  /// the worker may have just passed the flag check and be about to write out
  /// a batch it already took, while the caller believes it has stopped. That
  /// is exactly how one rx_write_batch (16 frames) once leaked out.
  ///
  /// The TX direction cannot get an equally strong guarantee -- it has to
  /// keep draining BPF, otherwise the kernel buffer fills up and turns into
  /// kernel-side frame drops. While paused it reads and discards instead, and
  /// it re-checks before every submission fragment, so after this function
  /// returns at most one fragment already in flight can still complete.
  ///
  ///  Must not be called from inside the RX/TX worker threads -- it would
  /// deadlock on itself. The existing callers all sit on the control path
  /// (link state changes, device soft reset), which satisfies this premise.
  void SetPaused(bool paused) noexcept;

  [[nodiscard]] bool Paused() const noexcept { return paused_.load(std::memory_order_acquire); }

 private:
  /// RX injector thread: takes frames from the FrameRing and writes them to
  /// the link in batches.
  void RunReceiveInjector() noexcept;

  /// TX extractor thread: reads frames from the link and submits them to USB
  /// in batches.
  void RunTransmitExtractor() noexcept;

  usb::DataChannel* data_channel_;
  net::LinkBackend* link_;
  BridgeConfig config_;

  /// The RX queue: libusb event thread (producer) -> RX injector thread
  /// (consumer).
  std::unique_ptr<FrameRing> rx_ring_;

  PathCounters counters_;
  std::atomic<std::uint64_t> tx_backpressure_events_{0};
  std::atomic<std::uint64_t> link_kernel_drops_{0};

  std::thread receive_injector_;
  std::thread transmit_extractor_;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> paused_{false};

  /// The RX injector thread's acknowledgement bit for paused_; SetPaused(true)
  /// waits for exactly this.
  ///
  /// Written by the RX injector thread **exclusively**: cleared at the start
  /// of every loop turn and set once the thread has parked. Clearing it every
  /// turn is the key -- otherwise a SetPaused(false) immediately followed by
  /// SetPaused(true) could read a leftover true from the previous turn and
  /// return at once, voiding the guarantee right there.
  std::atomic<bool> rx_paused_ack_{false};

  /// The frame view array reused by the RX injector thread, so no batch has
  /// to allocate.
  std::vector<FrameView> rx_batch_;
};

/// Renders a statistics snapshot as one human-readable line.
[[nodiscard]] std::string FormatStatsLine(const BridgeStats& previous, const BridgeStats& current,
                                          double seconds);

}  // namespace tethernet::core
