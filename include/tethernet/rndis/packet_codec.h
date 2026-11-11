// Encoding and decoding of REMOTE_NDIS_PACKET_MSG -- **data hot path**.
//
// This is the only protocol code in the project executed for every frame, so:
//   * Everything is inline in the header so the compiler can inline it into the bridge
//     loop;
//   * No std::expected (allocates std::string), no exceptions, no memory allocation;
//     errors are expressed through enums and counters;
//   * Everything is noexcept.
//
// The two directions have different shapes and must not be confused:
//
//   device -> host (PacketMessageReader):
//     One bulk IN transfer carries 1..N PACKET_MSGs, traversed by stepping over
//     MessageLength. Trailing garbage must be tolerated (devices also use "add 1 extra
//     byte" to avoid ZLP).
//
//   host -> device (PacketMessageWriter):
//     Aggregate multiple frames into one bulk OUT transfer, constrained three ways: the
//     device's MaxTransferSize, MaxPacketsPerMessage, and per-packet alignment
//     determined by PacketAlignmentFactor.
#pragma once

#include "tethernet/common/byte_order.h"
#include "tethernet/common/i18n.h"
#include "tethernet/rndis/protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace tethernet::rndis {

// =============================================================================
// Decode: device -> host
// =============================================================================

/// Result of iterating over one bulk IN transfer.
enum class ReadOutcome : std::uint8_t {
  kFrame,          ///< Successfully extracted a frame.
  kEndOfTransfer,  ///< Normal end (there may be tolerated trailing padding).
  kMalformed,      ///< Malformed data encountered; parsing of this transfer must stop.
};

/// Specific reason for malformed data, used for statistics and troubleshooting.
enum class MalformedReason : std::uint8_t {
  kNone,
  kNotPacketMessage,  ///< MessageType is not REMOTE_NDIS_PACKET_MSG.
  kMessageTooShort,   ///< MessageLength < 44.
  kMessageOverruns,   ///< MessageLength exceeds the remaining buffer.
  kDataOutOfBounds,   ///< DataOffset + DataLength exceeds MessageLength.
  kFrameTooShort,     ///< DataLength < 14, cannot hold an Ethernet header.
  kFrameTooLong,      ///< DataLength exceeds the per-frame limit allowed by this host.
};

/// Extract Ethernet frames one by one from a single bulk IN transfer buffer.
///
/// Usage:
/// ```
/// PacketMessageReader reader(transfer_bytes, max_frame_bytes);
/// std::span<const std::byte> frame;
/// while (reader.Next(frame) == ReadOutcome::kFrame) {
///   ... handle frame ...
/// }
/// ```
class PacketMessageReader {
 public:
  PacketMessageReader(std::span<const std::byte> transfer, std::uint32_t max_frame_bytes) noexcept
      : transfer_(transfer), max_frame_bytes_(max_frame_bytes) {}

  /// Take the next frame. When kFrame is returned, `frame` points inside the transfer
  /// with no copy.
  [[nodiscard]] ReadOutcome Next(std::span<const std::byte>& frame) noexcept {
    const std::size_t remaining = transfer_.size() - cursor_;

    // Not enough remaining for a complete header: treat as trailing padding and end
    // normally.
    //
    // This tolerance is **required**: RNDIS specifies that neither host nor device sends
    // ZLPs; instead, when the transfer length is an exact integer multiple of the
    // endpoint's max packet size, one extra byte is appended to make it a short packet.
    // That byte falls outside all MessageLengths; treating it as an error would
    // misreport once per 2048-byte transfer.
    if (remaining < kPacketMsgHeaderBytes) {
      trailing_padding_bytes_ = static_cast<std::uint32_t>(remaining);
      return ReadOutcome::kEndOfTransfer;
    }

    const std::byte* message = transfer_.data() + cursor_;

    const std::uint32_t message_type = LoadLe32(message + kMessageTypeOffset);
    if (message_type != ToRaw(MessageType::kPacket)) [[unlikely]] {
      return Fail(MalformedReason::kNotPacketMessage);
    }

    const std::uint32_t message_length = LoadLe32(message + kMessageLengthOffset);
    if (message_length < kPacketMsgHeaderBytes) [[unlikely]] {
      return Fail(MalformedReason::kMessageTooShort);
    }
    if (message_length > remaining) [[unlikely]] {
      return Fail(MalformedReason::kMessageOverruns);
    }

    const std::uint32_t data_offset = LoadLe32(message + kPacketDataOffsetOffset);
    const std::uint32_t data_length = LoadLe32(message + kPacketDataLengthOffset);

    // Absolute offset = 8 + DataOffset (protocol.h rule 2).
    // Use 64-bit arithmetic to avoid overflow when the device reports a huge value.
    const std::uint64_t data_begin = static_cast<std::uint64_t>(kOffsetFieldBase) + data_offset;
    const std::uint64_t data_end = data_begin + data_length;
    if (data_end > message_length) [[unlikely]] {
      return Fail(MalformedReason::kDataOutOfBounds);
    }
    if (data_length < kEthernetHeaderBytes) [[unlikely]] {
      return Fail(MalformedReason::kFrameTooShort);
    }
    if (data_length > max_frame_bytes_) [[unlikely]] {
      return Fail(MalformedReason::kFrameTooLong);
    }

    frame = transfer_.subspan(cursor_ + static_cast<std::size_t>(data_begin), data_length);
    cursor_ += message_length;
    ++frames_decoded_;
    return ReadOutcome::kFrame;
  }

  [[nodiscard]] std::uint32_t FramesDecoded() const noexcept { return frames_decoded_; }

  [[nodiscard]] MalformedReason Reason() const noexcept { return reason_; }

  /// Number of trailing bytes ignored as padding (normally 0 or 1).
  [[nodiscard]] std::uint32_t TrailingPaddingBytes() const noexcept {
    return trailing_padding_bytes_;
  }

  /// Bytes consumed, used to verify that the entire transfer was parsed.
  [[nodiscard]] std::size_t BytesConsumed() const noexcept { return cursor_; }

 private:
  ReadOutcome Fail(MalformedReason reason) noexcept {
    reason_ = reason;
    return ReadOutcome::kMalformed;
  }

  std::span<const std::byte> transfer_;
  std::uint32_t max_frame_bytes_;
  std::size_t cursor_ = 0;
  std::uint32_t frames_decoded_ = 0;
  std::uint32_t trailing_padding_bytes_ = 0;
  MalformedReason reason_ = MalformedReason::kNone;
};

/// Message identifier for a MalformedReason.
///
/// Returns an identifier instead of a ready-made string to preserve constexpr -- the
/// actual text must be rendered in the user's current language; callers take it with
/// `Text(MalformedReasonMessage(r))`.
[[nodiscard]] constexpr Msg MalformedReasonMessage(MalformedReason reason) noexcept {
  switch (reason) {
    case MalformedReason::kNone:
      return Msg::kRndisMalformedNone;
    case MalformedReason::kNotPacketMessage:
      return Msg::kRndisMalformedNotPacketMessage;
    case MalformedReason::kMessageTooShort:
      return Msg::kRndisMalformedMessageTooShort;
    case MalformedReason::kMessageOverruns:
      return Msg::kRndisMalformedMessageOverruns;
    case MalformedReason::kDataOutOfBounds:
      return Msg::kRndisMalformedDataOutOfBounds;
    case MalformedReason::kFrameTooShort:
      return Msg::kRndisMalformedFrameTooShort;
    case MalformedReason::kFrameTooLong:
      return Msg::kRndisMalformedFrameTooLong;
  }
  return Msg::kRndisMalformedUnknown;
}

/// Human-readable name for MalformedReason (in the current language).
[[nodiscard]] inline std::string_view MalformedReasonName(MalformedReason reason) noexcept {
  return Text(MalformedReasonMessage(reason));
}

// =============================================================================
// Encode: host -> device
// =============================================================================

/// Aggregate multiple Ethernet frames into one bulk OUT transfer.
///
/// Alignment detail (this is the easiest place to get wrong):
///   RNDIS specifies that "for every PACKET_MSG except the last, MessageLength includes
///   trailing alignment padding; the last does not include external padding". Therefore
///   this implementation expands the **previous** message's MessageLength to absorb the
///   intermediate padding bytes only when appending the **next** message -- so the last
///   message's MessageLength is naturally 44 + frame length, fully compliant with the
///   specification.
class PacketMessageWriter {
 public:
  /// @param transfer      Output buffer (usually a libusb transfer buffer).
  /// @param limits        Aggregation limits negotiated with the device.
  /// @param endpoint_max_packet  wMaxPacketSize of the bulk OUT endpoint, used to avoid
  ///                             ZLP.
  struct Limits {
    std::uint32_t max_transfer_bytes = 0;  ///< Device MaxTransferSize.
    std::uint32_t max_messages = 1;        ///< Device MaxPacketsPerMessage.
    std::uint32_t alignment_bytes = 1;     ///< 1 << PacketAlignmentFactor.
  };

  PacketMessageWriter(std::span<std::byte> transfer, const Limits& limits,
                      std::uint32_t endpoint_max_packet) noexcept
      : transfer_(transfer),
        endpoint_max_packet_(endpoint_max_packet),
        max_messages_(limits.max_messages == 0 ? 1 : limits.max_messages),
        alignment_bytes_(limits.alignment_bytes == 0 ? 1 : limits.alignment_bytes) {
    // Effective capacity is the smaller of "buffer size" and "device MaxTransferSize",
    // minus 1 byte reserved for possible ZLP-avoidance padding.
    const std::size_t device_limit = limits.max_transfer_bytes == 0
                                         ? transfer.size()
                                         : static_cast<std::size_t>(limits.max_transfer_bytes);
    const std::size_t usable = device_limit < transfer.size() ? device_limit : transfer.size();
    capacity_ = usable > kZlpPadReserve ? usable - kZlpPadReserve : 0;
  }

  /// Try to append a frame. Returns false if this batch is full and the caller should
  /// submit first and retry.
  ///
  /// Frame length must be >= 14; the caller is responsible for guaranteeing this (frames
  /// read from BPF always satisfy it).
  [[nodiscard]] bool TryAppend(std::span<const std::byte> frame) noexcept {
    if (message_count_ >= max_messages_) [[unlikely]] {
      return false;
    }

    const auto frame_length = static_cast<std::uint32_t>(frame.size());

    // If there is already a message, compute how much padding is needed to satisfy
    // alignment.
    const std::size_t aligned_cursor =
        message_count_ == 0 ? cursor_ : AlignUp<std::size_t>(cursor_, alignment_bytes_);
    const std::size_t needed = aligned_cursor + kPacketMsgHeaderBytes + frame_length;
    if (needed > capacity_) {
      return false;
    }

    // Attribute padding bytes to the **previous** message's MessageLength (see class
    // documentation).
    if (aligned_cursor != cursor_) {
      const auto padding = static_cast<std::uint32_t>(aligned_cursor - cursor_);
      std::memset(transfer_.data() + cursor_, 0, padding);
      const std::uint32_t previous_length =
          LoadLe32(transfer_.data() + last_header_offset_ + kMessageLengthOffset);
      StoreLe32(transfer_.data() + last_header_offset_ + kMessageLengthOffset,
                previous_length + padding);
      cursor_ = aligned_cursor;
    }

    std::byte* message = transfer_.data() + cursor_;
    const std::uint32_t message_length = kPacketMsgHeaderBytes + frame_length;

    StoreLe32(message + kMessageTypeOffset, ToRaw(MessageType::kPacket));
    StoreLe32(message + kMessageLengthOffset, message_length);
    // DataOffset is 36, not 44 -- base point is message start + 8.
    StoreLe32(message + kPacketDataOffsetOffset, kPacketInlineDataOffset);
    StoreLe32(message + kPacketDataLengthOffset, frame_length);
    // Remaining fields (OOB, per-packet info, VcHandle, Reserved) are always 0 in this
    // project.
    std::memset(message + kPacketOobDataOffsetOffset, 0,
                kPacketMsgHeaderBytes - kPacketOobDataOffsetOffset);

    std::memcpy(message + kPacketMsgHeaderBytes, frame.data(), frame_length);

    last_header_offset_ = cursor_;
    cursor_ += message_length;
    payload_bytes_ += frame_length;
    ++message_count_;
    return true;
  }

  /// Finish this batch and return the byte count to submit to libusb.
  ///
  /// Appends 1 byte 0x00 when necessary to avoid a ZLP: RNDIS explicitly requires the
  /// host to **never** send a zero-length packet, and when the transfer length is an
  /// exact integer multiple of the endpoint wMaxPacketSize, the USB host controller needs
  /// a short packet to mark the end of the transfer. This is what Linux usbnet does --
  /// the extra byte falls outside all MessageLengths and the device must tolerate
  /// trailing garbage.
  [[nodiscard]] std::uint32_t Finish() noexcept {
    if (cursor_ == 0) {
      return 0;
    }
    if (endpoint_max_packet_ != 0 && cursor_ % endpoint_max_packet_ == 0) {
      transfer_[cursor_] = std::byte{0};
      ++cursor_;
      zlp_padding_added_ = true;
    }
    return static_cast<std::uint32_t>(cursor_);
  }

  [[nodiscard]] std::uint32_t MessageCount() const noexcept { return message_count_; }

  /// Aggregated Ethernet frame payload bytes (excluding RNDIS headers and padding), for
  /// throughput statistics.
  [[nodiscard]] std::uint64_t PayloadBytes() const noexcept { return payload_bytes_; }

  [[nodiscard]] bool ZlpPaddingAdded() const noexcept { return zlp_padding_added_; }

  [[nodiscard]] bool Empty() const noexcept { return message_count_ == 0; }

  /// Maximum frame length the current batch can still hold (0 means no more frames fit).
  /// Callers can use this to avoid "take out a frame only to find it does not fit"
  /// backtracking logic.
  [[nodiscard]] std::uint32_t RemainingFrameCapacity() const noexcept {
    if (message_count_ >= max_messages_) {
      return 0;
    }
    const std::size_t aligned_cursor =
        message_count_ == 0 ? cursor_ : AlignUp<std::size_t>(cursor_, alignment_bytes_);
    const std::size_t overhead = aligned_cursor + kPacketMsgHeaderBytes;
    return overhead >= capacity_ ? 0 : static_cast<std::uint32_t>(capacity_ - overhead);
  }

 private:
  /// Bytes reserved for ZLP avoidance.
  static constexpr std::size_t kZlpPadReserve = 1;

  std::span<std::byte> transfer_;
  std::uint32_t endpoint_max_packet_;
  std::uint32_t max_messages_;
  std::uint32_t alignment_bytes_;
  std::size_t capacity_ = 0;

  std::size_t cursor_ = 0;
  std::size_t last_header_offset_ = 0;
  std::uint32_t message_count_ = 0;
  std::uint64_t payload_bytes_ = 0;
  bool zlp_padding_added_ = false;
};

}  // namespace tethernet::rndis
