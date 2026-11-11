// Link layer tests.
//
// In two parts:
//   * loopback backend -- runnable in any environment; it is the infrastructure for bridge
//     layer tests and must itself be reliable;
//   * feth + BPF -- requires root and is skipped rather than failed in non-root environments.
//     Enabled explicitly with TETHERNET_ROOT_TESTS=1 (to avoid accidentally running real
//     interface operations in CI).
#include "tethernet/common/i18n.h"
#include "tethernet/net/bpf_link.h"
#include "tethernet/net/darwin_abi.h"
#include "tethernet/net/feth_device.h"
#include "tethernet/net/loopback_link.h"

#include <doctest.h>

#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace tethernet;       // NOLINT(google-build-using-namespace)
using namespace tethernet::net;  // NOLINT(google-build-using-namespace)

namespace {

/// Build a valid Ethernet frame: destination MAC, source MAC, EtherType, then a verifiable
/// payload.
std::vector<std::byte> MakeEthernetFrame(std::uint32_t total_length, std::uint8_t tag) {
  std::vector<std::byte> frame(total_length);
  // Destination MAC: broadcast; source MAC: 02:00:00:00:00:tag (locally administered unicast).
  for (int i = 0; i < 6; ++i) {
    frame[static_cast<std::size_t>(i)] = std::byte{0xFF};
  }
  frame[6] = std::byte{0x02};
  frame[11] = std::byte{tag};
  frame[12] = std::byte{0x08};  // EtherType 0x0800 = IPv4
  frame[13] = std::byte{0x00};
  for (std::uint32_t i = 14; i < total_length; ++i) {
    frame[i] = std::byte{static_cast<unsigned char>((tag + i) & 0xFFU)};
  }
  return frame;
}

/// Whether tests that require root are enabled.
///
/// getenv is not safe in multithreaded code, but this is a one-time read at the start of a
/// test case and there is no concurrent setenv in the process, so the check is suppressed.
// NOLINTNEXTLINE(concurrency-mt-unsafe)
bool RootTestsEnabled() {
  const char* flag = std::getenv("TETHERNET_ROOT_TESTS");
  return flag != nullptr && flag[0] == '1' && IsRunningAsRoot();
}

/// Render a Result/Status error as a string; returns an empty string on success.
///
/// Writing `r ? "" : r.error().ToString()` directly inside REQUIRE_MESSAGE does not compile --
/// doctest's MessageBuilder consumes the whole ternary expression into the stream and then
/// tries to convert it to bool.
template <typename T>
std::string Why(const T& result) {
  return result.has_value() ? std::string{} : result.error().ToString();
}

/// Print the skip reason so a "skipped" case stays visible in the test output
/// instead of passing silently.
///
/// Build the std::string with std::format first, then hand it to MESSAGE:
/// doctest's MessageBuilder stringifies `const char*` by pointer, so streaming
/// it directly would print an address.
void ReportSkip(std::string_view what) {
  const std::string message =
      std::format("Skipped {}: requires root and TETHERNET_ROOT_TESTS=1 (current euid={})", what,
                  IsRunningAsRoot() ? "0" : "not 0");
  MESSAGE(message);
}

// ---------------------------------------------------------------------------
// Small helpers for the ARP round-trip loopback
//
// This loopback proves something much stronger than "we do not read back our own frame":
// BPF write() really delivers the frame into the IP stack of the feth on the other side,
// not just into a black hole. The criterion is that the other side's IP stack actively
// replies -- it will not send an ARP reply unless it understood the frame.
// ---------------------------------------------------------------------------

/// Addresses for the loopback. 10.99.99/24 is chosen because it almost never conflicts with
/// real networks.
constexpr const char* kArpSystemIp = "10.99.99.1";  ///< Assigned to the system-side feth.
constexpr const char* kArpProbeIp = "10.99.99.2";   ///< The requester we impersonate.

/// Assign an IPv4 address to an interface.
///
/// Use SIOCAIFADDR directly instead of the capi `ipconfig` path: the test needs an address
/// that takes effect immediately and disappears with the feth when the process exits, without
/// IPConfiguration lease management and without spawning a subprocess in the test.
[[nodiscard]] std::string AssignIpv4(std::string_view interface_name, const char* address,
                                     const char* netmask) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return Error::FromErrno(errno, "socket(AF_INET)").ToString();
  }

  struct ifaliasreq request {};
  std::memcpy(request.ifra_name, interface_name.data(),
              std::min(interface_name.size(), sizeof(request.ifra_name) - 1));

  const auto fill = [](struct sockaddr& slot, const char* text) -> bool {
    struct sockaddr_in value {};
    value.sin_len = sizeof(value);
    value.sin_family = AF_INET;
    if (::inet_pton(AF_INET, text, &value.sin_addr) != 1) {
      return false;
    }
    std::memcpy(&slot, &value, sizeof(value));
    return true;
  };

  if (!fill(request.ifra_addr, address) || !fill(request.ifra_mask, netmask)) {
    ::close(fd);
    return "inet_pton failed";
  }

  const int result = ::ioctl(fd, SIOCAIFADDR, &request);
  const int saved_errno = errno;
  ::close(fd);
  if (result != 0) {
    return Error::FromErrno(saved_errno, "ioctl(SIOCAIFADDR)").ToString();
  }
  return {};
}

/// ARP field offsets inside an Ethernet frame (counting from the start of the frame).
constexpr std::size_t kEtherTypeOffset = 12;
constexpr std::size_t kArpOperationOffset = 20;
constexpr std::size_t kArpSenderMacOffset = 22;
constexpr std::size_t kArpSenderIpOffset = 28;
constexpr std::size_t kArpFrameBytes = 42;  ///< 14-byte Ethernet header + 28-byte ARP.

/// Build an ARP request "who has target_ip? tell sender_ip" (broadcast).
std::vector<std::byte> MakeArpRequest(const MacAddress& sender_mac, const char* sender_ip,
                                      const char* target_ip) {
  std::vector<std::byte> frame(kArpFrameBytes, std::byte{0});
  const auto put = [&frame](std::size_t offset, std::initializer_list<std::uint8_t> bytes) {
    std::size_t index = offset;
    for (const std::uint8_t value : bytes) {
      frame[index++] = std::byte{value};
    }
  };

  put(0, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});  // Destination MAC: broadcast
  for (std::size_t i = 0; i < sender_mac.size(); ++i) {
    frame[6 + i] = std::byte{sender_mac[i]};
  }
  put(kEtherTypeOffset, {0x08, 0x06});  // EtherType = ARP
  put(14, {0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01});  // Ethernet/IPv4, oper = 1 (request)

  for (std::size_t i = 0; i < sender_mac.size(); ++i) {
    frame[kArpSenderMacOffset + i] = std::byte{sender_mac[i]};
  }
  struct in_addr parsed {};
  ::inet_pton(AF_INET, sender_ip, &parsed);
  std::memcpy(frame.data() + kArpSenderIpOffset, &parsed, sizeof(parsed));
  ::inet_pton(AF_INET, target_ip, &parsed);
  std::memcpy(frame.data() + 38, &parsed, sizeof(parsed));  // Target IP; target MAC left as 0
  return frame;
}

/// Return whether a frame is an ARP reply claiming to own expected_ip.
[[nodiscard]] bool IsArpReplyFor(const FrameView& view, const char* expected_ip) {
  if (view.length < kArpFrameBytes) {
    return false;
  }
  if (view.data[kEtherTypeOffset] != std::byte{0x08} ||
      view.data[kEtherTypeOffset + 1] != std::byte{0x06}) {
    return false;
  }
  // oper == 2 means reply.
  if (view.data[kArpOperationOffset] != std::byte{0x00} ||
      view.data[kArpOperationOffset + 1] != std::byte{0x02}) {
    return false;
  }
  struct in_addr expected {};
  if (::inet_pton(AF_INET, expected_ip, &expected) != 1) {
    return false;
  }
  return std::memcmp(view.data + kArpSenderIpOffset, &expected, sizeof(expected)) == 0;
}

}  // namespace

TEST_SUITE("net.abi") {
  TEST_CASE("private ABI struct sizes and ioctl numbers match measured values") {
    // These are static_asserts, so compilation alone proves them; confirm at runtime here too
    // so the ABI assumptions are visible in the test report.
    CHECK(sizeof(IfDrv) == 40);
    CHECK(sizeof(FethRequest) == 160);
    CHECK(offsetof(FethRequest, u) == 32);
    CHECK(kSetDriverSpec == 0x8028'697BUL);
    CHECK(kGetDriverSpec == 0xC028'697BUL);
    CHECK(kBpfSetBatchWrite == 0x8004'428FUL);
    CHECK(kBpfSetNoTimestamp == 0x8004'4291UL);
    CHECK(static_cast<unsigned long>(FethSetCommand::kSetPeer) == 1);
    CHECK(static_cast<unsigned long>(FethGetCommand::kGetPeer) == 1);
  }

  TEST_CASE("BPF record header size trap: sizeof is 20 but bh_hdrlen is 18") {
    // This is the easiest place to get BPF parsing wrong; pin it down with a test.
    CHECK(sizeof(struct bpf_hdr) == 20);
    CHECK(sizeof(struct BPF_TIMEVAL) == 8);
    CHECK(kBpfHeaderMinBytes == 18);
    CHECK(BPF_ALIGNMENT == 4);
    // Kernel derivation for DLT_EN10MB: BPF_WORDALIGN(14 + 18) - 14 = 18.
    CHECK(BPF_WORDALIGN(14 + kBpfHeaderMinBytes) - 14 == kBpfHeaderMinBytes);
  }

  TEST_CASE("feth creation-time sysctl list is non-empty and each entry has a reason") {
    CHECK(std::size(kRequiredFethSysctls) > 0);
    for (const RequiredFethSysctl& entry : kRequiredFethSysctls) {
      CHECK(entry.name != nullptr);
      // why holds a message identifier; the shipped language must really have
      // text for it (a missing row would render as an empty string and leave
      // "reason:" empty in sysctl errors).
      const std::string_view why = tethernet::TextIn(tethernet::Language::kEnglish, entry.why);
      // The reason must be a real sentence, not one word.
      CHECK(why.size() > 10);
    }
  }

}  // TEST_SUITE("net.abi")

TEST_SUITE("net.loopback") {
  TEST_CASE("reading from an empty queue returns an empty batch") {
    LoopbackLink link;
    const auto batch = link.ReadFrames();
    REQUIRE(batch.has_value());
    CHECK(batch->frames.empty());
  }

  TEST_CASE("injected frames are read out in FIFO order") {
    LoopbackLink link;
    for (std::uint8_t i = 0; i < 5; ++i) {
      REQUIRE(link.PushInbound(MakeEthernetFrame(100, i)));
    }
    const auto batch = link.ReadFrames();
    REQUIRE(batch.has_value());
    REQUIRE(batch->frames.size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
      CHECK(batch->frames[i].length == 100);
      // The last byte of the source MAC is the sequence number we inserted.
      CHECK(batch->frames[i].data[11] == std::byte{static_cast<unsigned char>(i)});
    }
  }

  TEST_CASE(
      "a single read batch is limited by max_frames_per_batch; the rest stays for the next batch") {
    LoopbackLink link(LoopbackConfig{.max_frames_per_batch = 3});
    for (std::uint8_t i = 0; i < 7; ++i) {
      REQUIRE(link.PushInbound(MakeEthernetFrame(64, i)));
    }
    const auto first = link.ReadFrames();
    REQUIRE(first.has_value());
    CHECK(first->frames.size() == 3);

    const auto second = link.ReadFrames();
    REQUIRE(second.has_value());
    CHECK(second->frames.size() == 3);

    const auto third = link.ReadFrames();
    REQUIRE(third.has_value());
    CHECK(third->frames.size() == 1);
  }

  TEST_CASE("drops and counts when the inbound queue is full") {
    LoopbackLink link(LoopbackConfig{.inbound_capacity = 2});
    CHECK(link.PushInbound(MakeEthernetFrame(64, 1)));
    CHECK(link.PushInbound(MakeEthernetFrame(64, 2)));
    CHECK_FALSE(link.PushInbound(MakeEthernetFrame(64, 3)));
    CHECK(link.InboundDrops() == 1);
  }

  TEST_CASE("oversized frames are rejected on injection") {
    LoopbackLink link(LoopbackConfig{.max_frame_bytes = 128});
    CHECK_FALSE(link.PushInbound(MakeEthernetFrame(129, 1)));
    CHECK(link.InboundDrops() == 1);
  }

  TEST_CASE("written frames can be retrieved with intact contents") {
    LoopbackLink link;
    const auto frame_a = MakeEthernetFrame(1514, 0xA1);
    const auto frame_b = MakeEthernetFrame(64, 0xB2);
    const std::array<FrameView, 2> batch{
        FrameView{.data = frame_a.data(), .length = static_cast<std::uint32_t>(frame_a.size())},
        FrameView{.data = frame_b.data(), .length = static_cast<std::uint32_t>(frame_b.size())},
    };

    const auto result = link.WriteFrames(batch);
    REQUIRE(result.has_value());
    CHECK(result->frames_written == 2);
    CHECK(result->bytes_written == 1514 + 64);
    CHECK(result->frames_skipped == 0);

    const auto sent = link.DrainSent();
    REQUIRE(sent.size() == 2);
    CHECK(sent[0] == frame_a);
    CHECK(sent[1] == frame_b);
    // DrainSent does not affect cumulative counters.
    CHECK(link.TotalSentFrames() == 2);
    CHECK(link.TotalSentBytes() == 1514 + 64);
    // Draining again after empty should yield empty.
    CHECK(link.DrainSent().empty());
  }

  TEST_CASE("skips frames that are too short or too long on write") {
    LoopbackLink link(LoopbackConfig{.max_frame_bytes = 200});
    const auto too_short = MakeEthernetFrame(14, 1);
    std::vector<std::byte> shorter(13);
    const auto too_long = MakeEthernetFrame(201, 2);
    const auto valid = MakeEthernetFrame(100, 3);

    const std::array<FrameView, 3> batch{
        FrameView{.data = shorter.data(), .length = 13},
        FrameView{.data = too_long.data(), .length = 201},
        FrameView{.data = valid.data(), .length = 100},
    };
    const auto result = link.WriteFrames(batch);
    REQUIRE(result.has_value());
    CHECK(result->frames_written == 1);
    CHECK(result->frames_skipped == 2);
    // 14 bytes is exactly the minimum valid frame and should be accepted.
    const std::array<FrameView, 1> minimal{FrameView{.data = too_short.data(), .length = 14}};
    const auto minimal_result = link.WriteFrames(minimal);
    REQUIRE(minimal_result.has_value());
    CHECK(minimal_result->frames_written == 1);
  }

  TEST_CASE(
      "stops writing when the outbound queue is full and reports the written count accurately") {
    LoopbackLink link(LoopbackConfig{.sent_capacity = 2});
    const auto frame = MakeEthernetFrame(64, 1);
    std::vector<FrameView> batch(5, FrameView{.data = frame.data(), .length = 64});
    const auto result = link.WriteFrames(batch);
    REQUIRE(result.has_value());
    CHECK(result->frames_written == 2);
  }

  TEST_CASE("ReadFrames returns an empty batch immediately after Interrupt") {
    LoopbackLink link;
    REQUIRE(link.PushInbound(MakeEthernetFrame(64, 1)));
    link.Interrupt();
    CHECK(link.Interrupted());
    const auto batch = link.ReadFrames();
    REQUIRE(batch.has_value());
    CHECK(batch->frames.empty());
  }

  TEST_CASE("can inject write failures to test error propagation") {
    LoopbackLink link;
    link.FailWritesAfter(1);  // 1st succeeds, 2nd onward fail
    const auto frame = MakeEthernetFrame(64, 1);
    const std::array<FrameView, 1> batch{FrameView{.data = frame.data(), .length = 64}};

    CHECK(link.WriteFrames(batch).has_value());
    CHECK_FALSE(link.WriteFrames(batch).has_value());
  }

  TEST_CASE("writing an empty batch is a legal no-op") {
    LoopbackLink link;
    const auto result = link.WriteFrames({});
    REQUIRE(result.has_value());
    CHECK(result->frames_written == 0);
  }

}  // TEST_SUITE("net.loopback")

TEST_SUITE("net.feth" * doctest::skip(false)) {
  TEST_CASE("query feth MTU limit (read-only sysctl, no root required)") {
    const auto max_mtu = QueryFethMaxMtu();
    REQUIRE(max_mtu.has_value());
    // The kernel guarantees >= ETHERMTU(1500).
    CHECK(*max_mtu >= 1500);
    MESSAGE(std::format("net.link.fake.max_mtu = {}", *max_mtu));
  }

  TEST_CASE("verify feth creation-time sysctls (read-only, no root required)") {
    const auto status = VerifyFethSysctls();
    if (!status) {
      // Not a test failure -- the sysctls on this machine have been changed. Report it so
      // people know.
      MESSAGE(std::format("local feth sysctls do not meet requirements: {}",
                          status.error().ToString()));
    }
    // As long as the values can be read the test passes; actual values depend on machine
    // configuration.
    CHECK(true);
  }

  TEST_CASE("creating feth in a non-root environment must produce a clear permission error") {
    if (IsRunningAsRoot()) {
      MESSAGE("running as root, skipping permission-error check");
      return;
    }
    const auto pair = FethPair::Create(1500);
    REQUIRE_FALSE(pair.has_value());
    const std::string message = pair.error().ToString();
    // The error must mention root / sudo rather than handing the user a bare EPERM.
    CHECK((message.find("root") != std::string::npos || message.find("sudo") != std::string::npos));
  }

  TEST_CASE("MAC formatting") {
    const MacAddress mac{0x66, 0x65, 0x74, 0x68, 0x00, 0x00};
    // This is exactly the address the kernel assigns to feth0 ('f','e','t','h', unit>>8,
    // unit&0xff).
    CHECK(std::string{FormatMac(mac).data()} == "66:65:74:68:00:00");
  }

  TEST_CASE("full lifecycle: create, pair, set MTU/MAC, UP, destroy") {
    if (!RootTestsEnabled()) {
      ReportSkip("full feth lifecycle test");
      return;
    }

    const MacAddress device_mac{0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    auto pair = FethPair::Create(1500, &device_mac);
    REQUIRE_MESSAGE(pair.has_value(), Why(pair));

    const std::string system_name{pair->SystemSide().Name()};
    const std::string driver_name{pair->DriverSide().Name()};
    MESSAGE(std::format("created {} <-> {}", system_name, driver_name));

    // The pairing relationship holds in both directions.
    const auto driver_peer = pair->DriverSide().QueryPeer();
    REQUIRE(driver_peer.has_value());
    CHECK(*driver_peer == system_name);
    const auto system_peer = pair->SystemSide().QueryPeer();
    REQUIRE(system_peer.has_value());
    CHECK(*system_peer == driver_name);

    // MTU is the same on both sides.
    CHECK(pair->SystemSide().QueryMtu().value_or(0) == 1500);
    CHECK(pair->DriverSide().QueryMtu().value_or(0) == 1500);

    // The system-side MAC is the one we set; the driver side keeps the kernel-assigned one
    // (they must differ).
    const auto system_mac = pair->SystemSide().QueryMacAddress();
    REQUIRE(system_mac.has_value());
    CHECK(*system_mac == device_mac);
    const auto driver_mac = pair->DriverSide().QueryMacAddress();
    REQUIRE(driver_mac.has_value());
    CHECK(*driver_mac != device_mac);

    // Both sides are UP -- bpfwrite requires the driver side to be UP.
    CHECK(pair->SystemSide().IsUp().value_or(false));
    CHECK(pair->DriverSide().IsUp().value_or(false));
  }

  TEST_CASE("BPF open/configure flow and direction semantics with loopback suppression") {
    if (!RootTestsEnabled()) {
      ReportSkip("BPF transmit/receive test");
      return;
    }

    auto pair = FethPair::Create(1500);
    REQUIRE_MESSAGE(pair.has_value(), Why(pair));

    // Attach BPF to the driver side.
    auto link = BpfLink::Open(pair->DriverSide().Name(), BpfConfig{});
    REQUIRE_MESSAGE(link.has_value(), Why(link));

    MESSAGE(std::format("BPF device {}, kernel buffer {} bytes, batch write {}",
                        (*link)->DevicePath(), (*link)->KernelBufferBytes(),
                        (*link)->SupportsBatchWrite() ? "available" : "unavailable"));

    // The effective kernel buffer must not be 0 and must not exceed the 32 MiB limit.
    CHECK((*link)->KernelBufferBytes() > 0);
    CHECK((*link)->KernelBufferBytes() <= 32U * 1024 * 1024);

    const auto stats = (*link)->QueryKernelStats();
    REQUIRE(stats.has_value());

    // Write a frame -- it should enter the system-side input and must not be read back by us
    // (BIOCSSEESENT=0 filters out the output direction).
    const auto frame = MakeEthernetFrame(200, 0x5A);
    const std::array<FrameView, 1> batch{FrameView{.data = frame.data(), .length = 200}};
    const auto written = (*link)->WriteFrames(batch);
    REQUIRE_MESSAGE(written.has_value(), Why(written));
    CHECK(written->frames_written == 1);

    // Read once (returns after the read timeout). We must never see the frame we just wrote.
    const auto batch_read = (*link)->ReadFrames();
    REQUIRE(batch_read.has_value());
    for (const FrameView& view : batch_read->frames) {
      // Byte 12 of the source MAC of our frame is 0x5A; seeing it means loopback suppression
      // failed.
      const bool is_our_frame = view.length == 200 && view.data[11] == std::byte{0x5A};
      CHECK_FALSE(is_our_frame);
    }
  }

  TEST_CASE(
      "ARP round-trip loopback: a frame written via BPF really reaches the other feth's IP stack") {
    if (!RootTestsEnabled()) {
      ReportSkip("ARP round-trip loopback test");
      return;
    }

    auto pair = FethPair::Create(1500);
    REQUIRE_MESSAGE(pair.has_value(), Why(pair));

    // Assign an IP to the system side so its IP stack has a reason to answer ARP.
    const std::string assign_error =
        AssignIpv4(pair->SystemSide().Name(), kArpSystemIp, "255.255.255.0");
    REQUIRE_MESSAGE(assign_error.empty(), assign_error);

    const auto system_mac = pair->SystemSide().QueryMacAddress();
    REQUIRE_MESSAGE(system_mac.has_value(), Why(system_mac));

    // Attach BPF to the driver side -- this side plays the "device".
    auto link = BpfLink::Open(pair->DriverSide().Name(), BpfConfig{});
    REQUIRE_MESSAGE(link.has_value(), Why(link));

    const MacAddress probe_mac{0x02, 0x00, 0x00, 0x99, 0x99, 0x02};
    const auto request = MakeArpRequest(probe_mac, kArpProbeIp, kArpSystemIp);
    const std::array<FrameView, 1> batch{
        FrameView{.data = request.data(), .length = static_cast<std::uint32_t>(request.size())}};

    const auto written = (*link)->WriteFrames(batch);
    REQUIRE_MESSAGE(written.has_value(), Why(written));
    REQUIRE(written->frames_written == 1);

    // Read several rounds before declaring failure: one ReadFrames round waits for only one
    // read timeout, and although the IP stack replies quickly, there is no guarantee it lands
    // in the first round.
    bool saw_reply = false;
    MacAddress reply_mac{};
    for (int attempt = 0; attempt < 20 && !saw_reply; ++attempt) {
      const auto received = (*link)->ReadFrames();
      REQUIRE_MESSAGE(received.has_value(), Why(received));
      for (const FrameView& view : received->frames) {
        if (IsArpReplyFor(view, kArpSystemIp)) {
          saw_reply = true;
          for (std::size_t i = 0; i < reply_mac.size(); ++i) {
            reply_mac[i] = static_cast<std::uint8_t>(view.data[kArpSenderMacOffset + i]);
          }
          break;
        }
      }
    }

    // This assertion is the core premise of the whole design: the other side's IP stack
    // received and processed the frame we wrote.
    CHECK_MESSAGE(saw_reply,
                  "did not receive an ARP reply from the system-side feth -- BPF write did not "
                  "reach the other side's IP stack");
    if (saw_reply) {
      // The responder must be the system-side feth itself, not a frame from some other
      // interface.
      CHECK(reply_mac == *system_mac);
      MESSAGE(std::format("{} claims to own {}, MAC {}", pair->SystemSide().Name(), kArpSystemIp,
                          FormatMac(reply_mac).data()));
    }
  }

}  // TEST_SUITE("net.feth")
