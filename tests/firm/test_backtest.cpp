#include <gtest/gtest.h>

#include "daily.hpp"
#include "queue_model.hpp"

namespace {

using namespace lab::backtest;

TEST(QueueModel, FillsOnlyAfterTheQueueAheadTrades) {
  auto o = place(1, 'B', 1'000'000, 200, 500);  // 500 shares ahead of us at $100
  EXPECT_EQ(on_execution(o, 'B', 1'000'000, 300).qty, 0u);
  EXPECT_EQ(o.ahead, 200u);
  const auto f = on_execution(o, 'B', 1'000'000, 300);  // 200 ahead, then 100 of ours
  EXPECT_EQ(f.qty, 100u);
  EXPECT_EQ(f.price, 1'000'000u);
  EXPECT_EQ(o.open, 100u);
  EXPECT_EQ(on_execution(o, 'B', 1'000'000, 500).qty, 100u);
  EXPECT_EQ(o.open, 0u);
}

TEST(QueueModel, IgnoresTheOtherSideAndBetterPrices) {
  auto o = place(1, 'S', 1'000'000, 100, 0);
  EXPECT_EQ(on_execution(o, 'B', 1'000'000, 500).qty, 0u);  // a bid traded, not our side
  EXPECT_EQ(on_execution(o, 'S', 999'900, 500).qty, 0u);    // an ask below ours traded first
  EXPECT_EQ(o.open, 100u);
}

TEST(QueueModel, TradingThroughOurPriceFillsUsInFull) {
  auto o = place(1, 'B', 1'000'000, 300, 10'000);
  const auto f = on_execution(o, 'B', 999'900, 100);  // a seller swept below our bid
  EXPECT_EQ(f.qty, 300u);
  EXPECT_EQ(f.price, 1'000'000u);
}

TEST(QueueModel, CancelsCapTheQueueAhead) {
  auto o = place(1, 'B', 1'000'000, 100, 1'000);
  on_level(o, 400);  // the level shrank: at most 400 can be ahead now
  EXPECT_EQ(o.ahead, 400u);
  on_level(o, 900);  // orders joining behind us do not move us back
  EXPECT_EQ(o.ahead, 400u);
}

TEST(QueueModel, AnOpposingOrderAtOurPriceFillsUs) {
  auto o = place(1, 'B', 1'000'000, 300, 0);
  EXPECT_EQ(on_opposite_best(o, 1'000'100, 500).qty, 0u);
  const auto f = on_opposite_best(o, 1'000'000, 200);
  EXPECT_EQ(f.qty, 200u);
  EXPECT_EQ(o.open, 100u);
}

TEST(Daily, ParsesPricesWithoutFloatingPoint) {
  EXPECT_EQ(parse_price4("157.92"), 1'579'200u);
  EXPECT_EQ(parse_price4("$1.5"), 15'000u);
  EXPECT_EQ(parse_price4("40"), 400'000u);
  EXPECT_EQ(parse_price4("0.0001"), 1u);
  EXPECT_FALSE(parse_price4("1.23456"));
  EXPECT_FALSE(parse_price4("1,000"));
  EXPECT_FALSE(parse_price4(""));
}

TEST(Daily, ParsesALineAndSkipsTheHeader) {
  EXPECT_FALSE(parse_daily_line("date,open,high,low,close,volume"));
  const auto b = parse_daily_line("2019-01-30,163.25,166.15,160.23,165.25,61109780");
  ASSERT_TRUE(b);
  EXPECT_EQ(b->date, "2019-01-30");
  EXPECT_EQ(b->open, 1'632'500u);
  EXPECT_EQ(b->low, 1'602'300u);
  EXPECT_EQ(b->close, 1'652'500u);
  EXPECT_EQ(b->volume, 61'109'780u);
}

}  // namespace
