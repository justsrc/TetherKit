// RNDIS data channel: a pool of asynchronous bulk IN / OUT transfers.
//
//  The teardown algorithm that avoids use-after-free (the most important
//   part of this file)
//
//   libusb_close does **not** reap in-flight transfers for you. do_close()
//   only does list_del + nulling transfer->dev_handle for the transfers still
//   in flight, and then logs "Device handle closed while transfer was still
//   being processed" -- it **does not invoke your callback and does not free
//   the memory**. The later close of the darwin backend calls
//   USBInterfaceClose, which triggers the IOKit abort, and
//   darwin_async_io_callback still runs on an internal libusb thread and calls
//   usbi_signal_transfer_completion(). If you have already called
//   libusb_free_transfer by then, that is a use-after-free.
//
//   The correct order (this is what Shutdown() implements):
//     1. Set the shutdown flag -- the callback sees it and stops resubmitting,
//        so the in-flight count converges on its own;
//     2. Call libusb_cancel_transfer on the in-flight transfers;
//     3. **Wait for the in-flight count to reach zero** (every callback has
//        come back);
//     4. Only then libusb_free_transfer and release the buffers;
//     5. Only after that may the caller destroy the Device (release_interface
//        -> close).
//
//    Step 3 **must not be waited for on the libusb event thread** -- the
//      callback runs on exactly that thread, so waiting there is waiting for
//      yourself, a guaranteed deadlock. Shutdown() can therefore only be
//      called from the control thread or the main thread; the code reminds
//      of that with both a comment and an assertion.
//
//   On the granularity of darwin's cancel: libusb_cancel_transfer on darwin
//   is darwin_abort_transfers -> **AbortPipe(pipeRef)**, that is, cancelling
//   one transfer cancels **all** in-flight transfers **on that endpoint**
//   (they all come back as kIOReturnAborted -> LIBUSB_TRANSFER_CANCELLED).
//   So really one call per endpoint would suffice, but calling them one by
//   one is idempotent and clearer; this micro-optimization is not made.
//
//  Why "fewer but larger" beats "many but small"
//
//   The darwin backend calls darwin_get_pipe_properties before **every**
//   submit_bulk_transfer, which on IOUSBInterfaceInterface >= 550 amounts to
//   two IOKit user-client calls (GetPipePropertiesV3 + GetEndpointPropertiesV3).
//   In other words, every libusb_submit_transfer on macOS costs at least 3
//   IOKit round trips, not 1.
//   Conversely, the darwin backend has **no length limit and does no
//   fragmentation** for a single bulk transfer (only the Linux backend has the
//   MAX_BULK_BUFFER_LENGTH = 16384 URB splitting).
//   -> Conclusion: use fewer, larger transfers.
#pragma once

#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "tetherkit/common/error.h"
#include "tetherkit/common/frame_ring.h"
#include "tetherkit/common/spsc_ring.h"
#include "tetherkit/common/stats.h"
#include "tetherkit/rndis/messages.h"
#include "tetherkit/usb/device.h"

namespace tetherkit::usb {

/// Data channel configuration.
struct DataChannelConfig {
  /// How many bulk IN transfers are in flight concurrently.
  ///
  /// 8 x 16 KiB = 128 KiB in flight. The value from the research is that "a
  /// total in flight of >= 256 KiB" is needed to keep the controller queue
  /// from running dry at USB 2.0 high speed (a 53.2 MB/s ceiling) -- at
  /// 50 MB/s a 1 ms gap wastes 50 KB. So the default is 16 x 16 KiB =
  /// 256 KiB.
  std::uint32_t rx_transfer_count = 16;

  /// The byte size of each bulk IN buffer.
  ///
  /// It must be >= the MaxTransferSize we advertise in INITIALIZE_MSG,
  /// otherwise the large transfers the device aggregates overflow or get
  /// truncated. The runtime overrides this value with the negotiated result.
  std::uint32_t rx_transfer_bytes = 16 * 1024;

  /// How many bulk OUT transfers are in flight concurrently.
  ///
  /// The TX side does not need as many as RX: the frames are accumulated and
  /// sent in batches by us, and 4 are enough to overlap "assemble the next
  /// batch" with "the previous batch is in flight".
  std::uint32_t tx_transfer_count = 4;

  /// The byte size of each bulk OUT buffer. The runtime overrides it with the
  /// negotiated device MaxTransferSize.
  std::uint32_t tx_transfer_bytes = 16 * 1024;

  /// The bulk IN timeout (milliseconds).
  ///
  /// **It must be 0 (infinite)**: the RNDIS data channel can be idle for a
  /// long time (the user is not transferring anything), and with a timeout
  /// set it would time out and resubmit over and over, every resubmission
  /// costing 3 IOKit round trips -- pure waste. darwin passes the timeout to
  /// IOKit as both noDataTimeout and completionTimeout, and 0 means wait
  /// forever.
  std::uint32_t rx_timeout_millis = 0;

  /// The bulk OUT timeout (milliseconds). A timeout on TX is reasonable -- we
  /// need to notice when the device gets stuck.
  std::uint32_t tx_timeout_millis = 5'000;
};

/// The result of one SendFrames.
///
/// Why not just return a single number: it used to return only "the number of
/// frames sent", and as a result two kinds of skipped frames (the ones
/// exceeding the device MaxTransferSize and the ones shorter than an
/// Ethernet header) were counted as "sent" -- the comment claimed "the caller
/// can infer the drops from the return value", and the caller simply could
/// not. Splitting "how much was consumed", "how much was really sent" and
/// "how much was skipped" is what makes the books balance.
struct SendOutcome {
  /// The caller should advance its offset by this many frames (including the
  /// skipped ones). 0 means the transfer pool was full and not a single frame
  /// went in.
  std::uint32_t consumed = 0;
  /// The number of frames that really entered a bulk OUT transfer.
  std::uint32_t sent_frames = 0;
  /// The total payload bytes of those frames (excluding the RNDIS header and
  /// the padding).
  std::uint64_t sent_bytes = 0;
  /// The number of frames skipped because of an illegal length (too long /
  /// shorter than an Ethernet header). Always equal to consumed - sent_frames.
  std::uint32_t skipped = 0;
};

/// The data channel abstraction.
///
/// The granularity of the abstraction is the **batch**, not the frame: one
/// virtual call handles dozens to hundreds of frames, and the cost amortized
/// per frame is far below 1 ns. This is deliberate -- a virtual call per
/// frame would have been unacceptable.
class DataChannel {
 public:
  DataChannel() = default;
  DataChannel(const DataChannel&) = delete;
  DataChannel& operator=(const DataChannel&) = delete;
  DataChannel(DataChannel&&) = delete;
  DataChannel& operator=(DataChannel&&) = delete;
  virtual ~DataChannel() = default;

  /// Starts receiving: pushes the decoded Ethernet frames into `rx_ring`.
  ///
  /// Receiving keeps running after the call, until Shutdown().
  [[nodiscard]] virtual Status StartReceiving(FrameRing& rx_ring,
                                              DirectionCounters& rx_counters) = 0;

  /// Aggregates a batch of Ethernet frames into RNDIS messages and sends them
  /// to the device. **Does not block.**
  ///
  /// `consumed < frames.size()` means the transfer pool has no free slot for
  /// the moment (backpressure). The caller should keep the remaining frames
  /// and wait with WaitForSendCapacity() until a slot is free and then retry
  /// -- **do not drop them**: the upstream (the kernel BPF buffer) is the
  /// elastic queue that should absorb bursts.
  [[nodiscard]] virtual Result<SendOutcome> SendFrames(std::span<const FrameView> frames) = 0;

  /// Waits for a free transfer slot to appear in the TX direction.
  ///
  /// Returning true means "it is worth calling SendFrames again": a slot has
  /// become free, or the channel has entered shutdown (in which case the
  /// retry gets an error and the caller exits on its own). false means the
  /// timeout expired with still no capacity.
  ///
  /// Only the same calling thread as SendFrames may use it (the TX extractor
  /// thread). The implementation must guarantee that Shutdown() can wake a
  /// waiting thread -- but the caller should not rely on that for shutdown
  /// either; it should use a finite timeout and re-check the shutdown flag
  /// after every wakeup.
  [[nodiscard]] virtual bool WaitForSendCapacity(std::uint32_t timeout_millis) = 0;

  /// Stops and reaps every in-flight transfer.
  ///
  /// **Must not be called from the libusb event thread** (that deadlocks, see
  /// the file header).
  virtual void Shutdown() = 0;

  /// Whether TX currently has a free transfer slot.
  [[nodiscard]] virtual bool CanSend() const noexcept = 0;

  /// The maximum number of bytes a single transfer can hold (TX direction).
  [[nodiscard]] virtual std::uint32_t MaxTransferBytes() const noexcept = 0;

  /// The I/O errors accumulated in the async send completion callback (STALL,
  /// transfer failures, ...).
  ///
  /// Why this is a separate interface instead of letting the data channel
  /// write the bridge's TX counters directly: the contract of
  /// DirectionCounters is "**only one thread** may call Add*" (it uses a
  /// relaxed load+store instead of fetch_add, and is safe precisely because
  /// there is a single writer). But TX has two writers -- the submission
  /// side on the TX extractor thread, the error side on the libusb event
  /// thread. Forcing them into the same counter breaks that invariant (and
  /// TSan reports it).
  /// Hence two separately single-written counters, merged by
  /// Bridge::Snapshot() **at read time**.
  [[nodiscard]] virtual std::uint64_t AsyncSendErrors() const noexcept = 0;
};

/// A data channel implementation based on the libusb asynchronous API.
class UsbDataChannel final : public DataChannel {
 public:
  /// @param device      The device with its interfaces already claimed
  /// @param parameters  The RNDIS negotiation result (decides the aggregation
  ///                    limit and the alignment)
  /// @param config      The transfer pool parameters
  [[nodiscard]] static Result<std::unique_ptr<UsbDataChannel>> Create(
      Device& device, const rndis::NegotiatedParameters& parameters,
      const DataChannelConfig& config);

  // Copy and move are already deleted in the base class DataChannel; they are
  // restated here to satisfy the static checks.
  UsbDataChannel(const UsbDataChannel&) = delete;
  UsbDataChannel& operator=(const UsbDataChannel&) = delete;
  UsbDataChannel(UsbDataChannel&&) = delete;
  UsbDataChannel& operator=(UsbDataChannel&&) = delete;
  ~UsbDataChannel() override;

  [[nodiscard]] Status StartReceiving(FrameRing& rx_ring,
                                      DirectionCounters& rx_counters) override;
  [[nodiscard]] Result<SendOutcome> SendFrames(std::span<const FrameView> frames) override;
  [[nodiscard]] bool WaitForSendCapacity(std::uint32_t timeout_millis) override;
  void Shutdown() override;

  [[nodiscard]] bool CanSend() const noexcept override;

  [[nodiscard]] std::uint32_t MaxTransferBytes() const noexcept override {
    return tx_transfer_bytes_;
  }

  [[nodiscard]] std::uint64_t AsyncSendErrors() const noexcept override {
    return async_send_errors_.load(std::memory_order_relaxed);
  }

  /// The number of transfers dropped because of a malformed RNDIS message,
  /// used to diagnose device bugs.
  [[nodiscard]] std::uint64_t MalformedTransfers() const noexcept {
    return malformed_transfers_.load(std::memory_order_relaxed);
  }

 private:
  UsbDataChannel() = default;

  /// One transfer of the pool together with its buffer.
  struct Slot {
    ::libusb_transfer* transfer = nullptr;
    std::byte* buffer = nullptr;
    std::uint32_t buffer_bytes = 0;
    UsbDataChannel* owner = nullptr;
    std::uint32_t index = 0;
  };

  // ---- libusb callbacks (static trampolines -> member functions) ----
  static void ReceiveCallbackTrampoline(::libusb_transfer* transfer);
  static void SendCallbackTrampoline(::libusb_transfer* transfer);

  void OnReceiveComplete(Slot& slot) noexcept;
  void OnSendComplete(Slot& slot) noexcept;

  /// Allocates one pool. The buffers are page aligned (darwin has no
  /// zero-copy DMA buffers, libusb_dev_mem_alloc returns NULL, so we have to
  /// use posix_memalign ourselves; aligning to hw.pagesize reduces the
  /// fragmentation when IOKit builds the DMA descriptors).
  [[nodiscard]] Status AllocatePool(std::vector<Slot>& pool, std::uint32_t count,
                                    std::uint32_t buffer_bytes);

  static void FreePool(std::vector<Slot>& pool) noexcept;

  /// Submits one RX transfer.
  [[nodiscard]] Status SubmitReceive(Slot& slot);

  Device* device_ = nullptr;
  rndis::NegotiatedParameters parameters_{};
  DataChannelConfig config_{};

  std::uint32_t tx_transfer_bytes_ = 0;

  std::vector<Slot> rx_pool_;
  std::vector<Slot> tx_pool_;

  /// The queue of free TX slot indices.
  ///
  /// The producer is the libusb event thread (the send completion callback
  /// returns the slot), the consumer is the BPF read thread (takes a slot to
  /// assemble the next batch). Exactly an SPSC.
  std::unique_ptr<SpscRing<std::uint32_t>> tx_free_slots_;

  /// The receive destination. Non-null after StartReceiving.
  FrameRing* rx_ring_ = nullptr;
  DirectionCounters* rx_counters_ = nullptr;

  /// The I/O errors accumulated in the async send completion callback.
  /// **Incremented by the libusb event thread only**, kept separate from the
  /// bridge's TX counters; see the description of AsyncSendErrors().
  std::atomic<std::uint64_t> async_send_errors_{0};

  /// The in-flight transfer count. The callbacks decrement it, Shutdown waits
  /// for it to reach zero.
  std::atomic<std::uint32_t> outstanding_{0};
  /// The shutdown flag. Once the callback sees it, it stops resubmitting.
  std::atomic<bool> shutting_down_{false};
  /// Whether the teardown has already completed (Shutdown is idempotent).
  bool shutdown_complete_ = false;

  std::atomic<std::uint64_t> malformed_transfers_{0};

  /// Waits for the in-flight count to reach zero.
  std::mutex drain_mutex_;
  std::condition_variable drain_condition_;

  /// The wait for a free TX slot.
  ///
  /// The source of truth for the predicate is tx_free_slots_ (the lock-free
  /// SPSC ring), which makes the mutex + condition variable nothing but a
  /// wakeup mechanism: the completion callback returns the slot, then takes
  /// the lock and notifies (the empty critical section idiom), which
  /// guarantees the waiter either sees the new slot in the predicate check or
  /// has already entered wait and will get the notification -- no missed
  /// wakeup. The completion callback runs on the libusb event thread and
  /// holds this lock only momentarily, so it does not amount to "the callback
  /// blocks".
  std::mutex send_capacity_mutex_;
  std::condition_variable send_capacity_cv_;
};

}  // namespace tetherkit::usb
