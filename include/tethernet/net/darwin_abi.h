// Darwin private ABI declarations.
//
// Nothing declared here is in the public macOS SDK, but the driver needs all of
// it. Every definition is copied from Apple's open-source xnu sources and was
// verified on this machine with static_asserts plus measured ioctl numbers.
//
// Why private ABI is acceptable here (risk assessment):
//   * `net/if_fake_var.h` is byte-identical (same md5) across every release tag
//     from xnu-7195 (macOS 11) to xnu-12377 (macOS 26); not a single byte of
//     change in 15 years. `ifconfig fethN peer fethM` uses exactly this ABI and
//     Apple's own ifconfig depends on it, so in practice it is frozen.
//   * `struct ifdrv` likewise, and its size participates directly in the _IOW
//     macro's ioctl-number computation -- get the size wrong and you get a
//     wrong ioctl number, where the kernel returns ENOTTY and does nothing
//     dangerous. The static_assert below pins the size at 40.
//   * The BPF private ioctls (BIOCSBATCHWRITE / BIOCSNOTSTAMP) are **optional
//     optimizations**, always feature-probed at runtime; on failure we fall
//     back to the generic path, so functional correctness is unaffected.
//     macOS 26 moved them from net/bpf.h to net/bpf_private.h (the SDK does
//     not provide that header).
//
// In short: the functional ABI (feth peer) has 15 years of history plus a
// static_assert backstop; the optimization ABI (BPF batch write) is probed.
// Neither can silently take a wrong path.
#pragma once

#include "tethernet/common/i18n.h"

#include <net/bpf.h>
#include <net/if.h>
#include <sys/ioccom.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <cstddef>
#include <cstdint>

namespace tethernet::net {

// =============================================================================
// struct ifdrv -- parameter struct for SIOCSDRVSPEC / SIOCGDRVSPEC
//
// Source: xnu/bsd/net/if.h (wrapped in #pragma pack(4), which does not change
// the layout on LP64/arm64). The SDK's net/if.h lacks the struct itself and
// only has the ioctl macros that reference it.
// =============================================================================

/// Parameter struct for driver-private ioctls.
struct IfDrv {
  char ifd_name[IFNAMSIZ];  ///< Target interface name.
  unsigned long ifd_cmd;    ///< Driver-private command code (IF_FAKE_S_CMD_* for feth).
  std::size_t ifd_len;      ///< Length of the data ifd_data points to.
  void* ifd_data;           ///< Command argument.
};

// The size must be exactly 40 bytes: it participates in the ioctl-number
// computation of _IOW('i', 123, struct ifdrv); a wrong size yields a nonexistent
// ioctl number. Measured SIOCSDRVSPEC = 0x8028697b.
static_assert(sizeof(IfDrv) == 40, "struct ifdrv must be 40 bytes or the ioctl number is wrong");
static_assert(offsetof(IfDrv, ifd_cmd) == 16);
static_assert(offsetof(IfDrv, ifd_len) == 24);
static_assert(offsetof(IfDrv, ifd_data) == 32);

/// Set driver-private parameters. Equivalent to the SDK's SIOCSDRVSPEC but
/// computed with our own IfDrv.
inline constexpr unsigned long kSetDriverSpec = _IOW('i', 123, IfDrv);
/// Read driver-private parameters.
inline constexpr unsigned long kGetDriverSpec = _IOWR('i', 123, IfDrv);

static_assert(kSetDriverSpec == 0x8028'697BUL, "SIOCSDRVSPEC number != the measured value");
static_assert(kGetDriverSpec == 0xC028'697BUL, "SIOCGDRVSPEC number != the measured value");

// =============================================================================
// if_fake (feth) private ABI
//
// Source: xnu/bsd/net/if_fake_var.h
// =============================================================================

/// The driver name of feth (the if_clone name; it goes into ifr_name as a
/// wildcard prefix when creating).
inline constexpr const char* kFethCloneName = "feth";

/// Command codes for SIOCSDRVSPEC.
enum class FethSetCommand : unsigned long {
  kNone = 0,
  kSetPeer = 1,  ///< Pair / unbind the peer.
  kSetMedia = 2,
  kSetDequeueStall = 3,
};

/// Command codes for SIOCGDRVSPEC.
enum class FethGetCommand : unsigned long {
  kNone = 0,
  kGetPeer = 1,
};

/// Upper bound of if_fake_media's media-type list.
inline constexpr std::size_t kFethMediaListMax = 27;

/// Parameter struct for SIOCSDRVSPEC / SIOCGDRVSPEC on feth.
///
/// The layout must match xnu's struct if_fake_request byte for byte:
///   uint64_t iffr_reserved[4];            // 32 bytes, **all zeros required**
///   union {                               // 128 bytes
///     char     iffru_buf[128];
///     struct   if_fake_media iffru_media; // also 128 bytes
///     char     iffru_peer_name[IFNAMSIZ];
///     uint32_t iffru_dequeue_stall;
///   };
struct FethRequest {
  /// Reserved field. The kernel requires it to be **all zeros**; a nonzero
  /// value fails outright with EINVAL.
  std::uint64_t reserved[4];

  union {
    char buffer[128];
    struct {
      std::int32_t current;
      std::uint32_t count;
      std::uint32_t media_reserved[3];
      std::int32_t list[kFethMediaListMax];
    } media;

    /// Peer interface name. Writing an empty string (first byte '\0') means
    /// unbind.
    char peer_name[IFNAMSIZ];
    std::uint32_t dequeue_stall;
  } u;
};

static_assert(sizeof(FethRequest) == 160, "struct if_fake_request must be 160 bytes");
static_assert(offsetof(FethRequest, u) == 32);
static_assert(sizeof(FethRequest::u) == 128);
static_assert(alignof(FethRequest) == 8);

// =============================================================================
// BPF private constants and ioctls
//
// Source: the PRIVATE section of xnu/bsd/net/bpf.h (moved to
// bsd/net/bpf_private.h starting with macOS 26). The SDK provides none of
// them; everything is feature-probed at runtime.
// =============================================================================

/// Enables batched writes: one write() sends multiple frames.
///
/// The kernel implementation first appeared in macOS 14 / xnu-10063; macOS 13
/// and earlier have none of it. So it must be probed: if the ioctl returns
/// ENOTTY / EINVAL, fall back to per-frame write.
///
/// Preconditions: BIOCSHDRCMPLT must already be set to 1, and BIOCSETTC must
/// not have been set.
inline constexpr unsigned long kBpfSetBatchWrite = _IOW('B', 143, int);
static_assert(kBpfSetBatchWrite == 0x8004'428FUL, "BIOCSBATCHWRITE number != the measured value");

/// Disables capture timestamps so catchpacket skips its microtime() call.
///
/// We do not need timestamps (frames are forwarded straight to USB), so turning
/// them off saves one clock read per frame. Feature-probed the same way;
/// failure is harmless.
inline constexpr unsigned long kBpfSetNoTimestamp = _IOW('B', 145, int);
static_assert(kBpfSetNoTimestamp == 0x8004'4291UL, "BIOCSNOTSTAMP number != the measured value");

/// Sets the capture direction explicitly (a more precise BIOCSSEESENT).
inline constexpr unsigned long kBpfSetDirection = _IOW('B', 138, unsigned int);

/// Capture-direction values.
inline constexpr unsigned int kBpfDirectionNone = 0;
inline constexpr unsigned int kBpfDirectionIn = 0x1;
inline constexpr unsigned int kBpfDirectionOut = 0x2;
inline constexpr unsigned int kBpfDirectionInOut = kBpfDirectionIn | kBpfDirectionOut;

/// The over-length leeway bpfwrite allows.
///
/// The kernel check is `(len - hlen) > (ifp->if_mtu + BPF_WRITE_LEEWAY)` ->
/// EMSGSIZE. With BIOCSHDRCMPLT=1 (our usage) hlen == 0, so **the full frame
/// length (including the 14-byte Ethernet header) must be <= interface MTU +
/// 18**. At MTU=1500 the cap is 1518, which exactly fits standard 1514 frames
/// and 1518 frames carrying a VLAN tag.
inline constexpr std::uint32_t kBpfWriteLeeway = 18;

/// The bounds for BIOCSBLEN.
///
/// Since macOS 13 (xnu-8792) the upper bound is BPF_BUFSIZE_CAP, exposed on
/// this machine as 32 MiB via the read-only sysctl debug.bpf_bufsize_cap.
/// **Exceeding it raises no error**: the kernel silently clamps to the cap and
/// writes the actually-effective value back through _IOWR -- so you must use
/// the written-back value; see bpf_link.cc.
inline constexpr std::uint32_t kBpfMinBufferBytes = 32;

/// The minimum length of a BPF record header.
///
/// Critical trap: `sizeof(struct bpf_hdr)` on LP64 is **20** (18 bytes of
/// content padded by the compiler to 20), whereas the kernel's actual
/// `bh_hdrlen` for DLT_EN10MB is **18**
/// (SIZEOF_BPF_HDR=18, bif_hdrlen = BPF_WORDALIGN(14+18) - 14 = 18).
/// **When walking records you must use each record's own bh_hdrlen; using
/// sizeof desyncs the walk immediately.**
inline constexpr std::uint32_t kBpfHeaderMinBytes = 18;

static_assert(sizeof(struct bpf_hdr) == 20,
              "LP64 sizeof(struct bpf_hdr) should be 20 (18 bytes content + 2 padding)");
static_assert(sizeof(struct BPF_TIMEVAL) == 8,
              "LP64 BPF timestamps are timeval32 (8 bytes), not 64-bit timeval");

// =============================================================================
// sysctls snapshotted at feth creation time
//
// These switches are read from sysctl into the interface's private flags at
// the instant of feth_clone_create(); **changing the sysctl after creation has
// no effect**. So they must be validated before creating the feth.
// =============================================================================

/// A feth sysctl whose value must be a specific number.
///
/// `why` stores a message identifier rather than a ready-made string: this
/// table is constexpr, and the "why" has to be rendered in the user's current
/// language, which can only happen at the moment of failure by consulting the
/// message table then.
struct RequiredFethSysctl {
  const char* name;
  std::int32_t required_value;
  Msg why;
};

/// The sysctls that must be verified before creating a feth.
///
/// If any of these values is wrong, the frames we read from BPF are not
/// "clean Ethernet frames":
inline constexpr RequiredFethSysctl kRequiredFethSysctls[] = {
    {"net.link.fake.hwcsum", 0, Msg::kNetSysctlWhyHwcsum},
    {"net.link.fake.fcs", 0, Msg::kNetSysctlWhyFcs},
    {"net.link.fake.tso_support", 0, Msg::kNetSysctlWhyTso},
    {"net.link.fake.lro", 0, Msg::kNetSysctlWhyLro},
    {"net.link.fake.trailer_length", 0, Msg::kNetSysctlWhyTrailer},
    {"net.link.fake.separate_frame_header", 0, Msg::kNetSysctlWhySeparateHeader},
};

}  // namespace tethernet::net
