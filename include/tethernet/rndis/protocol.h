// RNDIS (Remote NDIS) wire-format constants and field layout.
//
// This file is the **single source** of RNDIS protocol constants for the project;
// duplicates elsewhere are not allowed. All values come from the Microsoft RNDIS
// specification and have been cross-checked against the Linux kernel implementation
// (drivers/net/usb/rndis_host.c, include/linux/usb/rndis_host.h,
//  drivers/usb/gadget/function/rndis.c, include/linux/usb/cdc.h).
// See docs/RNDIS-PROTOCOL.md for detailed field tables and references.
//
// Three rules that are easiest to get wrong and must be remembered:
//
// 1. **All multi-byte fields are little-endian** (the protocol comes from Windows NDIS).
//    Always access them with LoadLe32 / StoreLe32 from byte_order.h; never map them
//    directly with a packed struct.
//
// 2. **The base point of offset fields is not the message start, but message start + 8.**
//    For DataOffset / OOBDataOffset / PerPacketInfoOffset in PACKET_MSG, and for
//    InformationBufferOffset in QUERY/SET/QUERY_CMPLT, the base point is "the start of
//    the block those offset fields belong to", i.e. message start + 8 bytes.
//        absolute offset = 8 + field value
//    So when "data immediately follows the 44-byte PACKET_MSG header", DataOffset = 36,
//    not 44. This is the number-one trap when implementing RNDIS.
//
// 3. **INDICATE_STATUS_MSG's StatusBufferOffset is an exception**: MS docs state its base
//    point is the message start (+0), inconsistent with the +8 convention above. This is
//    a contradiction in the specification itself. In practice the field is almost always
//    0 (MEDIA_CONNECT / MEDIA_DISCONNECT carry no status buffer), so parsing must
//    tolerate both interpretations and must not tear down the link because of a parse
//    error.
#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace tethernet::rndis {

// =============================================================================
// Version and timeouts
// =============================================================================

/// Protocol version the host declares in INITIALIZE_MSG.
inline constexpr std::uint32_t kMajorVersion = 1;
inline constexpr std::uint32_t kMinorVersion = 0;

/// Control channel timeout.
///
/// The specification gives ControlTimeoutPeriod as 10 seconds, but Linux shrinks it to 5
/// seconds because of the behavior of Microsoft's ActiveSync implementation. We take 5
/// seconds here: if a device really needs 10 seconds to reply, the link is already
/// unusable.
inline constexpr std::uint32_t kControlTimeoutMillis = 5'000;

/// Keep-alive period. The specification's KeepAliveTimeoutPeriod is 5 seconds, and the
/// semantics are "send only after 5 seconds have passed since the last message received
/// from the device", not unconditionally every 5 seconds.
inline constexpr std::uint32_t kKeepAliveTimeoutMillis = 5'000;

/// Control message buffer size.
///
/// The specification requires at least 1024 bytes on the host side (it suggests using
/// 0x400 for wLength of GET_ENCAPSULATED_RESPONSE). Windows actually uses the odd value
/// 1025 and Linux copies it. We take 1536 to leave headroom -- the return value of
/// OID_GEN_SUPPORTED_LIST can be long.
inline constexpr std::uint32_t kControlBufferBytes = 1536;

// =============================================================================
// Message type codes
// =============================================================================

/// Completion message = request message | this bit.
inline constexpr std::uint32_t kMessageCompletionFlag = 0x8000'0000U;

enum class MessageType : std::uint32_t {
  kPacket = 0x0000'0001U,          ///< Data channel: carries Ethernet frames.
  kInitialize = 0x0000'0002U,      ///< host -> device: initialization negotiation.
  kHalt = 0x0000'0003U,            ///< host -> device: halt, **device does not reply**.
  kQuery = 0x0000'0004U,           ///< host -> device: read OID.
  kSet = 0x0000'0005U,             ///< host -> device: write OID.
  kReset = 0x0000'0006U,           ///< host -> device: soft reset.
  kIndicateStatus = 0x0000'0007U,  ///< device -> host: **no RequestId, no reply
                                   ///< required**.
  kKeepAlive = 0x0000'0008U,       ///< Either side may initiate.

  kInitializeComplete = kInitialize | kMessageCompletionFlag,  // 0x80000002
  kQueryComplete = kQuery | kMessageCompletionFlag,            // 0x80000004
  kSetComplete = kSet | kMessageCompletionFlag,                // 0x80000005
  kResetComplete = kReset | kMessageCompletionFlag,            // 0x80000006
  kKeepAliveComplete = kKeepAlive | kMessageCompletionFlag,    // 0x80000008

  /// Private bus message, unused by this project; listed only to identify and ignore.
  kBus = 0xFF00'0001U,
};

/// Connection-oriented (CONDIS) message family. 802.3 Ethernet devices **will not** use
/// these; listed here only so that receiving one can be reported explicitly as
/// "connection-oriented device not supported" rather than "unknown message".
enum class CondisMessageType : std::uint32_t {
  kCreateVc = 0x0000'8001U,
  kDeleteVc = 0x0000'8002U,
  kActivateVc = 0x0000'8005U,
  kDeactivateVc = 0x0000'8006U,
  kIndicateStatus = 0x0000'8007U,
};

/// Convert any wire-format enum back to its raw LE32 value.
///
/// Constrained to "underlying type must be exactly uint32_t", so if someone accidentally
/// shrinks the underlying type of a protocol enum, this fails to compile directly
/// instead of silently corrupting the wire format.
template <typename E>
  requires std::is_enum_v<E> && std::same_as<std::underlying_type_t<E>, std::uint32_t>
[[nodiscard]] constexpr std::uint32_t ToRaw(E value) noexcept {
  return static_cast<std::uint32_t>(value);
}

/// Whether this is a completion message.
[[nodiscard]] constexpr bool IsCompletion(std::uint32_t message_type) noexcept {
  return (message_type & kMessageCompletionFlag) != 0;
}

/// Whether this belongs to the CONDIS (connection-oriented) message family.
[[nodiscard]] constexpr bool IsCondis(std::uint32_t message_type) noexcept {
  return (message_type & ~kMessageCompletionFlag) >= 0x0000'8000U &&
         (message_type & ~kMessageCompletionFlag) <= 0x0000'8FFFU;
}

/// Human-readable name for a message type; empty for unknown types.
[[nodiscard]] std::string_view MessageTypeName(std::uint32_t message_type) noexcept;

// =============================================================================
// Message lengths and field offsets
//
// All fields are LE32. The kXxxOffset constants below are **byte offsets within the
// message**, for direct use with LoadLe32(buffer + kXxxOffset).
// =============================================================================

/// The first two fields common to every RNDIS message.
inline constexpr std::uint32_t kMessageTypeOffset = 0;
inline constexpr std::uint32_t kMessageLengthOffset = 4;

/// The shortest valid RNDIS message is just Type + Length.
inline constexpr std::uint32_t kMessageHeaderBytes = 8;

/// Base point for "offset fields": message start + 8. See header rule 2.
inline constexpr std::uint32_t kOffsetFieldBase = 8;

// ---- REMOTE_NDIS_INITIALIZE_MSG (24 bytes) ----
inline constexpr std::uint32_t kInitializeMsgBytes = 24;
inline constexpr std::uint32_t kInitializeRequestIdOffset = 8;
inline constexpr std::uint32_t kInitializeMajorVersionOffset = 12;
inline constexpr std::uint32_t kInitializeMinorVersionOffset = 16;
inline constexpr std::uint32_t kInitializeMaxTransferSizeOffset = 20;

// ---- REMOTE_NDIS_INITIALIZE_CMPLT (52 bytes) ----
inline constexpr std::uint32_t kInitializeCmpltBytes = 52;
inline constexpr std::uint32_t kInitializeCmpltRequestIdOffset = 8;
inline constexpr std::uint32_t kInitializeCmpltStatusOffset = 12;
inline constexpr std::uint32_t kInitializeCmpltMajorVersionOffset = 16;
inline constexpr std::uint32_t kInitializeCmpltMinorVersionOffset = 20;
inline constexpr std::uint32_t kInitializeCmpltDeviceFlagsOffset = 24;
inline constexpr std::uint32_t kInitializeCmpltMediumOffset = 28;
inline constexpr std::uint32_t kInitializeCmpltMaxPacketsPerMessageOffset = 32;
inline constexpr std::uint32_t kInitializeCmpltMaxTransferSizeOffset = 36;
inline constexpr std::uint32_t kInitializeCmpltPacketAlignmentFactorOffset = 40;
inline constexpr std::uint32_t kInitializeCmpltAfListOffsetOffset = 44;
inline constexpr std::uint32_t kInitializeCmpltAfListSizeOffset = 48;

// ---- REMOTE_NDIS_HALT_MSG (12 bytes, device does not reply) ----
inline constexpr std::uint32_t kHaltMsgBytes = 12;
inline constexpr std::uint32_t kHaltRequestIdOffset = 8;

// ---- REMOTE_NDIS_QUERY_MSG / SET_MSG (28-byte header) ----
inline constexpr std::uint32_t kQuerySetHeaderBytes = 28;
inline constexpr std::uint32_t kQuerySetRequestIdOffset = 8;
inline constexpr std::uint32_t kQuerySetOidOffset = 12;
inline constexpr std::uint32_t kQuerySetInfoBufferLengthOffset = 16;
inline constexpr std::uint32_t kQuerySetInfoBufferOffsetOffset = 20;
inline constexpr std::uint32_t kQuerySetDeviceVcHandleOffset = 24;

/// Value to write for InformationBufferOffset when "the information buffer immediately
/// follows the 28-byte header".
inline constexpr std::uint32_t kQuerySetInlineInfoOffset = kQuerySetHeaderBytes - kOffsetFieldBase;
static_assert(kQuerySetInlineInfoOffset == 20);

// ---- REMOTE_NDIS_QUERY_CMPLT (24-byte header) ----
inline constexpr std::uint32_t kQueryCmpltHeaderBytes = 24;
inline constexpr std::uint32_t kQueryCmpltRequestIdOffset = 8;
inline constexpr std::uint32_t kQueryCmpltStatusOffset = 12;
inline constexpr std::uint32_t kQueryCmpltInfoBufferLengthOffset = 16;
inline constexpr std::uint32_t kQueryCmpltInfoBufferOffsetOffset = 20;

/// Value of InformationBufferOffset when "the information buffer immediately follows the
/// 24-byte header".
inline constexpr std::uint32_t kQueryCmpltInlineInfoOffset = kQueryCmpltHeaderBytes - kOffsetFieldBase;
static_assert(kQueryCmpltInlineInfoOffset == 16);

// ---- REMOTE_NDIS_SET_CMPLT (16 bytes) ----
inline constexpr std::uint32_t kSetCmpltBytes = 16;
inline constexpr std::uint32_t kSetCmpltRequestIdOffset = 8;
inline constexpr std::uint32_t kSetCmpltStatusOffset = 12;

// ---- REMOTE_NDIS_RESET_MSG (12 bytes, **no RequestId**) ----
inline constexpr std::uint32_t kResetMsgBytes = 12;
/// Offset 8 is Reserved, not RequestId. Therefore RESET cannot be matched by ID and only
/// one RESET may be in flight at a time.
inline constexpr std::uint32_t kResetReservedOffset = 8;

// ---- REMOTE_NDIS_RESET_CMPLT (16 bytes, **no RequestId**) ----
inline constexpr std::uint32_t kResetCmpltBytes = 16;
inline constexpr std::uint32_t kResetCmpltStatusOffset = 8;
/// Non-zero means addressing information (packet filter, multicast list, functional
/// address) was lost in the reset and the host must re-send the corresponding SETs.
inline constexpr std::uint32_t kResetCmpltAddressingResetOffset = 12;

// ---- REMOTE_NDIS_INDICATE_STATUS_MSG (20-byte header, **no RequestId**) ----
inline constexpr std::uint32_t kIndicateStatusHeaderBytes = 20;
inline constexpr std::uint32_t kIndicateStatusStatusOffset = 8;
inline constexpr std::uint32_t kIndicateStatusBufferLengthOffset = 12;
/// Note the base-point controversy; see header rule 3.
inline constexpr std::uint32_t kIndicateStatusBufferOffsetOffset = 16;

/// Diagnostic information structure carried by INDICATE_STATUS (RNDIS_Diagnostic_Info,
/// 8 bytes).
inline constexpr std::uint32_t kDiagnosticInfoBytes = 8;
inline constexpr std::uint32_t kDiagnosticInfoDiagStatusOffset = 0;
inline constexpr std::uint32_t kDiagnosticInfoErrorOffsetOffset = 4;

// ---- REMOTE_NDIS_KEEPALIVE_MSG (12 bytes) / KEEPALIVE_CMPLT (16 bytes) ----
inline constexpr std::uint32_t kKeepAliveMsgBytes = 12;
inline constexpr std::uint32_t kKeepAliveRequestIdOffset = 8;
inline constexpr std::uint32_t kKeepAliveCmpltBytes = 16;
inline constexpr std::uint32_t kKeepAliveCmpltRequestIdOffset = 8;
inline constexpr std::uint32_t kKeepAliveCmpltStatusOffset = 12;

// ---- REMOTE_NDIS_PACKET_MSG (44-byte header) ----
inline constexpr std::uint32_t kPacketMsgHeaderBytes = 44;
inline constexpr std::uint32_t kPacketDataOffsetOffset = 8;
inline constexpr std::uint32_t kPacketDataLengthOffset = 12;
inline constexpr std::uint32_t kPacketOobDataOffsetOffset = 16;
inline constexpr std::uint32_t kPacketOobDataLengthOffset = 20;
inline constexpr std::uint32_t kPacketNumOobDataElementsOffset = 24;
inline constexpr std::uint32_t kPacketPerPacketInfoOffsetOffset = 28;
inline constexpr std::uint32_t kPacketPerPacketInfoLengthOffset = 32;
inline constexpr std::uint32_t kPacketVcHandleOffset = 36;
inline constexpr std::uint32_t kPacketReservedOffset = 40;

/// Value to write for DataOffset when "the Ethernet frame immediately follows the
/// 44-byte header". **It is 36, not 44.**
inline constexpr std::uint32_t kPacketInlineDataOffset = kPacketMsgHeaderBytes - kOffsetFieldBase;
static_assert(kPacketInlineDataOffset == 36,
              "DataOffset base point is message start + 8; see header rule 2");

/// device -> host direction: when aggregating multiple packets, each PACKET_MSG should
/// start at an 8-byte-aligned offset.
inline constexpr std::uint32_t kDeviceToHostPacketAlignment = 8;

/// Legal upper bound of PacketAlignmentFactor (1 << 7 = 128 bytes).
/// A larger value reported by the device is a protocol violation; implementations must
/// clamp it or absurd padding lengths will be computed.
inline constexpr std::uint32_t kMaxPacketAlignmentFactor = 7;

// =============================================================================
// Status codes
// =============================================================================

/// RNDIS_STATUS_* status codes.
///
/// Deliberately named StatusCode rather than Status: `tethernet::Status` is already an
/// alias for `std::expected<void, Error>`, and defining another `Status` in
/// tethernet::rndis would shadow it, making it impossible to write "may fail with no
/// return value" signatures in this namespace.
enum class StatusCode : std::uint32_t {
  // ---- Success / pending ----
  kSuccess = 0x0000'0000U,
  kPending = 0x0000'0103U,

  // ---- Informational ----
  kNotRecognized = 0x0001'0001U,
  kNotCopied = 0x0001'0002U,
  kNotAccepted = 0x0001'0003U,
  kCallActive = 0x0001'0007U,

  // ---- Indications (0x4001xxxx, appear in INDICATE_STATUS_MSG) ----
  kOnline = 0x4001'0003U,
  kResetStart = 0x4001'0004U,
  kResetEnd = 0x4001'0005U,
  kRingStatus = 0x4001'0006U,
  kClosed = 0x4001'0007U,
  kWanLineUp = 0x4001'0008U,
  kWanLineDown = 0x4001'0009U,
  kWanFragment = 0x4001'000AU,
  kMediaConnect = 0x4001'000BU,     ///< Link up -- we bring feth UP on this.
  kMediaDisconnect = 0x4001'000CU,  ///< Link down.
  kHardwareLineUp = 0x4001'000DU,
  kHardwareLineDown = 0x4001'000EU,
  kInterfaceUp = 0x4001'000FU,
  kInterfaceDown = 0x4001'0010U,
  kMediaBusy = 0x4001'0011U,
  kMediaSpecificIndication = 0x4001'0012U,
  kLinkSpeedChange = 0x4001'0013U,
  kNetworkChange = 0x4001'0018U,

  // ---- Warnings (0x8xxxxxxx) ----
  kBufferOverflow = 0x8000'0005U,
  kNotResettable = 0x8001'0001U,
  kSoftErrors = 0x8001'0003U,
  kHardErrors = 0x8001'0004U,

  // ---- Errors (0xCxxxxxxx) ----
  kFailure = 0xC000'0001U,
  kResources = 0xC000'009AU,
  kNotSupported = 0xC000'00BBU,
  kClosing = 0xC001'0002U,
  kBadVersion = 0xC001'0004U,
  kBadCharacteristics = 0xC001'0005U,
  kAdapterNotFound = 0xC001'0006U,
  kOpenFailed = 0xC001'0007U,
  kDeviceFailed = 0xC001'0008U,
  kMulticastFull = 0xC001'0009U,
  kMulticastExists = 0xC001'000AU,
  kMulticastNotFound = 0xC001'000BU,
  kRequestAborted = 0xC001'000CU,
  kResetInProgress = 0xC001'000DU,
  kClosingIndicating = 0xC001'000EU,
  kInvalidPacket = 0xC001'000FU,
  kOpenListFull = 0xC001'0010U,
  kAdapterNotReady = 0xC001'0011U,
  kAdapterNotOpen = 0xC001'0012U,
  kNotIndicating = 0xC001'0013U,
  kInvalidLength = 0xC001'0014U,
  kInvalidData = 0xC001'0015U,
  kBufferTooShort = 0xC001'0016U,
  kInvalidOid = 0xC001'0017U,
  kAdapterRemoved = 0xC001'0018U,
  kUnsupportedMedia = 0xC001'0019U,
  kGroupAddressInUse = 0xC001'001AU,
  kNoCable = 0xC001'001FU,
  kTokenRingOpenError = 0xC001'1000U,
};

/// Whether this is a failure status (0xC0000000 and above are errors by NDIS convention).
[[nodiscard]] constexpr bool IsFailure(std::uint32_t status) noexcept {
  return (status & 0xC000'0000U) == 0xC000'0000U;
}

/// Human-readable name for a status code; empty for unknown values.
[[nodiscard]] std::string_view StatusName(std::uint32_t status) noexcept;

// =============================================================================
// OID
// =============================================================================

enum class Oid : std::uint32_t {
  // ---- General (OID_GEN_*) ----
  kGenSupportedList = 0x0001'0101U,        ///< Variable length: N LE32 OIDs. in_len must
                                           ///< be 0 on query.
  kGenHardwareStatus = 0x0001'0102U,       ///< LE32: 0=Ready 1=Initializing 2=Reset
                                           ///< 3=Closing 4=NotReady
  kGenMediaSupported = 0x0001'0103U,       ///< LE32 array.
  kGenMediaInUse = 0x0001'0104U,           ///< LE32 array.
  kGenMaximumFrameSize = 0x0001'0106U,     ///< LE32: **excludes** 14-byte Ethernet header,
                                           ///< typically 1500.
  kGenLinkSpeed = 0x0001'0107U,            ///< LE32: units of **100 bps**; 0 when
                                           ///< disconnected.
  kGenTransmitBlockSize = 0x0001'010AU,    ///< LE32
  kGenReceiveBlockSize = 0x0001'010BU,     ///< LE32
  kGenVendorId = 0x0001'010CU,             ///< LE32: lower 24 bits are the IEEE OUI.
  kGenVendorDescription = 0x0001'010DU,    ///< Variable-length ASCII, **NUL termination not
                                           ///< guaranteed**.
  kGenCurrentPacketFilter = 0x0001'010EU,  ///< LE32 bitmask, readable and writable.
  kGenMaximumTotalSize = 0x0001'0111U,     ///< LE32: maximum total frame length including
                                           ///< header.
  kGenMacOptions = 0x0001'0113U,           ///< LE32: MacOption bitmask.
  kGenMediaConnectStatus = 0x0001'0114U,   ///< LE32: **0=connected, 1=disconnected** (note
                                           ///< 0 means connected).
  kGenPhysicalMedium = 0x0001'0202U,       ///< LE32: PhysicalMedium; **optional OID**,
                                           ///< failure is non-fatal.

  // ---- Statistics (OID_GEN_*_OK/ERROR): may return 4 or 8 bytes; both must be accepted
  // ----
  kGenXmitOk = 0x0002'0101U,
  kGenRcvOk = 0x0002'0102U,
  kGenXmitError = 0x0002'0103U,
  kGenRcvError = 0x0002'0104U,
  kGenRcvNoBuffer = 0x0002'0105U,

  // ---- 802.3 specific ----
  kEthernetPermanentAddress = 0x0101'0101U,  ///< 6-byte MAC.
  kEthernetCurrentAddress = 0x0101'0102U,    ///< 6-byte MAC.
  kEthernetMulticastList = 0x0101'0103U,     ///< SET: 6N-byte MAC array. **Do not rely
                                             ///< on QUERY**.
  kEthernetMaximumListSize = 0x0101'0104U,   ///< LE32: maximum multicast list entries.
  kEthernetMacOptions = 0x0101'0105U,        ///< LE32: Ethernet802_3MacOption bitmask.
};

/// Human-readable name for an OID; empty for unknown values.
[[nodiscard]] std::string_view OidName(std::uint32_t oid) noexcept;

/// Whether this OID returns a variable-length value.
///
/// This distinction is critical: for **fixed-length** OIDs, QUERY's
/// InformationBufferLength must be set to >= the expected response length and the same
/// number of zero bytes must be appended at the end of the message, otherwise Microsoft's
/// ActiveSync implementation returns RNDIS_STATUS_INVALID_LENGTH; for **variable-length**
/// OIDs it must be 0, or the same error occurs. This undocumented behavior was discovered
/// by sniffing the ActiveSync 4.1 Windows driver.
[[nodiscard]] constexpr bool IsVariableLengthOid(Oid oid) noexcept {
  switch (oid) {
    case Oid::kGenSupportedList:
    case Oid::kGenVendorDescription:
    case Oid::kGenMediaSupported:
    case Oid::kGenMediaInUse:
    case Oid::kEthernetMulticastList:
      return true;
    default:
      return false;
  }
}

// =============================================================================
// Packet filter bitmasks (NDIS_PACKET_TYPE_*)
// =============================================================================

enum class PacketFilter : std::uint32_t {
  kDirected = 0x0000'0001U,      ///< Destination MAC equals the local address.
  kMulticast = 0x0000'0002U,     ///< Multicast address in the multicast list.
  kAllMulticast = 0x0000'0004U,  ///< All multicast, ignore the multicast list.
  kBroadcast = 0x0000'0008U,     ///< ff:ff:ff:ff:ff:ff
  kSourceRouting = 0x0000'0010U,
  kPromiscuous = 0x0000'0020U,  ///< All frames.
  kSmt = 0x0000'0040U,
  kAllLocal = 0x0000'0080U,
  kGroup = 0x0000'1000U,
  kAllFunctional = 0x0000'2000U,
  kFunctional = 0x0000'4000U,
  kMacFrame = 0x0000'8000U,
};

/// Packet filter combination used by this project, kept consistent with Linux rndis_host
/// (0x0000002D).
///
/// Why PROMISCUOUS is enabled: we bridge the device as a "network cable" to feth, and the
/// host side may send and receive frames with arbitrary MACs (e.g. if upper layers do
/// bridging or run VMs), so we cannot receive only directed frames. ALL_MULTICAST for
/// the same reason -- avoid maintaining a multicast list and still missing IPv6 neighbor
/// discovery.
inline constexpr std::uint32_t kDefaultPacketFilter =
    ToRaw(PacketFilter::kDirected) | ToRaw(PacketFilter::kBroadcast) |
    ToRaw(PacketFilter::kAllMulticast) | ToRaw(PacketFilter::kPromiscuous);
static_assert(kDefaultPacketFilter == 0x0000'002DU);

// =============================================================================
// Medium, physical medium, device flags
// =============================================================================

enum class Medium : std::uint32_t {
  kEthernet = 0x0000'0000U,  ///< 802.3; also UNSPECIFIED. This project only supports
                             ///< this one.
  kTokenRing = 1,            ///< 802.5
  kFddi = 2,
  kWan = 3,
  kLocalTalk = 4,
  kArcnetRaw = 6,
  kArcnet8782 = 7,  ///< ARCNET 878.2
  kAtm = 8,
  kWirelessLan = 9,
  kIrda = 0x0A,
  kBpc = 0x0B,
  kCoWan = 0x0C,
  kIeee1394 = 0x0D,
};

enum class PhysicalMedium : std::uint32_t {
  kUnspecified = 0,
  kWirelessLan = 1,
  kCableModem = 2,
  kPhoneLine = 3,
  kPowerLine = 4,
  kDsl = 5,
  kFibreChannel = 6,
  kIeee1394 = 7,
  kWirelessWan = 8,
};

/// DeviceFlags bitmask of INITIALIZE_CMPLT.
enum class DeviceFlags : std::uint32_t {
  kConnectionless = 0x0000'0001U,      ///< Ethernet devices should set this bit.
  kConnectionOriented = 0x0000'0002U,  ///< CONDIS, not supported by this project.
  kRawData = 0x0000'0004U,
};

/// Values of OID_GEN_MEDIA_CONNECT_STATUS. **Note 0 means connected.**
enum class MediaState : std::uint32_t {
  kConnected = 0,
  kDisconnected = 1,
};

/// OID_GEN_MAC_OPTIONS bitmask.
enum class MacOption : std::uint32_t {
  kCopyLookaheadData = 0x0000'0001U,
  kReceiveSerialized = 0x0000'0002U,
  kTransfersNotPend = 0x0000'0004U,
  kNoLoopback = 0x0000'0008U,
  kFullDuplex = 0x0000'0010U,
  kEotxIndication = 0x0000'0020U,
  k8021pPriority = 0x0000'0040U,
};

// =============================================================================
// USB-layer constants (control channel and descriptor identification)
// =============================================================================

/// bRequest for SEND_ENCAPSULATED_COMMAND (CDC specification).
inline constexpr std::uint8_t kRequestSendEncapsulatedCommand = 0x00;
/// bRequest for GET_ENCAPSULATED_RESPONSE.
inline constexpr std::uint8_t kRequestGetEncapsulatedResponse = 0x01;

/// bmRequestType: host -> device, class request, interface recipient.
inline constexpr std::uint8_t kControlOutRequestType = 0x21;
/// bmRequestType: device -> host, class request, interface recipient.
inline constexpr std::uint8_t kControlInRequestType = 0xA1;

/// Notification on the interrupt IN endpoint: 8 bytes = two LE32s.
///
/// Note this is **not** the CDC usb_cdc_notification structure, but RNDIS's own format:
///   offset 0..3 = Notification (0x00000001 = RESPONSE_AVAILABLE)
///   offset 4..7 = Reserved (0)
inline constexpr std::uint32_t kNotificationBytes = 8;
inline constexpr std::uint32_t kNotificationResponseAvailable = 0x0000'0001U;
inline constexpr std::uint32_t kNotificationValueOffset = 0;
inline constexpr std::uint32_t kNotificationReservedOffset = 4;

/// USB interface class/subclass/protocol triple, used to identify RNDIS devices.
struct InterfaceSignature {
  std::uint8_t interface_class;
  std::uint8_t interface_subclass;
  std::uint8_t interface_protocol;

  [[nodiscard]] constexpr bool operator==(const InterfaceSignature&) const = default;
};

/// All known forms of RNDIS communication-class interface (four seen in practice).
///
/// (a) Standard MS RNDIS: CDC Communication / ACM / vendor-specific protocol
/// (b) ActiveSync (Windows Mobile 5 / Windows Phone): USB_CLASS_MISC
/// (c) Wireless RNDIS (phone USB tethering, WWAN modules): Wireless Controller / RF /
///     RNDIS
/// (d) Novatel/Verizon USB730L variant
inline constexpr InterfaceSignature kControlSignatureMicrosoft{0x02, 0x02, 0xFF};
inline constexpr InterfaceSignature kControlSignatureActiveSync{0xEF, 0x01, 0x01};
inline constexpr InterfaceSignature kControlSignatureWireless{0xE0, 0x01, 0x03};
inline constexpr InterfaceSignature kControlSignatureNovatel{0xEF, 0x04, 0x01};

/// RNDIS data-class interface: CDC Data, no subclass, no protocol.
inline constexpr InterfaceSignature kDataSignature{0x0A, 0x00, 0x00};

/// Whether this is a known RNDIS communication-class interface signature.
[[nodiscard]] constexpr bool IsRndisControlSignature(const InterfaceSignature& signature) noexcept {
  return signature == kControlSignatureMicrosoft || signature == kControlSignatureActiveSync ||
         signature == kControlSignatureWireless || signature == kControlSignatureNovatel;
}

// =============================================================================
// Frame size derivation
// =============================================================================

/// Ethernet header length.
inline constexpr std::uint32_t kEthernetHeaderBytes = 14;

/// "Hard header" of the RNDIS data channel = Ethernet header + PACKET_MSG header.
///
/// Linux's hard_header_len is this 58; MaxTransferSize derivation uses it as the base.
inline constexpr std::uint32_t kHardHeaderBytes = kEthernetHeaderBytes + kPacketMsgHeaderBytes;
static_assert(kHardHeaderBytes == 58);

/// Standard Ethernet MTU.
inline constexpr std::uint32_t kDefaultMtu = 1500;

/// Given an MTU, compute the maximum bytes a frame occupies on USB (including RNDIS
/// header and Ethernet header).
[[nodiscard]] constexpr std::uint32_t HardMtuFor(std::uint32_t mtu) noexcept {
  return mtu + kHardHeaderBytes;
}

/// Compute the MaxTransferSize to declare in INITIALIZE_MSG.
///
/// Follows Linux's algorithm: `(hard_mtu + maxpacket + 1) & ~(maxpacket - 1)`, i.e.
/// round up to an integer multiple of the endpoint max packet size and leave one packet
/// of headroom. For MTU=1500 on high-speed (maxpacket=512) this yields 2048; on
/// full-speed (64) it yields 1600.
[[nodiscard]] constexpr std::uint32_t MaxTransferSizeFor(std::uint32_t mtu,
                                                         std::uint32_t endpoint_max_packet) noexcept {
  const std::uint32_t hard_mtu = HardMtuFor(mtu);
  return (hard_mtu + endpoint_max_packet + 1) & ~(endpoint_max_packet - 1);
}

static_assert(MaxTransferSizeFor(1500, 512) == 2048);
static_assert(MaxTransferSizeFor(1500, 64) == 1600);

/// Hard upper limit of MaxTransferSize for USB 1.1 devices (required by specification).
inline constexpr std::uint32_t kUsb11MaxTransferSize = 0x4000;

}  // namespace tethernet::rndis
