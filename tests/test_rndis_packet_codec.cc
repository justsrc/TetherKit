// Unit tests for the REMOTE_NDIS_PACKET_MSG codec -- the correctness
// lifeline of the data hot path.
//
// The focus is on:
//   1. The DataOffset base point is the message start +8 (the encoder writes
//      36, the decoder locates at 8+36);
//   2. The alignment rule for multi-packet aggregation: the padding belongs
//      to the **previous** message's MessageLength, and the last message
//      carries no trailing padding;
//   3. ZLP avoidance: when the transfer length is exactly a multiple of the
//      endpoint's maximum packet size, one byte is added;
//   4. The decoder must tolerate trailing padding (otherwise every full
//      transfer reports a spurious error);
//   5. All malformed inputs are recognized as kMalformed without reading out
//      of bounds.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <doctest.h>

#include "language_guard.h"

#include "tetherkit/common/byte_order.h"
#include "tetherkit/rndis/packet_codec.h"
#include "tetherkit/rndis/protocol.h"

using namespace tetherkit;         // NOLINT(google-build-using-namespace)
using namespace tetherkit::rndis;  // NOLINT(google-build-using-namespace)

namespace {

constexpr std::uint32_t kMaxFrame = 2048;

/// Builds an Ethernet frame whose content can be self-verified.
std::vector<std::byte> MakeFrame(std::uint32_t length, std::uint8_t tag) {
  std::vector<std::byte> frame(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    frame[i] = std::byte{static_cast<unsigned char>((tag + i) & 0xFFU)};
  }
  return frame;
}

bool FrameMatches(std::span<const std::byte> frame, std::uint32_t length, std::uint8_t tag) {
  if (frame.size() != length) {
    return false;
  }
  for (std::uint32_t i = 0; i < length; ++i) {
    if (frame[i] != std::byte{static_cast<unsigned char>((tag + i) & 0xFFU)}) {
      return false;
    }
  }
  return true;
}

/// Hand-builds a PACKET_MSG (for testing the decoder, independent of the
/// encoder).
void WritePacketMessage(std::span<std::byte> out, std::span<const std::byte> frame,
                        std::uint32_t data_offset_field, std::uint32_t message_length,
                        std::uint32_t data_length_field) {
  StoreLe32(out.data() + kMessageTypeOffset, ToRaw(MessageType::kPacket));
  StoreLe32(out.data() + kMessageLengthOffset, message_length);
  StoreLe32(out.data() + kPacketDataOffsetOffset, data_offset_field);
  StoreLe32(out.data() + kPacketDataLengthOffset, data_length_field);
  for (std::uint32_t i = kPacketOobDataOffsetOffset; i < kPacketMsgHeaderBytes; i += 4) {
    StoreLe32(out.data() + i, 0);
  }
  const std::size_t absolute = kOffsetFieldBase + data_offset_field;
  if (!frame.empty() && absolute + frame.size() <= out.size()) {
    std::memcpy(out.data() + absolute, frame.data(), frame.size());
  }
}

/// Collects every frame decoded out of one transfer (copied out, for
/// assertions).
struct DecodeSummary {
  std::vector<std::vector<std::byte>> frames;
  ReadOutcome final_outcome = ReadOutcome::kEndOfTransfer;
  MalformedReason reason = MalformedReason::kNone;
  std::uint32_t trailing_padding = 0;
};

DecodeSummary DecodeAll(std::span<const std::byte> transfer,
                        std::uint32_t max_frame_bytes = kMaxFrame) {
  DecodeSummary summary;
  PacketMessageReader reader(transfer, max_frame_bytes);
  std::span<const std::byte> frame;
  while (true) {
    const ReadOutcome outcome = reader.Next(frame);
    if (outcome != ReadOutcome::kFrame) {
      summary.final_outcome = outcome;
      break;
    }
    summary.frames.emplace_back(frame.begin(), frame.end());
  }
  summary.reason = reader.Reason();
  summary.trailing_padding = reader.TrailingPaddingBytes();
  return summary;
}

}  // namespace

TEST_SUITE("rndis.packet_codec") {
  // ---------------------------------------------------------------------------
  // Encoding
  // ---------------------------------------------------------------------------

  TEST_CASE("Encoding a single frame: DataOffset must be written as 36") {
    std::array<std::byte, 4096> transfer{};
    PacketMessageWriter writer(
        transfer, {.max_transfer_bytes = 2048, .max_messages = 1, .alignment_bytes = 1}, 512);

    const auto frame = MakeFrame(1514, 0x11);
    REQUIRE(writer.TryAppend(frame));
    const std::uint32_t total = writer.Finish();

    CHECK(writer.MessageCount() == 1);
    CHECK(writer.PayloadBytes() == 1514);
    CHECK(total == kPacketMsgHeaderBytes + 1514);

    CHECK(LoadLe32(transfer.data() + kMessageTypeOffset) == ToRaw(MessageType::kPacket));
    CHECK(LoadLe32(transfer.data() + kMessageLengthOffset) == kPacketMsgHeaderBytes + 1514);
    // The key assertion: 36, not 44.
    CHECK(LoadLe32(transfer.data() + kPacketDataOffsetOffset) == 36);
    CHECK(LoadLe32(transfer.data() + kPacketDataLengthOffset) == 1514);
    // OOB / per-packet info / VcHandle / Reserved must all be zero.
    CHECK(LoadLe32(transfer.data() + kPacketOobDataOffsetOffset) == 0);
    CHECK(LoadLe32(transfer.data() + kPacketOobDataLengthOffset) == 0);
    CHECK(LoadLe32(transfer.data() + kPacketNumOobDataElementsOffset) == 0);
    CHECK(LoadLe32(transfer.data() + kPacketPerPacketInfoOffsetOffset) == 0);
    CHECK(LoadLe32(transfer.data() + kPacketPerPacketInfoLengthOffset) == 0);
    CHECK(LoadLe32(transfer.data() + kPacketVcHandleOffset) == 0);
    CHECK(LoadLe32(transfer.data() + kPacketReservedOffset) == 0);

    // The frame data lands at absolute offset 8 + 36 = 44.
    CHECK(FrameMatches(std::span<const std::byte>{transfer}.subspan(44, 1514), 1514, 0x11));
  }

  TEST_CASE("Encoding is limited by MaxPacketsPerMessage") {
    std::array<std::byte, 8192> transfer{};
    PacketMessageWriter writer(
        transfer, {.max_transfer_bytes = 8192, .max_messages = 2, .alignment_bytes = 1}, 512);
    CHECK(writer.TryAppend(MakeFrame(64, 1)));
    CHECK(writer.TryAppend(MakeFrame(64, 2)));
    CHECK_FALSE(writer.TryAppend(MakeFrame(64, 3)));  // over the message count limit
    CHECK(writer.MessageCount() == 2);
  }

  TEST_CASE("Encoding is limited by the device's MaxTransferSize") {
    std::array<std::byte, 8192> transfer{};
    // Room for exactly one 44+64=108 byte message (plus 1 byte of ZLP margin).
    PacketMessageWriter writer(
        transfer, {.max_transfer_bytes = 150, .max_messages = 10, .alignment_bytes = 1}, 512);
    CHECK(writer.TryAppend(MakeFrame(64, 1)));
    CHECK_FALSE(writer.TryAppend(MakeFrame(64, 2)));
    CHECK(writer.MessageCount() == 1);
  }

  TEST_CASE("Encoding an empty batch: Finish returns 0") {
    std::array<std::byte, 1024> transfer{};
    PacketMessageWriter writer(transfer, {.max_transfer_bytes = 1024}, 512);
    CHECK(writer.Empty());
    CHECK(writer.Finish() == 0);
  }

  TEST_CASE(
      "Multi-packet aggregation: the padding goes to the previous message, the last has none") {
    std::array<std::byte, 8192> transfer{};
    // 8-byte alignment (PacketAlignmentFactor = 3).
    PacketMessageWriter writer(
        transfer, {.max_transfer_bytes = 8192, .max_messages = 3, .alignment_bytes = 8}, 512);

    // Frame length 61 -> message length 44+61 = 105. 105 is not a multiple of
    // 8, so it is aligned up to 112 (14x8); that means 7 bytes of padding
    // before the next message, and those 7 bytes are swallowed into the
    // MessageLength of the **first** message.
    REQUIRE(writer.TryAppend(MakeFrame(61, 0xA0)));
    REQUIRE(writer.TryAppend(MakeFrame(70, 0xB0)));
    REQUIRE(writer.TryAppend(MakeFrame(64, 0xC0)));
    const std::uint32_t total = writer.Finish();

    // First message: 105 -> widened to 112 (absorbing the 7 padding bytes).
    CHECK(LoadLe32(transfer.data() + kMessageLengthOffset) == 112);
    CHECK(112 % 8 == 0);

    // The second message starts at 112: 44+70 = 114 -> cumulative 226, aligned
    // up to 232, so the second message's MessageLength is widened to 114 + 6 =
    // 120.
    CHECK(LoadLe32(transfer.data() + 112 + kMessageLengthOffset) == 120);
    CHECK((112 + 120) % 8 == 0);

    // The third (last) message starts at 232: 44+64 = 108, with **no** trailing
    // padding.
    CHECK(LoadLe32(transfer.data() + 232 + kMessageLengthOffset) == 108);
    CHECK(total == 232 + 108);
    CHECK(writer.PayloadBytes() == 61 + 70 + 64);
  }

  TEST_CASE("Multi-packet aggregation: the encoder's output is fully reproduced by the decoder") {
    std::array<std::byte, 8192> transfer{};
    PacketMessageWriter writer(
        transfer, {.max_transfer_bytes = 8192, .max_messages = 4, .alignment_bytes = 8}, 512);
    const std::array<std::uint32_t, 4> lengths{61, 70, 1514, 14};
    const std::array<std::uint8_t, 4> tags{0xA0, 0xB0, 0xC0, 0xD0};
    for (std::size_t i = 0; i < lengths.size(); ++i) {
      REQUIRE(writer.TryAppend(MakeFrame(lengths[i], tags[i])));
    }
    const std::uint32_t total = writer.Finish();

    const auto summary = DecodeAll(std::span<const std::byte>{transfer}.first(total));
    CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
    REQUIRE(summary.frames.size() == 4);
    for (std::size_t i = 0; i < lengths.size(); ++i) {
      CHECK(FrameMatches(summary.frames[i], lengths[i], tags[i]));
    }
  }

  TEST_CASE(
      "ZLP avoidance: one byte is added when the length is a multiple of the max packet size") {
    std::array<std::byte, 4096> transfer{};
    constexpr std::uint32_t kEndpointMaxPacket = 512;
    // Make the total exactly 512: 44 + 468 = 512.
    PacketMessageWriter writer(
        transfer, {.max_transfer_bytes = 4096, .max_messages = 1, .alignment_bytes = 1},
        kEndpointMaxPacket);
    REQUIRE(writer.TryAppend(MakeFrame(468, 0x55)));
    const std::uint32_t total = writer.Finish();

    CHECK(writer.ZlpPaddingAdded());
    CHECK(total == 513);
    CHECK(total % kEndpointMaxPacket != 0);
    // The added byte must be 0, and it must be outside the MessageLength (512).
    CHECK(transfer[512] == std::byte{0});
    CHECK(LoadLe32(transfer.data() + kMessageLengthOffset) == 512);
  }

  TEST_CASE("ZLP avoidance: no byte is added when the length is not a multiple") {
    std::array<std::byte, 4096> transfer{};
    PacketMessageWriter writer(
        transfer, {.max_transfer_bytes = 4096, .max_messages = 1, .alignment_bytes = 1}, 512);
    REQUIRE(writer.TryAppend(MakeFrame(100, 0x66)));
    const std::uint32_t total = writer.Finish();
    CHECK_FALSE(writer.ZlpPaddingAdded());
    CHECK(total == kPacketMsgHeaderBytes + 100);
  }

  TEST_CASE("RemainingFrameCapacity reports accurately, which avoids backtracking after a frame") {
    std::array<std::byte, 8192> transfer{};
    PacketMessageWriter writer(
        transfer, {.max_transfer_bytes = 200, .max_messages = 5, .alignment_bytes = 1}, 512);
    // Capacity 200 - 1 (ZLP margin) = 199; minus the 44-byte header -> 155.
    CHECK(writer.RemainingFrameCapacity() == 155);
    REQUIRE(writer.TryAppend(MakeFrame(100, 1)));
    // 144 used so far; 199 - 144 - 44 = 11 left.
    CHECK(writer.RemainingFrameCapacity() == 11);
    // The report says only 11 more bytes fit, so 12 bytes must fail.
    CHECK_FALSE(writer.TryAppend(MakeFrame(12, 2)));
    CHECK(writer.TryAppend(MakeFrame(11, 2)));
    CHECK(writer.RemainingFrameCapacity() == 0);
  }

  TEST_CASE("RemainingFrameCapacity goes to zero once the message count is used up") {
    std::array<std::byte, 8192> transfer{};
    PacketMessageWriter writer(
        transfer, {.max_transfer_bytes = 8192, .max_messages = 1, .alignment_bytes = 1}, 512);
    REQUIRE(writer.TryAppend(MakeFrame(64, 1)));
    CHECK(writer.RemainingFrameCapacity() == 0);
  }

  // ---------------------------------------------------------------------------
  // Decoding
  // ---------------------------------------------------------------------------

  TEST_CASE("Decoding a single frame: the data is located by 8 + DataOffset") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 1514);
    const auto frame = MakeFrame(1514, 0x22);
    WritePacketMessage(transfer, frame, kPacketInlineDataOffset, kPacketMsgHeaderBytes + 1514,
                       1514);

    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
    REQUIRE(summary.frames.size() == 1);
    CHECK(FrameMatches(summary.frames[0], 1514, 0x22));
  }

  TEST_CASE("Decoding: the data does not follow the header immediately (DataOffset above 36)") {
    // The device is free to leave a gap between the header and the data; the
    // decoder must follow DataOffset.
    constexpr std::uint32_t kGap = 16;
    constexpr std::uint32_t kFrameLength = 100;
    const std::uint32_t message_length = kPacketMsgHeaderBytes + kGap + kFrameLength;
    std::vector<std::byte> transfer(message_length);
    const auto frame = MakeFrame(kFrameLength, 0x33);
    WritePacketMessage(transfer, frame, kPacketInlineDataOffset + kGap, message_length,
                       kFrameLength);

    const auto summary = DecodeAll(transfer);
    REQUIRE(summary.frames.size() == 1);
    CHECK(FrameMatches(summary.frames[0], kFrameLength, 0x33));
  }

  TEST_CASE("Decoding: tolerates the 1-byte ZLP-avoidance padding at the tail") {
    // This tolerance is mandatory -- otherwise every full transfer reports a
    // spurious malformed frame.
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64 + 1);
    WritePacketMessage(transfer, MakeFrame(64, 0x44), kPacketInlineDataOffset,
                       kPacketMsgHeaderBytes + 64, 64);

    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
    CHECK(summary.reason == MalformedReason::kNone);
    REQUIRE(summary.frames.size() == 1);
    CHECK(summary.trailing_padding == 1);
  }

  TEST_CASE("Decoding: tolerates up to 43 bytes of trailing garbage") {
    // A tail shorter than one full header (44 bytes) is always treated as
    // padding.
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64 + 43);
    WritePacketMessage(transfer, MakeFrame(64, 0x45), kPacketInlineDataOffset,
                       kPacketMsgHeaderBytes + 64, 64);
    // Fill the tail with non-zero garbage.
    for (std::size_t i = transfer.size() - 43; i < transfer.size(); ++i) {
      transfer[i] = std::byte{0xEE};
    }
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
    REQUIRE(summary.frames.size() == 1);
    CHECK(summary.trailing_padding == 43);
  }

  TEST_CASE("Decoding an empty transfer") {
    const auto summary = DecodeAll({});
    CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
    CHECK(summary.frames.empty());
  }

  TEST_CASE("Decoding malformed input: all of it is caught without reading out of bounds") {
    SUBCASE("MessageType is not PACKET_MSG") {
      std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
      WritePacketMessage(transfer, MakeFrame(64, 1), kPacketInlineDataOffset,
                         kPacketMsgHeaderBytes + 64, 64);
      StoreLe32(transfer.data() + kMessageTypeOffset, ToRaw(MessageType::kKeepAlive));
      const auto summary = DecodeAll(transfer);
      CHECK(summary.final_outcome == ReadOutcome::kMalformed);
      CHECK(summary.reason == MalformedReason::kNotPacketMessage);
    }

    SUBCASE("MessageLength is less than 44") {
      std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
      WritePacketMessage(transfer, MakeFrame(64, 1), kPacketInlineDataOffset, 40, 64);
      const auto summary = DecodeAll(transfer);
      CHECK(summary.final_outcome == ReadOutcome::kMalformed);
      CHECK(summary.reason == MalformedReason::kMessageTooShort);
    }

    SUBCASE("MessageLength overruns the remaining buffer") {
      std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
      WritePacketMessage(transfer, MakeFrame(64, 1), kPacketInlineDataOffset, 0xFFFF'FFFFU, 64);
      const auto summary = DecodeAll(transfer);
      CHECK(summary.final_outcome == ReadOutcome::kMalformed);
      CHECK(summary.reason == MalformedReason::kMessageOverruns);
    }

    SUBCASE("DataOffset + DataLength goes out of bounds") {
      std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
      WritePacketMessage(transfer, {}, kPacketInlineDataOffset, kPacketMsgHeaderBytes + 64, 1000);
      const auto summary = DecodeAll(transfer);
      CHECK(summary.final_outcome == ReadOutcome::kMalformed);
      CHECK(summary.reason == MalformedReason::kDataOutOfBounds);
    }

    SUBCASE("a huge DataOffset must not overflow the integer") {
      std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
      WritePacketMessage(transfer, {}, 0xFFFF'FFF8U, kPacketMsgHeaderBytes + 64, 64);
      const auto summary = DecodeAll(transfer);
      CHECK(summary.final_outcome == ReadOutcome::kMalformed);
      CHECK(summary.reason == MalformedReason::kDataOutOfBounds);
    }

    SUBCASE("DataLength is less than the Ethernet header length") {
      std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
      WritePacketMessage(transfer, MakeFrame(13, 1), kPacketInlineDataOffset,
                         kPacketMsgHeaderBytes + 64, 13);
      const auto summary = DecodeAll(transfer);
      CHECK(summary.final_outcome == ReadOutcome::kMalformed);
      CHECK(summary.reason == MalformedReason::kFrameTooShort);
    }

    SUBCASE("DataLength exceeds the local single-frame limit") {
      const std::uint32_t message_length = kPacketMsgHeaderBytes + 3000;
      std::vector<std::byte> transfer(message_length);
      WritePacketMessage(transfer, {}, kPacketInlineDataOffset, message_length, 3000);
      const auto summary = DecodeAll(transfer, 2048);
      CHECK(summary.final_outcome == ReadOutcome::kMalformed);
      CHECK(summary.reason == MalformedReason::kFrameTooLong);
    }
  }

  TEST_CASE("Decoding: the frames already decoded are kept when a later one is malformed") {
    // This is what a buggy real device must not be allowed to do: the first 2
    // frames must not be lost because the 3rd message is broken.
    std::array<std::byte, 8192> transfer{};
    PacketMessageWriter writer(
        transfer, {.max_transfer_bytes = 8192, .max_messages = 3, .alignment_bytes = 1}, 512);
    REQUIRE(writer.TryAppend(MakeFrame(64, 0x01)));
    REQUIRE(writer.TryAppend(MakeFrame(64, 0x02)));
    REQUIRE(writer.TryAppend(MakeFrame(64, 0x03)));
    const std::uint32_t total = writer.Finish();

    // Corrupt the type field of the third message.
    // With 1-byte alignment the messages sit next to each other, so the third
    // message's offset is the sum of the first two.
    const std::size_t third_offset = std::size_t{2} * (kPacketMsgHeaderBytes + 64);
    StoreLe32(transfer.data() + third_offset + kMessageTypeOffset, 0xDEAD'BEEFU);

    const auto summary = DecodeAll(std::span<const std::byte>{transfer}.first(total));
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kNotPacketMessage);
    REQUIRE(summary.frames.size() == 2);
    CHECK(FrameMatches(summary.frames[0], 64, 0x01));
    CHECK(FrameMatches(summary.frames[1], 64, 0x02));
  }

  TEST_CASE("Decoding: the frame view points inside the transfer buffer, no copy") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 100);
    WritePacketMessage(transfer, MakeFrame(100, 0x77), kPacketInlineDataOffset,
                       kPacketMsgHeaderBytes + 100, 100);

    PacketMessageReader reader(transfer, kMaxFrame);
    std::span<const std::byte> frame;
    REQUIRE(reader.Next(frame) == ReadOutcome::kFrame);
    // Proof of the zero copy: the frame pointer lands at absolute offset 44
    // inside the transfer buffer.
    CHECK(frame.data() == transfer.data() + 44);
    CHECK(reader.FramesDecoded() == 1);
    CHECK(reader.BytesConsumed() == kPacketMsgHeaderBytes + 100);
  }

  TEST_CASE("MalformedReason names are complete") {
    // The names are user-facing text and follow the language, so pin it first.
    const tetherkit::testing::ScopedLanguage guard{tetherkit::Language::kEnglish};
    CHECK(MalformedReasonName(MalformedReason::kNone) == "none");
    CHECK_FALSE(MalformedReasonName(MalformedReason::kNotPacketMessage).empty());
    CHECK_FALSE(MalformedReasonName(MalformedReason::kDataOutOfBounds).empty());
    CHECK_FALSE(MalformedReasonName(MalformedReason::kFrameTooLong).empty());
  }

  // ---------------------------------------------------------------------------
  // Round-trip property tests
  // ---------------------------------------------------------------------------

  TEST_CASE("Round trip: every alignment factor and frame length is reproduced losslessly") {
    std::array<std::byte, 16384> transfer{};
    for (std::uint32_t factor = 0; factor <= kMaxPacketAlignmentFactor; ++factor) {
      const std::uint32_t alignment = 1U << factor;
      for (const std::uint32_t length : {14U, 15U, 63U, 64U, 65U, 127U, 128U, 1514U}) {
        PacketMessageWriter writer(
            transfer,
            {.max_transfer_bytes = 16384, .max_messages = 4, .alignment_bytes = alignment}, 512);

        std::uint32_t appended = 0;
        for (std::uint8_t i = 0; i < 4; ++i) {
          if (!writer.TryAppend(MakeFrame(length, static_cast<std::uint8_t>(0x10 + i)))) {
            break;
          }
          ++appended;
        }
        REQUIRE(appended > 0);
        const std::uint32_t total = writer.Finish();

        const auto summary = DecodeAll(std::span<const std::byte>{transfer}.first(total));
        CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
        REQUIRE(summary.frames.size() == appended);
        for (std::uint32_t i = 0; i < appended; ++i) {
          CHECK(FrameMatches(summary.frames[i], length, static_cast<std::uint8_t>(0x10 + i)));
        }
      }
    }
  }

}  // TEST_SUITE("rndis.packet_codec")
