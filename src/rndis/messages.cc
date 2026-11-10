#include "tetherkit/rndis/messages.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>

#include "tetherkit/common/byte_order.h"
#include "tetherkit/common/i18n.h"
#include "tetherkit/common/logging.h"

namespace tetherkit::rndis {
namespace {

/// Build an error carrying the RNDIS status code's name.
///
/// The status-code-to-name map lives only in protocol.cc (tk_common knows
/// nothing about RNDIS), so the name is concatenated straight into the context
/// string here and the Error itself keeps only the raw value.
Error MakeStatusError(std::uint32_t status, std::string_view what) {
  const std::string_view name = StatusName(status);
  if (name.empty()) {
    return Error::FromRndisStatus(status, std::string{what});
  }
  return Error::FromRndisStatus(status, Tr(Msg::kRndisStatusSuffix, what, name));
}

/// Check that the buffer has at least `needed` bytes.
Status RequireBytes(std::span<const std::byte> buffer, std::size_t needed, std::string_view what) {
  if (buffer.size() < needed) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kRndisBufferTooShort, what, needed, buffer.size())));
  }
  return Ok();
}

/// Check the capacity of the buffer about to be written.
Status RequireCapacity(std::span<std::byte> buffer, std::size_t needed, std::string_view what) {
  if (buffer.size() < needed) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kRndisEncodeBufferTooSmall, what, needed, buffer.size())));
  }
  return Ok();
}

/// Validate a completion message: right type, self-consistent length,
/// successful status.
Status ValidateCompletion(std::span<const std::byte> buffer, MessageType expected_type,
                          std::uint32_t minimum_bytes, std::string_view what) {
  TETHERKIT_RETURN_IF_ERROR(RequireBytes(buffer, minimum_bytes, what));

  const std::uint32_t message_type = LoadLe32(buffer.data() + kMessageTypeOffset);
  if (message_type != ToRaw(expected_type)) {
    const std::string_view actual = MessageTypeName(message_type);
    return std::unexpected(Error::Generic(Tr(
        Msg::kRndisUnexpectedMessageType, what,
        actual.empty() ? Text(Msg::kRndisUnknownMessageType) : actual, message_type)));
  }

  const std::uint32_t message_length = LoadLe32(buffer.data() + kMessageLengthOffset);
  if (message_length < minimum_bytes || message_length > buffer.size()) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisInconsistentMessageLength, what,
                                             message_length, buffer.size(), minimum_bytes)));
  }
  return Ok();
}

/// Write the common header (Type + Length).
void WriteHeader(std::span<std::byte> buffer, MessageType type, std::uint32_t message_length) {
  StoreLe32(buffer.data() + kMessageTypeOffset, ToRaw(type));
  StoreLe32(buffer.data() + kMessageLengthOffset, message_length);
}

/// Resolve an inline buffer described by "offset + length", with its base
/// point at message start + 8.
///
/// Returns a view into `buffer`. An out-of-range offset or length yields an
/// error.
Result<std::span<const std::byte>> ResolveInlineBuffer(std::span<const std::byte> buffer,
                                                       std::uint32_t message_length,
                                                       std::uint32_t relative_offset,
                                                       std::uint32_t length,
                                                       std::string_view what) {
  if (length == 0) {
    return std::span<const std::byte>{};
  }
  // Absolute offset = 8 + field value; see rule 2 in the protocol.h file
  // header.
  const std::uint64_t absolute_offset =
      static_cast<std::uint64_t>(kOffsetFieldBase) + relative_offset;
  const std::uint64_t end = absolute_offset + length;
  if (end > message_length) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisInlineBufferOutOfBounds, what,
                                             absolute_offset, relative_offset, length,
                                             message_length)));
  }
  return buffer.subspan(static_cast<std::size_t>(absolute_offset), length);
}

}  // namespace

std::array<char, 18> FormatMac(const MacAddress& mac) noexcept {
  std::array<char, 18> text{};
  std::snprintf(text.data(), text.size(), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
  return text;
}

// =============================================================================
// Common header
// =============================================================================

Result<MessageHeader> DecodeMessageHeader(std::span<const std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(
      RequireBytes(buffer, kMessageHeaderBytes, Text(Msg::kRndisWhatMessageHeader)));
  return MessageHeader{
      .message_type = LoadLe32(buffer.data() + kMessageTypeOffset),
      .message_length = LoadLe32(buffer.data() + kMessageLengthOffset),
  };
}

// =============================================================================
// INITIALIZE
// =============================================================================

Result<std::uint32_t> Encode(const InitializeRequest& request, std::span<std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(
      RequireCapacity(buffer, kInitializeMsgBytes, "REMOTE_NDIS_INITIALIZE_MSG"));

  WriteHeader(buffer, MessageType::kInitialize, kInitializeMsgBytes);
  StoreLe32(buffer.data() + kInitializeRequestIdOffset, request.request_id);
  StoreLe32(buffer.data() + kInitializeMajorVersionOffset, request.major_version);
  StoreLe32(buffer.data() + kInitializeMinorVersionOffset, request.minor_version);
  StoreLe32(buffer.data() + kInitializeMaxTransferSizeOffset, request.max_transfer_size);
  return kInitializeMsgBytes;
}

Result<InitializeComplete> DecodeInitializeComplete(std::span<const std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(ValidateCompletion(buffer, MessageType::kInitializeComplete,
                                               kInitializeCmpltBytes,
                                               "REMOTE_NDIS_INITIALIZE_CMPLT"));

  const std::byte* base = buffer.data();
  InitializeComplete complete{
      .request_id = LoadLe32(base + kInitializeCmpltRequestIdOffset),
      .status = LoadLe32(base + kInitializeCmpltStatusOffset),
      .major_version = LoadLe32(base + kInitializeCmpltMajorVersionOffset),
      .minor_version = LoadLe32(base + kInitializeCmpltMinorVersionOffset),
      .device_flags = LoadLe32(base + kInitializeCmpltDeviceFlagsOffset),
      .medium = LoadLe32(base + kInitializeCmpltMediumOffset),
      .max_packets_per_message = LoadLe32(base + kInitializeCmpltMaxPacketsPerMessageOffset),
      .max_transfer_size = LoadLe32(base + kInitializeCmpltMaxTransferSizeOffset),
      .packet_alignment_factor = LoadLe32(base + kInitializeCmpltPacketAlignmentFactorOffset),
      .af_list_offset = LoadLe32(base + kInitializeCmpltAfListOffsetOffset),
      .af_list_size = LoadLe32(base + kInitializeCmpltAfListSizeOffset),
  };

  if (complete.status != ToRaw(StatusCode::kSuccess)) {
    return std::unexpected(MakeStatusError(complete.status, Tr(Msg::kRndisInitRejected)));
  }
  return complete;
}

Result<NegotiatedParameters> Negotiate(const InitializeComplete& complete,
                                       std::uint32_t requested_mtu,
                                       std::uint32_t host_transfer_size_limit) {
  // ---- The medium must be Ethernet ----
  if (complete.medium != ToRaw(Medium::kEthernet)) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisUnsupportedMedium, complete.medium)));
  }

  // ---- Connection-oriented devices are not supported ----
  const bool connectionless =
      (complete.device_flags & static_cast<std::uint32_t>(DeviceFlags::kConnectionless)) != 0;
  const bool connection_oriented =
      (complete.device_flags & static_cast<std::uint32_t>(DeviceFlags::kConnectionOriented)) != 0;
  if (connection_oriented && !connectionless) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisConnectionOriented)));
  }
  if (!connectionless) {
    // Some devices set neither bit. Be lenient: continue as connectionless
    // and only warn.
    TETHERKIT_WARN_TR(Msg::kRndisNoConnectionlessFlag, complete.device_flags);
  }

  // ---- Version ----
  if (complete.major_version != kMajorVersion) {
    return std::unexpected(Error::Generic(
        Tr(Msg::kRndisUnsupportedMajorVersion, complete.major_version, kMajorVersion)));
  }

  NegotiatedParameters params;
  params.connectionless = true;

  // ---- MaxPacketsPerMessage: treat 0 as 1 ----
  // Mainline Linux gadgets report 1; Android kernels with the Qualcomm
  // upstream-aggregation patch report 3 or more.
  params.max_packets_per_message = std::max<std::uint32_t>(1, complete.max_packets_per_message);

  // ---- PacketAlignmentFactor: clamp to 7 ----
  // Anything above 7 is a protocol violation. Without the clamp, 1u << factor
  // overflows or produces an absurd padding length.
  std::uint32_t alignment_factor = complete.packet_alignment_factor;
  if (alignment_factor > kMaxPacketAlignmentFactor) {
    TETHERKIT_WARN_TR(Msg::kRndisAlignmentFactorClamped, alignment_factor,
                      kMaxPacketAlignmentFactor);
    alignment_factor = kMaxPacketAlignmentFactor;
  }
  params.tx_alignment_bytes = 1U << alignment_factor;

  // ---- MaxTransferSize: clamp in both directions ----
  std::uint32_t device_limit = complete.max_transfer_size;
  if (device_limit <= kHardHeaderBytes) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kRndisMaxTransferTooSmall, device_limit, kHardHeaderBytes)));
  }

  // Upper clamp: some WinCE / Windows Mobile devices advertise giant 8KB or
  // 16KB frame caps that are meaningless at this link rate and would only make
  // us allocate huge transfer buffers. Do not follow the device blindly.
  if (device_limit > host_transfer_size_limit) {
    TETHERKIT_INFO_TR(Msg::kRndisMaxTransferClampedToHost, device_limit,
                      host_transfer_size_limit);
    device_limit = host_transfer_size_limit;
  }
  params.device_max_transfer_size = device_limit;

  // Lower clamp: if the device cannot hold a full frame computed from
  // requested_mtu, back out a workable MTU instead.
  // Measured case: an HTC Diamond reports 1536, less than hard_mtu(1558), so
  // the MTU has to drop to 1478.
  params.mtu = requested_mtu;
  const std::uint32_t hard_mtu = HardMtuFor(requested_mtu);
  if (device_limit < hard_mtu) {
    params.mtu = device_limit - kHardHeaderBytes;
    TETHERKIT_WARN_TR(Msg::kRndisMtuLoweredForTransferSize, device_limit, hard_mtu, requested_mtu,
                      params.mtu);
  }

  return params;
}

// =============================================================================
// HALT
// =============================================================================

Result<std::uint32_t> EncodeHalt(std::uint32_t request_id, std::span<std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(RequireCapacity(buffer, kHaltMsgBytes, "REMOTE_NDIS_HALT_MSG"));
  WriteHeader(buffer, MessageType::kHalt, kHaltMsgBytes);
  StoreLe32(buffer.data() + kHaltRequestIdOffset, request_id);
  return kHaltMsgBytes;
}

// =============================================================================
// QUERY / SET
// =============================================================================

Result<std::uint32_t> Encode(const QueryRequest& request, std::span<std::byte> buffer) {
  // Undocumented but mandatory rules (see the comment on
  // QueryRequest::expected_response_bytes):
  //   variable-length OID -> InformationBufferLength must be 0;
  //   fixed-length OID -> it must be >= the expected response length, and the
  //   message tail must really contain that many bytes.
  const bool variable_length = IsVariableLengthOid(request.oid);
  const std::uint32_t info_length = variable_length ? 0 : request.expected_response_bytes;

  const std::uint32_t message_length = kQuerySetHeaderBytes + info_length;
  TETHERKIT_RETURN_IF_ERROR(RequireCapacity(buffer, message_length, "REMOTE_NDIS_QUERY_MSG"));

  WriteHeader(buffer, MessageType::kQuery, message_length);
  StoreLe32(buffer.data() + kQuerySetRequestIdOffset, request.request_id);
  StoreLe32(buffer.data() + kQuerySetOidOffset, ToRaw(request.oid));
  StoreLe32(buffer.data() + kQuerySetInfoBufferLengthOffset, info_length);
  // When the length is 0 the offset must also be 0, or some devices reject
  // it.
  StoreLe32(buffer.data() + kQuerySetInfoBufferOffsetOffset,
            info_length == 0 ? 0 : kQuerySetInlineInfoOffset);
  StoreLe32(buffer.data() + kQuerySetDeviceVcHandleOffset, 0);

  // Zero-fill the tail as the placeholder area where the device writes the
  // result back.
  if (info_length != 0) {
    std::memset(buffer.data() + kQuerySetHeaderBytes, 0, info_length);
  }
  return message_length;
}

Result<QueryComplete> DecodeQueryComplete(std::span<const std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(ValidateCompletion(buffer, MessageType::kQueryComplete,
                                               kQueryCmpltHeaderBytes,
                                               "REMOTE_NDIS_QUERY_CMPLT"));

  const std::byte* base = buffer.data();
  const std::uint32_t message_length = LoadLe32(base + kMessageLengthOffset);
  const std::uint32_t status = LoadLe32(base + kQueryCmpltStatusOffset);
  const std::uint32_t info_length = LoadLe32(base + kQueryCmpltInfoBufferLengthOffset);
  const std::uint32_t info_offset = LoadLe32(base + kQueryCmpltInfoBufferOffsetOffset);

  QueryComplete complete{
      .request_id = LoadLe32(base + kQueryCmpltRequestIdOffset),
      .status = status,
      .information = {},
  };

  if (status != ToRaw(StatusCode::kSuccess)) {
    // Do not parse the information buffer on failure -- the device may not
    // have filled it. Return the status untouched for the caller to judge,
    // because "some optional OID is unsupported" is normal and must not be
    // treated as fatal across the board.
    return complete;
  }

  TETHERKIT_ASSIGN_OR_RETURN(
      complete.information,
      ResolveInlineBuffer(buffer, message_length, info_offset, info_length,
                          Text(Msg::kRndisWhatQueryCmpltInfoBuffer)));
  return complete;
}

Result<std::uint32_t> Encode(const SetRequest& request, std::span<std::byte> buffer) {
  const auto info_length = static_cast<std::uint32_t>(request.information.size());
  const std::uint32_t message_length = kQuerySetHeaderBytes + info_length;
  TETHERKIT_RETURN_IF_ERROR(RequireCapacity(buffer, message_length, "REMOTE_NDIS_SET_MSG"));

  WriteHeader(buffer, MessageType::kSet, message_length);
  StoreLe32(buffer.data() + kQuerySetRequestIdOffset, request.request_id);
  StoreLe32(buffer.data() + kQuerySetOidOffset, ToRaw(request.oid));
  StoreLe32(buffer.data() + kQuerySetInfoBufferLengthOffset, info_length);
  StoreLe32(buffer.data() + kQuerySetInfoBufferOffsetOffset,
            info_length == 0 ? 0 : kQuerySetInlineInfoOffset);
  StoreLe32(buffer.data() + kQuerySetDeviceVcHandleOffset, 0);

  if (info_length != 0) {
    std::memcpy(buffer.data() + kQuerySetHeaderBytes, request.information.data(), info_length);
  }
  return message_length;
}

Result<std::uint32_t> EncodeSetUint32(std::uint32_t request_id, Oid oid, std::uint32_t value,
                                      std::span<std::byte> buffer) {
  std::array<std::byte, 4> payload{};
  StoreLe32(payload.data(), value);
  return Encode(SetRequest{.request_id = request_id, .oid = oid, .information = payload}, buffer);
}

Result<SetComplete> DecodeSetComplete(std::span<const std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(
      ValidateCompletion(buffer, MessageType::kSetComplete, kSetCmpltBytes,
                         "REMOTE_NDIS_SET_CMPLT"));
  return SetComplete{
      .request_id = LoadLe32(buffer.data() + kSetCmpltRequestIdOffset),
      .status = LoadLe32(buffer.data() + kSetCmpltStatusOffset),
  };
}

// =============================================================================
// RESET
// =============================================================================

Result<std::uint32_t> EncodeReset(std::span<std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(RequireCapacity(buffer, kResetMsgBytes, "REMOTE_NDIS_RESET_MSG"));
  WriteHeader(buffer, MessageType::kReset, kResetMsgBytes);
  // Offset 8 is Reserved, not RequestId -- it must be written as 0.
  StoreLe32(buffer.data() + kResetReservedOffset, 0);
  return kResetMsgBytes;
}

Result<ResetComplete> DecodeResetComplete(std::span<const std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(ValidateCompletion(buffer, MessageType::kResetComplete,
                                               kResetCmpltBytes, "REMOTE_NDIS_RESET_CMPLT"));
  // Note the offsets: RESET_CMPLT has no RequestId, so Status sits at
  // offset 8 rather than 12.
  return ResetComplete{
      .status = LoadLe32(buffer.data() + kResetCmpltStatusOffset),
      .addressing_reset = LoadLe32(buffer.data() + kResetCmpltAddressingResetOffset) != 0,
  };
}

// =============================================================================
// KEEPALIVE
// =============================================================================

Result<std::uint32_t> EncodeKeepAlive(std::uint32_t request_id, std::span<std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(
      RequireCapacity(buffer, kKeepAliveMsgBytes, "REMOTE_NDIS_KEEPALIVE_MSG"));
  WriteHeader(buffer, MessageType::kKeepAlive, kKeepAliveMsgBytes);
  StoreLe32(buffer.data() + kKeepAliveRequestIdOffset, request_id);
  return kKeepAliveMsgBytes;
}

Result<KeepAliveComplete> DecodeKeepAliveComplete(std::span<const std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(ValidateCompletion(buffer, MessageType::kKeepAliveComplete,
                                               kKeepAliveCmpltBytes,
                                               "REMOTE_NDIS_KEEPALIVE_CMPLT"));
  return KeepAliveComplete{
      .request_id = LoadLe32(buffer.data() + kKeepAliveCmpltRequestIdOffset),
      .status = LoadLe32(buffer.data() + kKeepAliveCmpltStatusOffset),
  };
}

Result<std::uint32_t> EncodeKeepAliveComplete(std::uint32_t request_id, std::uint32_t status,
                                              std::span<std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(
      RequireCapacity(buffer, kKeepAliveCmpltBytes, "REMOTE_NDIS_KEEPALIVE_CMPLT"));
  WriteHeader(buffer, MessageType::kKeepAliveComplete, kKeepAliveCmpltBytes);
  StoreLe32(buffer.data() + kKeepAliveCmpltRequestIdOffset, request_id);
  StoreLe32(buffer.data() + kKeepAliveCmpltStatusOffset, status);
  return kKeepAliveCmpltBytes;
}

// =============================================================================
// INDICATE_STATUS
// =============================================================================

Result<IndicateStatus> DecodeIndicateStatus(std::span<const std::byte> buffer) {
  TETHERKIT_RETURN_IF_ERROR(RequireBytes(buffer, kIndicateStatusHeaderBytes,
                                         "REMOTE_NDIS_INDICATE_STATUS_MSG"));

  const std::byte* base = buffer.data();
  const std::uint32_t message_type = LoadLe32(base + kMessageTypeOffset);
  if (message_type != ToRaw(MessageType::kIndicateStatus)) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisExpectedIndicateStatus, message_type)));
  }

  const std::uint32_t message_length =
      std::min<std::uint32_t>(LoadLe32(base + kMessageLengthOffset),
                              static_cast<std::uint32_t>(buffer.size()));

  IndicateStatus indication{
      .status = LoadLe32(base + kIndicateStatusStatusOffset),
      .status_buffer = {},
  };

  const std::uint32_t buffer_length = LoadLe32(base + kIndicateStatusBufferLengthOffset);
  if (buffer_length == 0) {
    // The overwhelmingly common case: MEDIA_CONNECT / MEDIA_DISCONNECT carry
    // no payload.
    return indication;
  }

  // The base point of StatusBufferOffset is contradictory in the spec; try
  // both interpretations. See the documentation comment on
  // DecodeIndicateStatus in messages.h.
  const std::uint32_t relative_offset = LoadLe32(base + kIndicateStatusBufferOffsetOffset);
  const std::uint64_t candidates[] = {
      static_cast<std::uint64_t>(kOffsetFieldBase) + relative_offset,  // same as QUERY/SET
      relative_offset,                                                 // literal per MS docs
  };
  for (const std::uint64_t offset : candidates) {
    if (offset + buffer_length <= message_length) {
      indication.status_buffer =
          buffer.subspan(static_cast<std::size_t>(offset), buffer_length);
      break;
    }
  }

  if (indication.status_buffer.empty()) {
    // Both interpretations are out of range: drop the optional payload but
    // **still return success** -- the status itself is useful (it may be
    // MEDIA_DISCONNECT), and an unparseable optional field must not condemn
    // the link.
    TETHERKIT_WARN_TR(Msg::kRndisIndicateStatusBufferOutOfBounds, relative_offset, buffer_length,
                      message_length);
    return indication;
  }

  // At exactly 8 bytes, parse it as RNDIS_Diagnostic_Info -- the device uses
  // it to report which part of the message we sent was invalid
  // (DiagStatus + ErrorOffset), which is invaluable for troubleshooting.
  if (indication.status_buffer.size() == kDiagnosticInfoBytes) {
    indication.has_diagnostic_info = true;
    indication.diagnostic_status =
        LoadLe32(indication.status_buffer.data() + kDiagnosticInfoDiagStatusOffset);
    indication.diagnostic_error_offset =
        LoadLe32(indication.status_buffer.data() + kDiagnosticInfoErrorOffsetOffset);
  }
  return indication;
}

// =============================================================================
// OID payload parsing
// =============================================================================

Result<std::uint32_t> ParseUint32(std::span<const std::byte> information) {
  TETHERKIT_RETURN_IF_ERROR(RequireBytes(information, 4, Text(Msg::kRndisWhatOidUint32)));
  return LoadLe32(information.data());
}

Result<std::uint64_t> ParseCounter(std::span<const std::byte> information) {
  // Statistics OIDs may return either 4 or 8 bytes; both must be accepted.
  if (information.size() >= 8) {
    return LoadLe64(information.data());
  }
  if (information.size() >= 4) {
    return static_cast<std::uint64_t>(LoadLe32(information.data()));
  }
  return std::unexpected(Error::Generic(Tr(Msg::kRndisOidCounterBadLength, information.size())));
}

Result<MacAddress> ParseMac(std::span<const std::byte> information) {
  TETHERKIT_RETURN_IF_ERROR(
      RequireBytes(information, sizeof(MacAddress), Text(Msg::kRndisWhatOidMac)));
  MacAddress mac{};
  std::memcpy(mac.data(), information.data(), mac.size());
  return mac;
}

}  // namespace tetherkit::rndis
