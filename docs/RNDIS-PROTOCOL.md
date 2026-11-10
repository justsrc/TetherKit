# RNDIS protocol reference

This document is the complete wire-format detail needed to implement the RNDIS
**host side**. All values are consistent with
[`include/tetherkit/rndis/protocol.h`](../include/tetherkit/rndis/protocol.h)
(the single source of truth in the code) and were cross-checked against the Linux
kernel implementation: `drivers/net/usb/rndis_host.c`,
`include/linux/usb/rndis_host.h`, `drivers/usb/gadget/function/rndis.c`.

---

## 0. The three rules that are easiest to get wrong

### (1) All multi-byte fields are **little-endian**

The protocol originates from Windows NDIS. Never map the wire format directly
onto a packed struct -- first, it fails silently on big-endian machines, and
second, an RNDIS message's starting offset inside a USB buffer is only guaranteed
to be 4-byte aligned, while the Ethernet frame that immediately follows a
`PACKET_MSG` starts at whatever the device's `DataOffset` says, which can be any
value. This project always goes through `memcpy` + explicit byte-order
conversion (clang optimizes that into a single `ldr`/`rev`; measured 0.10 ns per
field, zero cost).

### (2) The base point of offset fields is **message start + 8**, not message start

The `DataOffset` / `OOBDataOffset` / `PerPacketInfoOffset` of `PACKET_MSG`, and
the `InformationBufferOffset` of `QUERY` / `SET` / `QUERY_CMPLT`, all use "the
start of the block the group of offset fields belongs to" as their base point,
i.e. 8 bytes after the message start:

```
absolute offset = 8 + field value
```

So when "the Ethernet frame immediately follows the 44-byte `PACKET_MSG`
header", `DataOffset` must be **36**, not 44. **This is trap number one when
implementing RNDIS.** The code pins it down with a `static_assert`:

```cpp
inline constexpr std::uint32_t kPacketInlineDataOffset =
    kPacketMsgHeaderBytes - kOffsetFieldBase;
static_assert(kPacketInlineDataOffset == 36,
              "DataOffset base point is message start + 8; see header rule 2");
```

### (3) `INDICATE_STATUS`'s `StatusBufferOffset` is the exception

Microsoft's documentation says its base point is **the message start** (+0),
**inconsistent** with the +8 convention above. This is a contradiction within the
spec itself. In practice the field is almost always 0 (`MEDIA_CONNECT` /
`MEDIA_DISCONNECT` carry no status buffer), so this project's parser tolerates
it with the following fallback order:

1. Length is 0 -> no payload, succeed immediately (the vast majority of cases);
2. First interpret it as "base point = message start + 8"; accept it if that
   lands within the message;
3. Otherwise interpret it as "base point = message start";
4. Out of range either way -> drop the status buffer but **still return success**
   -- the `status` itself is useful (it may be `MEDIA_DISCONNECT`), and the link
   must not be declared dead because one optional payload cannot be parsed.

---

## 1. Message type codes

| Message | Code | Notes |
|---|---|---|
| `REMOTE_NDIS_PACKET_MSG` | `0x00000001` | Data path |
| `REMOTE_NDIS_INITIALIZE_MSG` | `0x00000002` | |
| `REMOTE_NDIS_HALT_MSG` | `0x00000003` | **The device does not reply** |
| `REMOTE_NDIS_QUERY_MSG` | `0x00000004` | |
| `REMOTE_NDIS_SET_MSG` | `0x00000005` | |
| `REMOTE_NDIS_RESET_MSG` | `0x00000006` | **No RequestId** |
| `REMOTE_NDIS_INDICATE_STATUS_MSG` | `0x00000007` | Device-initiated, **no RequestId, no reply needed** |
| `REMOTE_NDIS_KEEPALIVE_MSG` | `0x00000008` | **Either side may initiate** |
| Completion message | request code \| `0x80000000` | |
| `RNDIS_MSG_BUS` | `0xFF000001` | Private; this project only recognizes and ignores it |

The connection-oriented (CONDIS) message family `0x00008001`~`0x00008007` is
never used by 802.3 devices. This project recognizes them only so it can report
"connection-oriented device not supported" explicitly instead of a vague "unknown
message".

---

## 2. Message lengths and field offsets

All fields are LE32.

| Message | Total length | Field offsets |
|---|---:|---|
| `INITIALIZE_MSG` | 24 | Type@0, Length@4, RequestId@8, MajorVersion@12, MinorVersion@16, MaxTransferSize@20 |
| `INITIALIZE_CMPLT` | 52 | Type@0, Length@4, RequestId@8, Status@12, MajorVersion@16, MinorVersion@20, DeviceFlags@24, Medium@28, **MaxPacketsPerMessage@32**, **MaxTransferSize@36**, **PacketAlignmentFactor@40**, AFListOffset@44, AFListSize@48 |
| `HALT_MSG` | 12 | Type@0, Length@4, RequestId@8 |
| `QUERY_MSG` = `SET_MSG` | 28 + payload | Type@0, Length@4, RequestId@8, Oid@12, InfoBufferLength@16, InfoBufferOffset@20, DeviceVcHandle@24 |
| `QUERY_CMPLT` | 24 + payload | Type@0, Length@4, RequestId@8, Status@12, InfoBufferLength@16, InfoBufferOffset@20 |
| `SET_CMPLT` | 16 | Type@0, Length@4, RequestId@8, Status@12 |
| `RESET_MSG` | 12 | Type@0, Length@4, **Reserved@8** (WARNING: not RequestId) |
| `RESET_CMPLT` | 16 | Type@0, Length@4, **Status@8** (WARNING: not 12), **AddressingReset@12** |
| `INDICATE_STATUS_MSG` | 20 + payload | Type@0, Length@4, Status@8, StatusBufferLength@12, StatusBufferOffset@16 |
| `KEEPALIVE_MSG` | 12 | Type@0, Length@4, RequestId@8 |
| `KEEPALIVE_CMPLT` | 16 | Type@0, Length@4, RequestId@8, Status@12 |
| `PACKET_MSG` header | 44 | Type@0, Length@4, **DataOffset@8**, DataLength@12, OOBDataOffset@16, OOBDataLength@20, NumOOBDataElements@24, PerPacketInfoOffset@28, PerPacketInfoLength@32, VcHandle@36, Reserved@40 |

The values the offset fields should hold when "data immediately follows the
header":

| Message | Header length | Offset field value |
|---|---:|---:|
| `PACKET_MSG` | 44 | **36** |
| `QUERY_MSG` / `SET_MSG` | 28 | **20** |
| `QUERY_CMPLT` | 24 | **16** |

---

## 3. Status codes

The key ones (for the full list see `StatusCode` in `protocol.h`):

| Name | Value | Meaning |
|---|---|---|
| `SUCCESS` | `0x00000000` | |
| `PENDING` | `0x00000103` | |
| `MEDIA_CONNECT` | `0x4001000B` | Link up |
| `MEDIA_DISCONNECT` | `0x4001000C` | Link down |
| `LINK_SPEED_CHANGE` | `0x40010013` | |
| `FAILURE` | `0xC0000001` | |
| `NOT_SUPPORTED` | `0xC00000BB` | Normal reply for an optional OID |
| `INVALID_LENGTH` | `0xC0010014` | Easy to misremember |
| `INVALID_DATA` | `0xC0010015` | Easy to misremember |
| `BUFFER_TOO_SHORT` | `0xC0010016` | |
| `INVALID_OID` | `0xC0010017` | Easy to misremember |

> These four warning-flagged values were once transcribed wrong in `error.cc`
> (written as `0xC000000D` / `0xC0010015` / `0xC0000010` / `0x80000005`). Today
> the status code table exists in exactly one place, `rndis/protocol.cc`, to
> avoid inconsistency between two copies.

Failure test: `(status & 0xC0000000) == 0xC0000000`.

---

## 4. OID

| OID | Value | Data format | Use in this project |
|---|---|---|---|
| `OID_GEN_SUPPORTED_LIST` | `0x00010101` | **Variable-length**: N LE32s | Unused |
| `OID_GEN_MAXIMUM_FRAME_SIZE` | `0x00010106` | LE32, **excludes** the Ethernet header | Validated; may lower the MTU |
| `OID_GEN_LINK_SPEED` | `0x00010107` | LE32, **unit 100 bps** | Log |
| `OID_GEN_VENDOR_ID` | `0x0001010C` | LE32; the low 24 bits are the OUI | Log |
| `OID_GEN_VENDOR_DESCRIPTION` | `0x0001010D` | **Variable-length** ASCII, **not guaranteed NUL-terminated** | Log |
| `OID_GEN_CURRENT_PACKET_FILTER` | `0x0001010E` | LE32 bitmask, readable and writable | **Data only flows after a non-zero value is set** |
| `OID_GEN_MEDIA_CONNECT_STATUS` | `0x00010114` | LE32: **0 = connected**, 1 = disconnected | Initial link state |
| `OID_GEN_PHYSICAL_MEDIUM` | `0x00010202` | LE32, **optional OID** | Log (failure is not fatal) |
| `OID_802_3_PERMANENT_ADDRESS` | `0x01010101` | 6-byte MAC | **The address the host-side feth adopts** |
| `OID_802_3_CURRENT_ADDRESS` | `0x01010102` | 6-byte MAC | Log |
| `OID_802_3_MULTICAST_LIST` | `0x01010103` | SET: 6N bytes | Unused (ALL_MULTICAST is enabled in the packet filter) |

**Warning:** for `OID_GEN_MEDIA_CONNECT_STATUS`, **0 means "connected"**, not
disconnected.

**Warning:** **do not rely on `QUERY OID_802_3_MULTICAST_LIST`**: the Linux
gadget returns a 4-byte `0xE0000000` for it, which does not match the "6-byte
MAC array" format. Use only the SET direction.

### The counter-intuitive rule for `InformationBufferLength`

This comes from sniffing the Windows driver for Microsoft ActiveSync 4.1; it is
**undocumented**:

| OID type | `InformationBufferLength` | Consequence of violating it |
|---|---|---|
| Returns a **fixed-length** result | Must be >= the expected response length, and the message tail must actually contain that many bytes | `RNDIS_STATUS_INVALID_LENGTH` |
| Returns a **variable-length** result | Must be **0** | Fails the same way |

This project's `Encode(QueryRequest)` handles it automatically based on
`IsVariableLengthOid()`; callers do not need to worry. A dedicated test case
pins this rule down.

---

## 5. Packet filter bitmask

| Name | Value |
|---|---|
| `DIRECTED` | `0x00000001` |
| `MULTICAST` | `0x00000002` |
| `ALL_MULTICAST` | `0x00000004` |
| `BROADCAST` | `0x00000008` |
| `PROMISCUOUS` | `0x00000020` |

This project uses `DIRECTED | BROADCAST | ALL_MULTICAST | PROMISCUOUS` =
**`0x0000002D`** (the same as Linux `rndis_host`).

Reason for `PROMISCUOUS`: we treat the device as an "Ethernet cable" bridged
into feth, so the host side may send and receive frames with any MAC (when
bridging further upstream or running virtual machines). Reason for
`ALL_MULTICAST`: it avoids maintaining a multicast table while still not
missing IPv6 neighbor discovery.

---

## 6. Host-side state machine

The spec defines three **device-side** states; the host side mirrors them and
adds transitional states:

```
kUninitialized --Start()/INITIALIZE--> kInitializing --CMPLT + negotiation passed--> kInitialized
      ^                                     |                                |
      |                               negotiation failed             SET filter != 0
      |                                     |                                v
      +-------------- HALT / disconnect ----+-------------------- kDataInitialized
                                                                             |
                                                                SET filter=0 | Stop()
                                                          (fall back one state) v
                                                                    kHalting
```

Semantics from the spec itself:

- After bus initialization the device is in RNDIS-uninitialized;
- Receiving `INITIALIZE_MSG` and replying with `INITIALIZE_CMPLT(SUCCESS)` ->
  RNDIS-initialized;
- Receiving `SET(OID_GEN_CURRENT_PACKET_FILTER, **non-zero**)` ->
  RNDIS-data-initialized, **and only now does data start flowing**;
- While in data-initialized, receiving `SET(filter = 0)` -> falls back to
  RNDIS-initialized;
- Receiving `HALT_MSG` or a bus disconnect at any time -> immediately back to
  RNDIS-uninitialized.

### Startup sequence (following Linux's `generic_rndis_bind`)

| Step | Operation | Fatal? |
|---|---|---|
| 1 | `INITIALIZE_MSG` -> `INITIALIZE_CMPLT`, negotiate parameters | Yes |
| 2 | `QUERY OID_GEN_PHYSICAL_MEDIUM` (`in_len=4`) | No (optional OID) |
| 3 | `QUERY OID_802_3_PERMANENT_ADDRESS` (`in_len=48`) | Yes |
| 4 | `QUERY` current MAC / max frame size / link speed / connection status / vendor info | No |
| 5 | `SET OID_GEN_CURRENT_PACKET_FILTER = 0x2D` | Yes |

If any fatal step fails, `HALT_MSG` is sent before returning the error, and the
state goes back to uninitialized instead of getting stuck in an intermediate
state.

### Three interaction details that are easy to get wrong

**(1) Control requests must be serialized.**
`GET_ENCAPSULATED_RESPONSE` can only "take the next queued response"; it has no
ability to select by `RequestId`. So only one request may be in flight at a
time.

**(2) The device can jump the queue.**
While waiting for some `*_CMPLT`, the device may well insert an
`INDICATE_STATUS_MSG` (media connection state change) first, or a
**device-initiated** `KEEPALIVE_MSG` -- in that case the host **must reply**
with `KEEPALIVE_CMPLT`, or the device may decide the host is dead and
disconnect. Therefore the control read loop must be written as a dispatcher
(dispatching on `MessageType`), not "blindly assuming whatever was just read is
the message we wanted".

**(3) The semantics of `RESET`.**
`RESET` is a soft reset: the control channel stays intact, but the device
discards **all** outstanding requests and packets. A non-zero `AddressingReset`
in `RESET_CMPLT` means addressing information (packet filter, multicast table)
was lost in the reset, and the host **must resend**
`SET(OID_GEN_CURRENT_PACKET_FILTER)` -- otherwise data will not flow again after
the reset. The Linux gadget sets `AddressingReset` to 1 unconditionally.

---

## 7. USB layer

### Control channel

| Request | `bmRequestType` | `bRequest` | `wValue` | `wIndex` |
|---|---|---|---|---|
| `SEND_ENCAPSULATED_COMMAND` | `0x21` (OUT\|Class\|Interface) | `0x00` | 0 | Communication class interface number |
| `GET_ENCAPSULATED_RESPONSE` | `0xA1` (IN\|Class\|Interface) | `0x01` | 0 | Communication class interface number |

**Warning:** **when the device has no valid response yet, the spec requires it
to return 1 byte `0x00` rather than STALL the control endpoint.** Therefore a
result of < 8 bytes (one RNDIS message header) must be treated as "not ready
yet, retry later", **not as an error**.

### Interrupt IN notification

8 bytes = two LE32s: `Notification`@0 (`0x00000001` = RESPONSE_AVAILABLE),
`Reserved`@4.

**Warning:** this is **not** CDC's `usb_cdc_notification` struct but RNDIS's own
format.

Device behavior comes in three flavors; an implementation must be compatible
with all of them:

1. The spec's way: the host waits for a notification on the interrupt IN;
2. Linux's way: **completely ignore** the interrupt endpoint and poll
   `GET_ENCAPSULATED_RESPONSE` directly, at most 10 times, 40 ms apart;
3. Conversely, there are also devices that **will only answer on the control
   endpoint after the interrupt endpoint has been read once**.

This project's policy: if an interrupt endpoint is present, wait for one
notification first (a timeout is not an error either), then poll **no matter
what**.

### Interface signatures (four known forms)

| Form | Communication interface | Data interface |
|---|---|---|
| Standard MS RNDIS | `0x02 / 0x02 / 0xFF` | `0x0A / 0x00 / 0x00` |
| ActiveSync (Windows Mobile / Phone) | `0xEF / 0x01 / 0x01` | Same as above |
| Wireless RNDIS (phone tethering, WWAN modules) | `0xE0 / 0x01 / 0x03` | Same as above |
| Novatel/Verizon USB730L variant | `0xEF / 0x04 / 0x01` | Same as above |

**Excluding pseudo-RNDIS**: class == `0x02` with a **non-zero** CDC ACM
`bmCapabilities` is a genuine cdc-acm modem, not RNDIS. **Warning:** **this
check may only be applied to class `0x02`** -- wireless-class (`0xE0`) RNDIS
functions repurpose the `bmCapabilities` field for their own use.

---

## 8. Multi-packet aggregation and ZLP

### Aggregation rules

- **device -> host**: every `PACKET_MSG` should start at an offset that is an
  integer multiple of 8 bytes from "the start of the multi-packet message";
- **host -> device**: must obey the `PacketAlignmentFactor` the device gives in
  `INITIALIZE_CMPLT`; alignment bytes = `1 << factor`, **legal upper bound 7**
  (=128 bytes);
- **`MessageLength` includes padding** -- every `PACKET_MSG` except the last has
  the trailing alignment padding inside its `MessageLength`; the last one
  **excludes** external padding;
- The total bytes of a single transfer <= the peer's `MaxTransferSize`, and the
  packet count <= `MaxPacketsPerMessage`;
- Traversal: `offset += MessageLength`.

This project's encoder assigns the padding to the **previous** message: only
when appending the next message does it go back and enlarge the previous
message's `MessageLength` to swallow the padding in between -- so the last
message's `MessageLength` is naturally `44 + frame length`, fully conforming to
the spec.

### ZLP avoidance

RNDIS explicitly requires that the host **must not** send zero-length packets
(the comment at the top of `rndis_host.c` reads exactly "DATA -- host must not
write zlps"). But when the transfer length happens to be an exact multiple of
the endpoint `wMaxPacketSize`, the USB host controller needs a short packet to
mark the end of the transfer.

The Linux `usbnet` approach (which this project follows): **append one `0x00`
byte** so the transfer becomes a short packet. This byte falls outside every
`MessageLength`; the device must tolerate trailing garbage.

**Warning:** conversely, **the host's RX parser must also tolerate
`actual_length > MessageLength`**. Without that tolerance, every full transfer
would be falsely reported as malformed once. This project's parser treats any
tail with less than one complete 44-byte header remaining as padding and
ignores it.

**Warning:** `LIBUSB_TRANSFER_ADD_ZERO_PACKET` is **not available** on macOS
(the header explicitly states that non-Linux systems return
`LIBUSB_ERROR_NOT_SUPPORTED`). This project does not rely on it.

---

## 9. `MaxTransferSize` -- the main lever for throughput

The `MaxTransferSize` the host advertises in `INITIALIZE_MSG` is **the only way
to get the device to aggregate multiple `PACKET_MSG`s into one bulk IN**. The
larger the advertised value, the more the device aggregates, and the lower the
per-frame USB and system call overhead.

Linux's algorithm is conservative (it advertises only one full frame):

```
hard_header_len = ETH_HLEN(14) + sizeof(rndis_data_hdr)(44) = 58
hard_mtu        = MTU + 58                    # MTU=1500 -> 1558
rx_urb_size     = (hard_mtu + maxpacket + 1) & ~(maxpacket - 1)
                # high-speed(512) -> 2048; full-speed(64) -> 1600
```

This project advertises **16 KiB** by default (tunable with
`--max-transfer-kb`), which leaves plenty of aggregation room while staying
within the spec's `0x4000` cap for USB 1.1 devices. The bulk IN buffer grows
along with it automatically -- it must be >= the value we advertise, or the
large transfers the device aggregates will overflow / be truncated.

---

## 10. Known device quirks

| quirk | Symptom | How this project handles it |
|---|---|---|
| **Broken Android CDC Union** | The CDC Union descriptor points at a non-existent interface number, or the CDC functional descriptor is missing entirely | Falls back to the hardcoded assumption "interface 0 = control, interface 1 = data" (same as Linux's `android_rndis_quirk`), and logs that the fallback path was taken |
| **`MaxPacketsPerMessage = 0`** | Some devices report 0 | Treat it as 1 |
| **Qualcomm upstream aggregation patch** | `MaxPacketsPerMessage` becomes a module parameter, default 3 | Adopted as reported; the TX side aggregates 3 packets |
| **`PacketAlignmentFactor` out of range** | Reports a value > 7 | Clamp to 7. Without clamping, `1u << 31` would compute an absurd padding length |
| **`MaxTransferSize` too small** | The HTC Diamond reports 1536 < `hard_mtu` (1558) | Derive backwards and lower the MTU to `1536 - 58 = 1478` |
| **`MaxTransferSize` too large** | WinCE / Windows Mobile devices tend to advertise 8 KB or 16 KB giant frames | Clamp to the host's own buffer cap; do not follow the device blindly |
| **Typical mainline gadget values** | `MaxPacketsPerMessage=1`, `MaxTransferSize=1580`, `PacketAlignmentFactor=0`, `DeviceFlags=CONNECTIONLESS`, `Medium=802_3` | Can serve as a test baseline (`MakeWellBehavedDevice` in `tests/mock_control_channel.h` is built to it, except that it advertises `MaxTransferSize=2048`) |

---

## 11. Timeout constants

| Constant | Spec value | This project | Rationale |
|---|---|---|---|
| `ControlTimeoutPeriod` | 10 seconds | **5 seconds** | Linux shrank it to 5 seconds because of ActiveSync behavior; if a reply really takes 10 seconds, the link is already unusable |
| `KeepAliveTimeoutPeriod` | 5 seconds | 5 seconds | The semantics are "send only when **any** message from the device has not arrived for 5 seconds", not an unconditional timer |
| Control buffer | >= 1024 bytes | **1536** | Windows uses the odd value 1025 and Linux copies it; 1536 leaves headroom |

**Warning:** on Apple Silicon, libusb **control transfers** are about 10x slower
than on x64
([libusb issue #1288](https://github.com/libusb/libusb/issues/1288)), so do not
set the keep-alive period too low -- every round trip is on a millisecond
budget.
