#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "lab/core/histogram.hpp"

namespace {

using lab::LatencyHistogram;

TEST(LatencyHistogram, SmallValuesAreExact) {
  for (std::uint64_t v = 0; v < 64; ++v) {
    EXPECT_EQ(LatencyHistogram::index_of(v), v);
    EXPECT_EQ(LatencyHistogram::highest_value_at(v), v);
  }
}

TEST(LatencyHistogram, BucketsAreContiguousAndCoverTheirValues) {
  for (std::uint64_t v : {64ULL, 65ULL, 127ULL, 128ULL, 1000ULL, 123456789ULL, ~0ULL}) {
    const auto idx = LatencyHistogram::index_of(v);
    ASSERT_LT(idx, LatencyHistogram::kBuckets);
    EXPECT_GE(LatencyHistogram::highest_value_at(idx), v);
    if (idx > 0) {
      EXPECT_LT(LatencyHistogram::highest_value_at(idx - 1), v);
    }
  }
}

TEST(LatencyHistogram, PercentilesWithinThreePercent) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 100'000; ++v) h.record(v);
  EXPECT_EQ(h.count(), 100'000u);
  EXPECT_EQ(h.min(), 1u);
  EXPECT_EQ(h.max(), 100'000u);
  EXPECT_NEAR(h.mean(), 50'000.5, 0.01);
  for (double p : {50.0, 90.0, 99.0, 99.9}) {
    const double expected = p / 100.0 * 100'000;
    EXPECT_NEAR(static_cast<double>(h.percentile(p)), expected, expected * 0.032) << "p" << p;
  }
  EXPECT_EQ(h.percentile(100), 100'000u);
}

TEST(LatencyHistogram, EmptyIsZero) {
  LatencyHistogram h;
  EXPECT_EQ(h.count(), 0u);
  EXPECT_EQ(h.min(), 0u);
  EXPECT_EQ(h.percentile(99), 0u);
}

TEST(LatencyHistogram, MergeAndReset) {
  LatencyHistogram a, b;
  a.record(10);
  b.record(1000);
  a.merge(b);
  EXPECT_EQ(a.count(), 2u);
  EXPECT_EQ(a.min(), 10u);
  EXPECT_EQ(a.max(), 1000u);
  a.reset();
  EXPECT_EQ(a.count(), 0u);
  EXPECT_EQ(a.max(), 0u);
}

TEST(LatencyHistogram, PrintsOneLine) {
  LatencyHistogram h;
  h.record(100);
  char buf[512] = {};
  std::FILE* f = fmemopen(buf, sizeof buf, "w");
  ASSERT_NE(f, nullptr);
  h.print(f, "hop", 0.5);
  std::fclose(f);
  EXPECT_NE(std::string(buf).find("p50=50"), std::string::npos) << buf;
}

TEST(LatencyHistogram, PercentileRankRoundsUp) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 50; ++v) h.record(v);
  // p99 of 50 values is rank 49.5, which rounds up to the 50th value.
  EXPECT_EQ(h.percentile(99), 50u);
  EXPECT_EQ(h.percentile(50), 25u);
}

}  // namespace
