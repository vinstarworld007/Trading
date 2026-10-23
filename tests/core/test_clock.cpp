#include <gtest/gtest.h>

#include <thread>

#include "lab/core/clock.hpp"

namespace {

TEST(Clock, MonotonicNeverGoesBackwards) {
  std::uint64_t prev = lab::mono_ns();
  for (int i = 0; i < 10'000; ++i) {
    const std::uint64_t now = lab::mono_ns();
    ASSERT_GE(now, prev);
    prev = now;
  }
}

TEST(Clock, TscAdvances) {
  const std::uint64_t a = lab::rdtsc();
  std::this_thread::sleep_for(std::chrono::milliseconds{1});
  EXPECT_GT(lab::rdtscp(), a);
}

TEST(Clock, CalibratedTscMeasuresSleepWithinTolerance) {
  const auto clock = lab::TscClock::calibrate(std::chrono::milliseconds{20});
  ASSERT_GT(clock.ticks_per_ns(), 0.0);

  const std::uint64_t t0 = lab::rdtsc();
  const std::uint64_t n0 = lab::mono_ns();
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  const std::uint64_t t1 = lab::rdtsc();
  const std::uint64_t n1 = lab::mono_ns();

  const double tsc_ns = clock.to_ns(t1 - t0);
  const double mono = static_cast<double>(n1 - n0);
  EXPECT_NEAR(tsc_ns, mono, mono * 0.05);
}

TEST(Clock, FromRatioConvertsBothWays) {
  const auto clock = lab::TscClock::from_ratio(3.0);
  EXPECT_DOUBLE_EQ(clock.to_ns(3000), 1000.0);
  EXPECT_EQ(clock.to_ticks(1000.0), 3000u);
}

}  // namespace
