// Thread scheduling policy.
//
// Why this is needed: Apple Silicon is a big.LITTLE architecture (this machine has 10
// cores = 4 performance cores + 6 efficiency cores). Under default QoS, data-path
// threads may be scheduled onto efficiency cores, which visibly reduces throughput and
// increases jitter. macOS **does not provide** an interface for pinning threads to
// specific physical cores (no sched_setaffinity); all we can do is express intent
// through a QoS class and let the kernel decide:
//
//   QOS_CLASS_USER_INTERACTIVE -- highest priority, kernel prefers performance cores.
//   QOS_CLASS_USER_INITIATED   -- next.
//   QOS_CLASS_DEFAULT          -- default.
//   QOS_CLASS_UTILITY / BACKGROUND -- prefer efficiency cores.
//
// We use USER_INTERACTIVE for data-path threads and UTILITY for auxiliary threads such
// as statistics and log flushing.
//
// About real-time threads (THREAD_TIME_CONSTRAINT_POLICY):
//   The Core Audio time-constraint policy can provide stronger latency guarantees, but
//   it requires the thread to yield voluntarily within its declared time budget; going
//   over budget causes a demotion penalty. Our BPF read() is a blocking call whose
//   duration is unpredictable, so it does not meet the preconditions for
//   time-constraint use and is not adopted.
#pragma once

#include <pthread.h>

#include <cstdint>
#include <string_view>

namespace tethernet {

/// Thread purpose, which determines the QoS level.
enum class ThreadRole : std::uint8_t {
  kDataPath,   ///< USB event loop, BPF read/write -- throughput and latency critical;
               ///< contend for performance cores.
  kControl,    ///< RNDIS control channel, keep-alive -- low frequency but needs timely
               ///< response.
  kAuxiliary,  ///< Statistics reporting, log flushing -- can yield.
};

/// Set the name and QoS for the current thread. Should be called once at the very
/// beginning of the thread function.
///
/// The name is written both to thread_local (for the log prefix) and to the kernel (for
/// lldb / Instruments).
void ConfigureCurrentThread(std::string_view name, ThreadRole role) noexcept;

/// Map ThreadRole to the numeric value of qos_class_t. Exposed separately for testing and
/// log output.
[[nodiscard]] unsigned int QosClassFor(ThreadRole role) noexcept;

}  // namespace tethernet
