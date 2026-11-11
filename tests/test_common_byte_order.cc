// Unit tests for byte_order.h.
//
// Key verification: unaligned access must be correct (offsets of RNDIS messages in USB
// buffers are not guaranteed to be aligned).
#include "tethernet/common/byte_order.h"

#include <doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>

using tethernet::AlignUp;
using tethernet::IsPowerOfTwo;
using tethernet::LoadBe16;
using tethernet::LoadLe16;
using tethernet::LoadLe32;
using tethernet::LoadLe64;
using tethernet::StoreBe16;
using tethernet::StoreLe32;
using tethernet::StoreLe64;

TEST_SUITE("common.byte_order") {
  TEST_CASE("little-endian reads interpret byte order as specified") {
    // The little-endian representation of 0x44332211 is 11 22 33 44.
    const std::array<std::byte, 8> bytes{std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
                                         std::byte{0x44}, std::byte{0x55}, std::byte{0x66},
                                         std::byte{0x77}, std::byte{0x88}};
    CHECK(LoadLe16(bytes.data()) == 0x2211U);
    CHECK(LoadLe32(bytes.data()) == 0x44332211U);
    CHECK(LoadLe64(bytes.data()) == 0x8877665544332211ULL);
  }

  TEST_CASE("big-endian reads interpret network byte order") {
    const std::array<std::byte, 2> bytes{std::byte{0x08}, std::byte{0x00}};
    // 0x0800 = ETHERTYPE_IP
    CHECK(LoadBe16(bytes.data()) == 0x0800U);
  }

  TEST_CASE("read-back after write is consistent") {
    std::array<std::byte, 16> buffer{};
    StoreLe32(buffer.data(), 0xDEADBEEFU);
    CHECK(LoadLe32(buffer.data()) == 0xDEADBEEFU);

    StoreLe64(buffer.data() + 8, 0x0123456789ABCDEFULL);
    CHECK(LoadLe64(buffer.data() + 8) == 0x0123456789ABCDEFULL);

    StoreBe16(buffer.data() + 4, 0x86DDU);  // ETHERTYPE_IPV6
    CHECK(LoadBe16(buffer.data() + 4) == 0x86DDU);
    // Big-endian writes must place the high byte first.
    CHECK(buffer[4] == std::byte{0x86});
    CHECK(buffer[5] == std::byte{0xDD});
  }

  TEST_CASE("reads and writes at unaligned offsets are also correct") {
    // This is the most error-prone scenario in this project: the start offset of the
    // Ethernet frame after REMOTE_NDIS_PACKET_MSG is determined by the device's DataOffset
    // and can be any value.
    std::array<std::byte, 32> buffer{};
    for (std::size_t offset = 0; offset < 8; ++offset) {
      StoreLe32(buffer.data() + offset, 0xCAFEBABEU);
      CHECK(LoadLe32(buffer.data() + offset) == 0xCAFEBABEU);

      StoreLe64(buffer.data() + offset + 8, 0xFEEDFACEDEADBEEFULL);
      CHECK(LoadLe64(buffer.data() + offset + 8) == 0xFEEDFACEDEADBEEFULL);
    }
  }

  TEST_CASE("single-byte reads and writes perform no conversion") {
    std::array<std::byte, 1> buffer{};
    tethernet::StoreLe<std::uint8_t>(buffer.data(), 0xA5U);
    CHECK(tethernet::LoadLe<std::uint8_t>(buffer.data()) == 0xA5U);
  }

  TEST_CASE("AlignUp rounds up to a power of two") {
    CHECK(AlignUp<std::uint32_t>(0, 4) == 0);
    CHECK(AlignUp<std::uint32_t>(1, 4) == 4);
    CHECK(AlignUp<std::uint32_t>(4, 4) == 4);
    CHECK(AlignUp<std::uint32_t>(5, 4) == 8);
    // RNDIS multi-packet aggregation requires each message to be aligned to 4 bytes.
    CHECK(AlignUp<std::uint32_t>(1514 + 44, 4) == 1560);
    CHECK(AlignUp<std::uint32_t>(63, 64) == 64);
  }

  TEST_CASE("IsPowerOfTwo correctly identifies powers of two") {
    CHECK_FALSE(IsPowerOfTwo<std::uint32_t>(0));
    CHECK(IsPowerOfTwo<std::uint32_t>(1));
    CHECK(IsPowerOfTwo<std::uint32_t>(2));
    CHECK_FALSE(IsPowerOfTwo<std::uint32_t>(3));
    CHECK(IsPowerOfTwo<std::uint32_t>(1024));
    CHECK_FALSE(IsPowerOfTwo<std::uint32_t>(1023));
  }

}  // TEST_SUITE("common.byte_order")
