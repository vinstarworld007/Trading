#include <gtest/gtest.h>

#include "lab/book/implied.hpp"
#include "lab/book/luld.hpp"
#include "lab/book/order_book.hpp"

namespace {

using lab::book::OrderBook;
using lab::book::Side;

TEST(Luld, TierOneBandsByReferencePrice) {
  // $100.00: 5%, $5.00 either side.
  auto b = lab::book::luld_band(1'000'000);
  EXPECT_EQ(b.lower, 950'000u);
  EXPECT_EQ(b.upper, 1'050'000u);
  // $2.00: 20%.
  b = lab::book::luld_band(20'000);
  EXPECT_EQ(b.lower, 16'000u);
  EXPECT_EQ(b.upper, 24'000u);
  // $0.50: the lesser of $0.15 and 75%.
  b = lab::book::luld_band(5'000);
  EXPECT_EQ(b.lower, 3'500u);
  EXPECT_EQ(b.upper, 6'500u);
  // $0.10: 75% is $0.075, rounded down to $0.07.
  b = lab::book::luld_band(1'000);
  EXPECT_EQ(b.lower, 300u);
  EXPECT_EQ(b.upper, 1'700u);
  // Override above $3.00 only; widths round down to whole cents.
  b = lab::book::luld_band(1'234'500, 2);
  EXPECT_EQ(b.upper - 1'234'500, 24'600u);
  EXPECT_EQ(lab::book::luld_band(0).upper, 0u);
}

TEST(Implied, ParseStrip) {
  const auto s = lab::book::parse_strip("ST12=SYM0001+SYM0002");
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->name, "ST12");
  EXPECT_EQ(s->leg_a, "SYM0001");
  EXPECT_EQ(s->leg_b, "SYM0002");
  EXPECT_FALSE(lab::book::parse_strip("ST12").has_value());
  EXPECT_FALSE(lab::book::parse_strip("=A+B").has_value());
  EXPECT_FALSE(lab::book::parse_strip("S=A+").has_value());
  EXPECT_FALSE(lab::book::parse_strip("TOOLONGNAME=A+B").has_value());
}

TEST(Implied, OutWalksBothLegs) {
  OrderBook a, b;
  a.side(Side::kSell).add(100, 300);
  a.side(Side::kSell).add(101, 500);
  b.side(Side::kSell).add(200, 400);
  b.side(Side::kSell).add(202, 100);
  // 300 at 100+200, then 100 at 101+200, then 100 at 101+202.
  const auto out = lab::book::implied_out(a.asks(), b.asks(), 5);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].price, 300u);
  EXPECT_EQ(out[0].shares, 300u);
  EXPECT_EQ(out[1].price, 301u);
  EXPECT_EQ(out[1].shares, 100u);
  EXPECT_EQ(out[2].price, 303u);
  EXPECT_EQ(out[2].shares, 100u);
  EXPECT_EQ(lab::book::implied_out(a.asks(), b.asks(), 1).size(), 1u);
  EXPECT_TRUE(lab::book::implied_out(a.bids(), b.asks(), 5).empty());
}

TEST(Implied, InFromStripAndOtherLeg) {
  OrderBook strip, other;
  strip.side(Side::kBuy).add(500, 200);
  strip.side(Side::kSell).add(510, 100);
  other.side(Side::kBuy).add(195, 50);
  other.side(Side::kSell).add(205, 300);
  const auto q = lab::book::implied_in(strip, other);
  ASSERT_TRUE(q.bid.has_value());
  EXPECT_EQ(q.bid->price, 295u);  // 500 - 205
  EXPECT_EQ(q.bid->shares, 200u);
  ASSERT_TRUE(q.ask.has_value());
  EXPECT_EQ(q.ask->price, 315u);  // 510 - 195
  EXPECT_EQ(q.ask->shares, 50u);
}

}  // namespace
