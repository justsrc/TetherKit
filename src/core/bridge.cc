#include "tetherkit/core/bridge.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>

#include "tetherkit/common/i18n.h"
#include "tetherkit/common/logging.h"
#include "tetherkit/common/scheduling.h"

namespace tetherkit::core {
namespace {

/// Upper bound on how long one TX-backpressure wait may block for a free
/// transfer slot (milliseconds).
///
/// The value only trades off two things: the worst-case shutdown/pause
/// response latency (every wakeup rechecks the flags) and how often we wake
/// needlessly while waiting. Slots actually free up every few hundred
/// microseconds (the completion time of one bulk OUT), and most waits are cut
/// short by the completion callback's notify, so the bound is rarely reached.
constexpr std::uint32_t kTxCapacityWaitMillis = 50;

/// Convert a byte count into Mbps.
[[nodiscard]] double ToMegabitsPerSecond(std::uint64_t bytes, double seconds) {
  if (seconds <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(bytes) * 8.0 / 1'000'000.0 / seconds;
}

[[nodiscard]] double ToPacketsPerSecond(std::uint64_t frames, double seconds) {
  if (seconds <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(frames) / seconds;
}

}  // namespace

Bridge::Bridge(usb::DataChannel& data_channel, net::LinkBackend& link,
               const BridgeConfig& config)
    : data_channel_(&data_channel), link_(&link), config_(config) {
  rx_ring_ = std::make_unique<FrameRing>(config_.rx_ring_frames, config_.max_frame_bytes);
  rx_batch_.reserve(config_.rx_write_batch);
}

Bridge::~Bridge() {
  Stop();
}

Status Bridge::Start() {
  if (running_.load(std::memory_order_acquire)) {
    return std::unexpected(Error::Generic(Tr(Msg::kCoreBridgeAlreadyRunning)));
  }
  stop_requested_.store(false, std::memory_order_release);

  // Start the data channel's receive side first -- it pushes frames into
  // rx_ring_. The order relative to starting the injector thread does not
  // matter (the queue is bounded, and it drops and counts when full), but
  // opening receive first lets us collect frames the moment the link is ready,
  // dropping a few fewer.
  TETHERKIT_RETURN_IF_ERROR(data_channel_->StartReceiving(*rx_ring_, counters_.rx));

  running_.store(true, std::memory_order_release);
  receive_injector_ = std::thread([this] { RunReceiveInjector(); });
  transmit_extractor_ = std::thread([this] { RunTransmitExtractor(); });

  TETHERKIT_INFO_TR(Msg::kCoreDataPathStarted, rx_ring_->Capacity(),
                    rx_ring_->StorageBytes() / 1024, config_.rx_write_batch,
                    config_.tx_submit_batch,
                    Text(link_->SupportsBatchWrite() ? Msg::kCoreBatchWriteAvailable
                                                     : Msg::kCoreBatchWriteUnavailable));
  return Ok();
}

void Bridge::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;  // not running, or already stopped
  }

  // ---------------------------------------------------------------------------
  // Teardown order (get it wrong and you leak frames or deadlock)
  //
  //  1. set the stop flag -- both threads' loop conditions test it;
  //  2. interrupt the link's blocking read() -- otherwise the TX extractor
  //     thread stays stuck inside it;
  //  3. join the two data-path threads;
  //  4. stop the data channel **last**.
  //
  // Step 4 must be last for two reasons:
  //   * DataChannel::Shutdown() waits for all in-flight USB transfer
  //     callbacks to return, and those callbacks write into rx_ring_ -- if the
  //     channel is stopped before the joins that is still fine; but the other
  //     way around, destroying rx_ring_ first is a use-after-free. This order
  //     keeps rx_ring_ alive throughout.
  //   * Shutdown() must never be called on the libusb event thread (it waits
  //     on itself and deadlocks), and here we are on whichever thread called
  //     Stop() (main or control), which is safe.
  // ---------------------------------------------------------------------------
  stop_requested_.store(true, std::memory_order_release);

  link_->Interrupt();

  if (receive_injector_.joinable()) {
    receive_injector_.join();
  }
  if (transmit_extractor_.joinable()) {
    transmit_extractor_.join();
  }

  data_channel_->Shutdown();

  TETHERKIT_INFO_TR(Msg::kCoreDataPathStopped);
}

// =============================================================================
// RX injector thread: FrameRing -> link
// =============================================================================

void Bridge::SetPaused(bool paused) noexcept {
  paused_.store(paused, std::memory_order_release);
  if (!paused) {
    return;
  }

  // Wait for the RX injector thread to confirm it has actually parked. Without
  // this, once this function returns the worker could still write a batch it
  // already dequeued to the link: it may have just cleared the flag check
  // above.
  //
  // Only wait while the threads are actually running -- nobody sets the flag
  // before Start() or after Stop(), so an unconditional wait would never
  // return.
  while (running_.load(std::memory_order_acquire) &&
         !stop_requested_.load(std::memory_order_acquire) &&
         !rx_paused_ack_.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

void Bridge::RunReceiveInjector() noexcept {
  ConfigureCurrentThread("rx-inject", ThreadRole::kDataPath);
  TETHERKIT_DEBUG_TR(Msg::kCoreRxInjectorStarted);

  std::uint32_t idle_spins = 0;

  while (!stop_requested_.load(std::memory_order_acquire)) {
    // Clear the ack flag before deciding whether to pause. The order cannot
    // be reversed -- leaving last round's true in place would make
    // SetPaused(true) believe this thread has already parked while it is
    // actually about to fetch frames.
    rx_paused_ack_.store(false, std::memory_order_release);

    if (paused_.load(std::memory_order_acquire)) {
      // During a reset: do not move frames, but do not drop them either --
      // frames in the queue resume once we unpause.
      //
      // Setting the ack bit is for SetPaused(true) to see: from here until the
      // next round observes paused_ back at false, this thread will not touch
      // rx_ring_, so the ack is trustworthy.
      rx_paused_ack_.store(true, std::memory_order_release);
      std::this_thread::yield();
      continue;
    }

    // Accumulate a batch before writing. The batch session releases its slots
    // only when it goes out of scope, so every view dequeued meanwhile stays
    // valid -- that is exactly the precondition for "hand zero-copy to a single
    // batched write()".
    rx_batch_.clear();
    {
      auto batch = rx_ring_->BeginBatchRead();
      while (rx_batch_.size() < config_.rx_write_batch) {
        const FrameView view = batch.Next();
        if (view.Empty()) {
          break;
        }
        rx_batch_.push_back(view);
      }

      if (rx_batch_.empty()) {
        // Queue empty. Spin briefly first to cover the tiny window where the
        // producer is writing the next frame, then yield the CPU -- pure
        // spinning burns a core for nothing, while yielding outright raises
        // latency under high throughput.
        if (++idle_spins < config_.rx_spin_before_yield) {
          continue;
        }
        idle_spins = 0;
        std::this_thread::yield();
        continue;
      }
      idle_spins = 0;

      const auto result = link_->WriteFrames(rx_batch_);
      if (!result) {
        counters_.rx.AddIoError();
        TETHERKIT_WARN_TR(Msg::kCoreLinkWriteFailed, rx_batch_.size(),
                          result.error().ToString());
        // A write failure does not exit the thread -- the interface may just
        // be temporarily down. The batch session still releases its slots
        // normally (the frames cannot be delivered anymore; keeping them would
        // only jam the queue).
      } else {
        if (result->frames_skipped != 0) {
          counters_.rx.AddDroppedOversize(result->frames_skipped);
        }
        const auto written = result->frames_written;
        if (written < rx_batch_.size()) {
          // The link did not swallow the whole batch (kernel send queue full).
          // The remaining frames are already out of the queue, so they can only
          // be dropped and counted -- this is real packet loss and must stay
          // visible to operators.
          counters_.rx.AddDroppedFull(rx_batch_.size() - written);
        }
        // Note: the kernel-side bs_drop is a **read**-direction statistic,
        // updated by the TX extractor thread from ReadFrames results; we do
        // not touch it here.
      }
    }  // batch session destructs -> a single PublishRead(n)
  }

  TETHERKIT_DEBUG_TR(Msg::kCoreRxInjectorExited);
}

// =============================================================================
// TX extractor thread: link -> USB
// =============================================================================

void Bridge::RunTransmitExtractor() noexcept {
  ConfigureCurrentThread("tx-extract", ThreadRole::kDataPath);
  TETHERKIT_DEBUG_TR(Msg::kCoreTxExtractorStarted);

  while (!stop_requested_.load(std::memory_order_acquire)) {
    // Blocking read. With BIOCIMMEDIATE=1 the BPF hands over whatever
    // accumulated in one go, so this is naturally "low rate, low latency;
    // high rate, large batches" -- we do not need to batch ourselves.
    const auto batch = link_->ReadFrames();
    if (!batch) {
      counters_.tx.AddIoError();
      TETHERKIT_WARN_TR(Msg::kCoreLinkReadFailed, batch.error().ToString());
      // A read failure can mean the interface was torn down. Wait a bit and
      // retry to avoid a busy loop spamming the log.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    link_kernel_drops_.store(batch->kernel_drops, std::memory_order_relaxed);

    if (batch->frames.empty()) {
      continue;  // read timeout expired with no packets in between; normal
    }
    if (paused_.load(std::memory_order_acquire)) {
      // During a reset the device discards all outstanding packets, so
      // sending to it is pure waste.
      counters_.tx.AddDroppedFull(batch->frames.size());
      continue;
    }

    // Submit to USB in chunks.
    std::size_t offset = 0;
    while (offset < batch->frames.size()) {
      // Recheck the stop and pause flags before every chunk: the backpressure
      // wait below can keep this loop parked here for a long time, so we
      // cannot rely only on the outer check. Dropping the remaining frames
      // while paused is deliberate (during an RNDIS reset the device discards
      // all outstanding packets); TX cannot get RX's strong guarantee (it must
      // keep draining BPF and cannot really stop), but narrowing the unsent
      // window to "one chunk" is free.
      if (stop_requested_.load(std::memory_order_acquire) ||
          paused_.load(std::memory_order_acquire)) {
        counters_.tx.AddDroppedFull(batch->frames.size() - offset);
        break;
      }

      const std::size_t chunk =
          std::min<std::size_t>(config_.tx_submit_batch, batch->frames.size() - offset);
      const std::span<const FrameView> slice = batch->frames.subspan(offset, chunk);

      const auto sent = data_channel_->SendFrames(slice);
      if (!sent) {
        counters_.tx.AddIoError();
        // Abandoned remaining frames must count as dropped -- this used to
        // break with only +1 io_error, leaving the rest out of every counter,
        // vanishing from the statistics entirely.
        counters_.tx.AddDroppedFull(batch->frames.size() - offset);
        TETHERKIT_WARN_TR(Msg::kCoreUsbSubmitFailed, chunk, sent.error().ToString());
        break;
      }

      // The bridge is the **only writer** of the TX counters
      // (DirectionCounters uses relaxed load+store and is safe only with a
      // single writer). The data channel side only counts errors from async
      // completion callbacks, merged in by Snapshot() when read. Frames and
      // bytes sent come straight from SendOutcome; frames skipped for illegal
      // length count as oversize drops rather than "sent".
      if (sent->sent_frames != 0) {
        counters_.tx.AddBatch(sent->sent_frames, sent->sent_bytes);
      }
      if (sent->skipped != 0) {
        counters_.tx.AddDroppedOversize(sent->skipped);
      }

      if (sent->consumed == 0) {
        // No free slot in the transfer pool -- this is backpressure. **Wait,
        // do not drop.**
        //
        // The old implementation dropped the remaining frames here
        // immediately, reasoning that "waiting would pile kernel-buffer
        // backlog into invisible kernel drops". That premise was wrong: the
        // kernel buffer is 4 MiB by default (about 130 ms of elasticity at
        // 250 Mbps), and its overflow counter bs_drop comes back with every
        // ReadFrames call and is displayed in the statistics line's "kernel
        // drops" column -- nothing invisible about it.
        // Measured consequence on real hardware: a full-frame chunk needs
        // about 26 transfer slots while the pool only has 4; slots free up
        // within a few hundred microseconds, yet the old code would not wait a
        // single microsecond and dropped 30%+ of TX traffic here under load
        // (docs/BENCHMARKS.md, "Real-device end-to-end measurements").
        //
        // The wait uses a bounded timeout: every wakeup returns to the top of
        // the loop and rechecks the stop/pause flags, so shutdown latency
        // stays bounded. The real overflow backstop is still the kernel
        // buffer -- only when it is full should anything drop, and that drop
        // is visible in the statistics.
        tx_backpressure_events_.fetch_add(1, std::memory_order_relaxed);
        (void)data_channel_->WaitForSendCapacity(kTxCapacityWaitMillis);
        continue;
      }
      offset += sent->consumed;
    }
  }

  TETHERKIT_DEBUG_TR(Msg::kCoreTxExtractorExited);
}

// =============================================================================
// Observability
// =============================================================================

BridgeStats Bridge::Snapshot() const {
  DirectionSnapshot tx = tetherkit::Snapshot(counters_.tx);
  // Merge the errors the data channel accumulated in its async completion
  // callbacks -- those are incremented by the libusb event thread and are
  // deliberately kept apart from the bridge's counters to preserve the
  // "exactly one writer per counter" invariant.
  tx.io_errors += data_channel_->AsyncSendErrors();

  return BridgeStats{
      .rx = tetherkit::Snapshot(counters_.rx),
      .tx = tx,
      .rx_queue_depth = rx_ring_->SizeSnapshot(),
      .link_kernel_drops = link_kernel_drops_.load(std::memory_order_relaxed),
      .tx_backpressure_events = tx_backpressure_events_.load(std::memory_order_relaxed),
  };
}

std::string FormatStatsLine(const BridgeStats& previous, const BridgeStats& current,
                            double seconds) {
  const DirectionSnapshot rx_delta = current.rx - previous.rx;
  const DirectionSnapshot tx_delta = current.tx - previous.tx;

  return Tr(
      Msg::kCoreStatsLine,
      ToPacketsPerSecond(rx_delta.frames, seconds),
      ToMegabitsPerSecond(rx_delta.bytes, seconds), rx_delta.TotalDropped(),
      ToPacketsPerSecond(tx_delta.frames, seconds),
      ToMegabitsPerSecond(tx_delta.bytes, seconds), tx_delta.TotalDropped(),
      current.rx_queue_depth, current.link_kernel_drops - previous.link_kernel_drops,
      current.tx_backpressure_events - previous.tx_backpressure_events);
}

}  // namespace tetherkit::core
