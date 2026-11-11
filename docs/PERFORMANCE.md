# Performance tuning guide

Measured numbers are in [BENCHMARKS.md](BENCHMARKS.md); the architectural
rationale is in [DESIGN.md](DESIGN.md). This document only covers **how to
tune**.

---

## The one-line conclusion

> **The optimization target is the number of system calls per frame, not CPU
> cycles.**

Total CPU cost per frame is about 70 ns; one system call costs about 700 ns. On
macOS a system call costs 3~10x what it costs on Linux, and that is the first
constraint on the entire design. So every tuning lever is about "carrying more
frames per system call / per USB round trip".

---

## Theoretical throughput ceiling

| Link | Theoretical payload | Frame rate at the 1514-byte full frame |
|---|---|---|
| USB 2.0 full-speed | ~12 Mbps | ~1 k pps |
| USB 2.0 high-speed | **~426 Mbps** (53.2 MB/s) | ~35 k pps |
| USB 3.0 SuperSpeed | ~4 Gbps (RNDIS can hardly saturate it in practice) | ~330 k pps |

Derivation of the high-speed 426 Mbps: `wMaxPacketSize=512`, at most 13 data
packets per 125 us microframe -> 6656 B / 125 us = 53.248 MB/s.

Compared with `FrameRing`'s cross-thread handoff of ~55 ns/frame (~18 M
frames/s), **the lock-free queue has two orders of magnitude of headroom**. The
bottleneck is always on the USB and system call side.

---

## Tuning knobs (ordered by payoff)

### 1. `--max-transfer-kb` (default 16) -- **biggest payoff**

The `MaxTransferSize` the host advertises in `INITIALIZE_MSG`. This is the
**only** way to get the **device** to aggregate multiple
`REMOTE_NDIS_PACKET_MSG`s into a single bulk IN.

- The larger the advertised value -> the more the device aggregates -> the fewer
  USB round trips and system calls amortized per frame.
- Linux advertises only 2048 (one full frame), very conservative. This project
  defaults to 16 KiB.
- Cap: the spec requires USB 1.1 devices to stay at or below `0x4000` (16 KiB).
- **Cost**: the bulk IN buffer automatically grows with it (it must be >= the
  advertised value); memory use = `rx-transfers x max-transfer-kb`. Default
  16 x 16 KiB = 256 KiB.

**Warning:** but **the device may not cooperate**: the mainline Linux gadget
reports `MaxPacketsPerMessage = 1`, i.e. it sends one packet at a time, so
advertising a larger value has no effect. Only Android kernels with the Qualcomm
upstream aggregation patch report 3 or more. The startup log prints the
negotiated result; look for `device aggregation limit N packets` to see whether
it took effect.

### 2. `--rx-transfers` (default 16) and `--rx-transfer-kb` (default 16)

The number of concurrent in-flight bulk IN transfers and the size of each. The
goal is to **keep the USB controller queue from ever running dry**.

- Total in flight = the product of the two; 256 KiB by default.
- Rationale: at 53 MB/s high speed, a 1 ms gap in the queue wastes 50 KB. Total
  in flight must be >= 256 KiB to cover the scheduling jitter of "userspace
  callback dispatch + resubmit".
- **Do not go "many and small"**: on macOS every `libusb_submit_transfer` costs
  at least 3 IOKit round trips (the darwin backend calls
  `darwin_get_pipe_properties` before every submission, which on
  `IOUSBInterfaceInterface >= 550` equals two user-client calls). Conversely,
  darwin imposes **no** cap on a single bulk length and does no splitting (16 KB
  splitting is the Linux backend's business, not macOS's).

### 3. `--bpf-buffer-kb` (default 4096)

The BPF kernel capture buffer. The kernel allocates **two copies** (store + free
double buffering), so the actual wired memory is twice this value.

- The cap is ambiguous: the two sources consulted point to
  `debug.bpf_maxbufsize` (512 KiB) and `debug.bpf_bufsize_cap` (32 MiB)
  respectively. **The code does not depend on which is right** -- when
  `BIOCSBLEN` exceeds the cap the kernel **does not report an error**; it
  silently clamps and writes the actual value back into the argument. We use the
  written-back value and print an INFO line when it was clamped.
- The upside of raising it is limited: BPF has only two buffers, and enlarging
  one does not shrink the loss window where "the store buffer also fills up
  during copyout". If packets are actually being lost, first look at "kernel
  drops" in the `--stats` output, then consider whether the RX injector thread
  cannot keep up.

### 4. `--mtu` (default 1500)

- The ceiling is constrained by `sysctl net.link.fake.max_mtu` (2048 on this
  machine).
- **Warning:** that sysctl is a **creation-time snapshot** for feth; changing it
  has no effect on existing interfaces.
- **Warning:** BPF writes have one more hard limit: with `hdrcmplt=1` the whole
  frame length must be <= the interface MTU + 18 (`BPF_WRITE_LEEWAY`).
- It is lowered automatically when the device reports a smaller
  `MaxTransferSize` or `OID_GEN_MAXIMUM_FRAME_SIZE`.

### 5. Compile options

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
```

- **Do not enable `-O3`**: the hot path is dominated by memcpy and system calls;
  `-O3`'s aggressive loop unrolling and vectorization gain almost nothing, and
  instead increase code size and hurt I-cache hits.
- **`-DTETHERNET_NATIVE_ARCH=ON` gains very little**: the arm64 baseline already
  emits LSE atomic instructions (`ldadd`); `-mcpu=` only brings scheduling-model
  differences, not an instruction-set unlock. Also note that
  `-mcpu=apple-m5` **is rejected by Apple clang 21** (`native` resolves to
  `apple-m4`).
- **Performance benchmarks must use a Release build with sanitizers disabled**;
  otherwise the numbers are worthless.

---

## How to tell where the bottleneck is

`--stats 1000` prints one line per second:

```
RX     3421 pps /   41.43 Mbps (dropped 0)  |  TX     1180 pps /   14.29 Mbps (dropped 0)  |  queue depth 0  kernel drops 0  backpressure 0
```

| Symptom | Meaning | Countermeasure |
|---|---|---|
| **queue depth** stays > 0 and near capacity | The RX injector thread (BPF write) cannot keep up with libusb callbacks | Confirm `batch write enabled` in the BPF-ready log (macOS 14+); raise `bridge.rx_write_batch` |
| **RX drops** keep growing | The RX queue is full; same as above | Same as above; or raise `bridge.rx_ring_frames` |
| **kernel drops** keep growing | The BPF kernel buffer overflowed; the TX extractor thread is not reading fast enough | Raise `--bpf-buffer-kb`; check whether the tx-extract thread is being starved by other load |
| **backpressure** keeps growing | The TX transfer pool is full; nothing can be written on the USB side | Raise `data_channel.tx_transfer_count`; or the device itself is the bottleneck |
| **TX drops** keep growing while backpressure is 0 | Oversized frames, or frames dropped while the link was down | Check the MTU negotiation result and the link state log |
| Both RX and TX far below expectation with no drops | The device side or the peer is the bottleneck | Check the link speed and the device aggregation limit in the startup log |

These two startup log lines are the key to telling whether aggregation is in
effect:

```
data channel ready: RX 16 x 16 KiB (256 KiB in flight), TX 4 x 16 KiB; device aggregation limit 1580 bytes / 1 packets, alignment 1 bytes
data path started: RX queue 2048 frames (4352 KiB), RX write batch 64 frames, TX submit batch 256 frames, link batch write available
```

- `device aggregation limit ... / 1 packets` -> the device does not support
  aggregation; raising `--max-transfer-kb` is pointless.
- `link batch write unavailable (writing one frame at a time)` -> the system is
  macOS 13 or earlier, `BIOCSBATCHWRITE` does not exist, the RX direction pays
  one system call per frame, and throughput will be clearly lower than on
  macOS 14+.

---

## Known performance limitations

| Limitation | Impact | Workaround? |
|---|---|---|
| macOS BPF has **no zero-copy** (no `BIOCSETZBUF`) | One kernel<->userspace copy per direction | No. FreeBSD has it, Darwin does not |
| macOS has **no** CPU affinity API | Data-path threads may be scheduled onto efficiency cores | No; QoS is the only way to express intent (`thread_affinity_policy` is essentially ineffective on Apple Silicon) |
| `libusb_dev_mem_alloc` returns `NULL` on darwin | No zero-copy DMA buffer | No. Switched to `posix_memalign` aligned to `hw.pagesize` (16384) to reduce the segmentation when IOKit builds DMA descriptors |
| libusb **control transfers** are about 10x slower on Apple Silicon | Keep-alives and OID queries are millisecond-scale | No. So do not shorten the keep-alive period, and never put control operations on the data hot path |
| `BIOCSBATCHWRITE` is macOS 14+ only | On macOS 13 and earlier, RX pays one system call per frame | No; it already falls back automatically |
| Community reports of kernel mbuf overflow panics in the feth data path at **> 5~8 Gbps** | Very low risk for this project | Measured RNDIS over USB 2.0: RX 325 / TX 235 Mbps; even USB 3 will hardly exceed 1~2 Gbps, far below that threshold |
| ~~Backpressure dropping TX frames under high load~~ | ~~Measured: 1889 frames dropped in 6 seconds~~ | **Fixed**: the bridge layer now waits for a free transfer slot instead of dropping, and TX frame drops went to 0. See the "Root cause and fix for TX frame drops" section of [BENCHMARKS.md](BENCHMARKS.md) |
| Under bidirectional concurrency RTT inflates from 0.4 ms to 16.6 ms | After the TX frame-drop fix it no longer causes a throughput collapse (bidirectional TX 4.8 -> 87 Mbps), but latency-sensitive workloads are still affected | Partially. Shrinking the RX queue depth helps, at the cost of peak RX throughput; the amount of improvement has not been measured |
| TCP TX is bimodal (two stable states around 240 / around 300 Mbps) | Throughput is unstable, but both states have zero retransmits, so it is not packet loss | Cause not yet identified |

---

## Measured results (completed 2026-07-28)

The four "not yet measured" items previously listed in this section now **all
have measured values**. Full data and methodology are in
[BENCHMARKS.md](BENCHMARKS.md); only the conclusions are recorded here:

| Previous estimate | Measured | Difference |
|---|---|---|
| BPF `write()` ~700 ns (from research based on `write(/dev/null)`) | **2181 ns** | **about 3x as expensive** |
| `BIOCSBATCHWRITE` speedup factor unknown | 805 ns/frame (batch >= 32) | **about 2.7x** |
| Whether end-to-end throughput could approach 426 Mbps | RX **324~327 Mbps** (77% of the theoretical ceiling) | Close, but not reached |
| feth round-trip latency unknown | **24.5 us** | Only about 5% of the end-to-end RTT |

Three takeaways:

1. **BPF writes are not the bottleneck.** Although the measurement is 3x the
   estimate, batch writes still sustain 1.24 million frames/s (about 15 Gbps at
   1514 bytes), while USB 2.0 at full speed needs only about 27,000 frames/s --
   a 45x margin. This document's earlier worry that "BPF writes determine the
   theoretical RX-side ceiling" can be put to rest.
2. **The cost is almost independent of frame length** (1934 ns at 64 bytes vs
   2181 ns at 1514 bytes, only 13% apart); it is all fixed per-call cost --
   which is why batch writes are what matter, not shrinking frames.
3. **The TX direction used to be the weak side; the root cause has been found
   and fixed.** The symptom was that frame drops occurred on this project's own
   TX path; the root cause was that the bridge layer **immediately dropped** the
   rest of a batch when the bulk OUT transfer pool was full, when a slot would
   actually free up after only a few hundred microseconds. After switching to
   waiting: TX frame drops 1889 -> **0**, TCP TX retransmits 3829 -> **0**,
   bidirectional concurrent TX **4.8 -> 87 Mbps**. This also invalidated the
   earlier tuning advice -- after the fix, `--tx-transfers` at 4 / 8 / 16 makes
   no distinguishable difference, so the default stays 4. Details are in the
   "Root cause and fix for TX frame drops" section of BENCHMARKS.md.

**Lesson**: that drop logic came with a very persuasive comment ("waiting would
let the kernel buffer pile up into invisible drops"), and both premises were
wrong -- `bs_drop` is read back on every batch and displayed in the stats line,
and the 4 MiB kernel buffer is exactly the level that should absorb bursts.
**When writing down a reason for "deliberately doing this", be able to state its
magnitude**: here the pool is 4 slots deep vs about 26 slots needed for one
slice -- once the magnitudes are lined up, the problem shows itself.

Still unresolved: the bimodal TCP TX behavior, and the RTT inflation under
bidirectional concurrency. The list is in the final section of
[BENCHMARKS.md](BENCHMARKS.md).
