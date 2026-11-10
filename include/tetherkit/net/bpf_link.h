// BPF link backend: reads and writes raw Ethernet frames directly on the
// driver-side feth interface.
//
// Every ioctl order and parameter choice in this file has a source-level
// rationale; read the comments in full before changing anything.
//
// Configuration sequence (the order is hard-constrained, per xnu
// bsd/net/bpf.c):
//   1. open("/dev/bpf%d")  -- macOS has **no** /dev/bpf clone node; iterate the
//                             numbered devices.
//   2. BIOCSBLEN           -- **must come before BIOCSETIF** (bpf.c has
//                             `if (d->bd_bif != 0) return EINVAL`).
//   3. BIOCSHDRCMPLT = 1   -- **must come before BIOCSBATCHWRITE**; it also
//                             decides whether frames pass through verbatim
//                             (see below).
//   4. BIOCSETIF           -- bind to the interface; only after this can the
//                             DLT be queried.
//   5. BIOCGDLT            -- verify it is DLT_EN10MB.
//   6. BIOCIMMEDIATE = 1   -- decides the latency profile of reads (below).
//   7. BIOCSSEESENT = 0    -- look only at the input direction (below).
//   8. BIOCSBATCHWRITE = 1 -- optional optimization, feature-probed.
//   9. BIOCSNOTSTAMP = 1   -- optional optimization, feature-probed.
//
// The three most critical parameter choices:
//
//   * **BIOCSHDRCMPLT = 1 is mandatory.** With hdrcmplt=0, bpfwrite peels the
//     first 14 bytes of the user data into a sockaddr and lets ether_frameout
//     rebuild the frame header -- so the source MAC gets rewritten by the
//     driver and the forwarded frame is no longer the original frame from the
//     device. Only hdrcmplt=1 takes the DLIL_OUTPUT_FLAGS_RAW path and passes
//     the frame through verbatim.
//
//   * **BIOCSSEESENT = 0 exactly filters out the frames we ourselves wrote.**
//     When attached to the driver-side interface: frames the host sends from
//     the system side show up here as the **input** direction; frames we write
//     show up here as **output**. SEESENT=0 -> bd_direction = BPF_D_IN, which
//     keeps only input -- exactly the frames we want to forward to USB.
//     Without it you read back your own writes and form a loop.
//
//   * **BIOCIMMEDIATE = 1 plus a dedicated thread blocking in read() is the
//     optimal model; do not use kqueue.** With immediate, every arriving packet
//     raises bpf_wakeup, and when read() wakes it delivers all packets
//     accumulated in the meantime as one batch -- low latency at low speeds,
//     automatic aggregation into large batches at high speeds (behavior like
//     Linux NAPI), one syscall per batch. The kqueue approach has exactly the
//     same readiness criterion but pays an extra kevent() for it. Without
//     immediate, the only way to avoid "return only when the buffer fills" is
//     BIOCSRTIMEOUT -- whose real resolution is pinned by the 10 ms clock tick
//     (the kernel stores tvtohz(tv)-1; hz=100 on this machine), an
//     unacceptable latency.
//
// Two read-side details that must be remembered:
//
//   * **read()'s buffer length must be exactly bd_bufsize**, otherwise bpfread
//     exits at the top with
//     `if (uio_resid(uio) != d->bd_bufsize) return EINVAL`. And BIOCSBLEN does
//     **not** report an error when over the limit; it silently clamps to the
//     cap and writes the effective value back into the argument, so you must
//     use the written-back value.
//
//   * **Walk records with bh_hdrlen, not sizeof(struct bpf_hdr).**
//     On LP64 sizeof is 20 (18 bytes of content, padded), while the kernel
//     writes bh_hdrlen = 18 for DLT_EN10MB. Using 20 desyncs the walk
//     immediately.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tetherkit/common/error.h"
#include "tetherkit/net/link_backend.h"

namespace tetherkit::net {

/// Configuration parameters for the BPF backend.
struct BpfConfig {
  /// Kernel packet-capture buffer size.
  ///
  /// The cap is sysctl debug.bpf_bufsize_cap (32 MiB on macOS 13+); exceeding
  /// it is silently truncated. The kernel allocates **two** buffers of this
  /// size per descriptor (a store + free double buffer), so actual wired
  /// memory usage is twice this value.
  ///
  /// Derivation of the 4 MiB default: at full 1 Gbps, 1514-byte frames are
  /// about 82 k pps; one copyout window, worst case 10 ms, needs roughly
  /// 1.2 MB of buffering; 4 MiB leaves a 3x margin. Anything bigger merely
  /// wastes wired memory, because BPF has only the two buffers and enlarging
  /// one does not improve the drop window.
  std::uint32_t kernel_buffer_bytes = 4U * 1024 * 1024;

  /// Max single-frame length (Ethernet header included). Should equal
  /// interface MTU + Ethernet header and must not exceed MTU + 18.
  std::uint32_t max_frame_bytes = 1518;

  /// Upper bound on how many frames one ReadFrames decodes.
  ///
  /// This is the capacity of the FrameView array. A 4 MiB buffer fits at most
  /// about 4Mi/32 = 131 k minimum-size frames (64-byte frame + 18-byte header
  /// + alignment), but real traffic is never all minimum-size; 8192 covers the
  /// vast majority of batches, and anything beyond carries over to the parse
  /// pass after the next read.
  std::size_t max_frames_per_batch = 8192;

  /// Whether to try enabling batched writes (BIOCSBATCHWRITE, macOS 14+).
  bool try_batch_write = true;

  /// Whether to try disabling capture timestamps (BIOCSNOTSTAMP), saving one
  /// microtime() per frame.
  bool try_disable_timestamp = true;

  /// Read timeout. Determines how quickly Interrupt() can stop the loop when
  /// there is no traffic at all.
  ///
  /// Note the kernel's actual resolution is 10 ms (it stores tvtohz(tv) - 1
  /// ticks; kern.clockrate hz=100 on this machine), and passing {0,0} becomes
  /// **a permanent block**. 200 ms is prompt enough for shutdown without
  /// needless wakeups.
  std::uint32_t read_timeout_millis = 200;
};

/// A BPF descriptor attached to the given interface.
class BpfLink final : public LinkBackend {
 public:
  /// Opens and configures a BPF descriptor bound to `interface_name`.
  ///
  /// Requires root: /dev/bpf* is 0600 root:wheel, and macOS has **no**
  /// access_bpf group like FreeBSD's to lean on.
  [[nodiscard]] static Result<std::unique_ptr<BpfLink>> Open(std::string_view interface_name,
                                                            const BpfConfig& config);

  // Copy and move are already deleted in the base LinkBackend; restated here
  // to satisfy static checks.
  BpfLink(const BpfLink&) = delete;
  BpfLink& operator=(const BpfLink&) = delete;
  BpfLink(BpfLink&&) = delete;
  BpfLink& operator=(BpfLink&&) = delete;
  ~BpfLink() override;

  [[nodiscard]] Result<ReadBatch> ReadFrames() override;
  [[nodiscard]] Result<WriteResult> WriteFrames(FrameBatch frames) override;

  [[nodiscard]] std::uint32_t MaxFrameBytes() const noexcept override { return max_frame_bytes_; }

  [[nodiscard]] bool SupportsBatchWrite() const noexcept override { return batch_write_enabled_; }

  void Interrupt() noexcept override;

  /// The buffer size actually in effect in the kernel (possibly clamped to the
  /// cap).
  [[nodiscard]] std::uint32_t KernelBufferBytes() const noexcept { return kernel_buffer_bytes_; }

  /// The opened device-node path, for logging.
  [[nodiscard]] std::string_view DevicePath() const noexcept { return device_path_; }

  /// Kernel-side statistics: cumulative received / dropped packet counts.
  struct KernelStats {
    std::uint64_t received = 0;
    std::uint64_t dropped = 0;
  };

  [[nodiscard]] Result<KernelStats> QueryKernelStats() const;

 private:
  BpfLink() = default;

  /// Per-frame write(); the fallback path when batched writes are unavailable.
  [[nodiscard]] Result<WriteResult> WriteFramesIndividually(FrameBatch frames);

  /// Multiple frames per write() (BIOCSBATCHWRITE).
  [[nodiscard]] Result<WriteResult> WriteFramesBatched(FrameBatch frames);

  int fd_ = -1;
  std::string device_path_;
  std::string interface_name_;

  std::uint32_t kernel_buffer_bytes_ = 0;
  std::uint32_t max_frame_bytes_ = 0;
  bool batch_write_enabled_ = false;
  bool timestamp_disabled_ = false;

  /// Read buffer. Length must be exactly kernel_buffer_bytes_ (a hard
  /// requirement of bpfread).
  std::vector<std::byte> read_buffer_;
  /// Frame views decoded for this batch, pointing into read_buffer_.
  std::vector<FrameView> read_frames_;
  /// The last successfully read cumulative kernel drop count (bs_drop).
  ///
  /// When BIOCGSTATS fails sporadically, **keep the previous value** rather
  /// than reporting 0: bs_drop is a cumulative counter and consumers take
  /// "current - previous" differences from it; reporting 0 makes that
  /// difference underflow through unsigned arithmetic and print an
  /// astronomical "kernel drops" figure. Only accessed by the read thread, so
  /// no atomics needed.
  std::uint64_t last_kernel_drops_ = 0;
  /// Assembly buffer for batched writes.
  std::vector<std::byte> write_buffer_;

  /// Shutdown flag. Once Interrupt() sets it, ReadFrames returns an empty
  /// batch within one read-timeout period.
  ///
  /// Choice of wakeup mechanism: BPF's read() blocks, and breaking it has
  /// three possible routes --
  ///   (a) send a signal so BPF_SLEEP (with PCATCH) returns EINTR;
  ///   (b) close the fd (a use-after-close race, unacceptable);
  ///   (c) set a moderate BIOCSRTIMEOUT so read() wakes periodically and
  ///       checks this flag.
  /// (c) was chosen: no signal handler needs to be installed in the process,
  /// and there is no race. BIOCIMMEDIATE=1 keeps reads returning immediately
  /// when packets exist; the read timeout only affects how soon shutdown is
  /// noticed when there is no traffic at all.
  std::atomic<bool> interrupted_{false};
};

}  // namespace tetherkit::net
