// End-to-end tests for the bridge layer.
//
// MockDataChannel (the USB side) + LoopbackLink (the interface side) run the
// complete data path, covering the real multi-threaded movement, batching,
// backpressure, pausing and graceful shutdown.
//
// These cases only make full sense under ThreadSanitizer:
//   cmake -B build-tsan -DTETHERNET_ENABLE_TSAN=ON && ctest --test-dir build-tsan -R core
#include "tethernet/core/bridge.h"
#include "tethernet/net/loopback_link.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "language_guard.h"
#include "mock_data_channel.h"

using namespace tethernet;        // NOLINT(google-build-using-namespace)
using namespace tethernet::core;  // NOLINT(google-build-using-namespace)
using tethernet::net::LoopbackConfig;
using tethernet::net::LoopbackLink;
using tethernet::testing::MockDataChannel;

namespace {

/// Builds an Ethernet frame whose content can be self-verified.
std::vector<std::byte> MakeFrame(std::uint32_t length, std::uint8_t tag) {
  std::vector<std::byte> frame(length);
  for (int i = 0; i < 6; ++i) {
    frame[static_cast<std::size_t>(i)] = std::byte{0xFF};  // broadcast destination MAC
  }
  frame[6] = std::byte{0x02};
  frame[11] = std::byte{tag};
  frame[12] = std::byte{0x08};
  frame[13] = std::byte{0x00};
  for (std::uint32_t i = 14; i < length; ++i) {
    frame[i] = std::byte{static_cast<unsigned char>((tag + i) & 0xFFU)};
  }
  return frame;
}

std::vector<std::vector<std::byte>> MakeFrames(std::uint32_t count, std::uint32_t length) {
  std::vector<std::vector<std::byte>> frames;
  frames.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    frames.push_back(MakeFrame(length, static_cast<std::uint8_t>(i & 0xFFU)));
  }
  return frames;
}

/// Polls until a condition holds, waiting at most `timeout`.
///
/// The data path is asynchronous, so sleeping for a fixed duration to "wait
/// for it to finish" is not an option -- that fails sporadically on slow
/// machines. Wait for the condition, not for the time.
template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

/// Bridge configuration for the tests: small queue, small batches, so the
/// boundaries are hit more easily.
[[nodiscard]] BridgeConfig TestConfig() {
  BridgeConfig config;
  config.rx_ring_frames = 256;
  config.max_frame_bytes = 1518;
  config.rx_write_batch = 16;
  config.rx_spin_before_yield = 4;  // let the idle spin yield the CPU faster in tests
  config.tx_submit_batch = 32;
  return config;
}

}  // namespace

TEST_SUITE("core.bridge") {
  TEST_CASE("Start and stop: the threads come up and go down cleanly, the channel is closed") {
    MockDataChannel channel;
    LoopbackLink link;
    Bridge bridge(channel, link, TestConfig());

    CHECK_FALSE(bridge.Running());
    REQUIRE(bridge.Start().has_value());
    CHECK(bridge.Running());
    CHECK(channel.Receiving());

    // A repeated Start must be rejected.
    CHECK_FALSE(bridge.Start().has_value());

    bridge.Stop();
    CHECK_FALSE(bridge.Running());
    CHECK(channel.ShutdownCalled());

    // Stop is idempotent.
    bridge.Stop();
  }

  TEST_CASE("RX direction: frames from the device are carried onto the link intact") {
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.sent_capacity = 4096});
    Bridge bridge(channel, link, TestConfig());
    REQUIRE(bridge.Start().has_value());

    constexpr std::uint32_t kCount = 100;
    const auto frames = MakeFrames(kCount, 1514);
    CHECK(channel.InjectFromDevice(frames) == kCount);

    REQUIRE(WaitFor([&] { return link.TotalSentFrames() >= kCount; }));

    const auto received = link.DrainSent();
    REQUIRE(received.size() == kCount);
    // Both the FIFO order and the content must be preserved.
    for (std::size_t i = 0; i < kCount; ++i) {
      CHECK(received[i] == frames[i]);
    }

    const BridgeStats stats = bridge.Snapshot();
    CHECK(stats.rx.frames == kCount);
    CHECK(stats.rx.bytes == static_cast<std::uint64_t>(kCount) * 1514);

    bridge.Stop();
  }

  TEST_CASE("TX direction: frames received on the link are submitted to the device intact") {
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.inbound_capacity = 4096, .max_frames_per_batch = 64});
    Bridge bridge(channel, link, TestConfig());
    REQUIRE(bridge.Start().has_value());

    constexpr std::uint32_t kCount = 100;
    const auto frames = MakeFrames(kCount, 512);
    for (const std::vector<std::byte>& frame : frames) {
      REQUIRE(link.PushInbound(frame));
    }

    REQUIRE(WaitFor([&] { return channel.SentFrameCount() >= kCount; }));

    const auto delivered = channel.DrainSentToDevice();
    REQUIRE(delivered.size() == kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
      CHECK(delivered[i] == frames[i]);
    }

    const BridgeStats stats = bridge.Snapshot();
    CHECK(stats.tx.frames == kCount);

    bridge.Stop();
  }

  TEST_CASE("Both directions run at once without interfering with each other") {
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{
        .inbound_capacity = 8192, .sent_capacity = 8192, .max_frames_per_batch = 64});
    Bridge bridge(channel, link, TestConfig());
    REQUIRE(bridge.Start().has_value());

    constexpr std::uint32_t kCount = 500;
    const auto tx_frames = MakeFrames(kCount, 700);

    // TX side: the host sends to the device.
    std::thread pusher([&] {
      for (const std::vector<std::byte>& frame : tx_frames) {
        while (!link.PushInbound(frame)) {
          std::this_thread::yield();
        }
      }
    });

    // RX side: the device sends to the host. Inject in batches, retry when the
    // queue is full.
    std::uint32_t injected = 0;
    while (injected < kCount) {
      const auto chunk = MakeFrames(std::min<std::uint32_t>(32, kCount - injected), 1000);
      const std::uint32_t accepted = channel.InjectFromDevice(chunk);
      injected += accepted;
      if (accepted == 0) {
        std::this_thread::yield();
      }
    }
    pusher.join();

    REQUIRE(WaitFor([&] { return channel.SentFrameCount() >= kCount; }));
    REQUIRE(WaitFor([&] { return link.TotalSentFrames() >= kCount; }));

    const BridgeStats stats = bridge.Snapshot();
    CHECK(stats.rx.frames >= kCount);
    CHECK(stats.tx.frames >= kCount);

    bridge.Stop();
  }

  TEST_CASE("When the RX queue is full frames are dropped and counted, the producer never blocks") {
    MockDataChannel channel;
    // The link's write capacity is tiny, so the RX injection thread cannot
    // write out and the queue fills up quickly.
    LoopbackLink link(LoopbackConfig{.sent_capacity = 8});
    auto config = TestConfig();
    config.rx_ring_frames = 16;
    Bridge bridge(channel, link, config);
    REQUIRE(bridge.Start().has_value());

    // Inject far more frames than the queue can hold.
    std::uint32_t total_accepted = 0;
    for (int round = 0; round < 20; ++round) {
      total_accepted += channel.InjectFromDevice(MakeFrames(64, 1514));
    }

    // The key assertion: InjectFromDevice never blocks, and the rejected frames
    // are counted as drops.
    const BridgeStats stats = bridge.Snapshot();
    CHECK(total_accepted < 20 * 64);  // rejections really happened
    CHECK(stats.rx.TotalDropped() > 0);

    bridge.Stop();
  }

  TEST_CASE("TX backpressure: wait for capacity instead of dropping, then deliver every frame") {
    // This is the regression test for the root cause of the real-device frame
    // drops: the old implementation dropped the remaining frames of a batch the
    // moment SendFrames returned 0 (bridge.cc carried a wrong argument that
    // "waiting turns into invisible kernel drops"), and under heavy load it
    // threw away more than 30% of the TX traffic at the transfer-pool door.
    // The correct behavior is to wait for a slot to free up and retry --
    // bursts are absorbed by the upstream kernel BPF buffer.
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.inbound_capacity = 4096, .max_frames_per_batch = 64});
    Bridge bridge(channel, link, TestConfig());

    // Make the mock channel accept not a single frame -- emulates a fully
    // exhausted transfer pool.
    channel.SetAcceptLimit(0);
    REQUIRE(bridge.Start().has_value());

    constexpr std::uint32_t kFrameCount = 200;
    for (const std::vector<std::byte>& frame : MakeFrames(kFrameCount, 512)) {
      REQUIRE(link.PushInbound(frame));
    }

    // The bridge must fall into waiting: the backpressure event count climbs
    // and WaitForSendCapacity is called, but not one frame is dropped and not
    // one frame is sent.
    REQUIRE(WaitFor([&] { return bridge.Snapshot().tx_backpressure_events > 0; }));
    REQUIRE(WaitFor([&] { return channel.WaitCalls() > 0; }));
    CHECK(bridge.Snapshot().tx.TotalDropped() == 0);
    CHECK(channel.SentFrameCount() == 0);

    // Capacity returns (as if an in-flight transfer completed and its slot came
    // back): the frames queued at the door must be delivered **without losing a
    // single one**, and in the same order.
    // The wait condition must cover the mock counter and the bridge counter
    // together: the mock increments inside SendFrames while the bridge adds the
    // batch only after SendFrames returns, with no happens-before between them
    // -- waiting on the mock counter alone and then asserting on the bridge
    // counter sporadically reads a stale value one batch behind on slow
    // machines.
    channel.SetAcceptLimit(0xFFFF'FFFFU);
    REQUIRE(WaitFor([&] {
      return channel.SentFrameCount() >= kFrameCount && bridge.Snapshot().tx.frames >= kFrameCount;
    }));

    const BridgeStats stats = bridge.Snapshot();
    CHECK(stats.tx.TotalDropped() == 0);
    CHECK(stats.tx.frames == kFrameCount);
    CHECK(channel.SentFrameCount() == kFrameCount);

    const auto delivered = channel.DrainSentToDevice();
    REQUIRE(delivered.size() == kFrameCount);
    const auto expected = MakeFrames(kFrameCount, 512);
    for (std::size_t i = 0; i < delivered.size(); ++i) {
      CHECK(delivered[i] == expected[i]);
    }

    bridge.Stop();
  }

  TEST_CASE("TX backpressure: on partial acceptance the rest is retried, none lost or reordered") {
    // The pool is not full but cannot take a whole batch at once (SendFrames
    // accepts partially): the remaining frames must be retried in place.
    // At most 7 frames are accepted per call with a batch of 32 frames, which
    // forces the "several partial acceptances make up one batch" path.
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.inbound_capacity = 4096, .max_frames_per_batch = 64});
    Bridge bridge(channel, link, TestConfig());

    channel.SetAcceptLimit(7);
    REQUIRE(bridge.Start().has_value());

    constexpr std::uint32_t kFrameCount = 100;
    for (const std::vector<std::byte>& frame : MakeFrames(kFrameCount, 512)) {
      REQUIRE(link.PushInbound(frame));
    }

    REQUIRE(WaitFor([&] { return channel.SentFrameCount() >= kFrameCount; }));
    CHECK(bridge.Snapshot().tx.TotalDropped() == 0);

    const auto delivered = channel.DrainSentToDevice();
    REQUIRE(delivered.size() == kFrameCount);
    const auto expected = MakeFrames(kFrameCount, 512);
    for (std::size_t i = 0; i < delivered.size(); ++i) {
      CHECK(delivered[i] == expected[i]);
    }

    bridge.Stop();
  }

  TEST_CASE("TX backpressure: Stop() returns promptly while a wait is in progress") {
    // The backpressure wait introduced a new blocking point, and the shutdown
    // must not be wedged by it: the TX thread rechecks the shutdown flag every
    // time it wakes up (at most about 50 ms).
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.inbound_capacity = 4096, .max_frames_per_batch = 64});
    Bridge bridge(channel, link, TestConfig());

    channel.SetAcceptLimit(0);
    REQUIRE(bridge.Start().has_value());

    for (const std::vector<std::byte>& frame : MakeFrames(200, 512)) {
      REQUIRE(link.PushInbound(frame));
    }
    REQUIRE(WaitFor([&] { return bridge.Snapshot().tx_backpressure_events > 0; }));

    const auto stop_begin = std::chrono::steady_clock::now();
    bridge.Stop();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_begin;

    CHECK_FALSE(bridge.Running());
    // The upper bound is "wait timeout + BPF read timeout + margin". The real
    // constraint is on the order of tens of milliseconds; it is relaxed to 2
    // seconds here only so slow CI machines do not flake.
    CHECK(stop_elapsed < std::chrono::seconds(2));
    // The frames not sent by the time of the shutdown count as drops -- the
    // books must balance, nothing may vanish into thin air.
    CHECK(bridge.Snapshot().tx.TotalDropped() > 0);
  }

  TEST_CASE("TX backpressure: a pause lands during the wait, the rest is dropped, thread alive") {
    // A pause (RNDIS soft reset) takes precedence over the wait: the device
    // drops unfinished packets anyway, so hoarding them is pointless.
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.inbound_capacity = 4096, .max_frames_per_batch = 64});
    Bridge bridge(channel, link, TestConfig());

    channel.SetAcceptLimit(0);
    REQUIRE(bridge.Start().has_value());

    for (const std::vector<std::byte>& frame : MakeFrames(100, 512)) {
      REQUIRE(link.PushInbound(frame));
    }
    REQUIRE(WaitFor([&] { return bridge.Snapshot().tx_backpressure_events > 0; }));
    CHECK(bridge.Snapshot().tx.TotalDropped() == 0);

    bridge.SetPaused(true);
    REQUIRE(WaitFor([&] { return bridge.Snapshot().tx.TotalDropped() > 0; }));
    CHECK(channel.SentFrameCount() == 0);

    // After resuming, new frames are delivered as usual -- the thread did not
    // wedge inside the wait.
    bridge.SetPaused(false);
    channel.SetAcceptLimit(0xFFFF'FFFFU);
    for (const std::vector<std::byte>& frame : MakeFrames(20, 512)) {
      REQUIRE(link.PushInbound(frame));
    }
    REQUIRE(WaitFor([&] { return channel.SentFrameCount() >= 20; }));

    bridge.Stop();
  }

  TEST_CASE("Nothing moves while paused; after resuming the RX queue keeps being sent") {
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.sent_capacity = 4096});
    Bridge bridge(channel, link, TestConfig());
    REQUIRE(bridge.Start().has_value());

    bridge.SetPaused(true);
    CHECK(bridge.Paused());

    constexpr std::uint32_t kCount = 32;
    CHECK(channel.InjectFromDevice(MakeFrames(kCount, 256)) == kCount);

    // No frame may be written to the link while paused. Give the injection
    // thread a moment to prove it really did not move.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(link.TotalSentFrames() == 0);
    // The frames are still in the queue, not dropped.
    CHECK(bridge.Snapshot().rx_queue_depth == kCount);

    // After resuming everything must be sent -- a pause does not drop frames.
    bridge.SetPaused(false);
    REQUIRE(WaitFor([&] { return link.TotalSentFrames() >= kCount; }));
    CHECK(bridge.Snapshot().rx.TotalDropped() == 0);

    bridge.Stop();
  }

  TEST_CASE("SetPaused never deadlocks in any lifecycle phase") {
    // SetPaused(true) waits for the RX injection thread to confirm that it has
    // stopped. That wait carries two deadlock risks, both far more serious
    // than the frame-loss bug it fixes (what it blocks is the control path:
    // link state changes, device soft resets):
    //   * nobody sets the confirmation bit when the thread is not running at
    //     all;
    //   * a stale confirmation bit makes the wait logic itself wrong.
    // All three phases are walked through here; a deadlock anywhere shows up
    // as a timeout of this case rather than as a silent pass.
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.sent_capacity = 4096});
    Bridge bridge(channel, link, TestConfig());

    // Phase one: before Start().
    bridge.SetPaused(true);
    CHECK(bridge.Paused());
    bridge.SetPaused(false);

    REQUIRE(bridge.Start().has_value());

    // Phase two: rapid toggling while running. The confirmation bit is cleared
    // at the top of every round, so a later SetPaused(true) does not read the
    // previous round's leftover value and return early -- returning early would
    // mean the guarantee is void.
    for (int i = 0; i < 50; ++i) {
      bridge.SetPaused(true);
      bridge.SetPaused(false);
    }

    // After the toggling the real pause guarantee is still in effect.
    bridge.SetPaused(true);
    constexpr std::uint32_t kCount = 16;
    CHECK(channel.InjectFromDevice(MakeFrames(kCount, 256)) == kCount);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(link.TotalSentFrames() == 0);

    bridge.Stop();

    // Phase three: after Stop().
    bridge.SetPaused(true);
    CHECK(bridge.Paused());
  }

  TEST_CASE("While paused, frames in the TX direction are dropped and counted") {
    // Unlike RX: on the TX side a pause must drop, because during an RNDIS
    // reset the device discards every unfinished packet, and hoarding them
    // only wastes memory.
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.inbound_capacity = 512, .max_frames_per_batch = 64});
    Bridge bridge(channel, link, TestConfig());
    REQUIRE(bridge.Start().has_value());
    bridge.SetPaused(true);

    for (const std::vector<std::byte>& frame : MakeFrames(50, 256)) {
      REQUIRE(link.PushInbound(frame));
    }

    REQUIRE(WaitFor([&] { return bridge.Snapshot().tx.TotalDropped() > 0; }));
    CHECK(channel.SentFrameCount() == 0);

    bridge.Stop();
  }

  TEST_CASE("A link write failure does not kill the thread; it keeps working after recovery") {
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.sent_capacity = 4096});
    link.FailWritesAfter(1);  // every write from the 2nd one on fails
    Bridge bridge(channel, link, TestConfig());
    REQUIRE(bridge.Start().has_value());

    for (int round = 0; round < 5; ++round) {
      // It does not matter how many frames were queued; all we need is enough
      // traffic to keep writing to the link.
      (void)channel.InjectFromDevice(MakeFrames(16, 256));
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // The key point: the bridge is still running, it neither crashed nor exited
    // because of the write failure.
    CHECK(bridge.Running());
    CHECK(bridge.Snapshot().rx.io_errors > 0);

    bridge.Stop();
  }

  TEST_CASE("A USB submission failure is recorded as an I/O error, the bridge keeps running") {
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.inbound_capacity = 512, .max_frames_per_batch = 64});
    Bridge bridge(channel, link, TestConfig());
    channel.SetSendShouldFail(true);
    REQUIRE(bridge.Start().has_value());

    for (const std::vector<std::byte>& frame : MakeFrames(50, 256)) {
      REQUIRE(link.PushInbound(frame));
    }

    REQUIRE(WaitFor([&] { return bridge.Snapshot().tx.io_errors > 0; }));
    CHECK(bridge.Running());

    bridge.Stop();
  }

  TEST_CASE("stats line renders rates and drops") {
    // The stats line is user-facing text; pin the language before asserting.
    const tethernet::testing::ScopedLanguage guard{tethernet::Language::kEnglish};
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{.sent_capacity = 4096});
    Bridge bridge(channel, link, TestConfig());
    REQUIRE(bridge.Start().has_value());

    const BridgeStats before = bridge.Snapshot();
    constexpr std::uint32_t kCount = 50;
    CHECK(channel.InjectFromDevice(MakeFrames(kCount, 1514)) == kCount);
    REQUIRE(WaitFor([&] { return link.TotalSentFrames() >= kCount; }));
    const BridgeStats after = bridge.Snapshot();

    const std::string line = FormatStatsLine(before, after, 1.0);
    CHECK(line.find("RX") != std::string::npos);
    CHECK(line.find("TX") != std::string::npos);
    CHECK(line.find("pps") != std::string::npos);
    CHECK(line.find("Mbps") != std::string::npos);
    CHECK(line.find("queue depth") != std::string::npos);

    bridge.Stop();
  }

  TEST_CASE("Shutdown keeps the statistics already queued, and stops safely under traffic") {
    MockDataChannel channel;
    LoopbackLink link(LoopbackConfig{
        .inbound_capacity = 8192, .sent_capacity = 8192, .max_frames_per_batch = 64});
    Bridge bridge(channel, link, TestConfig());
    REQUIRE(bridge.Start().has_value());

    // Stopping while traffic flows -- this is the scenario that exposes
    // teardown-order problems most easily.
    std::atomic<bool> keep_going{true};
    std::thread producer([&] {
      while (keep_going.load(std::memory_order_acquire)) {
        (void)channel.InjectFromDevice(MakeFrames(8, 512));
        for (const std::vector<std::byte>& frame : MakeFrames(8, 512)) {
          (void)link.PushInbound(frame);
        }
        std::this_thread::yield();
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bridge.Stop();  // stop while traffic flows
    keep_going.store(false, std::memory_order_release);
    producer.join();

    CHECK_FALSE(bridge.Running());
    CHECK(channel.ShutdownCalled());
  }

}  // TEST_SUITE("core.bridge")
