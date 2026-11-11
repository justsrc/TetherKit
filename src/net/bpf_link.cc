#include "tethernet/net/bpf_link.h"

#include "tethernet/common/byte_order.h"
#include "tethernet/common/i18n.h"
#include "tethernet/common/logging.h"
#include "tethernet/net/darwin_abi.h"
#include "tethernet/net/feth_device.h"

#include <fcntl.h>
#include <net/bpf.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>

namespace tethernet::net {
namespace {

/// The upper limit for scanning /dev/bpf%d.
///
/// The kernel recreates a node on demand when the current last node is opened
/// (`if (minor(dev) == nbpfilter - 1) bpf_make_dev_t(...)` inside bpfopen);
/// the ceiling is controlled by the sysctl debug.bpf_maxdevices (256 on this
/// machine). This constant takes 256 to match.
constexpr int kMaxBpfDeviceIndex = 256;

/// The per-write() byte ceiling for batched writes.
///
/// The kernel's absolute ceiling is BPF_WRITE_MAX = 16 MiB, but that large is
/// unnecessary: 256 KiB already fits over a hundred full frames and the fixed
/// per-syscall overhead has long since been amortized, while a bigger assembly
/// buffer only hurts L2 cache hits.
constexpr std::size_t kMaxBatchWriteBytes = std::size_t{256} * 1024;

/// One ioctl; on failure wraps errno and the call name into an Error.
[[nodiscard]] Status CallIoctl(int fd, unsigned long request, void* argument,
                               std::string_view what) {
  if (::ioctl(fd, request, argument) < 0) {
    return std::unexpected(Error::FromErrno(0, std::string{what}));
  }
  return Ok();
}

/// Attempts an **optional** ioctl: a failure is not an error, just a false
/// return.
[[nodiscard]] bool TryOptionalIoctl(int fd, unsigned long request, void* argument,
                                    std::string_view what) {
  if (::ioctl(fd, request, argument) < 0) {
    // ENOTTY / EINVAL means this macOS version does not know the private ioctl,
    // an expected case.
    TETHERNET_DEBUG_TR(Msg::kNetOptionalIoctlUnavailable, what, errno);
    return false;
  }
  return true;
}

}  // namespace

BpfLink::~BpfLink() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void BpfLink::Interrupt() noexcept {
  interrupted_.store(true, std::memory_order_release);
}

Result<std::unique_ptr<BpfLink>> BpfLink::Open(std::string_view interface_name,
                                              const BpfConfig& config) {
  if (interface_name.size() >= kInterfaceNameCapacity) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetInterfaceNameTooLong, interface_name)));
  }
  if (config.max_frame_bytes < kMinEthernetFrameBytes) {
    return std::unexpected(Error::Generic(
        Tr(Msg::kNetFrameLimitBelowMinimum, config.max_frame_bytes, kMinEthernetFrameBytes)));
  }

  auto link = std::unique_ptr<BpfLink>(new BpfLink());
  link->interface_name_ = interface_name;
  link->max_frame_bytes_ = config.max_frame_bytes;

  // ---------------------------------------------------------------------------
  // 1. Open /dev/bpf%d
  //
  // macOS has **no** /dev/bpf clone node (measured: ls /dev/bpf -> No such
  // file), so the numbered nodes must be tried one by one: EBUSY means someone
  // else holds that minor, try the next; ENOENT means the kernel-made node
  // ceiling has been reached.
  // ---------------------------------------------------------------------------
  int last_errno = 0;
  for (int index = 0; index < kMaxBpfDeviceIndex; ++index) {
    std::string path = std::format("/dev/bpf{}", index);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
      link->fd_ = fd;
      link->device_path_ = std::move(path);
      break;
    }
    last_errno = errno;
    if (last_errno == EBUSY) {
      continue;  // held by another capture program, try the next
    }
    if (last_errno == ENOENT) {
      break;  // ceiling reached; there will be no node with a higher number
    }
    if (last_errno == EACCES || last_errno == EPERM) {
      return std::unexpected(
          Error::FromErrno(last_errno, Tr(Msg::kNetBpfOpenDenied, path)));
    }
    // Any other errno: keep trying the next number, report at the end.
  }

  if (link->fd_ < 0) {
    return std::unexpected(
        Error::FromErrno(last_errno, Tr(Msg::kNetBpfNoDeviceAvailable)));
  }

  // ---------------------------------------------------------------------------
  // 2. BIOCSBLEN -- **must come before BIOCSETIF**
  //
  // In bpf.c: `if (d->bd_bif != 0 || (d->bd_flags & BPF_DETACHING)) return
  // EINVAL`. And an over-limit value does **not** error; it is silently clamped
  // to BPF_BUFSIZE_CAP and the effective value is written back through _IOWR --
  // so every read() below must use this written-back value as the length.
  // ---------------------------------------------------------------------------
  auto buffer_bytes = static_cast<unsigned int>(config.kernel_buffer_bytes);
  TETHERNET_RETURN_IF_ERROR(CallIoctl(link->fd_, BIOCSBLEN, &buffer_bytes, "ioctl(BIOCSBLEN)"));
  link->kernel_buffer_bytes_ = buffer_bytes;
  if (buffer_bytes != config.kernel_buffer_bytes) {
    TETHERNET_INFO_TR(Msg::kNetBpfBufferClamped, config.kernel_buffer_bytes, buffer_bytes);
  }

  // ---------------------------------------------------------------------------
  // 3. BIOCSHDRCMPLT = 1 -- **mandatory**, and must come before BIOCSBATCHWRITE
  //
  // With hdrcmplt=0, bpfwrite strips the first 14 bytes and rebuilds the frame
  // header (the source MAC gets rewritten by the driver); =1 takes the
  // DLIL_OUTPUT_FLAGS_RAW pass-through path instead. Batched writes
  // additionally hard-require it to be 1.
  //
  //  And on feth the consequence is far worse than "the header gets
  // rewritten": **without this ioctl, write() fails outright with ENXIO**
  // (if_fake's output path cannot handle the AF_UNSPEC rebuild branch).
  // Controlled experiment in AGENTS.md section 7, item 14. When debugging
  // ENXIO, do not suspect the interface name.
  // ---------------------------------------------------------------------------
  int header_complete = 1;
  TETHERNET_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCSHDRCMPLT, &header_complete, "ioctl(BIOCSHDRCMPLT)"));

  // ---------------------------------------------------------------------------
  // 4. BIOCSETIF -- bind to the interface
  // ---------------------------------------------------------------------------
  ::ifreq bind_request{};
  std::memcpy(bind_request.ifr_name, interface_name.data(), interface_name.size());
  if (const auto status =
          CallIoctl(link->fd_, BIOCSETIF, &bind_request, "ioctl(BIOCSETIF)");
      !status) {
    Error error = status.error();
    if (error.Code() == ENXIO) {
      return std::unexpected(std::move(error).WithContext(
          Tr(Msg::kNetBpfInterfaceMissing, interface_name)));
    }
    return std::unexpected(
        std::move(error).WithContext(Tr(Msg::kNetBpfBindFailed, interface_name)));
  }

  // ---------------------------------------------------------------------------
  // 5. BIOCGDLT -- validate the data-link type
  //
  // feth's bpfattach uses DLT_EN10MB, so this is always 1; the check guards
  // against someone accidentally binding BPF to some other interface type,
  // where every frame-format assumption would be wrong.
  // ---------------------------------------------------------------------------
  unsigned int data_link_type = 0;
  TETHERNET_RETURN_IF_ERROR(CallIoctl(link->fd_, BIOCGDLT, &data_link_type, "ioctl(BIOCGDLT)"));
  if (data_link_type != DLT_EN10MB) {
    return std::unexpected(Error::Generic(
        Tr(Msg::kNetBpfWrongDataLinkType, interface_name, data_link_type, DLT_EN10MB)));
  }

  // ---------------------------------------------------------------------------
  // 6. BIOCIMMEDIATE = 1 -- immediate delivery
  //
  // Without it, read() returns only when the buffer **fills up**; with a 4 MiB
  // buffer the latency would be catastrophically high. With it, the reader
  // wakes on every packet and each wake delivers everything that piled up in
  // the meantime -- low traffic gives low latency, high traffic gives large
  // batches automatically, NAPI-like behavior.
  // ---------------------------------------------------------------------------
  unsigned int immediate = 1;
  TETHERNET_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCIMMEDIATE, &immediate, "ioctl(BIOCIMMEDIATE)"));

  // ---------------------------------------------------------------------------
  // 7. BIOCSSEESENT = 0 -- capture the input direction only
  //
  // This step is **the anti-loopback key**: the frames we write appear on this
  // interface in the output direction, and SEESENT=0 filters them out, leaving
  // only the input frames the host sends from the peer side.
  // ---------------------------------------------------------------------------
  unsigned int see_sent = 0;
  TETHERNET_RETURN_IF_ERROR(CallIoctl(link->fd_, BIOCSSEESENT, &see_sent, "ioctl(BIOCSSEESENT)"));

  // ---------------------------------------------------------------------------
  // 8. BIOCSRTIMEOUT -- read timeout, used to answer shutdown
  //
  // The kernel stores tvtohz(tv) - 1 ticks; on this machine hz=100 -> a 10 ms
  // resolution. Passing {0,0} becomes **a permanent block**, so a non-zero
  // value is mandatory here.
  // ---------------------------------------------------------------------------
  ::timeval read_timeout{};
  read_timeout.tv_sec = config.read_timeout_millis / 1000;
  read_timeout.tv_usec = static_cast<__darwin_suseconds_t>(
      (config.read_timeout_millis % 1000) * 1000);
  TETHERNET_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCSRTIMEOUT, &read_timeout, "ioctl(BIOCSRTIMEOUT)"));

  // BIOCPROMISC is deliberately **not** set: feth's feth_output_common hands
  // every frame to the peer and taps it unconditionally, with no MAC filtering
  // whatsoever -- whether a frame is readable depends only on direction. Setting
  // promisc would only add one IFF_PROMISC reference count and one kernel
  // event; pure waste.

  // ---------------------------------------------------------------------------
  // 9. Optional optimizations: batched writes and disabled timestamps
  // (both feature-probed)
  // ---------------------------------------------------------------------------
  if (config.try_batch_write) {
    int enable = 1;
    link->batch_write_enabled_ =
        TryOptionalIoctl(link->fd_, kBpfSetBatchWrite, &enable, "BIOCSBATCHWRITE");
  }
  if (config.try_disable_timestamp) {
    int disable = 1;
    link->timestamp_disabled_ =
        TryOptionalIoctl(link->fd_, kBpfSetNoTimestamp, &disable, "BIOCSNOTSTAMP");
  }

  // ---------------------------------------------------------------------------
  // 10. Allocate buffers
  // ---------------------------------------------------------------------------
  link->read_buffer_.resize(link->kernel_buffer_bytes_);
  link->read_frames_.reserve(config.max_frames_per_batch);
  if (link->batch_write_enabled_) {
    link->write_buffer_.resize(kMaxBatchWriteBytes);
  }

  TETHERNET_INFO_TR(Msg::kNetBpfReady, link->device_path_, link->interface_name_,
                    link->kernel_buffer_bytes_ / 1024, link->max_frame_bytes_,
                    Text(link->batch_write_enabled_ ? Msg::kNetBpfBatchWriteEnabled
                                                    : Msg::kNetBpfBatchWriteUnavailable),
                    Text(link->timestamp_disabled_ ? Msg::kNetBpfTimestampDisabled
                                                   : Msg::kNetBpfTimestampEnabled));

  return link;
}

Result<BpfLink::KernelStats> BpfLink::QueryKernelStats() const {
  ::bpf_stat stats{};
  if (::ioctl(fd_, BIOCGSTATS, &stats) < 0) {
    return std::unexpected(Error::FromErrno(0, "ioctl(BIOCGSTATS)"));
  }
  return KernelStats{.received = stats.bs_recv, .dropped = stats.bs_drop};
}

Result<ReadBatch> BpfLink::ReadFrames() {
  read_frames_.clear();

  // Every early return must carry last_kernel_drops_ rather than the default 0:
  // kernel_drops is a **cumulative** count, and consumers take differences
  // between adjacent samples. When idle, the read timeout comes through here
  // every 200 ms; reporting 0 would "zero out" drops that already happened, the
  // next sample with traffic would re-report the full count as new drops, and
  // the interim sample's difference would underflow on the unsigned type.
  if (interrupted_.load(std::memory_order_acquire)) {
    return ReadBatch{.kernel_drops = last_kernel_drops_};
  }

  // The read() length **must exactly equal** the kernel's bd_bufsize, otherwise
  // bpfread fails at the top with `if (uio_resid(uio) != d->bd_bufsize) return
  // EINVAL`.
  const ssize_t received = ::read(fd_, read_buffer_.data(), read_buffer_.size());
  if (received < 0) {
    if (errno == EINTR || errno == EAGAIN) {
      // Interrupted by a signal or timed out with no data; let the caller keep
      // looping.
      return ReadBatch{.kernel_drops = last_kernel_drops_};
    }
    return std::unexpected(Error::FromErrno(0, Tr(Msg::kNetBpfReadFailed, device_path_)));
  }
  if (received == 0) {
    return ReadBatch{.kernel_drops = last_kernel_drops_};  // read timeout elapsed with no packets
  }

  // ---------------------------------------------------------------------------
  // Walk the BPF records
  //
  // Record layout: [bpf_hdr (bh_hdrlen bytes, incl. alignment padding)]
  //                [frame data (bh_caplen bytes)]
  // Next record's offset = BPF_WORDALIGN(bh_hdrlen + bh_caplen).
  //
  //  The record's bh_hdrlen must be used, never sizeof(struct bpf_hdr).
  //   On LP64, sizeof is 20 (18 bytes of content + 2 bytes of compiler
  //   padding), while the kernel writes bh_hdrlen = 18 for DLT_EN10MB
  //   (SIZEOF_BPF_HDR=18, bif_hdrlen = BPF_WORDALIGN(14 + 18) - 14 = 18).
  //   Using 20 goes out of step immediately.
  // ---------------------------------------------------------------------------
  const auto total = static_cast<std::size_t>(received);
  std::size_t offset = 0;

  while (offset + kBpfHeaderMinBytes <= total) {
    const std::byte* record = read_buffer_.data() + offset;

    // Field-by-field reads instead of a struct map: a record's start is only
    // guaranteed 4-byte aligned.
    const std::uint32_t capture_length = LoadLe32(record + offsetof(::bpf_hdr, bh_caplen));
    const std::uint32_t original_length = LoadLe32(record + offsetof(::bpf_hdr, bh_datalen));
    const std::uint32_t header_length = LoadLe16(record + offsetof(::bpf_hdr, bh_hdrlen));

    if (header_length < kBpfHeaderMinBytes) [[unlikely]] {
      TETHERNET_WARN_TR(Msg::kNetBpfHeaderTooShort, header_length, kBpfHeaderMinBytes);
      break;
    }

    const std::size_t record_bytes = static_cast<std::size_t>(header_length) + capture_length;
    if (offset + record_bytes > total) [[unlikely]] {
      // Record truncated: should never happen in normal operation (the kernel
      // does not write records that straddle the buffer end).
      TETHERNET_WARN_TR(Msg::kNetBpfRecordOutOfBounds, offset, record_bytes, total);
      break;
    }

    // Only fully captured frames are forwarded. capture < original means a
    // filter is installed and truncated the frame; forwarding a partial frame
    // to USB would only confuse the peer. We install no filter, so this branch
    // should not trigger.
    if (capture_length == original_length && capture_length >= kMinEthernetFrameBytes &&
        capture_length <= max_frame_bytes_) [[likely]] {
      read_frames_.push_back(FrameView{.data = record + header_length, .length = capture_length});
    } else [[unlikely]] {
      TETHERNET_TRACE_TR(Msg::kNetBpfSkipOversizedRecord, capture_length, original_length,
                         max_frame_bytes_);
    }

    offset += BPF_WORDALIGN(record_bytes);

    if (read_frames_.size() >= read_frames_.capacity()) {
      // The frame array is full. The remaining records are skipped this round
      // -- is the data still in the kernel buffer? No: it has already been
      // copyout-ed and would be lost. max_frames_per_batch must therefore be
      // sized generously enough. Warn here rather than fail silently.
      TETHERNET_WARN_TR(Msg::kNetBpfBatchFrameLimit, read_frames_.capacity(), total - offset);
      break;
    }
  }

  // When BIOCGSTATS fails sporadically, keep the previous cumulative value --
  // reporting 0 would make the consumer's difference underflow; see the note
  // on last_kernel_drops_.
  if (const auto stats = QueryKernelStats()) {
    last_kernel_drops_ = stats->dropped;
  }
  return ReadBatch{
      .frames = read_frames_,
      .kernel_drops = last_kernel_drops_,
  };
}

Result<WriteResult> BpfLink::WriteFrames(FrameBatch frames) {
  if (frames.empty()) {
    return WriteResult{};
  }
  return batch_write_enabled_ ? WriteFramesBatched(frames) : WriteFramesIndividually(frames);
}

Result<WriteResult> BpfLink::WriteFramesIndividually(FrameBatch frames) {
  WriteResult result;
  for (const FrameView& frame : frames) {
    if (frame.length < kMinEthernetFrameBytes || frame.length > max_frame_bytes_) [[unlikely]] {
      ++result.frames_skipped;
      continue;
    }
    const ssize_t written = ::write(fd_, frame.data, frame.length);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      // ENOBUFS is transient (the interface's send queue is full) and must not
      // be treated as fatal; the caller sees frames_written < frames.size()
      // and knows not everything was written.
      if (errno == ENOBUFS || errno == EAGAIN) {
        break;
      }
      return std::unexpected(
          Error::FromErrno(0, Tr(Msg::kNetBpfWriteFailed, device_path_, frame.length)));
    }
    ++result.frames_written;
    result.bytes_written += static_cast<std::uint64_t>(written);
  }
  return result;
}

Result<WriteResult> BpfLink::WriteFramesBatched(FrameBatch frames) {
  // The batched-write buffer layout is **fully symmetric** with the read side:
  // consecutive
  //   [bpf_hdr (bh_hdrlen bytes)][frame data (bh_caplen bytes)]
  // records, each aligned to BPF_WORDALIGN(bh_hdrlen + bh_caplen).
  //
  // Kernel checks: bh_hdrlen >= 18, bh_caplen == bh_datalen, bh_hdrlen <=
  // remaining length. The timestamp field is ignored and need not be filled in.
  //
  // Here bh_hdrlen takes 18 (kBpfHeaderMinBytes) rather than sizeof(struct
  // bpf_hdr)=20: both are legal (the kernel skips anything past 18), but 18 is
  // the most compact packing and matches the value the kernel writes on the
  // read side.
  WriteResult result;
  std::size_t cursor = 0;
  std::uint32_t pending_frames = 0;
  std::uint64_t pending_bytes = 0;

  // Write out whatever has been assembled, in one shot.
  const auto flush = [&]() -> Status {
    if (cursor == 0) {
      return Ok();
    }
    const ssize_t written = ::write(fd_, write_buffer_.data(), cursor);
    if (written < 0) {
      if (errno == ENOBUFS || errno == EAGAIN || errno == EINTR) {
        // Transient failure: this batch went nowhere, mirrored faithfully in
        // the return value.
        cursor = 0;
        pending_frames = 0;
        pending_bytes = 0;
        return Ok();
      }
      return std::unexpected(Error::FromErrno(
          0, Tr(Msg::kNetBpfBatchWriteFailed, device_path_, pending_frames, cursor)));
    }
    result.frames_written += pending_frames;
    result.bytes_written += pending_bytes;
    cursor = 0;
    pending_frames = 0;
    pending_bytes = 0;
    return Ok();
  };

  for (const FrameView& frame : frames) {
    if (frame.length < kMinEthernetFrameBytes || frame.length > max_frame_bytes_) [[unlikely]] {
      ++result.frames_skipped;
      continue;
    }

    const std::size_t record_bytes = kBpfHeaderMinBytes + frame.length;
    const std::size_t aligned_bytes = BPF_WORDALIGN(record_bytes);
    if (cursor + aligned_bytes > write_buffer_.size()) {
      TETHERNET_RETURN_IF_ERROR(flush());
      if (aligned_bytes > write_buffer_.size()) [[unlikely]] {
        // A single frame exceeds the whole assembly buffer: a configuration
        // error; skip and count.
        ++result.frames_skipped;
        continue;
      }
    }

    std::byte* record = write_buffer_.data() + cursor;
    // The kernel ignores the timestamp field; zeroing it is enough.
    std::memset(record, 0, kBpfHeaderMinBytes);
    StoreLe32(record + offsetof(::bpf_hdr, bh_caplen), frame.length);
    StoreLe32(record + offsetof(::bpf_hdr, bh_datalen), frame.length);
    StoreLe16(record + offsetof(::bpf_hdr, bh_hdrlen),
              static_cast<std::uint16_t>(kBpfHeaderMinBytes));
    std::memcpy(record + kBpfHeaderMinBytes, frame.data, frame.length);

    // Zero the alignment padding too, so leftovers from the previous batch are
    // never handed to the kernel.
    if (aligned_bytes > record_bytes) {
      std::memset(record + record_bytes, 0, aligned_bytes - record_bytes);
    }

    cursor += aligned_bytes;
    ++pending_frames;
    pending_bytes += frame.length;
  }

  TETHERNET_RETURN_IF_ERROR(flush());
  return result;
}

}  // namespace tethernet::net
