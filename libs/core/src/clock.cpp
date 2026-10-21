#include "lab/core/clock.hpp"

#include <thread>
#include <utility>

namespace lab {

TscClock TscClock::calibrate(std::chrono::milliseconds window) {
  // Bracket each mono_ns() read with TSC reads and keep the midpoint, so the
  // syscall's own cost does not skew the ratio.
  auto sample = [] {
    const std::uint64_t t0 = rdtsc();
    const std::uint64_t ns = mono_ns();
    const std::uint64_t t1 = rdtsc();
    return std::pair{t0 + (t1 - t0) / 2, ns};
  };
  const auto [tsc_begin, ns_begin] = sample();
  std::this_thread::sleep_for(window);
  const auto [tsc_end, ns_end] = sample();
  const double ticks = static_cast<double>(tsc_end - tsc_begin);
  const double ns = static_cast<double>(ns_end - ns_begin);
  return TscClock{ns > 0 ? ticks / ns : 1.0};
}

}  // namespace lab
