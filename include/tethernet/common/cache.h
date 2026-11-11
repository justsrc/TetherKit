// Compile-time constants related to caching.
#pragma once

#include <cstddef>

namespace tethernet {

/// Cache line size. Hardcoded to 128.
///
/// Why 128: Apple Silicon reports **128** for `sysctl hw.cachelinesize`.
/// Aligning to the usual 64 bytes means the producer and consumer indices can still land
/// on the same 128-byte cache line, so false sharing remains -- measured cross-core
/// bouncing raises the per-entry cost of the lock-free queue from single-digit nanoseconds
/// to 60+ nanoseconds.
///
/// Why **not** `std::hardware_destructive_interference_size`:
/// It does exist in Apple libc++ (in `<new>`, `__cpp_lib_hardware_interference_size`
/// = 201703), but it reports **256** (constructive reports 64), which does not match the
/// real 128. Using it doubles every padding, wastes twice the cache and memory, and
/// introduces cross-TU ABI warnings.
/// The measured hardware value is the right one, so it is hardcoded.
inline constexpr std::size_t kCacheLineSize = 128;

/// Alignment specifier that gives a member its own cache line.
#define TETHERNET_CACHE_ALIGNED alignas(::tethernet::kCacheLineSize)

}  // namespace tethernet
