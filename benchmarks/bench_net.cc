// Benchmarks for the feth / BPF link layer -- requires root.
//
// This group fills in the items long left open at the end of docs/BENCHMARKS.md: the real
// cost of BPF writes, the speedup of BIOCSBATCHWRITE over per-frame writes, the per-frame
// cost of batched reads, and the round-trip latency of the feth data path. Before this,
// these numbers all came from research estimates (that ~700 ns write cost came from
// `write(/dev/null)`, not a measurement on a real BPF descriptor).
//
// Fixture layout:
//   feth0 (system side, assigned 10.99.99.1) <-> feth1 (driver side)
//   * driver_perframe / driver_batched: two BPF descriptors both attached to feth1, with
//     BIOCSBATCHWRITE disabled and enabled respectively, for a per-frame vs batched write A/B;
//   * system_link: attached to feth0, used to inject traffic toward feth1 to measure read cost.
//
// Write cost inherently includes the overhead of "the peer IP stack receiving and dropping
// the frame" -- that is exactly the cost the real RX path pays per frame, so measuring it
// this way is correct and should not be deliberately avoided.
#include "bench_net.h"

#include "tethernet/net/bpf_link.h"
#include "tethernet/net/feth_device.h"

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
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <vector>

#include "harness.h"

namespace tethernet::bench {
namespace {

constexpr std::uint32_t kFullFrameBytes = 1514;
constexpr std::uint32_t kSmallFrameBytes = 64;

/// Address used by the fixture. 10.99.99/24 is unlikely to conflict with real networks.
constexpr const char* kSystemIp = "10.99.99.1";
constexpr const char* kProbeIp = "10.99.99.2";

/// Field offsets within an ARP frame (counted from the start of the frame).
constexpr std::size_t kEtherTypeOffset = 12;
constexpr std::size_t kArpOperationOffset = 20;
constexpr std::size_t kArpSenderIpOffset = 28;
constexpr std::size_t kArpFrameBytes = 42;

std::vector<std::byte> MakeFrame(std::uint32_t length) {
  std::vector<std::byte> frame(length, std::byte{0});
  // Broadcast destination MAC + locally administered source MAC, EtherType set to 0x0800.
  // The contents themselves do not matter; what matters is that it is a structurally valid
  // Ethernet frame that the peer will actually receive and then drop.
  for (int i = 0; i < 6; ++i) {
    frame[static_cast<std::size_t>(i)] = std::byte{0xFF};
  }
  frame[6] = std::byte{0x02};
  frame[kEtherTypeOffset] = std::byte{0x08};
  for (std::uint32_t i = 14; i < length; ++i) {
    frame[i] = std::byte{static_cast<unsigned char>(i & 0xFFU)};
  }
  return frame;
}

/// Builds an ARP request for "who has kSystemIp", used to measure round-trip latency.
std::vector<std::byte> MakeArpRequest() {
  std::vector<std::byte> frame(kArpFrameBytes, std::byte{0});
  const auto put = [&frame](std::size_t offset, std::initializer_list<std::uint8_t> bytes) {
    std::size_t index = offset;
    for (const std::uint8_t value : bytes) {
      frame[index++] = std::byte{value};
    }
  };
  put(0, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  put(6, {0x02, 0x00, 0x00, 0x99, 0x99, 0x02});
  put(kEtherTypeOffset, {0x08, 0x06});
  put(14, {0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01});
  put(22, {0x02, 0x00, 0x00, 0x99, 0x99, 0x02});
  struct in_addr parsed {};
  ::inet_pton(AF_INET, kProbeIp, &parsed);
  std::memcpy(frame.data() + kArpSenderIpOffset, &parsed, sizeof(parsed));
  ::inet_pton(AF_INET, kSystemIp, &parsed);
  std::memcpy(frame.data() + 38, &parsed, sizeof(parsed));
  return frame;
}

[[nodiscard]] bool IsArpReply(const FrameView& view) {
  if (view.length < kArpFrameBytes) {
    return false;
  }
  if (view.data[kEtherTypeOffset] != std::byte{0x08} ||
      view.data[kEtherTypeOffset + 1] != std::byte{0x06}) {
    return false;
  }
  if (view.data[kArpOperationOffset] != std::byte{0x00} ||
      view.data[kArpOperationOffset + 1] != std::byte{0x02}) {
    return false;
  }
  struct in_addr expected {};
  if (::inet_pton(AF_INET, kSystemIp, &expected) != 1) {
    return false;
  }
  return std::memcmp(view.data + kArpSenderIpOffset, &expected, sizeof(expected)) == 0;
}

/// Assigns an IPv4 address to the interface (SIOCAIFADDR). Returns an empty string on success.
[[nodiscard]] std::string AssignIpv4(std::string_view interface_name, const char* address) {
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
  if (!fill(request.ifra_addr, address) || !fill(request.ifra_mask, "255.255.255.0")) {
    ::close(fd);
    return "inet_pton failed";
  }
  const int result = ::ioctl(fd, SIOCAIFADDR, &request);
  const int saved_errno = errno;
  ::close(fd);
  return result == 0 ? std::string{} : Error::FromErrno(saved_errno, "ioctl(SIOCAIFADDR)").ToString();
}

/// Benchmark fixture. Lifetime is owned by the static object in RegisterNetBenchmarks.
struct Fixture {
  std::unique_ptr<net::FethPair> pair;
  std::unique_ptr<net::BpfLink> driver_perframe;  ///< feth1, batch write disabled
  std::unique_ptr<net::BpfLink> driver_batched;   ///< feth1, batch write enabled
  std::unique_ptr<net::BpfLink> system_link;      ///< feth0, used to inject traffic
  bool batch_write_available = false;
};

Fixture* g_fixture = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// Write benchmark: passes `frames_per_write` frames as one batch to WriteFrames.
///
/// The measurement unit is frames so numbers for per-frame and batched writes can be
/// compared directly.
std::uint64_t BenchWrite(net::BpfLink& link, std::uint64_t frame_iterations,
                         std::uint32_t frame_bytes, std::uint32_t frames_per_write) {
  const std::vector<std::byte> frame = MakeFrame(frame_bytes);
  std::vector<FrameView> batch(frames_per_write);
  for (FrameView& view : batch) {
    view = FrameView{.data = frame.data(), .length = frame_bytes};
  }

  std::uint64_t written = 0;
  while (written < frame_iterations) {
    const auto result = link.WriteFrames(batch);
    if (!result) {
      break;  // peer queue saturated etc.: honestly count only the frames actually written.
    }
    written += result->frames_written;
    DoNotOptimize(result->bytes_written);
  }
  return written;
}

/// Read benchmark: injects `burst` frames from feth0, then reads them back on feth1.
///
/// This measures the combined cost of write + read, not pure read -- the harness can only
/// time the entire closure. To obtain the pure read cost, subtract the write benchmark value
/// for the same batch size.
std::uint64_t BenchWriteThenRead(std::uint64_t frame_iterations, std::uint32_t frame_bytes,
                                 std::uint32_t burst) {
  Fixture& fixture = *g_fixture;
  const std::vector<std::byte> frame = MakeFrame(frame_bytes);
  std::vector<FrameView> batch(burst);
  for (FrameView& view : batch) {
    view = FrameView{.data = frame.data(), .length = frame_bytes};
  }

  std::uint64_t completed = 0;
  while (completed < frame_iterations) {
    const auto written = fixture.system_link->WriteFrames(batch);
    if (!written || written->frames_written == 0) {
      break;
    }
    std::uint32_t drained = 0;
    // Give up after a few spins: feth drops frames under queue pressure, so not getting all
    // of them back is normal.
    for (int attempt = 0; attempt < 4 && drained < written->frames_written; ++attempt) {
      const auto received = fixture.driver_batched->ReadFrames();
      if (!received) {
        break;
      }
      drained += static_cast<std::uint32_t>(received->frames.size());
      DoNotOptimize(received->kernel_drops);
    }
    completed += written->frames_written;
  }
  return completed;
}

/// feth data-path round-trip latency: writes an ARP request from the driver side and waits
/// for the reply from the system-side IP stack.
///
/// This is the only true latency metric -- the previous items are amortized costs in the
/// throughput sense.
std::uint64_t BenchArpRoundTrip(std::uint64_t iterations) {
  Fixture& fixture = *g_fixture;
  const std::vector<std::byte> request = MakeArpRequest();
  const std::array<FrameView, 1> batch{
      FrameView{.data = request.data(), .length = kArpFrameBytes}};

  std::uint64_t completed = 0;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    if (!fixture.driver_perframe->WriteFrames(batch)) {
      break;
    }
    bool answered = false;
    for (int attempt = 0; attempt < 8 && !answered; ++attempt) {
      const auto received = fixture.driver_perframe->ReadFrames();
      if (!received) {
        break;
      }
      for (const FrameView& view : received->frames) {
        if (IsArpReply(view)) {
          answered = true;
          break;
        }
      }
    }
    if (!answered) {
      break;  // the system side stops replying once the neighbor table is cached; stop here
              // instead of reporting a false result.
    }
    ++completed;
  }
  return completed;
}

}  // namespace

bool RegisterNetBenchmarks(Runner& runner, std::string& skip_reason) {
  if (!net::IsRunningAsRoot()) {
    skip_reason = "requires root (to create an feth pair and open /dev/bpf*)";
    return false;
  }

  static Fixture fixture;

  auto pair = net::FethPair::Create(1500);
  if (!pair) {
    skip_reason = std::format("failed to create feth pair: {}", pair.error().ToString());
    return false;
  }
  fixture.pair = std::make_unique<net::FethPair>(std::move(*pair));

  const std::string assign_error = AssignIpv4(fixture.pair->SystemSide().Name(), kSystemIp);
  if (!assign_error.empty()) {
    skip_reason = std::format("failed to assign address to system side: {}", assign_error);
    return false;
  }

  const auto open = [&](std::string_view interface_name,
                        bool batch_write) -> std::unique_ptr<net::BpfLink> {
    net::BpfConfig config;
    config.try_batch_write = batch_write;
    auto link = net::BpfLink::Open(interface_name, config);
    return link ? std::move(*link) : nullptr;
  };

  fixture.driver_perframe = open(fixture.pair->DriverSide().Name(), false);
  fixture.driver_batched = open(fixture.pair->DriverSide().Name(), true);
  fixture.system_link = open(fixture.pair->SystemSide().Name(), true);
  if (fixture.driver_perframe == nullptr || fixture.driver_batched == nullptr ||
      fixture.system_link == nullptr) {
    skip_reason = "failed to open BPF descriptor";
    return false;
  }
  fixture.batch_write_available = fixture.driver_batched->SupportsBatchWrite();
  g_fixture = &fixture;

  // Frames per round: writes are system-call-level costs (hundreds of nanoseconds to
  // microseconds), so tens of thousands of frames are stable enough; more would only make
  // each round take longer.
  constexpr std::uint64_t kWriteOps = 20000;
  constexpr std::uint64_t kReadOps = 20000;

  runner.Add("BPF write (per-frame write)", "1514 bytes",
             Config{.ops_per_round = kWriteOps, .bytes_per_op = kFullFrameBytes},
             [](std::uint64_t n) {
               return BenchWrite(*g_fixture->driver_perframe, n, kFullFrameBytes, 1);
             });
  runner.Add("BPF write (per-frame write)", "64 bytes",
             Config{.ops_per_round = kWriteOps, .bytes_per_op = kSmallFrameBytes},
             [](std::uint64_t n) {
               return BenchWrite(*g_fixture->driver_perframe, n, kSmallFrameBytes, 1);
             });

  if (fixture.batch_write_available) {
    for (const std::uint32_t per_write : {8U, 32U, 64U, 128U}) {
      runner.Add("BPF write (BIOCSBATCHWRITE)",
                 std::format("1514 bytes x {} frames per write", per_write),
                 Config{.ops_per_round = kWriteOps, .bytes_per_op = kFullFrameBytes},
                 [per_write](std::uint64_t n) {
                   return BenchWrite(*g_fixture->driver_batched, n, kFullFrameBytes, per_write);
                 });
    }
    runner.Add("BPF write (BIOCSBATCHWRITE)", "64 bytes x 64 frames per write",
               Config{.ops_per_round = kWriteOps, .bytes_per_op = kSmallFrameBytes},
               [](std::uint64_t n) {
                 return BenchWrite(*g_fixture->driver_batched, n, kSmallFrameBytes, 64);
               });
  }

  runner.Add("feth round trip (write+read total)", "1514 bytes x 64 frames per batch",
             Config{.ops_per_round = kReadOps, .bytes_per_op = kFullFrameBytes},
             [](std::uint64_t n) { return BenchWriteThenRead(n, kFullFrameBytes, 64); });

  // Round-trip latency runs only a few hundred times: each iteration must wait for a real
  // reply from the peer IP stack, which is under a millisecond but far slower than the
  // previous operations, so running more adds no information.
  runner.Add("feth round-trip latency", "ARP request -> system-side IP stack reply",
             Config{.ops_per_round = 200}, [](std::uint64_t n) { return BenchArpRoundTrip(n); });

  return true;
}

void ShutdownNetBenchmarks() noexcept {
  if (g_fixture == nullptr) {
    return;
  }
  // Order matters: close BPF descriptors first, then destroy the feth interface pair. The
  // other way around would leave BpfLink's destructor with an interface that no longer exists.
  g_fixture->system_link.reset();
  g_fixture->driver_batched.reset();
  g_fixture->driver_perframe.reset();
  g_fixture->pair.reset();
  g_fixture = nullptr;
}

}  // namespace tethernet::bench
