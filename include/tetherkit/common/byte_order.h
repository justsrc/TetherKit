// Wire-format byte order read/write.
//
// The RNDIS wire format is **little-endian** (the protocol comes from Windows NDIS),
// while fields in Ethernet headers such as EtherType are big-endian. Both are needed,
// so this header provides both Le/Be families of functions.
//
// Implementation notes:
//   * Use std::memcpy rather than pointer casts to avoid undefined behavior on
//     unaligned accesses -- RNDIS messages in USB transfer buffers are only guaranteed
//     4-byte aligned at their start, and the Ethernet frame that follows a
//     REMOTE_NDIS_PACKET_MSG may start at any offset.
//     Clang optimizes "memcpy to a local variable + byteswap" into a single ldr/rev
//     instruction, so this is a zero-cost abstraction.
//   * Use std::endian for compile-time branching so it works correctly on big-endian
//     hosts too (though macOS is not big-endian).
#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tetherkit {

/// Unsigned integer types that appear in wire formats.
template <typename T>
concept WireUnsigned = std::unsigned_integral<T> && !std::same_as<T, bool> &&
                       (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

/// Read an integer from little-endian byte order.
template <WireUnsigned T>
[[nodiscard]] inline T LoadLe(const std::byte* src) noexcept {
  T value{};
  std::memcpy(&value, src, sizeof(T));
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::big) {
    value = std::byteswap(value);
  }
  return value;
}

/// Write an integer in little-endian byte order.
template <WireUnsigned T>
inline void StoreLe(std::byte* dst, T value) noexcept {
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::big) {
    value = std::byteswap(value);
  }
  std::memcpy(dst, &value, sizeof(T));
}

/// Read an integer from big-endian (network byte order).
template <WireUnsigned T>
[[nodiscard]] inline T LoadBe(const std::byte* src) noexcept {
  T value{};
  std::memcpy(&value, src, sizeof(T));
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::little) {
    value = std::byteswap(value);
  }
  return value;
}

/// Write an integer in big-endian (network byte order).
template <WireUnsigned T>
inline void StoreBe(std::byte* dst, T value) noexcept {
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::little) {
    value = std::byteswap(value);
  }
  std::memcpy(dst, &value, sizeof(T));
}

// Convenience aliases for common widths, so protocol parsing code reads closer to the
// field types in the specification.
[[nodiscard]] inline std::uint16_t LoadLe16(const std::byte* p) noexcept {
  return LoadLe<std::uint16_t>(p);
}

[[nodiscard]] inline std::uint32_t LoadLe32(const std::byte* p) noexcept {
  return LoadLe<std::uint32_t>(p);
}

[[nodiscard]] inline std::uint64_t LoadLe64(const std::byte* p) noexcept {
  return LoadLe<std::uint64_t>(p);
}

inline void StoreLe16(std::byte* p, std::uint16_t v) noexcept {
  StoreLe<std::uint16_t>(p, v);
}

inline void StoreLe32(std::byte* p, std::uint32_t v) noexcept {
  StoreLe<std::uint32_t>(p, v);
}

inline void StoreLe64(std::byte* p, std::uint64_t v) noexcept {
  StoreLe<std::uint64_t>(p, v);
}

[[nodiscard]] inline std::uint16_t LoadBe16(const std::byte* p) noexcept {
  return LoadBe<std::uint16_t>(p);
}

inline void StoreBe16(std::byte* p, std::uint16_t v) noexcept {
  StoreBe<std::uint16_t>(p, v);
}

/// Round up to the next integer multiple of `alignment`. `alignment` must be a power of two.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T AlignUp(T value, T alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

/// Return whether the value is a power of two (0 is not).
template <std::unsigned_integral T>
[[nodiscard]] constexpr bool IsPowerOfTwo(T value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace tetherkit
