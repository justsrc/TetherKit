// Registration entry point for RNDIS codec micro-benchmarks.
#pragma once

namespace tethernet::bench {

class Runner;

/// Registers RNDIS codec micro-benchmarks with the runner.
void RegisterRndisBenchmarks(Runner& runner);

}  // namespace tethernet::bench
