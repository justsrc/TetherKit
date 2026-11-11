#include "tethernet/usb/data_channel.h"

#include "tethernet/common/i18n.h"
#include "tethernet/common/logging.h"
#include "tethernet/rndis/packet_codec.h"

#include <sys/sysctl.h>

#include <cstdlib>
#include <format>

namespace tethernet::usb {
namespace {

/// Alignment in bytes for the transfer buffers.
///
/// The darwin backend provides no zero-copy DMA buffers (libusb_dev_mem_alloc
/// returns NULL), so we must allocate our own. Aligning to the system page
/// size reduces the number of memory segments IOKit has to build DMA
/// descriptors for this buffer out of. On Apple Silicon, hw.pagesize is
/// 16384.
[[nodiscard]] std::size_t QueryPageSize() {
  std::int64_t value = 0;
  std::size_t size = sizeof(value);
  if (::sysctlbyname("hw.pagesize", &value, &size, nullptr, 0) != 0 || value <= 0) {
    return 16384;  // the actual Apple Silicon value, as a backstop
  }
  return static_cast<std::size_t>(value);
}

}  // namespace

// =============================================================================
// Construction and destruction
// =============================================================================

Result<std::unique_ptr<UsbDataChannel>> UsbDataChannel::Create(
    Device& device, const rndis::NegotiatedParameters& parameters,
    const DataChannelConfig& config) {
  auto channel = std::unique_ptr<UsbDataChannel>(new UsbDataChannel());
  channel->device_ = &device;
  channel->parameters_ = parameters;
  channel->config_ = config;

  // The TX buffer must not exceed the device's advertised MaxTransferSize --
  // beyond it the device will reject or truncate.
  channel->tx_transfer_bytes_ =
      std::min(config.tx_transfer_bytes, parameters.device_max_transfer_size);
  if (channel->tx_transfer_bytes_ < rndis::kPacketMsgHeaderBytes + parameters.mtu +
                                        rndis::kEthernetHeaderBytes) {
    return std::unexpected(Error::Generic(Tr(
        Msg::kUsbTxBufferTooSmall, channel->tx_transfer_bytes_,
        rndis::kPacketMsgHeaderBytes + parameters.mtu + rndis::kEthernetHeaderBytes)));
  }

  TETHERNET_RETURN_IF_ERROR(
      channel->AllocatePool(channel->rx_pool_, config.rx_transfer_count, config.rx_transfer_bytes));
  TETHERNET_RETURN_IF_ERROR(channel->AllocatePool(channel->tx_pool_, config.tx_transfer_count,
                                                  channel->tx_transfer_bytes_));

  // TX free-slot queue: everything starts out free.
  channel->tx_free_slots_ = std::make_unique<SpscRing<std::uint32_t>>(config.tx_transfer_count);
  for (std::uint32_t i = 0; i < config.tx_transfer_count; ++i) {
    if (!channel->tx_free_slots_->TryPush(i)) {
      return std::unexpected(Error::Generic(Tr(Msg::kUsbTxSlotQueueInitFailed)));
    }
  }

  TETHERNET_INFO_TR(
      Msg::kUsbDataChannelReady, config.rx_transfer_count, config.rx_transfer_bytes / 1024,
      config.rx_transfer_count * config.rx_transfer_bytes / 1024, config.tx_transfer_count,
      channel->tx_transfer_bytes_ / 1024, parameters.device_max_transfer_size,
      parameters.max_packets_per_message, parameters.tx_alignment_bytes);

  return channel;
}

UsbDataChannel::~UsbDataChannel() {
  // Shutdown is idempotent; call it once here as a backstop in case the
  // caller forgets.
  Shutdown();
  FreePool(rx_pool_);
  FreePool(tx_pool_);
}

Status UsbDataChannel::AllocatePool(std::vector<Slot>& pool, std::uint32_t count,
                                    std::uint32_t buffer_bytes) {
  const std::size_t alignment = QueryPageSize();
  pool.resize(count);

  for (std::uint32_t i = 0; i < count; ++i) {
    Slot& slot = pool[i];
    slot.owner = this;
    slot.index = i;
    slot.buffer_bytes = buffer_bytes;

    void* raw = nullptr;
    // Round the length up to the alignment boundary too, so the tail does not
    // straddle a page.
    const std::size_t allocation =
        ((buffer_bytes + alignment - 1) / alignment) * alignment;
    if (::posix_memalign(&raw, alignment, allocation) != 0 || raw == nullptr) {
      return std::unexpected(
          Error::Generic(Tr(Msg::kUsbTransferAllocFailed, allocation, alignment)));
    }
    slot.buffer = static_cast<std::byte*>(raw);

    slot.transfer = ::libusb_alloc_transfer(0);
    if (slot.transfer == nullptr) {
      return std::unexpected(Error::Generic(Tr(Msg::kUsbAllocTransferFailed)));
    }
  }
  return Ok();
}

void UsbDataChannel::FreePool(std::vector<Slot>& pool) noexcept {
  for (Slot& slot : pool) {
    if (slot.transfer != nullptr) {
      ::libusb_free_transfer(slot.transfer);
      slot.transfer = nullptr;
    }
    // Memory from posix_memalign must be released with free (not delete).
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    std::free(slot.buffer);
    slot.buffer = nullptr;
  }
  pool.clear();
}

// =============================================================================
// Receive path (device -> host)
// =============================================================================

Status UsbDataChannel::StartReceiving(FrameRing& rx_ring, DirectionCounters& rx_counters) {
  rx_ring_ = &rx_ring;
  rx_counters_ = &rx_counters;

  for (Slot& slot : rx_pool_) {
    TETHERNET_RETURN_IF_ERROR(SubmitReceive(slot));
  }
  TETHERNET_DEBUG_TR(Msg::kUsbBulkInSubmitted, rx_pool_.size());
  return Ok();
}

Status UsbDataChannel::SubmitReceive(Slot& slot) {
  ::libusb_fill_bulk_transfer(
      slot.transfer, device_->Handle(), device_->BulkInEndpoint(),
      reinterpret_cast<unsigned char*>(slot.buffer), static_cast<int>(slot.buffer_bytes),
      &UsbDataChannel::ReceiveCallbackTrampoline, &slot,
      static_cast<unsigned int>(config_.rx_timeout_millis));

  // **Deliberately do not set LIBUSB_TRANSFER_SHORT_NOT_OK.**
  // A short packet is perfectly normal semantics on bulk IN (the device sends
  // a short packet to end the transfer when it has only half a buffer of
  // data). Setting the flag makes io.c reclassify "COMPLETED with
  // actual_length != requested length" as LIBUSB_TRANSFER_ERROR -- at which
  // point nearly every transfer would "fail".
  slot.transfer->flags = 0;

  // Increment the in-flight count before submitting: otherwise the callback
  // may finish before the increment and the counter underflows.
  outstanding_.fetch_add(1, std::memory_order_acq_rel);

  const int rc = ::libusb_submit_transfer(slot.transfer);
  if (rc != LIBUSB_SUCCESS) {
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    return std::unexpected(Error::FromLibUsb(rc, Tr(Msg::kUsbSubmitBulkInFailed)));
  }
  return Ok();
}

void UsbDataChannel::ReceiveCallbackTrampoline(::libusb_transfer* transfer) {
  auto* slot = static_cast<Slot*>(transfer->user_data);
  slot->owner->OnReceiveComplete(*slot);
}

void UsbDataChannel::OnReceiveComplete(Slot& slot) noexcept {
  // Warning: this runs on the libusb **event thread** while holding
  // ctx->event_waiters_lock. Therefore here:
  //   * never call the synchronous APIs (they return LIBUSB_ERROR_BUSY);
  //   * never do blocking I/O (it would stall every thread waiting on
  //     synchronous transfers);
  //   * only "unwrap + memcpy into the lock-free queue + resubmit at once".
  //     The real BPF write() belongs to another thread.
  const ::libusb_transfer* transfer = slot.transfer;
  bool should_resubmit = true;

  switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED: {
      const auto received = static_cast<std::size_t>(transfer->actual_length);
      if (received > 0 && rx_ring_ != nullptr) {
        // Batched publish: every frame in the transfer costs a single release
        // store.
        auto batch = rx_ring_->BeginBatchWrite();
        rndis::PacketMessageReader reader(
            std::span<const std::byte>{slot.buffer, received}, rx_ring_->MaxFrameBytes());

        std::span<const std::byte> frame;
        std::uint64_t frame_bytes = 0;
        while (true) {
          const rndis::ReadOutcome outcome = reader.Next(frame);
          if (outcome == rndis::ReadOutcome::kFrame) {
            if (!batch.Push(frame)) [[unlikely]] {
              // Downstream queue full -- the BPF writer thread cannot keep
              // up. Drop and count; never block here.
              rx_counters_->AddDroppedFull();
              continue;
            }
            frame_bytes += frame.size();
            continue;
          }
          if (outcome == rndis::ReadOutcome::kMalformed) {
            malformed_transfers_.fetch_add(1, std::memory_order_relaxed);
            rx_counters_->AddDroppedMalformed();
            TETHERNET_TRACE_TR(Msg::kUsbMalformedRndisMessage,
                               rndis::MalformedReasonName(reader.Reason()), reader.FramesDecoded());
          }
          break;
        }
        if (batch.Staged() != 0) {
          const std::uint32_t staged = batch.Staged();
          batch.Publish();
          // NOLINTNEXTLINE(readability-suspicious-call-argument)
          rx_counters_->AddBatch(staged, frame_bytes);
        }
      }
      break;
    }

    case LIBUSB_TRANSFER_STALL:
      // Endpoint halt. Clear it and continue -- libusb_clear_halt is a
      // synchronous IOKit call but has **no** usbi_handling_events guard, so
      // calling it from a callback is safe (at the cost of blocking the event
      // thread for tens of microseconds to milliseconds, hence only on a real
      // STALL).
      rx_counters_->AddIoError();
      TETHERNET_WARN_TR(Msg::kUsbBulkInStall);
      if (const auto status = device_->ClearHalt(device_->BulkInEndpoint()); !status) {
        TETHERNET_ERROR_TR(Msg::kUsbBulkInClearHaltFailed, status.error().ToString());
        should_resubmit = false;
      }
      break;

    case LIBUSB_TRANSFER_CANCELLED:
      // Shutdown path. Do **not** resubmit.
      should_resubmit = false;
      break;

    case LIBUSB_TRANSFER_NO_DEVICE:
      // Device unplugged. Stop resubmitting so the in-flight count
      // converges; upper-layer reconnection logic takes it from here.
      should_resubmit = false;
      TETHERNET_INFO_TR(Msg::kUsbBulkInDeviceGone);
      break;

    case LIBUSB_TRANSFER_TIMED_OUT:
      // rx_timeout_millis defaults to 0 (infinite), so this should not happen
      // normally. If it does, keep submitting.
      rx_counters_->AddIoError();
      break;

    case LIBUSB_TRANSFER_ERROR:
    case LIBUSB_TRANSFER_OVERFLOW:
    default:
      rx_counters_->AddIoError();
      TETHERNET_WARN_TR(Msg::kUsbBulkInFailed, static_cast<int>(transfer->status));
      break;
  }

  if (shutting_down_.load(std::memory_order_acquire)) {
    should_resubmit = false;
  }

  if (should_resubmit) {
    // Officially guaranteed safe: resubmitting the same transfer directly
    // inside the callback is fine (usbi_handle_transfer_completion has
    // already removed it from the flying list and cleared the IN_FLIGHT flag
    // before invoking the callback, and does not hold itransfer->lock).
    const int rc = ::libusb_submit_transfer(slot.transfer);
    if (rc == LIBUSB_SUCCESS) {
      return;  // in-flight count unchanged: one out, one in
    }
    TETHERNET_WARN_TR(Msg::kUsbBulkInResubmitFailed, ::libusb_error_name(rc));
  }

  // The transfer has left flight. Wake any Shutdown() waiting for zero.
  if (outstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    const std::lock_guard<std::mutex> guard(drain_mutex_);
    drain_condition_.notify_all();
  }
}

// =============================================================================
// Send path (host -> device)
// =============================================================================

bool UsbDataChannel::CanSend() const noexcept {
  return tx_free_slots_ != nullptr && tx_free_slots_->SizeSnapshot() > 0;
}

Result<SendOutcome> UsbDataChannel::SendFrames(std::span<const FrameView> frames) {
  if (frames.empty()) {
    return SendOutcome{};
  }
  if (shutting_down_.load(std::memory_order_acquire)) {
    return std::unexpected(Error::Generic(Tr(Msg::kUsbDataChannelShuttingDown)));
  }

  SendOutcome outcome;

  while (outcome.consumed < frames.size()) {
    std::uint32_t slot_index = 0;
    if (!tx_free_slots_->TryPop(slot_index)) {
      // No free transfer slot -- this is backpressure. Return the consumed
      // count truthfully; the caller should WaitForSendCapacity() and retry
      // the remaining frames rather than dropping them.
      break;
    }
    Slot& slot = tx_pool_[slot_index];

    // Aggregate as many frames as possible into this one transfer.
    rndis::PacketMessageWriter writer(
        std::span<std::byte>{slot.buffer, slot.buffer_bytes},
        rndis::PacketMessageWriter::Limits{
            .max_transfer_bytes = parameters_.device_max_transfer_size,
            .max_messages = parameters_.max_packets_per_message,
            .alignment_bytes = parameters_.tx_alignment_bytes,
        },
        device_->BulkMaxPacketSize());

    std::uint32_t scanned = 0;   // frames consumed from the input (including skipped short ones)
    std::uint32_t appended = 0;  // frames actually packed into this transfer
    bool append_failed = false;
    while (outcome.consumed + scanned < frames.size()) {
      const FrameView& frame = frames[outcome.consumed + scanned];
      if (frame.length < kMinEthernetFrameBytes) [[unlikely]] {
        // Illegal short frame: skip it and count it truthfully as skipped --
        // it was never sent.
        ++scanned;
        ++outcome.skipped;
        continue;
      }
      if (!writer.TryAppend(frame.Bytes())) {
        append_failed = true;
        break;  // this chunk is full (byte or packet cap) or the frame is oversized
      }
      ++scanned;
      ++appended;
    }

    if (writer.Empty()) {
      // Not a single frame fit. Either this run was all skipped short frames,
      // or the first legal frame exceeds the device's MaxTransferSize -- the
      // latter must be skipped and advanced past, or we spin forever.
      outcome.consumed += scanned;
      if (append_failed && outcome.consumed < frames.size()) {
        ++outcome.consumed;
        ++outcome.skipped;
      }
      if (!tx_free_slots_->TryPush(slot_index)) {
        TETHERNET_ERROR_TR(Msg::kUsbReturnTxSlotFailed, slot_index);
      }
      continue;
    }

    const std::uint32_t transfer_bytes = writer.Finish();
    const std::uint64_t payload_bytes = writer.PayloadBytes();
    const std::uint32_t message_count = writer.MessageCount();

    ::libusb_fill_bulk_transfer(
        slot.transfer, device_->Handle(), device_->BulkOutEndpoint(),
        reinterpret_cast<unsigned char*>(slot.buffer), static_cast<int>(transfer_bytes),
        &UsbDataChannel::SendCallbackTrampoline, &slot,
        static_cast<unsigned int>(config_.tx_timeout_millis));
    slot.transfer->flags = 0;

    outstanding_.fetch_add(1, std::memory_order_acq_rel);
    const int rc = ::libusb_submit_transfer(slot.transfer);
    if (rc != LIBUSB_SUCCESS) {
      outstanding_.fetch_sub(1, std::memory_order_acq_rel);
      if (!tx_free_slots_->TryPush(slot_index)) {
        TETHERNET_ERROR_TR(Msg::kUsbReturnTxSlotFailed, slot_index);
      }
      async_send_errors_.fetch_add(1, std::memory_order_relaxed);
      return std::unexpected(Error::FromLibUsb(rc, Tr(Msg::kUsbSubmitBulkOutFailed)));
    }

    // Frames and bytes are counted by the **bridge** (it is the only writer
    // of the TX counters). Here we only log the aggregation effect for
    // diagnostics.
    TETHERNET_TRACE_TR(Msg::kUsbBulkOutSubmitted, message_count, payload_bytes, transfer_bytes);
    outcome.consumed += scanned;
    outcome.sent_frames += appended;
    outcome.sent_bytes += payload_bytes;
  }

  return outcome;
}

bool UsbDataChannel::WaitForSendCapacity(std::uint32_t timeout_millis) {
  std::unique_lock<std::mutex> lock(send_capacity_mutex_);
  return send_capacity_cv_.wait_for(lock, std::chrono::milliseconds(timeout_millis), [this] {
    return shutting_down_.load(std::memory_order_acquire) ||
           tx_free_slots_->SizeSnapshot() > 0;
  });
}

void UsbDataChannel::SendCallbackTrampoline(::libusb_transfer* transfer) {
  auto* slot = static_cast<Slot*>(transfer->user_data);
  slot->owner->OnSendComplete(*slot);
}

void UsbDataChannel::OnSendComplete(Slot& slot) noexcept {
  // Also on the libusb event thread; also must not block.
  switch (slot.transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
      break;

    case LIBUSB_TRANSFER_STALL:
      async_send_errors_.fetch_add(1, std::memory_order_relaxed);
      TETHERNET_WARN_TR(Msg::kUsbBulkOutStall);
      if (const auto status = device_->ClearHalt(device_->BulkOutEndpoint()); !status) {
        TETHERNET_ERROR_TR(Msg::kUsbBulkOutClearHaltFailed, status.error().ToString());
      }
      break;

    case LIBUSB_TRANSFER_CANCELLED:
    case LIBUSB_TRANSFER_NO_DEVICE:
      break;

    default:
      async_send_errors_.fetch_add(1, std::memory_order_relaxed);
      TETHERNET_WARN_TR(Msg::kUsbBulkOutFailed, static_cast<int>(slot.transfer->status));
      break;
  }

  // Return the slot. Not returning it during shutdown does not matter --
  // nobody will take one anyway.
  if (!shutting_down_.load(std::memory_order_acquire)) {
    if (!tx_free_slots_->TryPush(slot.index)) {
      TETHERNET_ERROR_TR(Msg::kUsbReturnTxSlotFailed, slot.index);
    }
    // Wake a TX thread that may be waiting for a slot in
    // WaitForSendCapacity(). Empty-critical-section idiom: take the lock
    // briefly so the waiter cannot miss this notification in the "predicate
    // checked, not yet asleep" window.
    { const std::lock_guard<std::mutex> guard(send_capacity_mutex_); }
    send_capacity_cv_.notify_one();
  }

  if (outstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    const std::lock_guard<std::mutex> guard(drain_mutex_);
    drain_condition_.notify_all();
  }
}

// =============================================================================
// Teardown
// =============================================================================

void UsbDataChannel::Shutdown() {
  if (shutdown_complete_) {
    return;
  }
  // Warning: this function must **never** be called from the libusb event
  // thread: below we wait for the in-flight count to reach zero, and the
  // callbacks decrementing it run on that very thread -- waiting there is
  // waiting on ourselves, a guaranteed deadlock. The right callers are the
  // control thread or the main thread.

  // 1. Set the shutdown flag. Callbacks see it and stop resubmitting, so the
  //    in-flight count starts converging on its own. Also wake any thread
  //    still waiting for a slot in WaitForSendCapacity() (under the normal
  //    teardown order the TX thread is already joined by here; this is a
  //    defensive backstop -- e.g. when tests drive the channel directly).
  shutting_down_.store(true, std::memory_order_release);
  { const std::lock_guard<std::mutex> guard(send_capacity_mutex_); }
  send_capacity_cv_.notify_all();

  // 2. Cancel the in-flight transfers.
  //
  // On darwin libusb_cancel_transfer is AbortPipe and cancels **every**
  // in-flight transfer on that endpoint, so one call per endpoint would
  // technically suffice; calling per slot here is idempotent and clearer.
  // LIBUSB_ERROR_NOT_FOUND means it was not in flight to begin with, which is
  // normal.
  for (Slot& slot : rx_pool_) {
    if (slot.transfer != nullptr) {
      const int rc = ::libusb_cancel_transfer(slot.transfer);
      if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NOT_FOUND) {
        TETHERNET_DEBUG_TR(Msg::kUsbCancelBulkInReturned, ::libusb_error_name(rc));
      }
    }
  }
  for (Slot& slot : tx_pool_) {
    if (slot.transfer != nullptr) {
      const int rc = ::libusb_cancel_transfer(slot.transfer);
      if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NOT_FOUND) {
        TETHERNET_DEBUG_TR(Msg::kUsbCancelBulkOutReturned, ::libusb_error_name(rc));
      }
    }
  }

  // 3. Wait for the in-flight count to reach zero. **This is the key step
  //    for avoiding use-after-free** -- every callback must have run before
  //    we free the transfers and buffers.
  {
    constexpr auto kDrainTimeout = std::chrono::seconds(5);
    std::unique_lock<std::mutex> lock(drain_mutex_);
    const bool drained = drain_condition_.wait_for(lock, kDrainTimeout, [this] {
      return outstanding_.load(std::memory_order_acquire) == 0;
    });
    if (!drained) {
      // Timeout. Freeing the buffers here would be dangerous (callbacks may
      // still touch them), so **deliberately leak** -- leaking a few hundred
      // KB beats a use-after-free crash.
      const std::uint32_t stuck = outstanding_.load(std::memory_order_acquire);
      TETHERNET_ERROR_TR(Msg::kUsbTransferReclaimTimeout, stuck,
                         (stuck * config_.rx_transfer_bytes) / 1024);
      // Null the transfer pointers so FreePool skips them.
      for (Slot& slot : rx_pool_) {
        slot.transfer = nullptr;
        slot.buffer = nullptr;
      }
      for (Slot& slot : tx_pool_) {
        slot.transfer = nullptr;
        slot.buffer = nullptr;
      }
    }
  }

  shutdown_complete_ = true;
  TETHERNET_DEBUG_TR(Msg::kUsbDataChannelStopped);
}

}  // namespace tethernet::usb
