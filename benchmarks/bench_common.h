// Registration entry point for infrastructure-layer micro-benchmarks.
#pragma once

namespace tetherkit::bench {

class Runner;

/// Registers all tk_common micro-benchmarks with the runner.
void RegisterCommonBenchmarks(Runner& runner);

}  // namespace tetherkit::bench
