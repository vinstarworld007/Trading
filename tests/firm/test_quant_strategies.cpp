#include <gtest/gtest.h>

#include <cmath>

#include "lab/strategy/execution.hpp"
#include "lab/strategy/pairs.hpp"
#include "lab/strategy/thesis.hpp"
#include "lab/strategy/trend.hpp"

namespace {

using namespace lab::strategy;

// Feeds a pair whose log ratio is `ratio` around a $100 stock B.
PairSignal feed(PairsTrader& t, double ratio) {
  const auto b = 1'000'000u;
  const auto a = static_cast<std::uint32_t>(std::lround(b * std::exp(ratio)));
  return t.on_sample(a, b);
}

PairsTrader warmed() {
  PairsParams p;
  p.window = 100;
  p.min_samples = 50;
  PairsTrader t{p};
  for (int i = 0; i < 100; ++i) feed(t, (i % 2 ? 0.001 : -0.001));  // z of about +-1
  return t;
}

TEST(Pairs, ShortsTheSpreadWhenAIsRichAndClosesWhenItComesBack) {
  PairsTrader t = warmed();
  EXPECT_EQ(t.state(), PairState::kFlat);
  auto s = feed(t, 0.004);
  EXPECT_TRUE(s.entered);
  EXPECT_GT(s.z, 2.0);
  EXPECT_EQ(s.state, PairState::kShort);
  EXPECT_EQ(t.target_a(1'000'000), -2'000);  // $200,000 at $100: sell A
  EXPECT_EQ(t.target_b(1'000'000), 2'000);   // buy B
  s = feed(t, 0.0001);
  EXPECT_TRUE(s.exited);
  EXPECT_EQ(t.state(), PairState::kFlat);
  EXPECT_EQ(t.target_a(1'000'000), 0);
}

TEST(Pairs, LongsTheSpreadWhenAIsCheap) {
  PairsTrader t = warmed();
  const auto s = feed(t, -0.004);
  EXPECT_TRUE(s.entered);
  EXPECT_EQ(s.state, PairState::kLong);
  EXPECT_EQ(t.target_a(500'000), 4'000);  // $50 stock: 4,000 shares
}

TEST(Pairs, StopsWhenTheGapKeepsGrowingAndWaitsBeforeTradingAgain) {
  PairsTrader t = warmed();
  ASSERT_TRUE(feed(t, 0.004).entered);
  const auto s = feed(t, 0.02);
  EXPECT_TRUE(s.stopped);
  EXPECT_EQ(t.state(), PairState::kFlat);
  // Still stretched: no new entry until the z-score has come back inside exit_z.
  EXPECT_FALSE(feed(t, 0.02).entered);
}

TEST(Pairs, NoTradingBeforeMinSamples) {
  PairsParams p;
  p.window = 100;
  p.min_samples = 50;
  PairsTrader t{p};
  for (int i = 0; i < 20; ++i) feed(t, (i % 2 ? 0.001 : -0.001));
  EXPECT_FALSE(feed(t, 0.01).entered);
  EXPECT_EQ(t.state(), PairState::kFlat);
}

TEST(Trend, FollowsTheCrossOfTheAverages) {
  TrendParams p;
  p.fast = 2;
  p.slow = 4;
  p.notional = 100'000;
  TrendFollower t{p};
  for (std::uint32_t px : {1'000'000u, 1'000'000u, 1'000'000u})
    EXPECT_EQ(t.on_close(px).direction, 0);
  auto s = t.on_close(1'100'000);  // fast 105, slow 102.5
  EXPECT_EQ(s.direction, 1);
  EXPECT_TRUE(s.changed);
  EXPECT_EQ(s.fast_ma, 1'050'000u);
  EXPECT_EQ(s.slow_ma, 1'025'000u);
  EXPECT_EQ(t.target(1'000'000), 1'000);
  EXPECT_FALSE(t.on_close(1'100'000).changed);
  s = t.on_close(900'000);  // fast 100, slow 102.5
  EXPECT_EQ(s.direction, -1);
  EXPECT_TRUE(s.changed);
  EXPECT_FALSE(t.on_close(800'000).changed);
  EXPECT_EQ(t.target(800'000), -1'200);  // 1,250 rounded down to lots
}

TEST(Execution, WorksTowardTheTargetCountingWorkingOrders) {
  auto c = work_toward(1'000, 0, 0, 0, 500, 100);
  EXPECT_EQ(c.side, 'B');
  EXPECT_EQ(c.qty, 500u);
  c = work_toward(1'000, 200, 700, 0, 500, 100);
  EXPECT_EQ(c.side, 'B');
  EXPECT_EQ(c.qty, 100u);
  c = work_toward(1'000, 200, 800, 0, 500, 100);
  EXPECT_EQ(c.side, 0);
  c = work_toward(-300, 0, 0, 0, 1'000, 100);
  EXPECT_EQ(c.side, 'S');
  EXPECT_EQ(c.qty, 300u);
  c = work_toward(0, 50, 0, 0, 1'000, 100);  // less than a lot
  EXPECT_EQ(c.side, 0);
}

TEST(Execution, PovCapsAtParticipation) {
  PovParams p;  // 6%
  EXPECT_EQ(pov_child(p, 100'000, 1'000'000), 6'000u);
  EXPECT_EQ(pov_child(p, 100'000, 2'550), 2'500u);
  EXPECT_EQ(pov_child(p, 1'000, 1'000'000), 0u);
}

TEST(Thesis, ResearchesBuildsHoldsAndExitsNearFairValue) {
  ThesisParams p;
  p.fair_value = 500'000;  // $50
  p.target_shares = 1'000;
  p.research_days = 2;
  ThesisTrader t{p};
  EXPECT_EQ(t.on_day(1, 400'000, 0, 0).phase, ThesisPhase::kResearch);
  auto d = t.on_day(3, 400'000, 0, 0);
  EXPECT_EQ(d.phase, ThesisPhase::kBuild);
  EXPECT_EQ(d.target, 1'000);
  d = t.on_day(4, 410'000, 1'000, 400'000);
  EXPECT_EQ(d.phase, ThesisPhase::kHold);
  EXPECT_EQ(d.target, 1'000);
  d = t.on_day(5, 486'000, 1'000, 400'000);  // within 3% of $50
  EXPECT_EQ(d.phase, ThesisPhase::kExit);
  EXPECT_EQ(d.target, 0);
  EXPECT_FALSE(d.stopped);
  EXPECT_EQ(t.on_day(6, 486'000, 0, 0).phase, ThesisPhase::kDone);
}

TEST(Thesis, StopsWhenThePriceFallsFarBelowCost) {
  ThesisParams p;
  p.fair_value = 500'000;
  p.target_shares = 1'000;
  p.research_days = 0;
  ThesisTrader t{p};
  t.on_day(1, 400'000, 0, 0);
  const auto d = t.on_day(2, 290'000, 500, 400'000);  // 27.5% below cost
  EXPECT_EQ(d.phase, ThesisPhase::kExit);
  EXPECT_TRUE(d.stopped);
  EXPECT_EQ(d.target, 0);
}

TEST(Thesis, DoesNotBuyAboveTheEntryLevel) {
  ThesisParams p;
  p.fair_value = 500'000;
  p.research_days = 0;
  ThesisTrader t{p};
  EXPECT_EQ(t.on_day(1, 460'000, 0, 0).phase, ThesisPhase::kResearch);  // only 8% below
}

}  // namespace
