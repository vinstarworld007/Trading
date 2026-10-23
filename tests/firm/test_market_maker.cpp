#include <gtest/gtest.h>

#include "lab/strategy/market_maker.hpp"

namespace {

using namespace lab::strategy;

TEST(MarketMaker, JoinsANarrowSpreadAndImprovesAWideOne) {
  MarketMaker mm{MakerParams{}};
  auto q = mm.quote({10'000, 10'200}, 0);  // 2 ticks: join
  EXPECT_EQ(q.bid_px, 10'000u);
  EXPECT_EQ(q.ask_px, 10'200u);
  EXPECT_EQ(q.bid_qty, 100u);
  EXPECT_EQ(q.ask_qty, 100u);
  q = mm.quote({10'000, 10'500}, 0);  // 5 ticks: step inside
  EXPECT_EQ(q.bid_px, 10'100u);
  EXPECT_EQ(q.ask_px, 10'400u);
}

TEST(MarketMaker, LeansAgainstPositionWithoutCrossing) {
  MarketMaker mm{MakerParams{}};
  auto q = mm.quote({10'000, 10'500}, 200);  // long 2 lots: 2 ticks lower
  EXPECT_EQ(q.bid_px, 9'900u);
  EXPECT_EQ(q.ask_px, 10'200u);
  q = mm.quote({10'000, 10'200}, 1'000);  // very long: ask stops above the others' bid
  EXPECT_EQ(q.ask_px, 10'100u);
  EXPECT_EQ(q.bid_qty, 0u);  // at the position cap: no more buying
  EXPECT_EQ(q.ask_qty, 100u);
}

TEST(MarketMaker, HoldsWithinHoldTicks) {
  MakerParams p;
  EXPECT_FALSE(should_move(p, 10'000, 10'000));
  EXPECT_FALSE(should_move(p, 10'000, 10'100));
  EXPECT_FALSE(should_move(p, 10'100, 10'000));
  EXPECT_TRUE(should_move(p, 10'000, 10'200));
  p.hold_ticks = 0;
  EXPECT_TRUE(should_move(p, 10'000, 10'100));
}

TEST(MarketMaker, NoMarketNoQuote) {
  MarketMaker mm{MakerParams{}};
  EXPECT_EQ(mm.quote({0, 10'200}, 0).bid_qty, 0u);
  EXPECT_EQ(mm.quote({10'200, 10'200}, 0).ask_qty, 0u);
}

TEST(MarketMaker, OthersExcludesOurOwnOrder) {
  lab::oms::TopOfBook t{};
  t.bid_px[0] = 10'100;
  t.bid_qty[0] = 100;  // only us
  t.bid_px[1] = 10'000;
  t.bid_qty[1] = 500;
  t.ask_px[0] = 10'300;
  t.ask_qty[0] = 300;  // us and others
  auto m = others(t, 10'100, 100, 10'300, 100);
  EXPECT_EQ(m.bid, 10'000u);
  EXPECT_EQ(m.ask, 10'300u);
  m = others(t, 0, 0, 0, 0);
  EXPECT_EQ(m.bid, 10'100u);
  t.bid_qty[1] = 0;
  EXPECT_EQ(others(t, 10'100, 100, 0, 0).bid, 0u);  // nothing else on the bid
}

}  // namespace
