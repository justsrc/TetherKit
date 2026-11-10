// Encoding and decoding of RNDIS control messages.
//
// Design trade-off: **do not map the wire format with packed structs directly**; use
// "logical structs + explicit offset read/write" instead. Three reasons:
//   1. The wire format is little-endian; a packed struct fails silently on big-endian
//      hosts;
//   2. RNDIS messages in USB buffers are only guaranteed 4-byte aligned at their start;
//      member access on a packed struct is undefined behavior at certain offsets
//      (-fsanitize=alignment will flag it);
//   3. The base point of offset fields is the counterintuitive rule "message start + 8"
//      (see protocol.h header, rule 2). Explicit Encode/Decode functions concentrate that
//      conversion in one place and make it testable, while a struct mapping would force
//      every call site to remember the +/-8.
//
// Decode functions always perform **complete bounds and self-consistency checks**: data
// on the control channel comes from an external device and must be assumed to be
// malicious or buggy. A failed check returns Error rather than crashing or reading out
// of bounds.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "tetherkit/common/error.h"
#include "tetherkit/rndis/protocol.h"

namespace tetherkit::rndis {

/// 6-byte Ethernet MAC address.
using MacAddress = std::array<std::uint8_t, 6>;

/// Render a MAC as "aa:bb:cc:dd:ee:ff".
[[nodiscard]] std::array<char, 18> FormatMac(const MacAddress& mac) noexcept;

// =============================================================================
// Common header access
// =============================================================================

/// Common header of any RNDIS message.
struct MessageHeader {
  std::uint32_t message_type = 0;
  std::uint32_t message_length = 0;
};

/// Read the common header. Returns an error if the buffer is smaller than 8 bytes.
[[nodiscard]] Result<MessageHeader> DecodeMessageHeader(std::span<const std::byte> buffer);

// =============================================================================
// INITIALIZE
// =============================================================================

struct InitializeRequest {
  std::uint32_t request_id = 0;
  std::uint32_t major_version = kMajorVersion;
  std::uint32_t minor_version = kMinorVersion;
  /// Maximum bytes the host can receive from bulk IN in one transfer, computed with
  /// MaxTransferSizeFor().
  std::uint32_t max_transfer_size = 0;
};

/// Encode INITIALIZE_MSG. Buffer must be at least kInitializeMsgBytes; returns bytes
/// written.
[[nodiscard]] Result<std::uint32_t> Encode(const InitializeRequest& request,
                                          std::span<std::byte> buffer);

struct InitializeComplete {
  std::uint32_t request_id = 0;
  std::uint32_t status = 0;
  std::uint32_t major_version = 0;
  std::uint32_t minor_version = 0;
  std::uint32_t device_flags = 0;
  std::uint32_t medium = 0;
  /// Maximum PACKET_MSGs per transfer the device can carry. Most Android gadgets report
  /// 1; those with Qualcomm aggregation patches report 3 or more. Treated as 1 when 0.
  std::uint32_t max_packets_per_message = 0;
  /// Maximum bytes the device can receive in one transfer -- determines the host-side TX
  /// aggregation limit.
  std::uint32_t max_transfer_size = 0;
  /// host -> device alignment requirement, alignment bytes = 1 << this value, legal upper
  /// bound 7.
  std::uint32_t packet_alignment_factor = 0;
  std::uint32_t af_list_offset = 0;
  std::uint32_t af_list_size = 0;
};

[[nodiscard]] Result<InitializeComplete> DecodeInitializeComplete(std::span<const std::byte> buffer);

/// Parameters derived from INITIALIZE_CMPLT that the data path actually uses.
struct NegotiatedParameters {
  std::uint32_t device_max_transfer_size = 0;  ///< TX aggregation byte limit.
  std::uint32_t max_packets_per_message = 1;   ///< TX aggregation packet limit.
  std::uint32_t tx_alignment_bytes = 1;        ///< Per-packet alignment host -> device.
  std::uint32_t mtu = kDefaultMtu;             ///< Final MTU adopted.
  bool connectionless = true;
};

/// Validate and normalize INITIALIZE_CMPLT to produce parameters ready for the data path.
///
/// All known device quirks are handled here:
///   * MaxPacketsPerMessage is 0 -> treat as 1;
///   * PacketAlignmentFactor > 7 -> clamp to 7 (protocol violation, otherwise absurd
///     padding lengths would be computed);
///   * MaxTransferSize smaller than one full frame (hard_mtu) -> derive and lower the
///     MTU; too small (<= 58, cannot fit a header) -> error;
///   * MaxTransferSize reports 8KB/16KB jumbo frames (WinCE / Windows Mobile habit) ->
///     do not follow blindly, clamp to our own buffer limit.
[[nodiscard]] Result<NegotiatedParameters> Negotiate(const InitializeComplete& complete,
                                                     std::uint32_t requested_mtu,
                                                     std::uint32_t host_transfer_size_limit);

// =============================================================================
// HALT
// =============================================================================

/// Encode HALT_MSG. **The device does not reply**; once sent it can be considered
/// uninitialized.
[[nodiscard]] Result<std::uint32_t> EncodeHalt(std::uint32_t request_id,
                                               std::span<std::byte> buffer);

// =============================================================================
// QUERY / SET
// =============================================================================

struct QueryRequest {
  std::uint32_t request_id = 0;
  Oid oid = Oid::kGenSupportedList;
  /// Expected response size in bytes.
  ///
  /// For **fixed-length** OIDs this must be >= the expected response length, and the
  /// same number of zero bytes must be appended at the end of the message; for
  /// **variable-length** OIDs it must be 0. This undocumented requirement comes from the
  /// Microsoft ActiveSync implementation; violating it yields
  /// RNDIS_STATUS_INVALID_LENGTH. Encode corrects this automatically based on
  /// IsVariableLengthOid(), so callers can pass 0 and let it decide.
  std::uint32_t expected_response_bytes = 0;
};

/// Encode QUERY_MSG (including trailing padding chosen automatically by OID type).
[[nodiscard]] Result<std::uint32_t> Encode(const QueryRequest& request,
                                          std::span<std::byte> buffer);

struct QueryComplete {
  std::uint32_t request_id = 0;
  std::uint32_t status = 0;
  /// View of the information buffer inside `buffer`, with the same lifetime as `buffer`.
  std::span<const std::byte> information;
};

[[nodiscard]] Result<QueryComplete> DecodeQueryComplete(std::span<const std::byte> buffer);

struct SetRequest {
  std::uint32_t request_id = 0;
  Oid oid = Oid::kGenCurrentPacketFilter;
  std::span<const std::byte> information;
};

/// Encode SET_MSG.
[[nodiscard]] Result<std::uint32_t> Encode(const SetRequest& request, std::span<std::byte> buffer);

/// Convenience entry point: SET an LE32 value (most common, e.g. setting the packet
/// filter).
[[nodiscard]] Result<std::uint32_t> EncodeSetUint32(std::uint32_t request_id, Oid oid,
                                                    std::uint32_t value,
                                                    std::span<std::byte> buffer);

struct SetComplete {
  std::uint32_t request_id = 0;
  std::uint32_t status = 0;
};

[[nodiscard]] Result<SetComplete> DecodeSetComplete(std::span<const std::byte> buffer);

// =============================================================================
// RESET
// =============================================================================

/// Encode RESET_MSG.
///
/// **Note: RESET_MSG has no RequestId field** (offset 8 is Reserved), so responses
/// cannot be matched by ID and only one RESET may be in flight at a time.
[[nodiscard]] Result<std::uint32_t> EncodeReset(std::span<std::byte> buffer);

struct ResetComplete {
  std::uint32_t status = 0;
  /// Non-zero means addressing information (packet filter, multicast list) was lost in
  /// the reset and the host must re-send the corresponding SETs.
  bool addressing_reset = false;
};

[[nodiscard]] Result<ResetComplete> DecodeResetComplete(std::span<const std::byte> buffer);

// =============================================================================
// KEEPALIVE
// =============================================================================

[[nodiscard]] Result<std::uint32_t> EncodeKeepAlive(std::uint32_t request_id,
                                                    std::span<std::byte> buffer);

struct KeepAliveComplete {
  std::uint32_t request_id = 0;
  std::uint32_t status = 0;
};

[[nodiscard]] Result<KeepAliveComplete> DecodeKeepAliveComplete(std::span<const std::byte> buffer);

/// Encode KEEPALIVE_CMPLT.
///
/// Needed because **the device can also send KEEPALIVE_MSG proactively**; in that case
/// the host must reply with KEEPALIVE_CMPLT, otherwise the device may assume the host is
/// dead and disconnect.
[[nodiscard]] Result<std::uint32_t> EncodeKeepAliveComplete(std::uint32_t request_id,
                                                            std::uint32_t status,
                                                            std::span<std::byte> buffer);

// =============================================================================
// INDICATE_STATUS
// =============================================================================

struct IndicateStatus {
  std::uint32_t status = 0;
  /// View of the status buffer; empty in most cases (MEDIA_CONNECT/DISCONNECT carry no
  /// payload).
  std::span<const std::byte> status_buffer;
  /// If the status buffer is exactly the 8-byte RNDIS_Diagnostic_Info, these fields are
  /// populated.
  bool has_diagnostic_info = false;
  std::uint32_t diagnostic_status = 0;
  std::uint32_t diagnostic_error_offset = 0;
};

/// Decode INDICATE_STATUS_MSG.
///
/// **The base point of StatusBufferOffset is contradictory in the specification** (MS
/// docs say message start, while sibling QUERY/SET use message start + 8). This
/// implementation is tolerant in the following order:
///   1. If the length is 0 -- no payload, succeed directly (the vast majority of cases);
///   2. Interpret with base "message start + 8" first; adopt if it falls within the
///      message;
///   3. Otherwise interpret with base "message start";
///   4. If both are out of bounds -- discard the status buffer but **still return
///      success**, because the status itself is useful and a link must not be torn down
///      just because an optional payload cannot be parsed.
[[nodiscard]] Result<IndicateStatus> DecodeIndicateStatus(std::span<const std::byte> buffer);

// =============================================================================
// OID payload parsing
// =============================================================================

/// Read an LE32 from a QUERY_CMPLT information buffer.
[[nodiscard]] Result<std::uint32_t> ParseUint32(std::span<const std::byte> information);

/// Read a counter value from an information buffer.
///
/// Statistics OIDs (OID_GEN_XMIT_OK, etc.) may return 4 or 8 bytes; both must be
/// accepted.
[[nodiscard]] Result<std::uint64_t> ParseCounter(std::span<const std::byte> information);

/// Read a 6-byte MAC from an information buffer.
[[nodiscard]] Result<MacAddress> ParseMac(std::span<const std::byte> information);

}  // namespace tetherkit::rndis
