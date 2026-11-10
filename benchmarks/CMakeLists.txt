# benchmarks -- performance benchmarks
#
# google/benchmark is not introduced: this project only needs three kinds of metrics --
# "nanoseconds per frame / pps / Gbps". The lightweight harness bundled here (see
# benchmarks/harness.h) is sufficient and can directly output a Markdown summary table to
# docs/BENCHMARKS.md, saving a parsing layer.

add_executable(
  tetherkit_bench
  bench_main.cc
  bench_common.cc
  bench_net.cc
  bench_rndis.cc
  harness.cc)

# tk_core PUBLIC-links all other modules (the benchmarks need the RNDIS codec and net loopback).
target_link_libraries(tetherkit_bench PRIVATE tetherkit_base tk_version tk_core)
target_include_directories(tetherkit_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
