# Optimizations.cmake -- data path optimization options.
#
# Provides the INTERFACE target tetherkit_optimizations.
#
# Design trade-offs:
#  * No -march/-mcpu by default: the Apple Silicon baseline (armv8.4-a for M1)
#    already covers everything we need (LSE atomic instructions, NEON); the
#    extra benefit of -mcpu=native is marginal while tying the binary to a
#    specific chip generation. Turn it on with -DTETHERKIT_NATIVE_ARCH=ON
#    when needed.
#  * No -O3: the hot paths here are dominated by memcpy and syscalls, where
#    -O3's aggressive loop unrolling and vectorization add almost nothing while
#    growing code size and hurting I-cache hit rates. -O2 (RelWithDebInfo's
#    default) is the better choice.
#  * -fno-plt / -fvisibility=hidden trim indirect jumps and symbol table size.

add_library(tetherkit_optimizations INTERFACE)

target_compile_options(
  tetherkit_optimizations
  INTERFACE
    # Hide internal symbols by default: this project exports no stable ABI,
    # and hidden symbols leave the optimizer freer to inline.
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    # Strict aliasing is dangerous for our handwritten wire-format parsing
    # (heavy reinterpret_cast use), so turn it off. The cost is negligible:
    # the hot paths' real bottleneck is syscalls, not alias analysis.
    -fno-strict-aliasing)

if(TETHERKIT_NATIVE_ARCH)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-mcpu=native TK_HAS_MCPU_NATIVE)
  if(TK_HAS_MCPU_NATIVE)
    target_compile_options(tetherkit_optimizations INTERFACE -mcpu=native)
  else()
    message(WARNING "The compiler does not support -mcpu=native; microarchitecture optimization skipped.")
  endif()
endif()

if(TETHERKIT_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT TK_IPO_OK OUTPUT TK_IPO_MSG)
  if(TK_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON PARENT_SCOPE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON PARENT_SCOPE)
  else()
    message(WARNING "LTO not supported, skipped: ${TK_IPO_MSG}")
  endif()
endif()
