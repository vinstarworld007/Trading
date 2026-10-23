#include <gtest/gtest.h>

#include <vector>

#include "lab/mold/fault_injector.hpp"

namespace {

using lab::mold::Fault;
using lab::mold::FaultConfig;
using lab::mold::FaultInjector;

std::vector<Fault> run(const FaultConfig& cfg, int n) {
  FaultInjector f{cfg};
  std::vector<Fault> out;
  for (int i = 0; i < n; ++i) out.push_back(f.next());
  return out;
}

TEST(FaultInjector, DoesNothingByDefault) {
  FaultInjector f;
  for (int i = 0; i < 1000; ++i) EXPECT_EQ(f.next(), Fault::kNone);
  EXPECT_EQ(f.stats().packets, 1000u);
}

TEST(FaultInjector, IsDeterministicForASeed) {
  FaultConfig cfg;
  cfg.drop = 0.05;
  cfg.duplicate = 0.05;
  cfg.reorder = 0.05;
  cfg.seed = 7;
  EXPECT_EQ(run(cfg, 5000), run(cfg, 5000));
  FaultConfig other = cfg;
  other.seed = 8;
  EXPECT_NE(run(cfg, 5000), run(other, 5000));
}

TEST(FaultInjector, RatesAreRoughlyAsConfigured) {
  FaultConfig cfg;
  cfg.drop = 0.02;
  cfg.duplicate = 0.01;
  cfg.reorder = 0.03;
  FaultInjector f{cfg};
  const int n = 200'000;
  for (int i = 0; i < n; ++i) f.next();
  const auto& s = f.stats();
  EXPECT_NEAR(static_cast<double>(s.dropped) / n, 0.02, 0.003);
  EXPECT_NEAR(static_cast<double>(s.duplicated) / n, 0.01 * 0.98, 0.003);
  EXPECT_NEAR(static_cast<double>(s.reordered) / n, 0.03 * 0.97, 0.004);
}

TEST(FaultInjector, BurstsDropConsecutivePackets) {
  FaultConfig cfg;
  cfg.burst = 0.001;
  cfg.burst_len = 25;
  const auto faults = run(cfg, 100'000);
  int longest = 0;
  int current = 0;
  for (Fault f : faults) {
    current = f == Fault::kDrop ? current + 1 : 0;
    longest = std::max(longest, current);
  }
  EXPECT_GE(longest, 25);
}

}  // namespace
