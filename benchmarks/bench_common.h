// Registration entry point for infrastructure-layer micro-benchmarks.
#pragma once

namespace tethernet::bench {

class Runner;

/// Registers all tn_common micro-benchmarks with the runner.
void RegisterCommonBenchmarks(Runner& runner);

}  // namespace tethernet::bench
