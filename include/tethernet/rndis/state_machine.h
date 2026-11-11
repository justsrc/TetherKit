// RNDIS host-side state machine.
//
// State diagram (the specification defines three device-side states; the host side
// mirrors them and adds transitional states):
//
//                     +------------------+
//        +----------->|  kUninitialized  |<------------+
//        |            +--------+---------+             |
//        |                     | Start()               | HALT reply received /
//        |                     | send INITIALIZE_MSG   | bus disconnected / fatal
//        |                     v                       | error
//        |            +------------------+             |
//        |            |  kInitializing   +-------------+
//        |            +--------+---------+ neg failure |
//        |                     | receive               |
//        |                     | INITIALIZE_CMPLT      |
//        |                     | and negotiation ok    |
//        |                     v                       |
//        |            +------------------+             |
//        |            |  kInitialized    |             |  <- control messages can be
//        |            +--------+---------+             |     exchanged, but the data
//        |                     | QUERY MAC/link, etc.  |     channel is **not open**
//        |                     | SET packet filter!=0  |
//        |                     v                       |
//        |            +------------------+             |
//        |        +---+ kDataInitialized +-------------+
//        |        |   +--------+---------+             |
//        |        |            | Stop()                |
//        |  SET filter=0       | send HALT_MSG         |
//        |  (step back)        v                       |
//        |            +------------------+             |
//        +------------+    kHalting      +-------------+
//                     +------------------+
//
//   Semantics from the specification:
//     * After bus initialization the device is in RNDIS-uninitialized;
//     * Receiving INITIALIZE_MSG and replying INITIALIZE_CMPLT(SUCCESS) ->
//       RNDIS-initialized;
//     * Receiving SET(OID_GEN_CURRENT_PACKET_FILTER, **non-zero**) ->
//       RNDIS-data-initialized, **data starts flowing only here**;
//     * Receiving SET(filter = 0) while data-initialized -> back to RNDIS-initialized;
//     * Receiving HALT_MSG or bus disconnect at any time -> immediately back to
//       RNDIS-uninitialized.
//
// Three protocol details that are easy to get wrong:
//
//   1. **Control requests must be serialized.** GET_ENCAPSULATED_RESPONSE can only "take
//      the next queued response" and has no ability to select by RequestId. So only one
//      request may be in flight at a time.
//
//   2. **The device can jump the queue.** While waiting for some *_CMPLT, the device may
//      well insert a REMOTE_NDIS_INDICATE_STATUS_MSG (media connection state change) or
//      a **device-initiated** REMOTE_NDIS_KEEPALIVE_MSG first (in which case the host
//      must reply with KEEPALIVE_CMPLT, otherwise the device may assume the host is dead
//      and disconnect). Therefore the control read loop must be written as a dispatcher:
//      dispatch by MessageType, not "blindly assume the one read is the one I want".
//
//   3. **RESET has no RequestId.** RESET_MSG's offset 8 is Reserved, and RESET_CMPLT's
//      offset 8 is Status (not 12). So RESET cannot be matched by ID and only one may be
//      in flight at a time. Moreover, when AddressingReset in RESET_CMPLT is non-zero,
//      the host **must re-send** SET(OID_GEN_CURRENT_PACKET_FILTER) (and
//      SET(OID_802_3_MULTICAST_LIST) if multicast was configured) -- otherwise data will
//      not flow again after reset.
#pragma once

#include "tethernet/common/error.h"
#include "tethernet/common/time.h"
#include "tethernet/rndis/control_channel.h"
#include "tethernet/rndis/messages.h"
#include "tethernet/rndis/protocol.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace tethernet::rndis {

/// Host-side state.
enum class State : std::uint8_t {
  kUninitialized,    ///< Uninitialized. Control channel is usable, but the device has
                     ///< not agreed to start.
  kInitializing,     ///< INITIALIZE_MSG sent, waiting for INITIALIZE_CMPLT.
  kInitialized,      ///< Negotiation complete. OIDs can be read/written, but the
                     ///< **data channel is not open yet**.
  kDataInitialized,  ///< Non-zero packet filter set, data is flowing.
  kHalting,          ///< HALT_MSG sent, shutting down.
};

[[nodiscard]] std::string_view StateName(State state) noexcept;

/// Device information reported by the device that the host cares about.
struct DeviceInfo {
  /// Device's permanent MAC. This is the address the host-side feth should adopt --
  /// under RNDIS semantics the device is that network interface, and the peer's ARP
  /// table and DHCP leases are built around it.
  MacAddress permanent_address{};
  /// Current MAC. For most devices this is the same as permanent.
  MacAddress current_address{};
  bool has_permanent_address = false;
  bool has_current_address = false;

  /// Link speed, in **units of 100 bps** (this is the NDIS convention, not bps). 0
  /// means disconnected.
  std::uint32_t link_speed_100bps = 0;
  /// Maximum frame size, **excluding** the 14-byte Ethernet header. Typically 1500.
  std::uint32_t maximum_frame_size = 0;
  /// Physical medium. Optional OID; remains kUnspecified when not available.
  PhysicalMedium physical_medium = PhysicalMedium::kUnspecified;
  /// Media connection state. **Note 0 means connected.**
  MediaState media_state = MediaState::kConnected;
  /// Vendor description string (NUL termination not guaranteed; normalized to
  /// std::string here).
  std::string vendor_description;
  std::uint32_t vendor_id = 0;

  /// Link speed converted to Mbps, for logging only.
  [[nodiscard]] double LinkSpeedMbps() const noexcept {
    return static_cast<double>(link_speed_100bps) * 100.0 / 1e6;
  }
};

/// State machine configuration.
struct StateMachineConfig {
  /// MTU to request. Negotiated down by Negotiate when the device cannot accommodate it.
  std::uint32_t requested_mtu = kDefaultMtu;

  /// MaxTransferSize the host declares in INITIALIZE_MSG, also the size of our bulk IN
  /// buffer.
  ///
  /// **This is the most important lever for RNDIS throughput**: it is the **only** way
  /// to get the device to aggregate multiple REMOTE_NDIS_PACKET_MSGs into one bulk IN
  /// transfer. The larger it is, the more the device aggregates and the lower the USB and
  /// syscall overhead amortized per frame. Linux conservatively reports only 2048 (one
  /// full frame); we report 16 KiB -- enough aggregation headroom without exceeding the
  /// specification's 0x4000 limit for USB 1.1 devices.
  std::uint32_t host_max_transfer_size = 16 * 1024;

  /// Maximum PACKET_MSGs to aggregate per bulk OUT in the host -> device direction. 0
  /// means no extra limit, fully follow the device's reported MaxPacketsPerMessage.
  ///
  /// Why this knob exists: the device's reported aggregation capability is **not
  /// necessarily trustworthy**. The mainline Linux gadget's `rndis_rm_hdr()` unpacks only
  /// one PACKET_MSG per bulk OUT, yet vendor kernels may still report
  /// MaxPacketsPerMessage=10. On such a device, every frame after the first would be
  /// silently dropped by the peer; clamping to 1 falls back to "one frame per transfer".
  ///
  /// **But do not treat it as the default explanation for frame drops.** On a measured
  /// Android device (reporting 10 packets / 15800 bytes) we ran an A/B: clamping to 1
  /// was actually slower (TCP TX 125 vs 234 Mbps) and did not improve the drop rate --
  /// that device's aggregation was **genuinely effective**. So this is a diagnostic /
  /// backstop knob, not a switch for a known defect. See docs/BENCHMARKS.md "real-device
  /// end-to-end measurements".
  std::uint32_t max_tx_packets_per_message = 0;

  /// Packet filter bitmask to set.
  std::uint32_t packet_filter = kDefaultPacketFilter;

  /// Control transfer timeout (milliseconds).
  std::uint32_t control_timeout_millis = kControlTimeoutMillis;

  /// Keep-alive period (milliseconds).
  ///
  /// Semantics are "send only after this long has passed since the last message received
  /// from the device", not unconditionally on a fixed schedule. Note that on Apple
  /// Silicon libusb control transfers are about 10x slower than on x64 (issue #1288), so
  /// do not set this value too small.
  std::uint32_t keepalive_interval_millis = kKeepAliveTimeoutMillis;

  /// Maximum number of response poll attempts per control request.
  ///
  /// Linux polls up to 10 times at 40 ms intervals (about 400 ms total). We wait for the
  /// interrupt endpoint notification first and poll after timeout, so this is mainly a
  /// backstop for devices with no interrupt endpoint.
  std::uint32_t response_poll_attempts = 10;

  /// Wait between each poll (milliseconds).
  std::uint32_t response_poll_interval_millis = 40;

  /// How many consecutive keep-alive failures before the link is considered dead.
  std::uint32_t keepalive_failure_threshold = 3;
};

/// Events reported by the state machine to the outside.
///
/// Callbacks are used instead of having the caller poll state because events like "media
/// connection state change" are pushed proactively by the device (INDICATE_STATUS);
/// polling would miss edges.
class StateMachineObserver {
 public:
  StateMachineObserver() = default;
  StateMachineObserver(const StateMachineObserver&) = delete;
  StateMachineObserver& operator=(const StateMachineObserver&) = delete;
  StateMachineObserver(StateMachineObserver&&) = delete;
  StateMachineObserver& operator=(StateMachineObserver&&) = delete;
  virtual ~StateMachineObserver() = default;

  /// State transition.
  virtual void OnStateChanged(State from, State to) = 0;

  /// Negotiation complete; the data path can be built with these parameters.
  virtual void OnNegotiated(const NegotiatedParameters& parameters, const DeviceInfo& info) = 0;

  /// Link up / down (from INDICATE_STATUS MEDIA_CONNECT / MEDIA_DISCONNECT, or from the
  /// result of QUERY OID_GEN_MEDIA_CONNECT_STATUS).
  virtual void OnLinkStateChanged(bool connected) = 0;

  /// Device reset complete and the host is required to re-send addressing
  /// configuration. The data path needs a brief pause.
  virtual void OnDeviceReset(bool addressing_lost) = 0;

  /// Link is no longer usable (consecutive keep-alive failures / device disconnect /
  /// unrecoverable protocol error).
  virtual void OnFatalError(const Error& error) = 0;
};

/// RNDIS host-side state machine.
///
/// **Threading constraint: all methods of this class must be called from the same
/// thread** (the control thread). This is not an implementation shortcut but a hard
/// libusb requirement -- synchronous control transfers return LIBUSB_ERROR_BUSY on the
/// event thread, so the control channel must own a non-event thread exclusively.
class StateMachine {
 public:
  StateMachine(ControlChannel& channel, StateMachineObserver& observer,
               const StateMachineConfig& config);

  StateMachine(const StateMachine&) = delete;
  StateMachine& operator=(const StateMachine&) = delete;
  StateMachine(StateMachine&&) = delete;
  StateMachine& operator=(StateMachine&&) = delete;
  ~StateMachine() = default;

  [[nodiscard]] State CurrentState() const noexcept { return state_; }

  [[nodiscard]] const DeviceInfo& Info() const noexcept { return info_; }

  [[nodiscard]] const NegotiatedParameters& Parameters() const noexcept { return parameters_; }

  /// Run the full startup sequence until kDataInitialized.
  ///
  /// Sequence (following Linux's generic_rndis_bind order, each step grounded):
  ///   1. INITIALIZE_MSG -> INITIALIZE_CMPLT, negotiate MaxTransferSize / alignment / MTU;
  ///   2. QUERY OID_GEN_PHYSICAL_MEDIUM (**optional OID, failure is non-fatal**);
  ///   3. QUERY OID_802_3_PERMANENT_ADDRESS (get the MAC for the host side);
  ///   4. QUERY OID_802_3_CURRENT_ADDRESS / OID_GEN_MAXIMUM_FRAME_SIZE /
  ///      OID_GEN_LINK_SPEED / OID_GEN_MEDIA_CONNECT_STATUS (all non-fatal);
  ///   5. SET OID_GEN_CURRENT_PACKET_FILTER = non-zero -> data starts flowing.
  /// Any fatal step failure sends HALT_MSG before returning an error.
  [[nodiscard]] Status Start();

  /// Graceful shutdown: SET filter = 0, then send HALT_MSG.
  ///
  /// Even if errors occur midway, best effort is made to send HALT -- without it the
  /// device may keep thinking the host is still there and leave its state dirty on next
  /// plug-in.
  void Stop();

  /// Perform one round of "periodic work that needs doing". Should be called regularly
  /// by the control thread in its loop.
  ///
  /// Does two things:
  ///   * Sends a KEEPALIVE when more than the keep-alive period has passed since the last
  ///     message was received from the device;
  ///   * Drains messages proactively pushed by the device on the control channel
  ///     (INDICATE_STATUS / device-initiated KEEPALIVE) so they do not pile up.
  ///
  /// Returning an error means the link is no longer usable and the caller should
  /// reconnect.
  [[nodiscard]] Status Poll();

  /// Milliseconds until the next round of periodic work is due. For the caller to decide
  /// how long to sleep.
  [[nodiscard]] std::uint32_t MillisUntilNextPoll() const noexcept;

  /// Proactively request a device soft reset.
  ///
  /// RESET is a soft reset: the control channel stays intact, but the device discards all
  /// outstanding requests and packets. If AddressingReset in RESET_CMPLT is non-zero,
  /// this method automatically re-sends the packet filter setting.
  [[nodiscard]] Status Reset();

 private:
  /// Result of one control request-response round trip.
  struct Exchange {
    /// Byte view of the response message, pointing into the control channel's internal
    /// buffer.
    std::span<const std::byte> response;
  };

  /// Send a request and wait for its response.
  ///
  /// Correctly handles messages that jump the queue during the wait (INDICATE_STATUS /
  /// device-initiated KEEPALIVE): dispatches them and keeps waiting for the one we want.
  [[nodiscard]] Result<Exchange> Transact(std::span<const std::byte> request,
                                          MessageType expected_reply);

  /// Read and dispatch one message pushed by the device.
  ///
  /// @param expected_reply The message type we are waiting for; if read, returns true
  ///                       and writes the view to out_response. Pass std::nullopt to
  ///                       "drain only pushed messages without waiting for any specific
  ///                       reply".
  [[nodiscard]] Result<bool> PumpOnce(std::optional<MessageType> expected_reply,
                                      std::span<const std::byte>& out_response);

  /// Handle an INDICATE_STATUS message.
  void HandleIndicateStatus(std::span<const std::byte> message);

  /// Handle a KEEPALIVE_MSG initiated by the device -- **must reply with
  /// KEEPALIVE_CMPLT**.
  [[nodiscard]] Status HandleDeviceKeepAlive(std::span<const std::byte> message);

  /// Query an OID. When `fatal` is false, the device returning "not supported" is not an
  /// error.
  [[nodiscard]] Result<std::span<const std::byte>> QueryOid(Oid oid,
                                                            std::uint32_t expected_bytes,
                                                            bool fatal);

  /// Write an LE32-valued OID.
  [[nodiscard]] Status SetOidUint32(Oid oid, std::uint32_t value);

  /// Collect device information (steps 2-4). Failures of non-fatal OIDs are only logged.
  [[nodiscard]] Status CollectDeviceInfo();

  /// Send HALT_MSG. Best effort, does not return an error.
  void SendHalt() noexcept;

  void TransitionTo(State next);

  /// Allocate the next RequestId.
  ///
  /// Increments from 1 and skips 0: some devices treat 0 as an "invalid ID".
  [[nodiscard]] std::uint32_t NextRequestId() noexcept;

  /// Record "just received a message from the device", used for keep-alive idle
  /// detection.
  void MarkDeviceActivity() noexcept { last_device_activity_ = MonotonicNanos(); }

  ControlChannel* channel_;
  StateMachineObserver* observer_;
  StateMachineConfig config_;

  State state_ = State::kUninitialized;
  DeviceInfo info_;
  NegotiatedParameters parameters_;

  std::uint32_t next_request_id_ = 1;
  Nanos last_device_activity_ = 0;
  Nanos next_keepalive_deadline_ = 0;
  std::uint32_t consecutive_keepalive_failures_ = 0;

  /// Packet filter value last set successfully. Used to replay it when RESET requires it.
  std::uint32_t active_packet_filter_ = 0;

  /// Assembly buffer for request messages. Control messages are small; one buffer is
  /// enough.
  std::vector<std::byte> request_buffer_;

  bool link_connected_ = false;
};

}  // namespace tethernet::rndis
