#pragma once

// Big-endian loads and stores for wire protocols (ITCH, OUCH, MoldUDP64 and
// SoupBinTCP are all big-endian). memcpy keeps them alignment-safe; the
// compiler turns each into a single load plus bswap.

#include <bit>
#include <cstdint>
#include <cstring>

#include "lab/core/compiler.hpp"

namespace lab {

static_assert(std::endian::native == std::endian::little, "lab assumes a little-endian host");

LAB_ALWAYS_INLINE std::uint16_t load_be16(const void* p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap16(v);
}

LAB_ALWAYS_INLINE std::uint32_t load_be32(const void* p) noexcept {
  return __builtin_bswap32(*static_cast<const std::uint32_t*>(p));
}

LAB_ALWAYS_INLINE std::uint64_t load_be64(const void* p) noexcept {
  std::uint64_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap64(v);
}

// 6-byte big-endian integer, used for ITCH nanosecond timestamps.
LAB_ALWAYS_INLINE std::uint64_t load_be48(const void* p) noexcept {
  const auto* b = static_cast<const unsigned char*>(p);
  return (std::uint64_t{load_be16(b)} << 32) | load_be32(b + 2);
}

LAB_ALWAYS_INLINE void store_be16(void* p, std::uint16_t v) noexcept {
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, sizeof v);
}

LAB_ALWAYS_INLINE void store_be32(void* p, std::uint32_t v) noexcept {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, sizeof v);
}

LAB_ALWAYS_INLINE void store_be64(void* p, std::uint64_t v) noexcept {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, sizeof v);
}

LAB_ALWAYS_INLINE void store_be48(void* p, std::uint64_t v) noexcept {
  auto* b = static_cast<unsigned char*>(p);
  store_be16(b, static_cast<std::uint16_t>(v >> 32));
  store_be32(b + 2, static_cast<std::uint32_t>(v));
}

}  // namespace lab
