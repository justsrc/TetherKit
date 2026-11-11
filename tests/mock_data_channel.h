// In-memory RNDIS data channel, for driving the bridge layer offline.
//
// It plays the "USB device" side:
//   * After StartReceiving, InjectFromDevice can inject "frames from the
//     device"; an internal producer thread pushes them into the bridge's RX
//     ring -- this simulates the libusb event-thread role.
//   * SendFrames accepts "frames the host wants to send to the device", for
//     test assertions.
//
// This lets the bridge's thread model, batching, backpressure, and statistics
// be fully tested on a machine without any USB device, including concurrency
// correctness under TSan.
#pragma once

#include "tethernet/common/frame_ring.h"
#include "tethernet/usb/data_channel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace tethernet::testing {

/// Data channel that emulates device behavior.
class MockDataChannel final : public usb::DataChannel {
 public:
  explicit MockDataChannel(std::uint32_t max_transfer_bytes = 16 * 1024)
      : max_transfer_bytes_(max_transfer_bytes) {}

  MockDataChannel(const MockDataChannel&) = delete;
  MockDataChannel& operator=(const MockDataChannel&) = delete;
  MockDataChannel(MockDataChannel&&) = delete;
  MockDataChannel& operator=(MockDataChannel&&) = delete;

  ~MockDataChannel() override { Shutdown(); }

  // ---------------------------------------------------------------------------
  // DataChannel implementation
  // ---------------------------------------------------------------------------

  [[nodiscard]] Status StartReceiving(FrameRing& rx_ring,
                                      DirectionCounters& rx_counters) override {
    rx_ring_ = &rx_ring;
    rx_counters_ = &rx_counters;
    receiving_.store(true, std::memory_order_release);
    return Ok();
  }

  [[nodiscard]] Result<usb::SendOutcome> SendFrames(std::span<const FrameView> frames) override {
    if (send_should_fail_.load(std::memory_order_acquire)) {
      return std::unexpected(Error::Generic("mock: send fails per the test's setup"));
    }

    // Emulates the transfer pool's limited capacity: accepts at most
    // accept_limit_ frames per call; beyond that, backpressure.
    const std::uint32_t limit = accept_limit_.load(std::memory_order_acquire);
    const auto accepted =
        static_cast<std::uint32_t>(std::min<std::size_t>(frames.size(), limit));

    std::uint64_t bytes = 0;
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      for (std::size_t i = 0; i < accepted; ++i) {
        sent_to_device_.emplace_back(frames[i].data, frames[i].data + frames[i].length);
        sent_bytes_ += frames[i].length;
        bytes += frames[i].length;
      }
    }
    sent_frames_.fetch_add(accepted, std::memory_order_relaxed);
    return usb::SendOutcome{
        .consumed = accepted, .sent_frames = accepted, .sent_bytes = bytes, .skipped = 0};
  }

  [[nodiscard]] bool WaitForSendCapacity(std::uint32_t timeout_millis) override {
    wait_calls_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(wait_mutex_);
    return wait_cv_.wait_for(lock, std::chrono::milliseconds(timeout_millis), [this] {
      return shutdown_called_.load(std::memory_order_acquire) ||
             accept_limit_.load(std::memory_order_acquire) > 0;
    });
  }

  void Shutdown() override {
    receiving_.store(false, std::memory_order_release);
    shutdown_called_.store(true, std::memory_order_release);
    // Wake any thread waiting for capacity inside WaitForSendCapacity (the
    // same empty-critical-section idiom as the real implementation).
    { const std::lock_guard<std::mutex> guard(wait_mutex_); }
    wait_cv_.notify_all();
  }

  [[nodiscard]] bool CanSend() const noexcept override {
    return accept_limit_.load(std::memory_order_acquire) > 0;
  }

  [[nodiscard]] std::uint32_t MaxTransferBytes() const noexcept override {
    return max_transfer_bytes_;
  }

  [[nodiscard]] std::uint64_t AsyncSendErrors() const noexcept override {
    return async_send_errors_.load(std::memory_order_relaxed);
  }

  /// Emulates errors raised inside the async completion callback (in the real
  /// implementation these come from STALL / transfer failure).
  void InjectAsyncSendError() noexcept {
    async_send_errors_.fetch_add(1, std::memory_order_relaxed);
  }

  // ---------------------------------------------------------------------------
  // Test injection
  // ---------------------------------------------------------------------------

  /// Pushes a batch of "frames from the device" directly into the bridge's RX
  /// ring.
  ///
  /// Uses BatchWrite, matching the real libusb callback path. Returns the
  /// number of frames actually enqueued (fewer than requested when the queue
  /// is full -- exactly the frame-drop path under test).
  [[nodiscard]] std::uint32_t InjectFromDevice(
      const std::vector<std::vector<std::byte>>& frames) {
    if (rx_ring_ == nullptr) {
      return 0;
    }
    auto batch = rx_ring_->BeginBatchWrite();
    std::uint32_t accepted = 0;
    std::uint64_t bytes = 0;
    for (const std::vector<std::byte>& frame : frames) {
      if (!batch.Push(frame)) {
        if (rx_counters_ != nullptr) {
          rx_counters_->AddDroppedFull();
        }
        continue;
      }
      ++accepted;
      bytes += frame.size();
    }
    if (accepted != 0 && rx_counters_ != nullptr) {
      rx_counters_->AddBatch(accepted, bytes);
    }
    return accepted;
  }

  /// Sets how many frames one SendFrames call may accept at most. Setting 0
  /// emulates a fully exhausted transfer pool (backpressure).
  ///
  /// Recovering from 0 to a non-zero value is equivalent to "transfer
  /// completed, slot returned", and wakes the bridge's TX thread waiting in
  /// WaitForSendCapacity.
  void SetAcceptLimit(std::uint32_t limit) {
    accept_limit_.store(limit, std::memory_order_release);
    { const std::lock_guard<std::mutex> guard(wait_mutex_); }
    wait_cv_.notify_all();
  }

  void SetSendShouldFail(bool fail) noexcept {
    send_should_fail_.store(fail, std::memory_order_release);
  }

  // ---------------------------------------------------------------------------
  // Observers
  // ---------------------------------------------------------------------------

  [[nodiscard]] std::uint64_t SentFrameCount() const noexcept {
    return sent_frames_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t SentByteCount() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return sent_bytes_;
  }

  [[nodiscard]] std::vector<std::vector<std::byte>> DrainSentToDevice() {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::vector<std::vector<std::byte>> drained;
    drained.swap(sent_to_device_);
    return drained;
  }

  [[nodiscard]] bool ShutdownCalled() const noexcept {
    return shutdown_called_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool Receiving() const noexcept {
    return receiving_.load(std::memory_order_acquire);
  }

  /// Number of times WaitForSendCapacity was called -- asserts that "the
  /// bridge actually waits instead of dropping".
  [[nodiscard]] std::uint64_t WaitCalls() const noexcept {
    return wait_calls_.load(std::memory_order_relaxed);
  }

 private:
  std::uint32_t max_transfer_bytes_;

  FrameRing* rx_ring_ = nullptr;
  DirectionCounters* rx_counters_ = nullptr;

  mutable std::mutex mutex_;
  std::vector<std::vector<std::byte>> sent_to_device_;
  std::uint64_t sent_bytes_ = 0;

  std::mutex wait_mutex_;
  std::condition_variable wait_cv_;
  std::atomic<std::uint64_t> wait_calls_{0};

  std::atomic<std::uint64_t> sent_frames_{0};
  std::atomic<std::uint32_t> accept_limit_{0xFFFF'FFFFU};
  std::atomic<bool> send_should_fail_{false};
  std::atomic<bool> receiving_{false};
  std::atomic<bool> shutdown_called_{false};
  std::atomic<std::uint64_t> async_send_errors_{0};
};

}  // namespace tethernet::testing
