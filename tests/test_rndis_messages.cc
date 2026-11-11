// Unit tests for RNDIS control-message encoding and decoding.
//
// Key coverage:
//   1. The base point for offset fields is "message start + 8" -- this is RNDIS trap number one;
//   2. RESET_MSG / RESET_CMPLT have no RequestId, so their field offsets differ from other
//      messages;
//   3. The InformationBufferLength rules for variable-length and fixed-length OIDs are opposite;
//   4. The base point of INDICATE_STATUS StatusBufferOffset is contradictory in the spec;
//   5. Normalization of device quirks (MaxPacketsPerMessage=0, alignment factor out of range,
//      MaxTransferSize too large or too small);
//   6. All malformed inputs must return an error rather than crashing or reading out of bounds.
#include "tethernet/common/byte_order.h"
#include "tethernet/rndis/messages.h"
#include "tethernet/rndis/protocol.h"

#include <doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace tethernet;         // NOLINT(google-build-using-namespace)
using namespace tethernet::rndis;  // NOLINT(google-build-using-namespace)

namespace {

/// Build an RNDIS message: write Type/Length first, then let the caller fill in fields.
std::vector<std::byte> MakeMessage(MessageType type, std::uint32_t message_length,
                                   std::uint32_t buffer_bytes = 0) {
  std::vector<std::byte> buffer(buffer_bytes == 0 ? message_length : buffer_bytes);
  StoreLe32(buffer.data() + kMessageTypeOffset, ToRaw(type));
  StoreLe32(buffer.data() + kMessageLengthOffset, message_length);
  return buffer;
}

/// Build an INITIALIZE_CMPLT like the one sent by the mainline Linux gadget (Android).
std::vector<std::byte> MakeInitializeComplete(std::uint32_t max_transfer_size = 2048,
                                              std::uint32_t max_packets = 1,
                                              std::uint32_t alignment_factor = 0,
                                              std::uint32_t status = 0,
                                              std::uint32_t medium = 0,
                                              std::uint32_t device_flags = 1) {
  auto buffer = MakeMessage(MessageType::kInitializeComplete, kInitializeCmpltBytes);
  std::byte* base = buffer.data();
  StoreLe32(base + kInitializeCmpltRequestIdOffset, 0x1234'5678U);
  StoreLe32(base + kInitializeCmpltStatusOffset, status);
  StoreLe32(base + kInitializeCmpltMajorVersionOffset, 1);
  StoreLe32(base + kInitializeCmpltMinorVersionOffset, 0);
  StoreLe32(base + kInitializeCmpltDeviceFlagsOffset, device_flags);
  StoreLe32(base + kInitializeCmpltMediumOffset, medium);
  StoreLe32(base + kInitializeCmpltMaxPacketsPerMessageOffset, max_packets);
  StoreLe32(base + kInitializeCmpltMaxTransferSizeOffset, max_transfer_size);
  StoreLe32(base + kInitializeCmpltPacketAlignmentFactorOffset, alignment_factor);
  return buffer;
}

}  // namespace

TEST_SUITE("rndis.messages") {
  // ---------------------------------------------------------------------------
  // Constant self-checks
  // ---------------------------------------------------------------------------

  TEST_CASE("protocol constants match the spec") {
    CHECK(ToRaw(MessageType::kPacket) == 0x0000'0001U);
    CHECK(ToRaw(MessageType::kInitialize) == 0x0000'0002U);
    CHECK(ToRaw(MessageType::kInitializeComplete) == 0x8000'0002U);
    CHECK(ToRaw(MessageType::kHalt) == 0x0000'0003U);
    CHECK(ToRaw(MessageType::kQueryComplete) == 0x8000'0004U);
    CHECK(ToRaw(MessageType::kResetComplete) == 0x8000'0006U);
    CHECK(ToRaw(MessageType::kIndicateStatus) == 0x0000'0007U);
    CHECK(ToRaw(MessageType::kKeepAliveComplete) == 0x8000'0008U);

    // A few status codes that are easy to misremember.
    CHECK(ToRaw(StatusCode::kNotSupported) == 0xC000'00BBU);
    CHECK(ToRaw(StatusCode::kInvalidLength) == 0xC001'0014U);
    CHECK(ToRaw(StatusCode::kInvalidData) == 0xC001'0015U);
    CHECK(ToRaw(StatusCode::kInvalidOid) == 0xC001'0017U);
    CHECK(ToRaw(StatusCode::kMediaConnect) == 0x4001'000BU);
    CHECK(ToRaw(StatusCode::kMediaDisconnect) == 0x4001'000CU);

    CHECK(ToRaw(Oid::kGenCurrentPacketFilter) == 0x0001'010EU);
    CHECK(ToRaw(Oid::kGenMediaConnectStatus) == 0x0001'0114U);
    CHECK(ToRaw(Oid::kEthernetPermanentAddress) == 0x0101'0101U);
    CHECK(ToRaw(Oid::kGenPhysicalMedium) == 0x0001'0202U);

    CHECK(kDefaultPacketFilter == 0x0000'002DU);
    CHECK(kHardHeaderBytes == 58);
  }

  TEST_CASE("offset base-point constants: the inline DataOffset value must be 36, not 44") {
    // This is RNDIS implementation trap number one: the base point for offset fields is
    // message start + 8.
    CHECK(kPacketInlineDataOffset == 36);
    CHECK(kPacketMsgHeaderBytes == 44);
    CHECK(kOffsetFieldBase == 8);
    CHECK(kQuerySetInlineInfoOffset == 20);
    CHECK(kQueryCmpltInlineInfoOffset == 16);
  }

  TEST_CASE("MaxTransferSize derivation matches Linux") {
    CHECK(MaxTransferSizeFor(1500, 512) == 2048);  // high speed
    CHECK(MaxTransferSizeFor(1500, 64) == 1600);   // full speed
    CHECK(HardMtuFor(1500) == 1558);
  }

  TEST_CASE("status codes and OID names are lookable") {
    CHECK(StatusName(0xC000'00BBU) == "RNDIS_STATUS_NOT_SUPPORTED");
    CHECK(StatusName(0x4001'000BU) == "RNDIS_STATUS_MEDIA_CONNECT");
    CHECK(StatusName(0xDEAD'BEEFU).empty());
    CHECK(OidName(0x0001'010EU) == "OID_GEN_CURRENT_PACKET_FILTER");
    CHECK(OidName(0xDEAD'BEEFU).empty());
    CHECK(MessageTypeName(0x8000'0002U) == "REMOTE_NDIS_INITIALIZE_CMPLT");
    // The CONDIS message family must be recognized (even if unsupported), not reported as
    // "unknown".
    CHECK(MessageTypeName(0x0000'8001U) == "RNDIS_MSG_MP_CREATE_VC");
    CHECK(IsCondis(0x0000'8001U));
    CHECK_FALSE(IsCondis(ToRaw(MessageType::kPacket)));
  }

  TEST_CASE("distinguishing variable-length and fixed-length OIDs") {
    CHECK(IsVariableLengthOid(Oid::kGenSupportedList));
    CHECK(IsVariableLengthOid(Oid::kGenVendorDescription));
    CHECK_FALSE(IsVariableLengthOid(Oid::kEthernetPermanentAddress));
    CHECK_FALSE(IsVariableLengthOid(Oid::kGenCurrentPacketFilter));
  }

// ---------------------------------------------------------------------------
// INITIALIZE
// ---------------------------------------------------------------------------

TEST_CASE("encode INITIALIZE_MSG") {
  std::array<std::byte, 64> buffer{};
  const auto written = Encode(
      InitializeRequest{.request_id = 7, .max_transfer_size = 2048}, buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kInitializeMsgBytes);

  CHECK(LoadLe32(buffer.data() + kMessageTypeOffset) == ToRaw(MessageType::kInitialize));
  CHECK(LoadLe32(buffer.data() + kMessageLengthOffset) == 24);
  CHECK(LoadLe32(buffer.data() + kInitializeRequestIdOffset) == 7);
  CHECK(LoadLe32(buffer.data() + kInitializeMajorVersionOffset) == 1);
  CHECK(LoadLe32(buffer.data() + kInitializeMinorVersionOffset) == 0);
  CHECK(LoadLe32(buffer.data() + kInitializeMaxTransferSizeOffset) == 2048);
}

TEST_CASE("encoding INITIALIZE_MSG reports an error when the buffer is too small") {
  std::array<std::byte, 16> too_small{};
  CHECK_FALSE(Encode(InitializeRequest{}, too_small).has_value());
}

TEST_CASE("decode INITIALIZE_CMPLT") {
  const auto buffer = MakeInitializeComplete();
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK(complete->request_id == 0x1234'5678U);
  CHECK(complete->status == 0);
  CHECK(complete->major_version == 1);
  CHECK(complete->max_transfer_size == 2048);
  CHECK(complete->max_packets_per_message == 1);
}

TEST_CASE("decode INITIALIZE_CMPLT: device returns a failure status") {
  const auto buffer = MakeInitializeComplete(2048, 1, 0, ToRaw(StatusCode::kFailure));
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE_FALSE(complete.has_value());
  CHECK(complete.error().Code() == static_cast<std::int64_t>(ToRaw(StatusCode::kFailure)));
  // The error string should carry the symbolic status name for troubleshooting.
  CHECK(complete.error().ToString().find("RNDIS_STATUS_FAILURE") != std::string::npos);
}

TEST_CASE("decode INITIALIZE_CMPLT: malformed input always errors without crashing") {
  SUBCASE("buffer too short") {
    std::array<std::byte, 8> tiny{};
    StoreLe32(tiny.data(), ToRaw(MessageType::kInitializeComplete));
    StoreLe32(tiny.data() + 4, kInitializeCmpltBytes);
    CHECK_FALSE(DecodeInitializeComplete(tiny).has_value());
  }
  SUBCASE("wrong message type") {
    auto buffer = MakeInitializeComplete();
    StoreLe32(buffer.data() + kMessageTypeOffset, ToRaw(MessageType::kQueryComplete));
    CHECK_FALSE(DecodeInitializeComplete(buffer).has_value());
  }
  SUBCASE("MessageLength exceeds buffer") {
    auto buffer = MakeInitializeComplete();
    StoreLe32(buffer.data() + kMessageLengthOffset, 0xFFFF'FFFFU);
    CHECK_FALSE(DecodeInitializeComplete(buffer).has_value());
  }
  SUBCASE("MessageLength smaller than the minimum") {
    auto buffer = MakeInitializeComplete();
    StoreLe32(buffer.data() + kMessageLengthOffset, 12);
    CHECK_FALSE(DecodeInitializeComplete(buffer).has_value());
  }
  SUBCASE("empty buffer") {
    CHECK_FALSE(DecodeInitializeComplete({}).has_value());
  }
}

// ---------------------------------------------------------------------------
// Negotiation and device quirk normalization
// ---------------------------------------------------------------------------

TEST_CASE("negotiation: typical parameters of the mainline Android gadget") {
  const auto buffer = MakeInitializeComplete(2048, 1, 0);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());

  const auto params = Negotiate(*complete, 1500, 65536);
  REQUIRE(params.has_value());
  CHECK(params->mtu == 1500);
  CHECK(params->device_max_transfer_size == 2048);
  CHECK(params->max_packets_per_message == 1);
  CHECK(params->tx_alignment_bytes == 1);  // factor 0 -> 1-byte alignment
}

TEST_CASE("negotiation quirk: MaxPacketsPerMessage 0 is treated as 1") {
  const auto buffer = MakeInitializeComplete(2048, 0, 0);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  const auto params = Negotiate(*complete, 1500, 65536);
  REQUIRE(params.has_value());
  CHECK(params->max_packets_per_message == 1);
}

TEST_CASE("negotiation quirk: out-of-range PacketAlignmentFactor is clamped to 7") {
  const auto buffer = MakeInitializeComplete(2048, 1, 31);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  const auto params = Negotiate(*complete, 1500, 65536);
  REQUIRE(params.has_value());
  // Without clamping, 1u << 31 would yield an absurd alignment value.
  CHECK(params->tx_alignment_bytes == 128);
}

TEST_CASE("negotiation quirk: Qualcomm aggregation patch reports MaxPacketsPerMessage=3") {
  const auto buffer = MakeInitializeComplete(3 * 1580, 3, 2);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  const auto params = Negotiate(*complete, 1500, 65536);
  REQUIRE(params.has_value());
  CHECK(params->max_packets_per_message == 3);
  CHECK(params->tx_alignment_bytes == 4);  // 1 << 2
}

TEST_CASE(
    "negotiation quirk: MTU is lowered when device MaxTransferSize is smaller than a full frame") {
  // Measured case: HTC Diamond reports 1536, smaller than hard_mtu(1558).
  const auto buffer = MakeInitializeComplete(1536, 1, 0);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  const auto params = Negotiate(*complete, 1500, 65536);
  REQUIRE(params.has_value());
  CHECK(params->mtu == 1536 - kHardHeaderBytes);  // 1478
}

TEST_CASE("negotiation quirk: WinCE reports a 16KB giant frame, clamped to the host limit") {
  const auto buffer = MakeInitializeComplete(16384, 1, 0);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  const auto params = Negotiate(*complete, 1500, 4096);
  REQUIRE(params.has_value());
  CHECK(params->device_max_transfer_size == 4096);
  CHECK(params->mtu == 1500);  // Still fits a full frame; MTU unchanged
}

TEST_CASE("negotiation fails: MaxTransferSize cannot fit the header") {
  const auto buffer = MakeInitializeComplete(40, 1, 0);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK_FALSE(Negotiate(*complete, 1500, 65536).has_value());
}

TEST_CASE("negotiation fails: non-Ethernet medium") {
  const auto buffer =
      MakeInitializeComplete(2048, 1, 0, 0, static_cast<std::uint32_t>(Medium::kWirelessLan));
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK_FALSE(Negotiate(*complete, 1500, 65536).has_value());
}

TEST_CASE("negotiation fails: connection-oriented device") {
  const auto buffer = MakeInitializeComplete(
      2048, 1, 0, 0, 0, static_cast<std::uint32_t>(DeviceFlags::kConnectionOriented));
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK_FALSE(Negotiate(*complete, 1500, 65536).has_value());
}

// ---------------------------------------------------------------------------
// QUERY / SET
// ---------------------------------------------------------------------------

TEST_CASE(
    "encode QUERY_MSG: fixed-length OIDs must carry a placeholder length and trailing zero bytes") {
  std::array<std::byte, 128> buffer{};
  const auto written = Encode(QueryRequest{.request_id = 3,
                                           .oid = Oid::kEthernetPermanentAddress,
                                           .expected_response_bytes = 48},
                              buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kQuerySetHeaderBytes + 48);

  CHECK(LoadLe32(buffer.data() + kQuerySetOidOffset) == ToRaw(Oid::kEthernetPermanentAddress));
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferLengthOffset) == 48);
  // The base point is message start + 8, so the inline data offset value is 20, not 28.
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferOffsetOffset) == 20);
  CHECK(LoadLe32(buffer.data() + kQuerySetDeviceVcHandleOffset) == 0);
}

TEST_CASE("encode QUERY_MSG: variable-length OIDs must have length 0") {
  // Undocumented ActiveSync behavior: passing a non-zero length for a variable-length OID is
  // rejected.
  std::array<std::byte, 128> buffer{};
  const auto written = Encode(QueryRequest{.request_id = 4,
                                           .oid = Oid::kGenSupportedList,
                                           .expected_response_bytes = 256},
                              buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kQuerySetHeaderBytes);
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferLengthOffset) == 0);
  // The offset must also be 0 when the length is 0.
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferOffsetOffset) == 0);
}

TEST_CASE("decode QUERY_CMPLT: locate the information buffer using the +8 base point") {
  auto buffer = MakeMessage(MessageType::kQueryComplete, kQueryCmpltHeaderBytes + 6);
  StoreLe32(buffer.data() + kQueryCmpltRequestIdOffset, 9);
  StoreLe32(buffer.data() + kQueryCmpltStatusOffset, 0);
  StoreLe32(buffer.data() + kQueryCmpltInfoBufferLengthOffset, 6);
  StoreLe32(buffer.data() + kQueryCmpltInfoBufferOffsetOffset, kQueryCmpltInlineInfoOffset);
  const std::array<std::uint8_t, 6> mac{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  for (std::size_t i = 0; i < mac.size(); ++i) {
    buffer[kQueryCmpltHeaderBytes + i] = std::byte{mac[i]};
  }

  const auto complete = DecodeQueryComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK(complete->request_id == 9);
  REQUIRE(complete->information.size() == 6);

  const auto parsed = ParseMac(complete->information);
  REQUIRE(parsed.has_value());
  CHECK(*parsed == MacAddress{0x02, 0x11, 0x22, 0x33, 0x44, 0x55});
}

TEST_CASE("decode QUERY_CMPLT: device returning not-supported is not a fatal error") {
  // Returning NOT_SUPPORTED for an optional OID (e.g. OID_GEN_PHYSICAL_MEDIUM) is normal;
  // decoding must succeed and hand the status to the caller.
  auto buffer = MakeMessage(MessageType::kQueryComplete, kQueryCmpltHeaderBytes);
  StoreLe32(buffer.data() + kQueryCmpltStatusOffset, ToRaw(StatusCode::kNotSupported));
  const auto complete = DecodeQueryComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK(complete->status == ToRaw(StatusCode::kNotSupported));
  CHECK(complete->information.empty());
}

TEST_CASE("decode QUERY_CMPLT: out-of-bounds information buffer must error") {
  auto buffer = MakeMessage(MessageType::kQueryComplete, kQueryCmpltHeaderBytes + 4);
  StoreLe32(buffer.data() + kQueryCmpltStatusOffset, 0);
  StoreLe32(buffer.data() + kQueryCmpltInfoBufferLengthOffset, 1024);  // lying
  StoreLe32(buffer.data() + kQueryCmpltInfoBufferOffsetOffset, kQueryCmpltInlineInfoOffset);
  CHECK_FALSE(DecodeQueryComplete(buffer).has_value());
}

TEST_CASE("encode SET_MSG and set one LE32") {
  std::array<std::byte, 64> buffer{};
  const auto written = EncodeSetUint32(11, Oid::kGenCurrentPacketFilter, kDefaultPacketFilter,
                                       buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kQuerySetHeaderBytes + 4);
  CHECK(LoadLe32(buffer.data() + kMessageTypeOffset) == ToRaw(MessageType::kSet));
  CHECK(LoadLe32(buffer.data() + kQuerySetOidOffset) == ToRaw(Oid::kGenCurrentPacketFilter));
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferLengthOffset) == 4);
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferOffsetOffset) == 20);
  CHECK(LoadLe32(buffer.data() + kQuerySetHeaderBytes) == kDefaultPacketFilter);
}

TEST_CASE("decode SET_CMPLT") {
  auto buffer = MakeMessage(MessageType::kSetComplete, kSetCmpltBytes);
  StoreLe32(buffer.data() + kSetCmpltRequestIdOffset, 11);
  StoreLe32(buffer.data() + kSetCmpltStatusOffset, 0);
  const auto complete = DecodeSetComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK(complete->request_id == 11);
  CHECK(complete->status == 0);
}

// ---------------------------------------------------------------------------
// RESET -- field offsets differ from other messages
// ---------------------------------------------------------------------------

TEST_CASE("encode RESET_MSG: offset 8 is Reserved, not RequestId") {
  std::array<std::byte, 32> buffer{};
  buffer[kResetReservedOffset] = std::byte{0xFF};  // Pre-fill to verify it gets zeroed
  const auto written = EncodeReset(buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kResetMsgBytes);
  CHECK(LoadLe32(buffer.data() + kMessageTypeOffset) == ToRaw(MessageType::kReset));
  CHECK(LoadLe32(buffer.data() + kMessageLengthOffset) == 12);
  CHECK(LoadLe32(buffer.data() + kResetReservedOffset) == 0);
}

TEST_CASE("decode RESET_CMPLT: Status is at offset 8, not 12") {
  auto buffer = MakeMessage(MessageType::kResetComplete, kResetCmpltBytes);
  StoreLe32(buffer.data() + kResetCmpltStatusOffset, 0);
  StoreLe32(buffer.data() + kResetCmpltAddressingResetOffset, 1);

  const auto complete = DecodeResetComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK(complete->status == 0);
  // Non-zero AddressingReset -> the host must re-send SET OID_GEN_CURRENT_PACKET_FILTER.
  CHECK(complete->addressing_reset);
}

TEST_CASE("decode RESET_CMPLT: no replay needed when AddressingReset is 0") {
  auto buffer = MakeMessage(MessageType::kResetComplete, kResetCmpltBytes);
  StoreLe32(buffer.data() + kResetCmpltAddressingResetOffset, 0);
  const auto complete = DecodeResetComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK_FALSE(complete->addressing_reset);
}

// ---------------------------------------------------------------------------
// KEEPALIVE
// ---------------------------------------------------------------------------

TEST_CASE("KEEPALIVE round trip") {
  std::array<std::byte, 32> request{};
  const auto written = EncodeKeepAlive(42, request);
  REQUIRE(written.has_value());
  CHECK(written.value() == kKeepAliveMsgBytes);
  CHECK(LoadLe32(request.data() + kKeepAliveRequestIdOffset) == 42);

  auto response = MakeMessage(MessageType::kKeepAliveComplete, kKeepAliveCmpltBytes);
  StoreLe32(response.data() + kKeepAliveCmpltRequestIdOffset, 42);
  const auto complete = DecodeKeepAliveComplete(response);
  REQUIRE(complete.has_value());
  CHECK(complete->request_id == 42);
}

TEST_CASE("host must also be able to encode KEEPALIVE_CMPLT (device may initiate keep-alive)") {
  std::array<std::byte, 32> buffer{};
  const auto written = EncodeKeepAliveComplete(77, ToRaw(StatusCode::kSuccess), buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kKeepAliveCmpltBytes);
  CHECK(LoadLe32(buffer.data() + kMessageTypeOffset) == ToRaw(MessageType::kKeepAliveComplete));
  CHECK(LoadLe32(buffer.data() + kKeepAliveCmpltRequestIdOffset) == 77);
  CHECK(LoadLe32(buffer.data() + kKeepAliveCmpltStatusOffset) == 0);
}

// ---------------------------------------------------------------------------
// INDICATE_STATUS -- tolerant handling of the contradictory base point
// ---------------------------------------------------------------------------

TEST_CASE("decode INDICATE_STATUS: MEDIA_CONNECT with no payload") {
  auto buffer = MakeMessage(MessageType::kIndicateStatus, kIndicateStatusHeaderBytes);
  StoreLe32(buffer.data() + kIndicateStatusStatusOffset, ToRaw(StatusCode::kMediaConnect));
  StoreLe32(buffer.data() + kIndicateStatusBufferLengthOffset, 0);

  const auto indication = DecodeIndicateStatus(buffer);
  REQUIRE(indication.has_value());
  CHECK(indication->status == ToRaw(StatusCode::kMediaConnect));
  CHECK(indication->status_buffer.empty());
  CHECK_FALSE(indication->has_diagnostic_info);
}

TEST_CASE("decode INDICATE_STATUS: interpret status buffer using the +8 base point") {
  const std::uint32_t total = kIndicateStatusHeaderBytes + kDiagnosticInfoBytes;
  auto buffer = MakeMessage(MessageType::kIndicateStatus, total);
  StoreLe32(buffer.data() + kIndicateStatusStatusOffset, ToRaw(StatusCode::kInvalidData));
  StoreLe32(buffer.data() + kIndicateStatusBufferLengthOffset, kDiagnosticInfoBytes);
  // +8 base point: 20 - 8 = 12
  StoreLe32(buffer.data() + kIndicateStatusBufferOffsetOffset,
            kIndicateStatusHeaderBytes - kOffsetFieldBase);
  StoreLe32(buffer.data() + kIndicateStatusHeaderBytes + kDiagnosticInfoDiagStatusOffset,
            ToRaw(StatusCode::kInvalidData));
  StoreLe32(buffer.data() + kIndicateStatusHeaderBytes + kDiagnosticInfoErrorOffsetOffset, 36);

  const auto indication = DecodeIndicateStatus(buffer);
  REQUIRE(indication.has_value());
  CHECK(indication->status_buffer.size() == kDiagnosticInfoBytes);
  REQUIRE(indication->has_diagnostic_info);
  CHECK(indication->diagnostic_status == ToRaw(StatusCode::kInvalidData));
  CHECK(indication->diagnostic_error_offset == 36);
}

TEST_CASE("decode INDICATE_STATUS: interpreting with the message-start base point also works") {
  // Some devices implement the MS spec literally (base point = message start).
  const std::uint32_t total = kIndicateStatusHeaderBytes + kDiagnosticInfoBytes;
  auto buffer = MakeMessage(MessageType::kIndicateStatus, total);
  StoreLe32(buffer.data() + kIndicateStatusStatusOffset, ToRaw(StatusCode::kInvalidData));
  StoreLe32(buffer.data() + kIndicateStatusBufferLengthOffset, kDiagnosticInfoBytes);
  // Base point = message start: write 20 directly
  StoreLe32(buffer.data() + kIndicateStatusBufferOffsetOffset, kIndicateStatusHeaderBytes);
  StoreLe32(buffer.data() + kIndicateStatusHeaderBytes + kDiagnosticInfoDiagStatusOffset, 0xAAU);

  const auto indication = DecodeIndicateStatus(buffer);
  REQUIRE(indication.has_value());
  // The +8 interpretation would go out of bounds (8+20+8=36 > 28), so it falls back to the
  // literal interpretation.
  REQUIRE(indication->status_buffer.size() == kDiagnosticInfoBytes);
  CHECK(indication->diagnostic_status == 0xAAU);
}

TEST_CASE("decode INDICATE_STATUS: still succeeds when both base points are out of bounds") {
  // Key behavior: the status itself is useful (it may be MEDIA_DISCONNECT), so we must not
  // declare the link dead just because an optional payload cannot be parsed.
  auto buffer = MakeMessage(MessageType::kIndicateStatus, kIndicateStatusHeaderBytes);
  StoreLe32(buffer.data() + kIndicateStatusStatusOffset, ToRaw(StatusCode::kMediaDisconnect));
  StoreLe32(buffer.data() + kIndicateStatusBufferLengthOffset, 4096);  // lying
  StoreLe32(buffer.data() + kIndicateStatusBufferOffsetOffset, 12);

  const auto indication = DecodeIndicateStatus(buffer);
  REQUIRE(indication.has_value());
  CHECK(indication->status == ToRaw(StatusCode::kMediaDisconnect));
  CHECK(indication->status_buffer.empty());
}

TEST_CASE("decode INDICATE_STATUS: wrong type errors") {
  auto buffer = MakeMessage(MessageType::kQueryComplete, kIndicateStatusHeaderBytes);
  CHECK_FALSE(DecodeIndicateStatus(buffer).has_value());
}

// ---------------------------------------------------------------------------
// OID payload parsing
// ---------------------------------------------------------------------------

TEST_CASE("ParseUint32 / ParseCounter / ParseMac") {
  std::array<std::byte, 8> payload{};
  StoreLe32(payload.data(), 0x0000'002DU);

  const auto value = ParseUint32(std::span<const std::byte>{payload}.first(4));
  REQUIRE(value.has_value());
  CHECK(*value == 0x0000'002DU);

  SUBCASE("counter OID returns 4 bytes") {
    const auto counter = ParseCounter(std::span<const std::byte>{payload}.first(4));
    REQUIRE(counter.has_value());
    CHECK(*counter == 0x0000'002DU);
  }
  SUBCASE("counter OID returns 8 bytes") {
    StoreLe64(payload.data(), 0x0000'0001'0000'002DULL);
    const auto counter = ParseCounter(payload);
    REQUIRE(counter.has_value());
    CHECK(*counter == 0x0000'0001'0000'002DULL);
  }
  SUBCASE("counter OID returning 2 bytes must error") {
    CHECK_FALSE(ParseCounter(std::span<const std::byte>{payload}.first(2)).has_value());
  }
  SUBCASE("insufficient MAC length must error") {
    CHECK_FALSE(ParseMac(std::span<const std::byte>{payload}.first(4)).has_value());
  }
}

TEST_CASE("FormatMac outputs the canonical format") {
  const MacAddress mac{0x02, 0x00, 0xAB, 0xCD, 0xEF, 0x01};
  const auto text = FormatMac(mac);
  CHECK(std::string_view{text.data()} == "02:00:ab:cd:ef:01");
}

// ---------------------------------------------------------------------------
// USB descriptor signature recognition
// ---------------------------------------------------------------------------

TEST_CASE("recognize the four known RNDIS communication-class interface signatures") {
  CHECK(IsRndisControlSignature({0x02, 0x02, 0xFF}));  // standard MS RNDIS
  CHECK(IsRndisControlSignature({0xEF, 0x01, 0x01}));  // ActiveSync
  CHECK(IsRndisControlSignature({0xE0, 0x01, 0x03}));  // wireless RNDIS (phone tethering)
  CHECK(IsRndisControlSignature({0xEF, 0x04, 0x01}));  // Novatel variant

  // A real CDC ACM modem (protocol=0x01) must not be mistaken for RNDIS.
  CHECK_FALSE(IsRndisControlSignature({0x02, 0x02, 0x01}));
  // CDC ECM is not RNDIS either.
  CHECK_FALSE(IsRndisControlSignature({0x02, 0x06, 0x00}));
  CHECK(kDataSignature == InterfaceSignature{0x0A, 0x00, 0x00});
}

}  // TEST_SUITE("rndis.messages")
