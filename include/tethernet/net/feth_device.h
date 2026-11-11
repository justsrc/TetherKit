// Lifecycle management for a feth (if_fake) virtual interface pair.
//
// Topology and data flow (verified against the xnu source feth_output_common()):
//
//     Host IP stack
//         |  IP / routes / DHCP
//         v
//     +--------+ peer pairing  +--------+      BPF
//     | feth0  | <-----------> | feth1  | <----------> Tethernet
//     |(system)|               |(driver)|
//     +--------+               +--------+
//
//   * Frames the host sends from feth0 arrive on feth1 as the **input**
//     direction, where BPF captures them.
//   * A frame we **write** to feth1's BPF takes feth1's output path and enters
//     feth0's input, where the host IP stack receives it. It does **not**
//     loop back to feth1.
//
// So BPF only needs to attach to feth1; one descriptor both sends and
// receives.
//
// Permissions: every operation in this file requires root (SIOCIFCREATE /
// SIOCSDRVSPEC / SIOCSIFLLADDR / SIOCSIFFLAGS / SIOCSIFMTU all carry a
// proc_suser check in the kernel).
#pragma once

#include "tethernet/common/error.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace tethernet::net {

/// A 6-byte MAC address.
using MacAddress = std::array<std::uint8_t, 6>;

/// Max length of a feth interface name (IFNAMSIZ = 16, terminator included).
inline constexpr std::size_t kInterfaceNameCapacity = 16;

/// The MTU cap feth supports, decided by sysctl net.link.fake.max_mtu
/// (2048 on this machine).
///
/// Note this sysctl is also a **creation-time snapshot**: feth_max_mtu is
/// computed at clone_create time, and changing the sysctl later has no effect
/// on existing interfaces.
[[nodiscard]] Result<std::uint32_t> QueryFethMaxMtu();

/// Verifies that the sysctls snapshotted at feth creation time all hold the
/// values we require.
///
/// **Must be called before creating the feth.** If any of these switches
/// (hwcsum / fcs / tso_support / lro / trailer_length / separate_frame_header)
/// is snapshotted nonzero at creation, the frames we read from BPF are not
/// clean Ethernet frames -- and changing the sysctl after creation is too late.
[[nodiscard]] Status VerifyFethSysctls();

/// Registration callback for interface creation / destruction.
///
/// Why it exists:
///   When the process is killed with SIGKILL, C++ destructors never run and
///   the feth stays in the kernel; SIGKILL defeats any signal handler. The
///   only reliable backstop is **persisting the names of created interfaces
///   and cleaning them up on the next start**. RAII cannot save a forcibly
///   killed process; this hook exists for exactly that.
///
/// Constraint: called synchronously on the same thread as the create / destroy
/// operation. The implementation should only do short work like appending or
/// deleting a line in a file, and must not log (the destroy path can run
/// inside a destructor, noexcept).
///
/// @param name    Interface name.
/// @param created true = just created, false = just destroyed.
using InterfaceRegistry = void (*)(std::string_view name, bool created) noexcept;

/// Installs / uninstalls the registration callback. Pass nullptr to uninstall.
/// Process-wide, thread-safe.
void SetInterfaceRegistry(InterfaceRegistry registry) noexcept;

/// Destroys a feth interface given by name. Used to clean up orphans left
/// behind after a process was forcibly killed.
///
/// Unlike FethInterface's destructor, this does not need to hold the object
/// first -- an orphan is precisely an interface no object is managing. The
/// name must look like `feth<digits>`; validating that is the caller's job.
[[nodiscard]] Status DestroyInterfaceByName(std::string_view name);

/// One feth interface. RAII: the destructor destroys the interface in the
/// kernel.
class FethInterface {
 public:
  FethInterface() = default;

  FethInterface(const FethInterface&) = delete;
  FethInterface& operator=(const FethInterface&) = delete;
  FethInterface(FethInterface&& other) noexcept;
  FethInterface& operator=(FethInterface&& other) noexcept;

  /// Destroys the interface. The kernel's feth_clone_destroy automatically
  /// unbinds the peer first.
  ~FethInterface();

  /// Creates a new feth interface.
  ///
  /// @param requested_name an empty string lets the kernel pick the smallest
  ///                       free unit number automatically (recommended);
  ///                       "feth7" pins the number and fails with EEXIST if
  ///                       it already exists.
  ///
  /// In wildcard mode the kernel writes the full name back; obtain it via
  /// Name().
  [[nodiscard]] static Result<FethInterface> Create(std::string_view requested_name = {});

  [[nodiscard]] bool Valid() const noexcept { return !name_.empty(); }

  [[nodiscard]] std::string_view Name() const noexcept { return name_; }

  /// Pairs with another feth interface.
  ///
  /// The kernel enforces five hard checks; any failure returns EINVAL:
  ///   1. ifd_len >= sizeof(if_fake_request) (160);
  ///   2. if_fake_request's reserved field must be all zeros;
  ///   3. the peer must also be a feth (ifnet_name() == "feth" and
  ///      type == IFT_ETHER);
  ///   4. neither side may already have a peer;
  ///   5. the caller must be root.
  [[nodiscard]] Status PeerWith(const FethInterface& peer);

  /// Unbinds the peer (writes an empty peer name).
  [[nodiscard]] Status Unpeer();

  /// Queries the current peer name; empty string when unpaired.
  [[nodiscard]] Result<std::string> QueryPeer() const;

  /// Sets the MTU. The cap comes from QueryFethMaxMtu(); exceeding it returns
  /// EINVAL.
  [[nodiscard]] Status SetMtu(std::uint32_t mtu);

  [[nodiscard]] Result<std::uint32_t> QueryMtu() const;

  /// Sets the MAC address. **Must be called before setting IFF_UP.**
  ///
  /// The kernel's default MAC is 'f','e','t','h', unit>>8, unit&0xff
  /// (so feth0 is 66:65:74:68:00:00).
  ///
  /// In the RNDIS scenario the **system-side** interface (feth0) should take
  /// the address the device reports via OID_802_3_PERMANENT_ADDRESS -- in
  /// RNDIS semantics the device is this interface, and the peer's ARP table
  /// and DHCP leases are all keyed on this MAC.
  ///
  /// **The driver-side interface (feth1) must keep the kernel-assigned MAC,
  /// different from feth0's**; otherwise both sides get the same IPv6
  /// link-local address and DAD reports an address conflict.
  [[nodiscard]] Status SetMacAddress(const MacAddress& mac);

  [[nodiscard]] Result<MacAddress> QueryMacAddress() const;

  /// Sets / clears IFF_UP. The kernel automatically adds IFF_RUNNING when
  /// setting UP.
  [[nodiscard]] Status SetUp(bool up);

  [[nodiscard]] Result<bool> IsUp() const;

  /// Gives up ownership of the interface; the destructor no longer destroys
  /// it (used to keep the live state around while debugging).
  void Release() noexcept { name_.clear(); }

 private:
  explicit FethInterface(std::string name) : name_(std::move(name)) {}

  /// Destroys the interface in the kernel and clears name_. Failure is only
  /// logged -- a destructor path can neither throw nor return errors.
  void Destroy() noexcept;

  std::string name_;
};

/// A pair of feth interfaces, fully configured and ready for use.
class FethPair {
 public:
  /// Creates and configures a pair of feth interfaces.
  ///
  /// The operation order is deliberate (a wrong order fails or leaves bad
  /// state):
  ///   1. Verify the creation-time sysctls -- before creating anything;
  ///   2. Create both interfaces;
  ///   3. Pair -- pair before UP so the link is up from the start
  ///      (SIOCGIFMEDIA's IFM_ACTIVE is immediately true);
  ///   4. Set the MTU;
  ///   5. Set the system-side MAC -- must happen before UP;
  ///   6. Set both sides UP -- bpfwrite hard-requires feth1 to be UP
  ///      (otherwise ENETDOWN), and feth0 must be UP for the host IP stack to
  ///      process received frames.
  ///
  /// @param mtu           The MTU for both sides.
  /// @param system_mac    The MAC to set on the system side; pass std::nullopt
  ///                      to keep the kernel-assigned address.
  [[nodiscard]] static Result<FethPair> Create(std::uint32_t mtu,
                                               const MacAddress* system_mac = nullptr);

  /// The system-side interface: the host assigns IPs, runs DHCP, and adds
  /// routes on this one.
  [[nodiscard]] const FethInterface& SystemSide() const noexcept { return system_side_; }

  /// The driver-side interface: Tethernet attaches BPF to it to receive and
  /// send raw frames.
  [[nodiscard]] const FethInterface& DriverSide() const noexcept { return driver_side_; }

  [[nodiscard]] FethInterface& SystemSide() noexcept { return system_side_; }

  [[nodiscard]] FethInterface& DriverSide() noexcept { return driver_side_; }

 private:
  FethPair(FethInterface system_side, FethInterface driver_side)
      : system_side_(std::move(system_side)), driver_side_(std::move(driver_side)) {}

  FethInterface system_side_;
  FethInterface driver_side_;
};

/// Formats a MAC as "aa:bb:cc:dd:ee:ff".
[[nodiscard]] std::array<char, 18> FormatMac(const MacAddress& mac) noexcept;

/// Whether the current process runs as root. Used to produce a clear error at
/// startup instead of a stream of EPERM.
[[nodiscard]] bool IsRunningAsRoot() noexcept;

}  // namespace tethernet::net
