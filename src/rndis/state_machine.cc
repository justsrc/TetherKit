#include "tetherkit/rndis/state_machine.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>

#include "tetherkit/common/byte_order.h"
#include "tetherkit/common/i18n.h"
#include "tetherkit/common/logging.h"

namespace tetherkit::rndis {
namespace {

/// Placeholder buffer size used when QUERYing a fixed-length OID.
///
/// For OIDs that return a fixed-length result, InformationBufferLength must be
/// >= the expected response length or Microsoft's ActiveSync implementation
/// replies with RNDIS_STATUS_INVALID_LENGTH. Linux passes 48 when querying the
/// MAC; we reuse the same "loose but sufficient" value as the default
/// placeholder for most fixed-length OIDs.
constexpr std::uint32_t kFixedOidPlaceholderBytes = 48;

/// Extract a string from an ASCII buffer that may lack a NUL terminator.
[[nodiscard]] std::string ToPrintableString(std::span<const std::byte> bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const std::byte byte : bytes) {
    const auto character = static_cast<char>(byte);
    if (character == '\0') {
      break;  // some devices do NUL-terminate; stop when we see it
    }
    // Filter out non-printable characters so control chars never reach the log.
    text.push_back(character >= 0x20 && character < 0x7F ? character : '?');
  }
  return text;
}

}  // namespace

std::string_view StateName(State state) noexcept {
  switch (state) {
    case State::kUninitialized:
      return Text(Msg::kRndisStateUninitialized);
    case State::kInitializing:
      return Text(Msg::kRndisStateInitializing);
    case State::kInitialized:
      return Text(Msg::kRndisStateInitialized);
    case State::kDataInitialized:
      return Text(Msg::kRndisStateDataInitialized);
    case State::kHalting:
      return Text(Msg::kRndisStateHalting);
  }
  return Text(Msg::kRndisStateUnknown);
}

StateMachine::StateMachine(ControlChannel& channel, StateMachineObserver& observer,
                           const StateMachineConfig& config)
    : channel_(&channel), observer_(&observer), config_(config) {
  request_buffer_.resize(kControlBufferBytes);
  last_device_activity_ = MonotonicNanos();
  next_keepalive_deadline_ =
      last_device_activity_ +
      static_cast<Nanos>(config_.keepalive_interval_millis) * kNanosPerMilli;
}

std::uint32_t StateMachine::NextRequestId() noexcept {
  // Skip 0: some devices treat RequestId == 0 as invalid (FreeBSD's
  // implementation fills everything with 0, showing devices generally do not
  // validate it, but we avoid it conservatively anyway).
  const std::uint32_t id = next_request_id_++;
  if (next_request_id_ == 0) {
    next_request_id_ = 1;
  }
  return id;
}

void StateMachine::TransitionTo(State next) {
  if (state_ == next) {
    return;
  }
  const State previous = state_;
  state_ = next;
  TETHERKIT_INFO_TR(Msg::kRndisStateTransition, StateName(previous), StateName(next));
  observer_->OnStateChanged(previous, next);
}

// =============================================================================
// Control channel round trips
// =============================================================================

Result<bool> StateMachine::PumpOnce(std::optional<MessageType> expected_reply,
                                   std::span<const std::byte>& out_response) {
  TETHERKIT_ASSIGN_OR_RETURN(const std::span<const std::byte> message,
                             channel_->ReceiveMessage());

  if (message.empty()) {
    // Spec: the device returns a single 0x00 byte instead of STALL while it
    // has no valid response yet. Not an error.
    return false;
  }
  MarkDeviceActivity();

  TETHERKIT_ASSIGN_OR_RETURN(const MessageHeader header, DecodeMessageHeader(message));
  const std::string_view name = MessageTypeName(header.message_type);
  TETHERKIT_DEBUG_TR(Msg::kRndisControlMessageReceived,
                     name.empty() ? Text(Msg::kRndisUnknownMessage) : name, header.message_type,
                     header.message_length);

  // ---- Unsolicited device pushes: dispatch them and keep waiting for ours ----
  if (header.message_type == ToRaw(MessageType::kIndicateStatus)) {
    HandleIndicateStatus(message);
    return false;
  }
  if (header.message_type == ToRaw(MessageType::kKeepAlive)) {
    // Device-initiated keep-alive. We **must reply**, otherwise the device may
    // decide the host is dead and disconnect.
    TETHERKIT_RETURN_IF_ERROR(HandleDeviceKeepAlive(message));
    return false;
  }

  // ---- Messages from connection-oriented devices: report them as unsupported
  // explicitly rather than with a vague "unknown message" ----
  if (IsCondis(header.message_type)) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisCondisMessageReceived,
                                             name.empty() ? Text(Msg::kRndisUnknown) : name)));
  }

  // ---- Is this the one we are waiting for? ----
  if (expected_reply.has_value() && header.message_type == ToRaw(*expected_reply)) {
    out_response = message;
    return true;
  }

  // Not the one we awaited, and not a known push. It may be a late response to
  // a previously timed-out request -- drop it and continue (treating it as
  // fatal would let a single timeout kill the link).
  TETHERKIT_WARN_TR(Msg::kRndisDiscardUnexpectedMessage,
                    name.empty() ? Text(Msg::kRndisUnknown) : name, header.message_type);
  return false;
}

Result<StateMachine::Exchange> StateMachine::Transact(std::span<const std::byte> request,
                                                      MessageType expected_reply) {
  TETHERKIT_RETURN_IF_ERROR(channel_->SendMessage(request));

  // Wait for the interrupt endpoint's RESPONSE_AVAILABLE notification first,
  // then fetch the response.
  //
  // Both device behaviors must be accommodated:
  //   * the spec-conforming one waits for the notification;
  //   * but some devices only answer on the control endpoint after the
  //     interrupt endpoint has been read at least once;
  //   * and Linux simply ignores the interrupt endpoint and polls the control
  //     endpoint directly.
  // So: if there is an interrupt endpoint, wait for one notification (a
  // timeout is not an error either), then poll regardless.
  // Never 0 either (timeout=0 means infinite wait on darwin). Note
  // response_poll_interval may be configured as 0 (it is in tests), so max()
  // must backstop it here.
  const std::uint32_t notification_timeout =
      std::max(kProbeOnlyTimeoutMillis,
               std::min(config_.control_timeout_millis, config_.response_poll_interval_millis * 4));
  const NotificationResult notification =
      channel_->WaitForNotification(notification_timeout);
  if (notification == NotificationResult::kResponseAvailable) {
    TETHERKIT_TRACE_TR(Msg::kRndisNotificationReceived);
  }

  std::span<const std::byte> response;
  for (std::uint32_t attempt = 0; attempt < config_.response_poll_attempts; ++attempt) {
    TETHERKIT_ASSIGN_OR_RETURN(const bool matched, PumpOnce(expected_reply, response));
    if (matched) {
      return Exchange{.response = response};
    }
    // Not ready yet. Sleep briefly and retry -- we must sleep here, otherwise
    // the control endpoint turns into a busy-poll, and each control transfer
    // costs milliseconds on Apple Silicon.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.response_poll_interval_millis));
  }

  const std::string_view name = MessageTypeName(ToRaw(expected_reply));
  return std::unexpected(Error::Generic(
      Tr(Msg::kRndisWaitReplyTimeout, name.empty() ? Text(Msg::kRndisResponse) : name,
         config_.response_poll_attempts, config_.response_poll_interval_millis)));
}

// =============================================================================
// Handling unsolicited device pushes
// =============================================================================

void StateMachine::HandleIndicateStatus(std::span<const std::byte> message) {
  const auto indication = DecodeIndicateStatus(message);
  if (!indication) {
    // A parse failure must not kill the link -- this is a pure notification.
    TETHERKIT_WARN_TR(Msg::kRndisIndicateStatusParseFailed, indication.error().ToString());
    return;
  }

  const std::string_view status_name = StatusName(indication->status);
  if (indication->has_diagnostic_info) {
    // The device uses Rndis_Diagnostic_Info to tell us "byte N of the message
    // you sent is invalid", which is invaluable for tracking down our own
    // encoding bugs, so we log it separately.
    TETHERKIT_WARN_TR(Msg::kRndisDeviceIndicatedWithDiagnostics,
                      status_name.empty() ? Text(Msg::kRndisUnknownStatus) : status_name,
                      indication->status, indication->diagnostic_status,
                      indication->diagnostic_error_offset);
  } else {
    TETHERKIT_INFO_TR(Msg::kRndisDeviceIndicated,
                      status_name.empty() ? Text(Msg::kRndisUnknownStatus) : status_name,
                      indication->status);
  }

  switch (static_cast<StatusCode>(indication->status)) {
    case StatusCode::kMediaConnect:
      if (!link_connected_) {
        link_connected_ = true;
        info_.media_state = MediaState::kConnected;
        observer_->OnLinkStateChanged(true);
      }
      break;

    case StatusCode::kMediaDisconnect:
      if (link_connected_) {
        link_connected_ = false;
        info_.media_state = MediaState::kDisconnected;
        observer_->OnLinkStateChanged(false);
      }
      break;

    default:
      // Other statuses are only logged. In particular kInvalidData -- it means
      // the message we sent was malformed, but we should not drop the link over
      // it (the diagnostics have already been printed).
      break;
  }
}

Status StateMachine::HandleDeviceKeepAlive(std::span<const std::byte> message) {
  // A device-initiated KEEPALIVE_MSG carries its RequestId at offset 8, with
  // the same layout as when the host initiates it.
  if (message.size() < kKeepAliveMsgBytes) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisKeepAliveMsgTooShort)));
  }
  const std::uint32_t request_id = LoadLe32(message.data() + kKeepAliveRequestIdOffset);
  TETHERKIT_DEBUG_TR(Msg::kRndisDeviceKeepAlive, request_id);

  TETHERKIT_ASSIGN_OR_RETURN(
      const std::uint32_t written,
      EncodeKeepAliveComplete(request_id, ToRaw(StatusCode::kSuccess), request_buffer_));
  return channel_->SendMessage(std::span<const std::byte>{request_buffer_.data(), written});
}

// =============================================================================
// OID reads and writes
// =============================================================================

Result<std::span<const std::byte>> StateMachine::QueryOid(Oid oid, std::uint32_t expected_bytes,
                                                          bool fatal) {
  const std::string_view name = OidName(ToRaw(oid));
  TETHERKIT_ASSIGN_OR_RETURN(
      const std::uint32_t written,
      Encode(QueryRequest{.request_id = NextRequestId(),
                          .oid = oid,
                          .expected_response_bytes = expected_bytes},
             request_buffer_));

  auto exchange = Transact(std::span<const std::byte>{request_buffer_.data(), written},
                           MessageType::kQueryComplete);
  if (!exchange) {
    if (!fatal) {
      TETHERKIT_DEBUG_TR(Msg::kRndisOptionalOidQueryFailed, name, exchange.error().ToString());
      return std::span<const std::byte>{};
    }
    return std::unexpected(
        std::move(exchange).error().WithContext(Tr(Msg::kRndisOidQueryFailed, name)));
  }

  TETHERKIT_ASSIGN_OR_RETURN(const QueryComplete complete,
                             DecodeQueryComplete(exchange->response));

  if (complete.status != ToRaw(StatusCode::kSuccess)) {
    const std::string_view status_name = StatusName(complete.status);
    if (!fatal) {
      // An optional OID returning NOT_SUPPORTED is perfectly normal (e.g.
      // OID_GEN_PHYSICAL_MEDIUM).
      TETHERKIT_DEBUG_TR(Msg::kRndisOptionalOidUnsupported, name,
                         status_name.empty() ? Text(Msg::kRndisUnknownStatus) : status_name);
      return std::span<const std::byte>{};
    }
    return std::unexpected(Error::FromRndisStatus(
        complete.status,
        Tr(Msg::kRndisOidQueryRejected, name,
           status_name.empty() ? Text(Msg::kRndisUnknownStatus) : status_name)));
  }
  return complete.information;
}

Status StateMachine::SetOidUint32(Oid oid, std::uint32_t value) {
  const std::string_view name = OidName(ToRaw(oid));
  TETHERKIT_ASSIGN_OR_RETURN(
      const std::uint32_t written,
      EncodeSetUint32(NextRequestId(), oid, value, request_buffer_));

  TETHERKIT_ASSIGN_OR_RETURN(
      const Exchange exchange,
      Transact(std::span<const std::byte>{request_buffer_.data(), written},
               MessageType::kSetComplete));

  TETHERKIT_ASSIGN_OR_RETURN(const SetComplete complete, DecodeSetComplete(exchange.response));
  if (complete.status != ToRaw(StatusCode::kSuccess)) {
    const std::string_view status_name = StatusName(complete.status);
    return std::unexpected(Error::FromRndisStatus(
        complete.status,
        Tr(Msg::kRndisOidSetRejected, name, value,
           status_name.empty() ? Text(Msg::kRndisUnknownStatus) : status_name)));
  }
  TETHERKIT_DEBUG_TR(Msg::kRndisOidSet, name, value);
  return Ok();
}

// =============================================================================
// Start-up sequence
// =============================================================================

Status StateMachine::CollectDeviceInfo() {
  // ---- Physical medium: **optional OID**, a failure is not fatal ----
  // Linux passes in_len 4 when querying it and treats failure as UNSPECIFIED,
  // then continues.
  if (const auto medium = QueryOid(Oid::kGenPhysicalMedium, 4, /*fatal=*/false);
      medium && !medium->empty()) {
    if (const auto value = ParseUint32(*medium)) {
      info_.physical_medium = static_cast<PhysicalMedium>(*value);
    }
  }

  // ---- Permanent MAC: **fatal** ----
  // This is the address the host-side feth will adopt; without it we cannot
  // build the interface correctly.
  {
    TETHERKIT_ASSIGN_OR_RETURN(
        const std::span<const std::byte> payload,
        QueryOid(Oid::kEthernetPermanentAddress, kFixedOidPlaceholderBytes, /*fatal=*/true));
    TETHERKIT_ASSIGN_OR_RETURN(info_.permanent_address, ParseMac(payload));
    info_.has_permanent_address = true;
    TETHERKIT_INFO_TR(Msg::kRndisPermanentMac, FormatMac(info_.permanent_address).data());
  }

  // ---- Current MAC: not fatal (most devices report the same as permanent) ----
  if (const auto payload =
          QueryOid(Oid::kEthernetCurrentAddress, kFixedOidPlaceholderBytes, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto mac = ParseMac(*payload)) {
      info_.current_address = *mac;
      info_.has_current_address = true;
    }
  }
  if (!info_.has_current_address) {
    info_.current_address = info_.permanent_address;
    info_.has_current_address = true;
  }

  // ---- Maximum frame size: not fatal (negotiation already gave us an MTU) ----
  if (const auto payload =
          QueryOid(Oid::kGenMaximumFrameSize, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.maximum_frame_size = *value;
      // The device says it accepts at most N bytes of payload while we
      // negotiated a larger MTU -- the device wins.
      if (*value != 0 && *value < parameters_.mtu) {
        TETHERKIT_WARN_TR(Msg::kRndisMtuLoweredToDeviceFrameSize, *value, parameters_.mtu);
        parameters_.mtu = *value;
      }
    }
  }

  // ---- Link speed: not fatal, logging only ----
  if (const auto payload = QueryOid(Oid::kGenLinkSpeed, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.link_speed_100bps = *value;
    }
  }

  // ---- Media connection status: not fatal. **Note 0 means connected** ----
  if (const auto payload = QueryOid(Oid::kGenMediaConnectStatus, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.media_state = static_cast<MediaState>(*value);
      link_connected_ = info_.media_state == MediaState::kConnected;
    }
  } else {
    // If we cannot query it, assume connected -- a device running RNDIS has a
    // good chance of having a working link.
    link_connected_ = true;
  }

  // ---- Vendor information: diagnostics only ----
  if (const auto payload = QueryOid(Oid::kGenVendorId, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.vendor_id = *value;
    }
  }
  // The vendor description is a **variable-length** OID, so expected_bytes
  // must be 0.
  if (const auto payload = QueryOid(Oid::kGenVendorDescription, 0, /*fatal=*/false);
      payload && !payload->empty()) {
    info_.vendor_description = ToPrintableString(*payload);
  }

  TETHERKIT_INFO_TR(Msg::kRndisDeviceInfo, FormatMac(info_.permanent_address).data(),
                    info_.LinkSpeedMbps(), info_.maximum_frame_size,
                    static_cast<std::uint32_t>(info_.physical_medium), info_.vendor_id,
                    info_.vendor_description);

  return Ok();
}

Status StateMachine::Start() {
  if (state_ != State::kUninitialized) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisStartWrongState, StateName(state_))));
  }

  TransitionTo(State::kInitializing);

  // ---- Step 1: INITIALIZE ----
  {
    TETHERKIT_ASSIGN_OR_RETURN(
        const std::uint32_t written,
        Encode(InitializeRequest{.request_id = NextRequestId(),
                                 .max_transfer_size = config_.host_max_transfer_size},
               request_buffer_));

    auto exchange = Transact(std::span<const std::byte>{request_buffer_.data(), written},
                              MessageType::kInitializeComplete);
    if (!exchange) {
      TransitionTo(State::kUninitialized);
      return std::unexpected(std::move(exchange).error().WithContext(Tr(Msg::kRndisInitFailed)));
    }

    auto complete = DecodeInitializeComplete(exchange->response);
    if (!complete) {
      SendHalt();
      TransitionTo(State::kUninitialized);
      return std::unexpected(std::move(complete).error());
    }

    auto negotiated = Negotiate(*complete, config_.requested_mtu, config_.host_max_transfer_size);
    if (!negotiated) {
      SendHalt();
      TransitionTo(State::kUninitialized);
      return std::unexpected(std::move(negotiated).error());
    }
    parameters_ = *negotiated;

    // The packet count the device reports is only its **claim**; callers may
    // distrust it (see the config field's comment).
    if (config_.max_tx_packets_per_message != 0 &&
        parameters_.max_packets_per_message > config_.max_tx_packets_per_message) {
      TETHERKIT_INFO_TR(Msg::kRndisMaxPacketsClamped, parameters_.max_packets_per_message,
                        config_.max_tx_packets_per_message);
      parameters_.max_packets_per_message = config_.max_tx_packets_per_message;
    }

    TETHERKIT_INFO_TR(Msg::kRndisNegotiated, complete->major_version, complete->minor_version,
                      parameters_.mtu, parameters_.device_max_transfer_size,
                      parameters_.max_packets_per_message, parameters_.tx_alignment_bytes);
  }

  TransitionTo(State::kInitialized);

  // ---- Steps 2-4: collect device information ----
  if (const auto status = CollectDeviceInfo(); !status) {
    SendHalt();
    TransitionTo(State::kUninitialized);
    return status;
  }

  // ---- Step 5: set the packet filter -> the device enters data-initialized
  // and data starts flowing ----
  if (const auto status = SetOidUint32(Oid::kGenCurrentPacketFilter, config_.packet_filter);
      !status) {
    SendHalt();
    TransitionTo(State::kUninitialized);
    return std::unexpected(Error{status.error()}.WithContext(Tr(Msg::kRndisPacketFilterFailed)));
  }
  active_packet_filter_ = config_.packet_filter;

  TransitionTo(State::kDataInitialized);
  observer_->OnNegotiated(parameters_, info_);
  observer_->OnLinkStateChanged(link_connected_);

  MarkDeviceActivity();
  next_keepalive_deadline_ =
      MonotonicNanos() +
      static_cast<Nanos>(config_.keepalive_interval_millis) * kNanosPerMilli;
  return Ok();
}

// =============================================================================
// Periodic work
// =============================================================================

std::uint32_t StateMachine::MillisUntilNextPoll() const noexcept {
  const Nanos now = MonotonicNanos();
  if (now >= next_keepalive_deadline_) {
    return 0;
  }
  return static_cast<std::uint32_t>((next_keepalive_deadline_ - now) / kNanosPerMilli);
}

Status StateMachine::Poll() {
  if (state_ != State::kDataInitialized && state_ != State::kInitialized) {
    return Ok();  // not ready or shutting down; nothing to do
  }

  // ---- Drain unsolicited device pushes first ----
  //
  // If we do not, they pile up in the device's response queue and push the
  // responses to our own requests behind them, forcing every Transact to spin
  // through several extra rounds to get the reply it wants.
  //
  // Only read the control endpoint when there is an interrupt notification --
  // otherwise every Poll would issue a GET_ENCAPSULATED_RESPONSE, which costs
  // milliseconds on Apple Silicon.
  //
  // Warning: **never pass 0 here**: on darwin libusb treats timeout=0 as an
  // infinite wait and would wedge the control thread forever. Use
  // kProbeOnlyTimeoutMillis (1 ms) -- Poll itself only runs every few hundred
  // milliseconds, so a 1 ms block is negligible.
  if (channel_->WaitForNotification(kProbeOnlyTimeoutMillis) ==
      NotificationResult::kResponseAvailable) {
    std::span<const std::byte> ignored;
    // Drain at most 8 messages so a device that pushes frantically cannot
    // pin us here.
    for (int i = 0; i < 8; ++i) {
      const auto pumped = PumpOnce(std::nullopt, ignored);
      if (!pumped) {
        return std::unexpected(
            Error{pumped.error()}.WithContext(Tr(Msg::kRndisDrainControlFailed)));
      }
      // PumpOnce returns false without an error when there are no more
      // messages, so it cannot distinguish "queue empty" from "handled a
      // push". Check the notification state once more.
      if (channel_->WaitForNotification(kProbeOnlyTimeoutMillis) !=
          NotificationResult::kResponseAvailable) {
        break;
      }
    }
  }

  // ---- Keep-alive ----
  //
  // The semantics are "more than a keep-alive period has passed since we last
  // received anything from the device", not an unconditional timer. When the
  // channel is busy a pointless keep-alive round trip is pure waste (on Apple
  // Silicon especially).
  const Nanos now = MonotonicNanos();
  if (now < next_keepalive_deadline_) {
    return Ok();
  }

  const Nanos idle_nanos = now - last_device_activity_;
  const Nanos interval_nanos =
      static_cast<Nanos>(config_.keepalive_interval_millis) * kNanosPerMilli;
  next_keepalive_deadline_ = now + interval_nanos;

  if (idle_nanos < interval_nanos) {
    // The channel has been carrying data all along; no keep-alive needed.
    return Ok();
  }

  TETHERKIT_ASSIGN_OR_RETURN(const std::uint32_t written,
                             EncodeKeepAlive(NextRequestId(), request_buffer_));
  auto exchange = Transact(std::span<const std::byte>{request_buffer_.data(), written},
                            MessageType::kKeepAliveComplete);
  if (!exchange) {
    ++consecutive_keepalive_failures_;
    TETHERKIT_WARN_TR(Msg::kRndisKeepAliveFailed, consecutive_keepalive_failures_,
                      config_.keepalive_failure_threshold, exchange.error().ToString());
    if (consecutive_keepalive_failures_ >= config_.keepalive_failure_threshold) {
      Error fatal = std::move(exchange).error().WithContext(
          Tr(Msg::kRndisKeepAliveDeadLink, consecutive_keepalive_failures_));
      observer_->OnFatalError(fatal);
      return std::unexpected(std::move(fatal));
    }
    return Ok();  // below the threshold; try again next period
  }

  const auto complete = DecodeKeepAliveComplete(exchange->response);
  if (!complete) {
    ++consecutive_keepalive_failures_;
    return Ok();
  }

  if (complete->status != ToRaw(StatusCode::kSuccess)) {
    // The device explicitly returned a failure status. That usually means it
    // wants us to reset.
    const std::string_view name = StatusName(complete->status);
    TETHERKIT_WARN_TR(Msg::kRndisKeepAliveRejected,
                      name.empty() ? Text(Msg::kRndisUnknownStatus) : name);
    return Reset();
  }

  consecutive_keepalive_failures_ = 0;
  TETHERKIT_TRACE_TR(Msg::kRndisKeepAliveOk);
  return Ok();
}

// =============================================================================
// Reset
// =============================================================================

Status StateMachine::Reset() {
  TETHERKIT_INFO_TR(Msg::kRndisResetStarted);

  // RESET_MSG has **no RequestId** (offset 8 is Reserved), so it cannot be
  // paired by ID; only one RESET may be in flight at a time.
  TETHERKIT_ASSIGN_OR_RETURN(const std::uint32_t written, EncodeReset(request_buffer_));
  TETHERKIT_ASSIGN_OR_RETURN(
      const Exchange exchange,
      Transact(std::span<const std::byte>{request_buffer_.data(), written},
               MessageType::kResetComplete));

  TETHERKIT_ASSIGN_OR_RETURN(const ResetComplete complete,
                             DecodeResetComplete(exchange.response));

  if (complete.status != ToRaw(StatusCode::kSuccess)) {
    const std::string_view name = StatusName(complete.status);
    return std::unexpected(Error::FromRndisStatus(
        complete.status, Tr(Msg::kRndisResetRejected,
                            name.empty() ? Text(Msg::kRndisUnknownStatus) : name)));
  }

  TETHERKIT_INFO_TR(Msg::kRndisResetDone, Text(complete.addressing_reset
                                                    ? Msg::kRndisAddressingLost
                                                    : Msg::kRndisAddressingKept));
  observer_->OnDeviceReset(complete.addressing_reset);

  if (complete.addressing_reset) {
    // AddressingReset non-zero -> the device dropped the packet filter and the
    // multicast table, so we **must resend**, otherwise data will not flow
    // again after the reset.
    //
    // (This driver does not maintain a multicast table -- ALL_MULTICAST is set
    //   in the packet filter, so only the filter needs resending. If a
    //   SET OID_802_3_MULTICAST_LIST is ever added, replay it here too.)
    const std::uint32_t filter =
        active_packet_filter_ != 0 ? active_packet_filter_ : config_.packet_filter;
    TETHERKIT_RETURN_IF_ERROR(SetOidUint32(Oid::kGenCurrentPacketFilter, filter));
    active_packet_filter_ = filter;
    TETHERKIT_INFO_TR(Msg::kRndisPacketFilterReplayed, filter);
  }

  consecutive_keepalive_failures_ = 0;
  MarkDeviceActivity();
  return Ok();
}

// =============================================================================
// Shutdown
// =============================================================================

void StateMachine::SendHalt() noexcept {
  // The device does **not** reply to HALT_MSG; once sent we may consider the
  // state uninitialized.
  const auto written = EncodeHalt(NextRequestId(), request_buffer_);
  if (!written) {
    TETHERKIT_WARN_TR(Msg::kRndisEncodeHaltFailed, written.error().ToString());
    return;
  }
  if (const auto status =
          channel_->SendMessage(std::span<const std::byte>{request_buffer_.data(), *written});
      !status) {
    // Failures on the shutdown path are only logged -- the device may already
    // have been unplugged, which is perfectly normal.
    TETHERKIT_DEBUG_TR(Msg::kRndisSendHaltFailed, status.error().ToString());
    return;
  }
  TETHERKIT_DEBUG_TR(Msg::kRndisHaltSent);
}

void StateMachine::Stop() {
  if (state_ == State::kUninitialized || state_ == State::kHalting) {
    return;
  }
  TransitionTo(State::kHalting);

  // Zero the packet filter first so the device stops sending data (spec: a
  // filter of 0 drops the device back to RNDIS-initialized), then send HALT.
  // Continue to HALT even if clearing fails -- without it the device keeps
  // believing the host is still there, and the next plug-in starts unclean.
  if (active_packet_filter_ != 0) {
    if (const auto status = SetOidUint32(Oid::kGenCurrentPacketFilter, 0); !status) {
      TETHERKIT_DEBUG_TR(Msg::kRndisClearPacketFilterFailed, status.error().ToString());
    } else {
      active_packet_filter_ = 0;
    }
  }

  SendHalt();
  TransitionTo(State::kUninitialized);
}

}  // namespace tetherkit::rndis
