// Registration entry point for feth / BPF link-layer benchmarks.
//
// Unlike the other benchmarks, this group requires root (to create feth interface pairs and
// open /dev/bpf*). When not running as root, no items are registered and the reason is
// reported via skip_reason -- the report must show "this group did not run" rather than
// letting it disappear silently.
#pragma once

#include <string>

namespace tetherkit::bench {

class Runner;

/// Sets up the feth + BPF fixture and registers the link-layer benchmarks.
///
/// @param skip_reason receives the skip reason when false is returned.
/// @return Whether registration succeeded. The fixture lifetime is owned by a static object
///         in this module and lives until process exit.
bool RegisterNetBenchmarks(Runner& runner, std::string& skip_reason);

/// Tears down the fixture (closes BPF descriptors, destroys the feth interface pair).
///
/// Must be called explicitly before main returns. Letting the fixture live until static
/// destruction crashes: FethDevice's destructor writes logs, but by then the logging module's
/// static mutex may already have been destroyed, throwing `std::system_error: mutex lock
/// failed` and exiting the process with 134 -- the benchmarks all finished, yet the exit code
/// is a failure.
void ShutdownNetBenchmarks() noexcept;

}  // namespace tetherkit::bench
