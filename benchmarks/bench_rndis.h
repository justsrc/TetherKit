// Registration entry point for RNDIS codec micro-benchmarks.
#pragma once

namespace tetherkit::bench {

class Runner;

/// Registers RNDIS codec micro-benchmarks with the runner.
void RegisterRndisBenchmarks(Runner& runner);

}  // namespace tetherkit::bench
