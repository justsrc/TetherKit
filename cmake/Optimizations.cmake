# Optimizations.cmake -- data path optimization options.
#
# Provides the INTERFACE target tethernet_optimizations.
#
# Design trade-offs:
#  * No -march/-mcpu by default: the Apple Silicon baseline (armv8.4-a for M1)
#    already covers everything we need (LSE atomic instructions, NEON); the
#    extra benefit of -mcpu=native is marginal while tying the binary to a
#    specific chip generation. Turn it on with -DTETHERNET_NATIVE_ARCH=ON
#    when needed.
#  * No -O3: the hot paths here are dominated by memcpy and syscalls, where
#    -O3's aggressive loop unrolling and vectorization add almost nothing while
#    growing code size and hurting I-cache hit rates. -O2 (RelWithDebInfo's
#    default) is the better choice.
#  * -fno-plt / -fvisibility=hidden trim indirect jumps and symbol table size.

add_library(tethernet_optimizations INTERFACE)

target_compile_options(
  tethernet_optimizations
  INTERFACE
    # Hide internal symbols by default: this project exports no stable ABI,
    # and hidden symbols leave the optimizer freer to inline.
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    # Strict aliasing is dangerous for our handwritten wire-format parsing
    # (heavy reinterpret_cast use), so turn it off. The cost is negligible:
    # the hot paths' real bottleneck is syscalls, not alias analysis.
    -fno-strict-aliasing)

if(TETHERNET_NATIVE_ARCH)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-mcpu=native TN_HAS_MCPU_NATIVE)
  if(TN_HAS_MCPU_NATIVE)
    target_compile_options(tethernet_optimizations INTERFACE -mcpu=native)
  else()
    message(WARNING "The compiler does not support -mcpu=native; microarchitecture optimization skipped.")
  endif()
endif()

if(TETHERNET_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT TN_IPO_OK OUTPUT TN_IPO_MSG)
  if(TN_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON PARENT_SCOPE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON PARENT_SCOPE)
  else()
    message(WARNING "LTO not supported, skipped: ${TN_IPO_MSG}")
  endif()
endif()
