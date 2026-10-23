#pragma once

#include <cstddef>

#define LAB_LIKELY(x) __builtin_expect(!!(x), 1)
#define LAB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LAB_ALWAYS_INLINE inline __attribute__((always_inline))
#define LAB_NOINLINE __attribute__((noinline))

namespace lab {

// Fixed at 64 rather than std::hardware_destructive_interference_size, which
// varies by compiler flags and would change the layout of shared-memory structs.
inline constexpr std::size_t kCacheLine = 64;

// Hint to the CPU that we are spinning. Keeps busy-poll loops polite to the
// sibling hyperthread and avoids memory-order mis-speculation on exit.
LAB_ALWAYS_INLINE void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

}  // namespace lab
