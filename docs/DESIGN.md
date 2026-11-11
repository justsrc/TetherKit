# Tethernet Design Document

This document explains **why it is designed this way**. For field offsets and
constants, see [RNDIS-PROTOCOL.md](RNDIS-PROTOCOL.md); for measured numbers,
see [BENCHMARKS.md](BENCHMARKS.md); for verified environment facts and pitfalls,
see [../AGENTS.md](../AGENTS.md).

---

## 1. Problem and approach

The macOS kernel **has no** RNDIS driver. This is not speculation -- every
`IOKitPersonalities` entry of every CDC-family kext under
`/System/Library/Extensions` was inspected:

| kext personality | match condition |
|---|---|
| `AppleUSBACMControl0` | class 2, subclass 2, protocol **0** |
| `AppleUSBACMControl1` | class 2, subclass 2, protocol **1** |
| `AppleUSBECMControl` | class 2, subclass **6** |
| `AppleUSBNCMControl` | class 2, subclass **13** |
| `AppleUSBWCMControl` | class 2, subclass **8** |

RNDIS communication-class interfaces are `{0x02, 0x02, 0xFF}` -- the protocol
is neither 0 nor 1, so **no personality matches**; the `{0xE0, 0x01, 0x03}`
variant commonly used by Android does not even have a personality for class
`0xE0`.

That leaves four ways to make an RNDIS device work on macOS:

| Approach | Pure userspace? | Requirement | Semantic level | Verdict |
|---|---|---|---|---|
| kext / NKE | no | kext signing entitlement granted by Apple, or SIP disabled | L2 | Effectively infeasible on Apple Silicon |
| `NEPacketTunnelProvider` | yes | paid developer account + entitlement + app bundle | **L3** | Must implement ARP/ND/DHCP ourselves |
| `utun` | yes | root | **L3** | Same, and we must strip and add Ethernet headers ourselves |
| **`feth` + BPF** | yes | root | **L2** | chosen by this project |

The decisive reason for `feth` + BPF is **semantic-level match**: RNDIS is
natively L2 (it carries full Ethernet frames), and `feth` + BPF is the only
path that can send and receive **raw Ethernet frames** in both directions from
pure userspace. This lets the phone-side DHCP server and ARP broadcasts reach
the host IP stack directly, so we do not need to implement ARP proxying,
neighbor discovery, or a DHCP client -- those three are the real complexity of
an L3 approach.

Costs: root is required (for `feth` creation and `/dev/bpf*`), one extra mbuf
copy per direction, and TX has a "whole frame <= MTU + 18" length limit.

---

## 2. Overall architecture

```
        +------------------------ macOS kernel ------------------------+
        |  IP stack / routing / DHCP client                           |
        |       |                                                     |
        |       v                                                     |
        |  +---------+   if_fake peer pair   +---------+              |
        |  |  feth0  | <------------------> |  feth1  |              |
        |  | (system |                      | (driver |              |
        |  |  side)  |                      |  side)  |              |
        |  +---------+                      +----+----+              |
        |   IP/routing configured               | BPF (a single      |
        |   MAC = address reported by device    | descriptor for     |
        |                                       | send and receive)  |
        +---------------------------------------+---------------------+
                                                |
        +---------------------------------------+---------------------+
        |  Tethernet (userspace)                v                     |
        |  +---------- core::Bridge (3 data-path threads) ---------+  |
        |  |  RX: bulk IN callback -> FrameRing -> batched BPF write|  |
        |  |  TX: batched BPF read -> RNDIS aggregation -> bulk OUT |  |
        |  +----------------------+-------------------------------+  |
        |  +------ rndis::StateMachine (control thread) ----------+   |
        |  |  INITIALIZE / QUERY / SET / KEEPALIVE / RESET /      |   |
        |  |  HALT / INDICATE_STATUS                              |   |
        |  +----------------------+-------------------------------+   |
        |  +------ usb::Device / DataChannel --------------------+   |
        |  |  control channel (EP0 class requests) + interrupt IN |   |
        |  |  notification; async bulk IN/OUT transfer pool       |   |
        |  +----------------------+-------------------------------+   |
        +-------------------------+-----------------------------------+
                                  | USB
                             +----+------+
                             | RNDIS dev |
                             +-----------+
```

### Modules and dependency direction

Dependencies are **strictly one-way**; reverse edges and cycles are forbidden:

```
tn_common  <- (no dependencies)  errors, logging, byte order, lock-free queue, statistics, thread QoS
   ^
tn_rndis   <- common            RNDIS wire format + state machine (**pure logic, no I/O**)
tn_net     <- common            feth lifetime + BPF link
tn_usb     <- common, rndis     libusb wrapper (rndis is used only for protocol constants)
   ^
tn_core    <- all of the above  data-path bridge + runtime orchestration
   ^
tethernet  <- core              command line
```

**`tn_rndis` not touching I/O is a deliberate constraint.** It lets the entire
RNDIS implementation (including the state machine) be fully unit-tested on a
machine with neither a USB device nor root -- and this project's development
machine is exactly such a machine. For that reason the `ControlChannel`
interface lives in the `rndis` layer (it describes RNDIS protocol semantics, not
USB semantics) and is implemented by `tn_usb`.

---

## 3. Data-flow semantics (the most important point, and the easiest to get wrong)

Confirmed against the xnu `feth_output_common()` source:

| Event | on feth0 | on feth1 |
|---|---|---|
| the host IP stack sends a frame from feth0 | **OUT** tap | **IN** tap, and enters as input |
| we `write()` a frame to feth1's BPF | enters the IP stack as **input** | **OUT** tap |

This yields two conclusions:

1. **BPF only needs to attach to feth1 (the driver side); a single descriptor
   handles both receive and send.**
2. **`BIOCSSEESENT = 0` must be set** (-> `bd_direction = BPF_D_IN`). It
   filters out exactly the frames we `write` ourselves (they are in the OUT
   direction on feth1). Without it a loopback forms and every frame we write is
   immediately read back by ourselves.

---

## 4. Concurrency model

Three data-path threads, plus libusb's own IOKit runloop thread and the state
machine's control thread. This machine has 4 performance cores and 6 efficiency
cores; three hot threads plus the libusb runloop exactly fill the performance
cluster without oversubscribing it.

| Thread | Responsibility | Blocking point | QoS |
|---|---|---|---|
| `usb-event` | `libusb_handle_events`, runs all transfer callbacks | event pipe | USER_INTERACTIVE |
| `rx-inject` | batch-pull frames from `FrameRing` -> batched BPF `write` | spin then yield when the queue is empty | USER_INTERACTIVE |
| `tx-extract` | blocking BPF `read` -> RNDIS aggregation -> async bulk OUT | BPF `read()` | USER_INTERACTIVE |
| `rndis-ctl` | state-machine `Poll()`: keep-alive + drain indications | `sleep` | USER_INITIATED |

### Why RX must be split into two threads

libusb's transfer callback holds `ctx->event_waiters_lock`; doing blocking I/O
inside the callback would stall **every thread waiting on a synchronous
transfer** (including the state-machine thread that runs the control channel).
BPF `write()` is a system call (about 700 ns and up). So the callback only does
"split RNDIS packets + memcpy into the lock-free queue + resubmit immediately";
the actual write-out is handed to the injection thread.

### Why TX only needs one thread

BPF `read()` is itself blocking and auto-batching (with `BIOCIMMEDIATE=1` it
wakes on every incoming packet and delivers all packets accumulated in the
meantime in one go), while `SendFrames` submits **asynchronously** and does not
block. "Read a batch -> submit a batch" in the same thread is the optimal
shape; an extra thread would only add one more cross-core handoff.

### Why not merge the three into a single kqueue event loop

Measured in investigation: one empty `kevent()` on macOS (timeout={0,0}, no
events) takes **13.4~13.7 us**, 20 times more expensive than a normal system
call. At 25 kpps, probing alone would eat 34% of one core. **kqueue may only be
used for genuine blocking waits, never for polling** -- and each of our three
paths already has a natural blocking point, so merging them brings no benefit.

### Why the control channel must have its own non-event thread

libusb's synchronous APIs (`libusb_control_transfer` /
`libusb_interrupt_transfer`) start with
`if (usbi_handling_events(ctx)) return LIBUSB_ERROR_BUSY;` -- this is a TLS
check and always fails when called from the event thread (including from inside
any transfer callback). So the state machine cannot live in the event loop; it
must have its own thread. `main` gives it the main thread.

### Why not use `THREAD_TIME_CONSTRAINT_POLICY`

Its semantics are "guarantee `computation` CPU time every `period`", which fits
strictly periodic audio-rendering threads. All three of our threads block for
long stretches in `read()` / `write()` / `handle_events()`; setting TC would
(a) clear the thread's QoS class and (b) get the thread demoted by the kernel
when it overdraws `computation`. In addition, macOS has **no** CPU affinity API
(`thread_affinity_policy` is effectively useless on Apple Silicon), so "pin to
core" is not feasible and only priority tuning is possible.

---

## 5. Performance design

For the full numbers, see [BENCHMARKS.md](BENCHMARKS.md). The core conclusion:

> **The optimization target is system calls per frame, not CPU cycles.**

The per-frame CPU cost totals about 70 ns (RNDIS encode ~15 + queue transfer
~55 + statistics ~0.5), while one system call costs about 700 ns -- **a system
call is 10 times the CPU cost**. System calls on macOS are 3-10 times more
expensive than on Linux; this is the first constraint of the whole design.

Four measures follow:

| Measure | Mechanism | Effect |
|---|---|---|
| RX batched writes | `BIOCSBATCHWRITE` (macOS 14+, probed at runtime, falls back to per-frame writes on failure) | one system call per batch of frames |
| TX multi-packet aggregation | RNDIS allows one bulk OUT to carry multiple `PACKET_MSG`s | one USB round trip per batch of frames |
| make the device aggregate too | report a large `MaxTransferSize` in `INITIALIZE_MSG` (16 KiB by default) | **the main lever for RX throughput** |
| batch-publish queue | `FrameRing::BatchWrite/BatchRead`, one release store per batch | 1514-byte frames 2.4x faster, 64-byte frames 5.0x faster |

### Why copy once, not zero-copy

The RX path does one memcpy per frame (USB buffer -> `FrameRing` slot).
Zero-copy would require handing the USB transfer buffer to the downstream
consumer, which means it cannot be resubmitted immediately; that would require
far more buffers than "in-flight transfers" and a return mechanism -- but
memcpy of 1514 bytes takes only 25-70 ns, 1/10 of a system call. **Pursuing
zero-copy for it is the wrong optimization direction.**

Zero-copy in the TX direction is **fundamentally impossible**: `struct bpf_hdr`
is only 20 bytes while an RNDIS packet header needs 44, a 24-byte shortfall in
headroom; feth's `tx_headroom` is 32 bytes, also not enough.

### The cache line is 128 bytes

`sysctl hw.cachelinesize` reports **128** (not the habitual 64). Aligning to 64
would leave the producer and consumer indices on the same 128-byte cache line
and false sharing would still be present.

`std::hardware_destructive_interference_size` is **intentionally not used**: it
does exist in Apple libc++ (in `<new>`), but reports **256**, not the actual
128 -- using it would double every padding.

---

## 6. Error handling

| Path | Mechanism | Rationale |
|---|---|---|
| initialization / control | `std::expected<T, Error>` | most failures are "expected environment conditions" (no device plugged in, no root, interface busy); the caller always has to handle them, so exceptions are the wrong tool |
| **data hot path** | **return counts + atomic counters** | the `std::string` inside `Error` allocates heap memory, which is unacceptable at 25k-80k pps |

`Error` carries a source domain (errno / libusb / RNDIS_STATUS / logical), and
`ToString()` is responsible for translating "libusb returned -3" into
`LIBUSB_ERROR_ACCESS(-3)`. `WithContext` can layer an outer reason to form an
"outer: inner" chain -- that separator comes from `detail::ContextSeparator()`
instead of being hard-coded in `error.h`, so the formatting stays consistent
with the active message table.

The error messages themselves all come from the message table (see section 6b),
not from string literals.

**The RNDIS status-code-to-name mapping lives in exactly one place,
`rndis/protocol.cc`** (a single source of truth); `tn_common` does not know
RNDIS and only outputs the hexadecimal value, while the symbolic name is
spliced into the context string by the rndis layer. (There used to be a second
copy in `error.cc` with 5 wrong values; it has been removed.)

---

## 6b. User-facing strings and the multi-language plumbing

User-facing text (errors, logs, help, status names) **is never written as a
string literal**; it all lives in
`include/tethernet/common/messages.def`. That file is an X-macro list expanded
to generate the `Msg` enum and the English string table -- all from the same
source, so **a message cannot be structurally missing from the table**. The
project is currently English-only, but the multi-language plumbing (the
`Language` enum, `TextIn`, `ParseLanguage`, the C ABI `tn_set_language`, the
helper's `setLanguage` XPC method, and `L10n.apply`) is deliberately kept so
that adding a language later stays a bounded change; the checklist for that
change is at the top of `include/tethernet/common/messages.def`.

| Access form | Use | Allocates? |
|---|---|---|
| `Tr(Msg::kFoo, args...)` | with arguments, same semantics as `std::format` | yes (returns `std::string`) |
| `Text(Msg::kFoo)` | no arguments, returns `string_view` | no, `noexcept` |
| `TETHERNET_INFO_TR(Msg::kFoo, ...)` etc. | logging | not evaluated when the level is off |

**Why not gettext**: it would mean pulling in libintl, running msgfmt at build
time, and looking up directories at runtime, in exchange for runtime failures of
the "installed on another machine, cannot find `.mo`, so everything turns into
English" kind. For a fixed set of a few hundred messages, a table compiled into
the binary is the simplest choice.

**Cost and mitigation**: format strings become runtime lookups, so
`std::format`'s compile-time placeholder checks are gone (`Tr` goes through
`vformat` internally). That guarantee is provided by
`tests/test_common_i18n.cc` instead -- it walks the whole table and checks each
message for well-formed placeholders, consistent argument indexing and types,
contiguous indices, and that automatic and manual numbering are not mixed in the
same string. A placeholder mistake fails `ctest -R common.i18n` immediately
instead of rendering as a half-formed log line on a user's machine.

`Tr()` is **forbidden on the data hot path**, for the same reason as `error.h`
(it always allocates). Hot-path code that needs text may only use `Text()`.

The language is decided by the host: the command line takes `--lang en` (and
reads environment variables for defaults where applicable), and the GUI passes
its choice through the C ABI `tn_set_language`. The GUI language menu offers
"Follow system" and "English". This is a **single process-wide state**, not one
per session -- logs and errors are produced from multiple threads, and making it
thread-local would let a single session emit output in two different languages.

---

## 7. Backpressure strategy

| Location | What if full | Rationale |
|---|---|---|
| RX queue (USB -> BPF) | **drop and count** | the libusb callback cannot block (it holds a lock); dropping is the only option |
| TX transfer pool (BPF -> USB) | **drop and count as backpressure** | waiting would let the BPF kernel buffer accumulate and eventually be dropped by the kernel -- drops that are **invisible** to operators. Dropping in userspace and counting makes the bottleneck visible. TCP will retransmit; UDP tolerates drops by design |
| RX while paused | **do not drop**; frames already in the queue go through after resume | the link is only temporarily down; the frames are still valid |
| TX while paused | **drop and count** | during an RNDIS soft reset the device drops all in-flight packets; buffering them would only waste memory |

---

## 8. Teardown order (to avoid use-after-free)

`libusb_close` **does not** reclaim in-flight transfers -- it only does
`list_del`, clears `dev_handle`, and logs; it does **not** call callbacks and
does **not** free memory. After that, IOKit aborts can still cause
`darwin_async_io_callback` to run on a libusb internal thread. If memory has
been freed by then, that is a use-after-free. This is one of the most common
libusb pitfalls.

Correct order (implemented jointly by `Runtime::Stop()` and
`UsbDataChannel::Shutdown()`):

```
1. Bridge::Stop()
     ├ set stop flag
     ├ link->Interrupt()        interrupt blocking BPF read
     ├ join rx-inject / tx-extract
     └ DataChannel::Shutdown()
          ├ set stop flag (callbacks must not resubmit)
          ├ cancel all in-flight transfers
          ├ **wait for in-flight count to reach zero** (every callback has returned)
          └ only then libusb_free_transfer + free buffers
2. close BPF
3. destroy the feth pair
4. state machine Stop(): SET filter=0 -> HALT
5. release USB interfaces, close handle
6. stop libusb event thread, libusb_exit
```

Two constraints to remember:

- **In step 1, "wait for the in-flight count to reach zero" must never run on
  the libusb event thread** -- the callbacks that decrement it run on that same
  thread, so waiting there would deadlock.
- On timeout, **leak memory deliberately rather than risk freeing it**. Leaking
  a few hundred KB is far better than a use-after-free crash; a clear error log
  must explain the decision.

---

## 9. Testability

The development machine has **neither a USB device** (`libusb_get_device_list`
returns 0) nor **root**. For that reason, every boundary that needs an external
resource is abstracted behind an interface:

| Interface | Production implementation | Test implementation |
|---|---|---|
| `rndis::ControlChannel` | `usb::UsbControlChannel` | `testing::MockControlChannel` |
| `usb::DataChannel` | `usb::UsbDataChannel` | `testing::MockDataChannel` |
| `net::LinkBackend` | `net::BpfLink` | `net::LoopbackLink` |

**The abstraction granularity is "batch", not "frame"**: one virtual call
processes dozens or hundreds of frames, so the per-frame overhead is far below
1 ns. A virtual call per frame would be unacceptable -- that design is
deliberately avoided.

This lets every state-machine path (including device-queued indications,
keep-alive failures, and reset replay) and every bridge path (including
bidirectional concurrency, backpressure, pause, and shutdown under traffic) be
tested offline and verified under ThreadSanitizer. feth/BPF cases that need
root are **skipped rather than failed** by default and can be enabled explicitly
with `TETHERNET_ROOT_TESTS=1`.

---

## 10. Use and risk of private ABIs

The project uses three things that are not in the public SDK:

| Private ABI | Purpose | Risk and mitigation |
|---|---|---|
| `struct ifdrv` | parameter to `SIOCSDRVSPEC` | its size participates in the ioctl number calculation; a wrong size would only yield a non-existent ioctl (returning `ENOTTY`, never doing anything dangerous). `static_assert` pins size=40, every field offset, and the derived `SIOCSDRVSPEC=0x8028697b` |
| `struct if_fake_request` | feth peer pairing | this file is byte-identical across every published tag from xnu-7195 (macOS 11) to xnu-12377 (macOS 26), and Apple's own `ifconfig fethN peer fethM` depends on it -- in practice it is frozen. Also pinned by `static_assert` (size=160) |
| `BIOCSBATCHWRITE` / `BIOCSNOTSTAMP` | performance optimizations | **purely optional**. Always probed at runtime; on failure the code falls back to the generic path without affecting correctness |

In one sentence: **functional ABIs have 15 years of validation and are backed by
`static_assert`; optimization ABIs are probed at runtime. Neither can silently
take the wrong path.**

---

## 11. Code conventions

| Category | Convention | Example |
|---|---|---|
| namespace | `lower_case` | `tethernet::rndis` |
| type | `CamelCase` | `PacketMessageWriter` |
| function and method (including accessors) | `CamelCase` | `MaxFrameBytes()` |
| local variable and parameter | `lower_case` | `frame_length` |
| private member | `lower_case_` | `bulk_in_endpoint_` |
| constant / enum value | `k` + `CamelCase` | `kPacketMsgHeaderBytes` |
| file | `snake_case`, `.h` / `.cc` | `packet_codec.h` |
| message identifier | `k` + module prefix + `CamelCase` | `kNetBpfBindFailed`, `kCliUnknownOption` |

Accessors also use `CamelCase` (not the standard-library `lower_case` style) so
that `readability-identifier-naming` can check everything mechanically, with no
exceptions left to memory.

`.clang-tidy` disables a number of checks that do not fit low-level systems
programming, and every one of them has a stated reason. In particular,
`performance-enum-size` must stay off -- this project's protocol enums **must**
have an underlying type that exactly matches the wire field width (RNDIS is all
LE32); shrinking them as the check suggests would corrupt the wire format.
