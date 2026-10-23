#pragma once

// Clocks for the hot path and for reporting.
//
// Hot threads timestamp with rdtsc() (a few ns, no syscall) and keep raw tick
// deltas. TscClock converts ticks to nanoseconds off the hot path, using a
// ratio calibrated against CLOCK_MONOTONIC at startup.

#include <chrono>
#include <cstdint>
#include <ctime>

#include "lab/core/compiler.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace lab {

LAB_ALWAYS_INLINE std::uint64_t mono_ns() noexcept {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

LAB_ALWAYS_INLINE std::uint64_t wall_ns() noexcept {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

// Raw cycle counter. On x86 this is the invariant TSC; elsewhere it falls back
// to CLOCK_MONOTONIC so the rest of the code still works (with 1 tick = 1 ns).
LAB_ALWAYS_INLINE std::uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  return mono_ns();
#endif
}

// Like rdtsc() but waits for earlier instructions to finish, for the end of a
// measured interval.
LAB_ALWAYS_INLINE std::uint64_t rdtscp() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  unsigned aux;
  return __rdtscp(&aux);
#else
  return mono_ns();
#endif
}

class TscClock {
 public:
  // Measures ticks per nanosecond over `window`. 50 ms gives better than
  // 0.01% accuracy on an invariant TSC.
  static TscClock calibrate(std::chrono::milliseconds window = std::chrono::milliseconds{50});

  // Builds a clock with a known ratio, mainly for tests.
  static TscClock from_ratio(double ticks_per_ns) noexcept { return TscClock{ticks_per_ns}; }

  double ticks_per_ns() const noexcept { return ticks_per_ns_; }
  double ghz() const noexcept { return ticks_per_ns_; }

  double to_ns(std::uint64_t ticks) const noexcept {
    return static_cast<double>(ticks) / ticks_per_ns_;
  }

  std::uint64_t to_ticks(double ns) const noexcept {
    return static_cast<std::uint64_t>(ns * ticks_per_ns_);
  }

 private:
  explicit TscClock(double ticks_per_ns) noexcept : ticks_per_ns_{ticks_per_ns} {}
  double ticks_per_ns_;
};

}  // namespace lab
