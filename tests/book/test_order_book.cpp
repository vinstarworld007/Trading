#include <gtest/gtest.h>

#include "lab/book/order_book.hpp"

namespace {

using lab::book::Level;
using lab::book::OrderBook;
using lab::book::Side;

// Copy of a level, or an all-zero level when it is missing.
Level at(const Level* l) { return l != nullptr ? *l : Level{}; }

TEST(OrderBook, BestLevelsAndDepthOrder) {
  OrderBook b;
  b.side(Side::kBuy).add(100, 10);
  b.side(Side::kBuy).add(102, 5);
  b.side(Side::kBuy).add(101, 7);
  b.side(Side::kBuy).add(102, 3);
  b.side(Side::kSell).add(105, 4);
  b.side(Side::kSell).add(103, 6);

  ASSERT_NE(b.bids().best(), nullptr);
  EXPECT_EQ(at(b.bids().best()).price, 102u);
  EXPECT_EQ(at(b.bids().best()).shares, 8u);
  EXPECT_EQ(at(b.bids().best()).orders, 2u);
  EXPECT_EQ(at(b.bids().level(1)).price, 101u);
  EXPECT_EQ(at(b.bids().level(2)).price, 100u);
  EXPECT_EQ(b.bids().level(3), nullptr);
  EXPECT_EQ(at(b.asks().best()).price, 103u);
  EXPECT_EQ(at(b.asks().level(1)).price, 105u);
  EXPECT_FALSE(b.crossed());
  EXPECT_FALSE(b.locked());
}

TEST(OrderBook, ReduceRemovesEmptyLevels) {
  OrderBook b;
  auto& bids = b.side(Side::kBuy);
  bids.add(100, 10);
  bids.add(100, 5);
  EXPECT_TRUE(bids.reduce(100, 4, false));
  EXPECT_EQ(at(bids.best()).shares, 11u);
  EXPECT_TRUE(bids.reduce(100, 6, true));
  EXPECT_EQ(at(bids.best()).orders, 1u);
  EXPECT_TRUE(bids.reduce(100, 5, true));
  EXPECT_TRUE(bids.empty());
}

TEST(OrderBook, ReduceReportsInconsistency) {
  OrderBook b;
  auto& asks = b.side(Side::kSell);
  asks.add(200, 10);
  EXPECT_FALSE(asks.reduce(201, 1, true)) << "no such level";
  EXPECT_FALSE(asks.reduce(200, 11, true)) << "more shares than the level holds";
  EXPECT_EQ(at(asks.best()).shares, 10u) << "failed reduce changes nothing";
}

TEST(OrderBook, DetectsLockedAndCrossed) {
  OrderBook b;
  b.side(Side::kBuy).add(100, 1);
  b.side(Side::kSell).add(100, 1);
  EXPECT_TRUE(b.locked());
  EXPECT_FALSE(b.crossed());
  b.side(Side::kBuy).add(101, 1);
  EXPECT_TRUE(b.crossed());
  EXPECT_FALSE(b.locked());
}

}  // namespace
