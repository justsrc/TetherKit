// Unit tests for the RNDIS state machine.
//
// The state machine is the most intricate part of the protocol implementation,
// and many of its paths are nearly impossible to reproduce on a real device
// (the device pushing an out-of-band message, a keep-alive failure, a replay
// demand after a reset). MockControlChannel plays the device side here, so
// all of those paths get covered.
#include "tethernet/rndis/state_machine.h"

#include <doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "language_guard.h"
#include "mock_control_channel.h"

// Same reasoning as the identically named helper in test_net_link.cc: writing
// `r ? "" : r.error().ToString()` directly inside REQUIRE_MESSAGE does not
// compile -- doctest's MessageBuilder swallows the whole ternary expression
// into the stream and then tries to convert it to bool.
namespace {
template <typename T>
std::string Why(const T& result) {
  return result.has_value() ? std::string{} : result.error().ToString();
}
}  // namespace

using namespace tethernet;         // NOLINT(google-build-using-namespace)
using namespace tethernet::rndis;  // NOLINT(google-build-using-namespace)
using tethernet::testing::MakeDeviceKeepAlive;
using tethernet::testing::MakeIndicateStatus;
using tethernet::testing::MakeInitializeComplete;
using tethernet::testing::MakeKeepAliveComplete;
using tethernet::testing::MakeQueryComplete;
using tethernet::testing::MakeQueryCompleteMac;
using tethernet::testing::MakeQueryCompleteUint32;
using tethernet::testing::MakeResetComplete;
using tethernet::testing::MakeSetComplete;
using tethernet::testing::MakeWellBehavedDevice;
using tethernet::testing::MockControlChannel;
using tethernet::testing::OidOf;
using tethernet::testing::RecordingObserver;
using tethernet::testing::RequestIdOf;

namespace {

constexpr MacAddress kDeviceMac{0x02, 0x1A, 0x11, 0x22, 0x33, 0x44};

/// Test configuration: squeezes the poll interval down to 0 so the tests run fast.
[[nodiscard]] StateMachineConfig FastConfig() {
  StateMachineConfig config;
  config.response_poll_interval_millis = 0;
  config.response_poll_attempts = 4;
  config.keepalive_interval_millis = 0;  // make the keep-alive expire at once, for testability
  return config;
}

}  // namespace

TEST_SUITE("rndis.state_machine") {
  TEST_CASE("State names are all present") {
    CHECK_FALSE(StateName(State::kUninitialized).empty());
    CHECK_FALSE(StateName(State::kInitializing).empty());
    CHECK_FALSE(StateName(State::kInitialized).empty());
    CHECK_FALSE(StateName(State::kDataInitialized).empty());
    CHECK_FALSE(StateName(State::kHalting).empty());
  }

  TEST_CASE("Full startup sequence: reaches kDataInitialized and reports the negotiated result") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());

    CHECK(machine.CurrentState() == State::kUninitialized);
    const auto status = machine.Start();
    REQUIRE_MESSAGE(status.has_value(), Why(status));

    CHECK(machine.CurrentState() == State::kDataInitialized);

    // The transition path must be uninitialized -> initializing -> initialized -> data ready.
    REQUIRE(observer.transitions.size() == 3);
    CHECK(observer.transitions[0].to == State::kInitializing);
    CHECK(observer.transitions[1].to == State::kInitialized);
    CHECK(observer.transitions[2].to == State::kDataInitialized);

    CHECK(observer.negotiated_count == 1);
    CHECK(observer.parameters_snapshot.mtu == 1500);
    CHECK(observer.parameters_snapshot.device_max_transfer_size == 2048);
    CHECK(observer.parameters_snapshot.max_packets_per_message == 1);
    CHECK(observer.parameters_snapshot.tx_alignment_bytes == 1);

    // Got the device MAC -- this is the address the host-side feth will use.
    CHECK(machine.Info().has_permanent_address);
    CHECK(machine.Info().permanent_address == kDeviceMac);
    CHECK(machine.Info().LinkSpeedMbps() == doctest::Approx(480.0));

    // The link reported "connected" once.
    REQUIRE(observer.link_events.size() == 1);
    CHECK(observer.link_events[0]);
  }

  TEST_CASE("Startup message order: INITIALIZE first, the SET packet filter last") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());

    REQUIRE_FALSE(channel.SentMessages().empty());
    // The first message must be INITIALIZE_MSG.
    CHECK(channel.SentMessageType(0) == ToRaw(MessageType::kInitialize));
    // The last message must be the SET (packet filter) -- that is what puts the
    // device into data-initialized.
    const std::size_t last = channel.SentMessages().size() - 1;
    CHECK(channel.SentMessageType(last) == ToRaw(MessageType::kSet));

    // Exactly one INITIALIZE was sent, no duplicate.
    CHECK(channel.CountSent(MessageType::kInitialize) == 1);

    // The packet filter value set must be the one we asked for
    // (DIRECTED|BROADCAST|ALL_MULTICAST|PROMISCUOUS).
    std::uint32_t filter = 0;
    REQUIRE(channel.FindSetUint32(Oid::kGenCurrentPacketFilter, filter));
    CHECK(filter == kDefaultPacketFilter);
    CHECK(filter == 0x0000'002DU);
  }

  TEST_CASE("Variable-length OIDs must use InformationBufferLength 0, fixed-length ones non-zero") {
    MockControlChannel channel;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> query_oid_and_length;
    channel.SetRequestHandler([&](std::span<const std::byte> request, MockControlChannel& mock) {
      const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
      if (type == ToRaw(MessageType::kQuery)) {
        query_oid_and_length.emplace_back(
            OidOf(request), LoadLe32(request.data() + kQuerySetInfoBufferLengthOffset));
      }
      MakeWellBehavedDevice(kDeviceMac)(request, mock);
    });
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());

    bool saw_fixed = false;
    bool saw_variable = false;
    for (const auto& [oid, length] : query_oid_and_length) {
      if (IsVariableLengthOid(static_cast<Oid>(oid))) {
        // A non-zero length on a variable-length OID is rejected by the
        // ActiveSync implementation.
        CHECK_MESSAGE(length == 0,
                      "variable-length OID " << OidName(oid) << " should have length 0");
        saw_variable = true;
      } else {
        CHECK_MESSAGE(length > 0,
                      "fixed-length OID " << OidName(oid) << " should have length greater than 0");
        saw_fixed = true;
      }
    }
    CHECK(saw_fixed);
    CHECK(saw_variable);  // the startup sequence queried OID_GEN_VENDOR_DESCRIPTION
  }

  TEST_CASE("An optional OID answering NOT_SUPPORTED does not block startup") {
    // OID_GEN_PHYSICAL_MEDIUM is optional; a device that does not support it
    // is perfectly normal.
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());
    CHECK(machine.CurrentState() == State::kDataInitialized);
    CHECK(machine.Info().physical_medium == PhysicalMedium::kUnspecified);
    CHECK(observer.fatal_errors.empty());
  }

  TEST_CASE("Not getting the permanent MAC is a fatal error, and a HALT is sent") {
    MockControlChannel channel;
    channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
      const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
      const std::uint32_t request_id = RequestIdOf(request);
      if (type == ToRaw(MessageType::kInitialize)) {
        mock.EnqueueResponse(MakeInitializeComplete(request_id));
        return;
      }
      if (type == ToRaw(MessageType::kQuery)) {
        // Every QUERY is answered with "not supported" -- including the
        // permanent MAC.
        mock.EnqueueResponse(MakeQueryComplete(request_id, {}, ToRaw(StatusCode::kNotSupported)));
      }
    });
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());

    const auto status = machine.Start();
    REQUIRE_FALSE(status.has_value());
    // After the failure it must go back to uninitialized, not get stuck in an
    // intermediate state.
    CHECK(machine.CurrentState() == State::kUninitialized);
    // And a HALT must have been sent to clean up the device-side state.
    CHECK(channel.CountSent(MessageType::kHalt) == 1);
  }

  TEST_CASE("When the device rejects initialization no QUERY is sent, it fails right away") {
    MockControlChannel channel;
    channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
      const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
      if (type == ToRaw(MessageType::kInitialize)) {
        mock.EnqueueResponse(
            MakeInitializeComplete(RequestIdOf(request), 2048, 1, 0, ToRaw(StatusCode::kFailure)));
      }
    });
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());

    REQUIRE_FALSE(machine.Start().has_value());
    CHECK(machine.CurrentState() == State::kUninitialized);
    CHECK(channel.CountSent(MessageType::kQuery) == 0);
  }

  TEST_CASE("A non-Ethernet medium is rejected") {
    MockControlChannel channel;
    channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
      if (LoadLe32(request.data() + kMessageTypeOffset) == ToRaw(MessageType::kInitialize)) {
        auto message = MakeInitializeComplete(RequestIdOf(request));
        StoreLe32(message.data() + kInitializeCmpltMediumOffset,
                  static_cast<std::uint32_t>(Medium::kWirelessLan));
        mock.EnqueueResponse(std::move(message));
      }
    });
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE_FALSE(machine.Start().has_value());
  }

  TEST_CASE("An INDICATE_STATUS inserted mid-request does not disturb request matching") {
    // This is a very common path on real hardware but nearly impossible to
    // reproduce on purpose.
    MockControlChannel channel;
    channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
      const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
      if (type == ToRaw(MessageType::kInitialize)) {
        // Queue a media-disconnect notification first, then the real response.
        mock.EnqueueResponse(MakeIndicateStatus(StatusCode::kMediaDisconnect));
        mock.EnqueueResponse(MakeInitializeComplete(RequestIdOf(request)));
        return;
      }
      MakeWellBehavedDevice(kDeviceMac)(request, mock);
    });
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());

    const auto status = machine.Start();
    REQUIRE_MESSAGE(status.has_value(), Why(status));
    CHECK(machine.CurrentState() == State::kDataInitialized);

    // The out-of-band message must be consumed, not left to crowd the queue
    // ahead of the later responses.
    CHECK(channel.PendingResponseCount() == 0);

    // About link events: the link state is **edge triggered**. The initial
    // state the implementation assumes at startup is "not connected", so one
    // MEDIA_DISCONNECT agrees with what we believe -> correctly produces no
    // event (deduplication). The subsequent QUERY of
    // OID_GEN_MEDIA_CONNECT_STATUS returns 0 (connected), and that is the
    // newer, authoritative state, so Start() reports "connected" once when it
    // finishes.
    REQUIRE(observer.link_events.size() == 1);
    CHECK(observer.link_events[0]);
  }

  TEST_CASE("A MEDIA_DISCONNECT after the link is up is what produces the disconnect event") {
    // This is the case where MEDIA_DISCONNECT really should raise an event:
    // there is an up -> down edge.
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    auto config = FastConfig();
    config.keepalive_interval_millis = 60'000;  // keep the keep-alive out of the way
    StateMachine machine(channel, observer, config);
    REQUIRE(machine.Start().has_value());

    // After startup the link is up.
    REQUIRE(observer.link_events.size() == 1);
    REQUIRE(observer.link_events[0]);

    // The device now pushes a MEDIA_DISCONNECT; Poll must pick it up and report
    // the disconnect.
    channel.EnqueueResponse(MakeIndicateStatus(StatusCode::kMediaDisconnect));
    REQUIRE(machine.Poll().has_value());

    REQUIRE(observer.link_events.size() == 2);
    CHECK_FALSE(observer.link_events[1]);

    // Push the same notification again -- edge triggered, no new event.
    channel.EnqueueResponse(MakeIndicateStatus(StatusCode::kMediaDisconnect));
    REQUIRE(machine.Poll().has_value());
    CHECK(observer.link_events.size() == 2);

    // Recovering the connection must produce a down -> up edge.
    channel.EnqueueResponse(MakeIndicateStatus(StatusCode::kMediaConnect));
    REQUIRE(machine.Poll().has_value());
    REQUIRE(observer.link_events.size() == 3);
    CHECK(observer.link_events[2]);
  }

  TEST_CASE("When the device sends a KEEPALIVE_MSG the host must answer with a KEEPALIVE_CMPLT") {
    // Not answering can make the device conclude that the host is dead and drop
    // the connection.
    MockControlChannel channel;
    constexpr std::uint32_t kDeviceKeepAliveId = 0xABCD'1234;
    channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
      const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
      if (type == ToRaw(MessageType::kInitialize)) {
        // Insert a device-initiated keep-alive before the response.
        mock.EnqueueResponse(MakeDeviceKeepAlive(kDeviceKeepAliveId));
        mock.EnqueueResponse(MakeInitializeComplete(RequestIdOf(request)));
        return;
      }
      MakeWellBehavedDevice(kDeviceMac)(request, mock);
    });
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());

    // The host must have sent a KEEPALIVE_CMPLT whose RequestId is the one the
    // device gave.
    bool found = false;
    for (const std::vector<std::byte>& message : channel.SentMessages()) {
      if (message.size() >= kKeepAliveCmpltBytes &&
          LoadLe32(message.data() + kMessageTypeOffset) == ToRaw(MessageType::kKeepAliveComplete)) {
        CHECK(LoadLe32(message.data() + kKeepAliveCmpltRequestIdOffset) == kDeviceKeepAliveId);
        CHECK(LoadLe32(message.data() + kKeepAliveCmpltStatusOffset) ==
              ToRaw(StatusCode::kSuccess));
        found = true;
      }
    }
    CHECK_MESSAGE(found, "the host did not reply to the device-initiated KEEPALIVE");
  }

  TEST_CASE("messages aimed at connection-oriented devices are explicitly rejected") {
    // The error message wording is asserted below; pin the language.
    const tethernet::testing::ScopedLanguage guard{tethernet::Language::kEnglish};
    MockControlChannel channel;
    channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
      if (LoadLe32(request.data() + kMessageTypeOffset) == ToRaw(MessageType::kInitialize)) {
        // Queue a CONDIS message.
        std::vector<std::byte> condis(kMessageHeaderBytes);
        StoreLe32(condis.data() + kMessageTypeOffset, 0x0000'8001U);  // MP_CREATE_VC
        StoreLe32(condis.data() + kMessageLengthOffset, kMessageHeaderBytes);
        mock.EnqueueResponse(std::move(condis));
        mock.EnqueueResponse(MakeInitializeComplete(RequestIdOf(request)));
      }
    });
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());

    const auto status = machine.Start();
    REQUIRE_FALSE(status.has_value());
    // The message must explicitly name connection-oriented devices rather
    // than the vague "unknown message".
    CHECK(status.error().ToString().find("connection-oriented") != std::string::npos);
  }

  TEST_CASE("keepalive success reports no error and sends none while the device is active") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    auto config = FastConfig();
    // Keep-alive period set very large: Poll must not send a keep-alive.
    config.keepalive_interval_millis = 60'000;
    StateMachine machine(channel, observer, config);
    REQUIRE(machine.Start().has_value());

    channel.ClearSentMessages();
    REQUIRE(machine.Poll().has_value());
    CHECK(channel.CountSent(MessageType::kKeepAlive) == 0);
  }

  TEST_CASE("keepalive expiry sends KEEPALIVE and clears the failure count after success") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    auto config = FastConfig();
    config.keepalive_interval_millis = 0;  // expires immediately
    StateMachine machine(channel, observer, config);
    REQUIRE(machine.Start().has_value());

    channel.ClearSentMessages();
    REQUIRE(machine.Poll().has_value());
    CHECK(channel.CountSent(MessageType::kKeepAlive) == 1);
    CHECK(observer.fatal_errors.empty());
  }

  TEST_CASE("reaching the keepalive failure threshold reports a fatal error") {
    // The fatal error wording is asserted below; pin the language.
    const tethernet::testing::ScopedLanguage guard{tethernet::Language::kEnglish};
    MockControlChannel channel;
    // The device answers only INITIALIZE / QUERY / SET and never KEEPALIVE ->
    // every poll times out.
    channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
      const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
      if (type == ToRaw(MessageType::kKeepAlive)) {
        return;  // deliberately no reply
      }
      MakeWellBehavedDevice(kDeviceMac)(request, mock);
    });
    RecordingObserver observer;
    auto config = FastConfig();
    config.keepalive_interval_millis = 0;
    config.keepalive_failure_threshold = 3;
    StateMachine machine(channel, observer, config);
    REQUIRE(machine.Start().has_value());

    // The first two failures must not be fatal.
    CHECK(machine.Poll().has_value());
    CHECK(observer.fatal_errors.empty());
    CHECK(machine.Poll().has_value());
    CHECK(observer.fatal_errors.empty());
    // The third one reaches the threshold.
    const auto third = machine.Poll();
    CHECK_FALSE(third.has_value());
    REQUIRE(observer.fatal_errors.size() == 1);
    CHECK(observer.fatal_errors[0].find("keep-alive") != std::string::npos);
  }

  TEST_CASE("Soft reset: when AddressingReset is non-zero the packet filter must be replayed") {
    // This is the easiest one to miss: without the replay, data never flows
    // again after the reset.
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());

    channel.ClearSentMessages();
    REQUIRE(machine.Reset().has_value());

    // The reset was reported, and it flags the addressing information as lost.
    REQUIRE(observer.reset_events.size() == 1);
    CHECK(observer.reset_events[0]);

    // A RESET_MSG must have been sent.
    CHECK(channel.CountSent(MessageType::kReset) == 1);
    // And the packet filter must have been replayed.
    std::uint32_t filter = 0;
    REQUIRE_MESSAGE(channel.FindSetUint32(Oid::kGenCurrentPacketFilter, filter),
                    "the packet filter setting was not replayed after the reset");
    CHECK(filter == kDefaultPacketFilter);
  }

  TEST_CASE("Soft reset: when AddressingReset is 0 no replay is needed") {
    MockControlChannel channel;
    channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
      const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
      if (type == ToRaw(MessageType::kReset)) {
        mock.EnqueueResponse(MakeResetComplete(/*addressing_reset=*/false));
        return;
      }
      MakeWellBehavedDevice(kDeviceMac)(request, mock);
    });
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());

    channel.ClearSentMessages();
    REQUIRE(machine.Reset().has_value());
    REQUIRE(observer.reset_events.size() == 1);
    CHECK_FALSE(observer.reset_events[0]);

    std::uint32_t filter = 0;
    CHECK_FALSE(channel.FindSetUint32(Oid::kGenCurrentPacketFilter, filter));
  }

  TEST_CASE("Shutdown: clear the packet filter first, then send HALT") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());

    channel.ClearSentMessages();
    machine.Stop();
    CHECK(machine.CurrentState() == State::kUninitialized);

    // Must SET filter = 0 first (so the device falls back to initialized and
    // stops sending data), then send HALT.
    std::uint32_t filter = 0xFFFF'FFFFU;
    REQUIRE(channel.FindSetUint32(Oid::kGenCurrentPacketFilter, filter));
    CHECK(filter == 0);
    CHECK(channel.CountSent(MessageType::kHalt) == 1);

    const std::size_t last = channel.SentMessages().size() - 1;
    CHECK(channel.SentMessageType(last) == ToRaw(MessageType::kHalt));
  }

  TEST_CASE("Shutdown is idempotent") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());

    machine.Stop();
    const std::size_t after_first = channel.SentMessages().size();
    machine.Stop();
    CHECK(channel.SentMessages().size() == after_first);
  }

  TEST_CASE("Start may only be called in the uninitialized state") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());
    CHECK_FALSE(machine.Start().has_value());
  }

  TEST_CASE("A device without an interrupt endpoint degrades to polling, startup still completes") {
    // The Linux host driver ignores the interrupt endpoint entirely and relies
    // on polling the control endpoint.
    MockControlChannel channel;
    channel.SetHasInterruptEndpoint(false);
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());

    const auto status = machine.Start();
    REQUIRE_MESSAGE(status.has_value(), Why(status));
    CHECK(machine.CurrentState() == State::kDataInitialized);
  }

  TEST_CASE("A control channel send failure fails startup without getting stuck") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    channel.FailSendOnCall(1);  // the very first INITIALIZE cannot be sent
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());

    REQUIRE_FALSE(machine.Start().has_value());
    CHECK(machine.CurrentState() == State::kUninitialized);
  }

  TEST_CASE(
      "Device quirk: Qualcomm aggregation patch (MaxPacketsPerMessage=3, alignment factor 2)") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac, /*max_transfer_size=*/3 * 1600,
                                                    /*max_packets=*/3, /*alignment_factor=*/2));
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());

    CHECK(observer.parameters_snapshot.max_packets_per_message == 3);
    CHECK(observer.parameters_snapshot.tx_alignment_bytes == 4);  // 1 << 2
  }

  TEST_CASE("Device quirk: an out-of-range alignment factor is clamped to 7") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac, 2048, 1, /*alignment_factor=*/31));
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());
    CHECK(observer.parameters_snapshot.tx_alignment_bytes == 128);
  }

  TEST_CASE("The MTU drops when the device's max frame size is below the negotiated MTU") {
    MockControlChannel channel;
    channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
      const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
      const std::uint32_t request_id = RequestIdOf(request);
      if (type == ToRaw(MessageType::kQuery) &&
          OidOf(request) == ToRaw(Oid::kGenMaximumFrameSize)) {
        mock.EnqueueResponse(MakeQueryCompleteUint32(request_id, 1400));
        return;
      }
      MakeWellBehavedDevice(kDeviceMac)(request, mock);
    });
    RecordingObserver observer;
    StateMachine machine(channel, observer, FastConfig());
    REQUIRE(machine.Start().has_value());
    CHECK(machine.Parameters().mtu == 1400);
  }

  TEST_CASE("MillisUntilNextPoll returns a positive value within the keep-alive period") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    auto config = FastConfig();
    config.keepalive_interval_millis = 5'000;
    StateMachine machine(channel, observer, config);
    REQUIRE(machine.Start().has_value());
    CHECK(machine.MillisUntilNextPoll() > 0);
    CHECK(machine.MillisUntilNextPoll() <= 5'000);
  }

}  // TEST_SUITE("rndis.state_machine")

TEST_SUITE("rndis.state_machine_timeout") {
  // Regression case for a class of hang defects that the mock cannot catch.
  //
  // Defect: Poll() called WaitForNotification(0), intending "do not wait, just
  //          probe", but on darwin libusb passes the timeout to IOKit as both
  //          noDataTimeout and completionTimeout, and **0 means wait
  //          forever** -- the control loop would wedge permanently in
  //          libusb_wait_for_event, not even answering SIGTERM.
  // The mock cannot catch this because it returns immediately for any timeout.
  // So this asserts the timeout value that is handed down instead.

  TEST_CASE("Never pass 0 to WaitForNotification (0 means an infinite wait on darwin)") {
    MockControlChannel channel;
    channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
    RecordingObserver observer;
    // Deliberately use a configuration whose response_poll_interval_millis is
    // 0 -- that is exactly the path on which Transact()'s
    // min(control_timeout, interval*4) computed 0.
    auto config = FastConfig();
    config.response_poll_interval_millis = 0;
    StateMachine machine(channel, observer, config);
    REQUIRE(machine.Start().has_value());
    REQUIRE(machine.Poll().has_value());

    REQUIRE_FALSE(channel.NotificationTimeouts().empty());
    for (const std::uint32_t timeout : channel.NotificationTimeouts()) {
      CHECK_MESSAGE(timeout > 0,
                    "WaitForNotification received 0 -- on darwin that means an infinite wait and "
                    "would wedge the control thread");
    }
  }

  TEST_CASE("kProbeOnlyTimeoutMillis itself must be non-zero") {
    CHECK(kProbeOnlyTimeoutMillis > 0);
  }

}  // TEST_SUITE("rndis.state_machine_timeout")
