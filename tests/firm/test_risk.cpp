#include <gtest/gtest.h>

#include "lab/risk/risk.hpp"

namespace {

using namespace lab;
using risk::Verdict;

constexpr double kTicksPerSecond = 1e9;  // 1 tick = 1 ns in these tests

risk::Limits limits() {
  risk::Limits l;
  l.max_order_qty = 500;
  l.max_order_notional = 1'000'000ULL * 10'000;
  l.collar_bps = 100;  // 1%
  l.max_position = 1'000;
  l.max_open_orders = 3;
  l.max_orders_per_second = 10;
  l.burst = 2;
  return l;
}

TEST(Risk, OrderSizeNotionalAndCollar) {
  risk::Risk r{limits(), kTicksPerSecond};
  oms::Oms o{1};
  const std::uint32_t mid = 1'000'000;  // $100
  EXPECT_EQ(r.check_new(o, 0, 'B', 501, mid, mid, 1), Verdict::kQuantity);
  EXPECT_EQ(r.check_new(o, 0, 'B', 0, mid, mid, 1), Verdict::kQuantity);
  EXPECT_EQ(r.check_new(o, 0, 'B', 100, 1'010'001, mid, 1), Verdict::kCollar);
  EXPECT_EQ(r.check_new(o, 0, 'B', 100, 989'999, mid, 1), Verdict::kCollar);
  EXPECT_EQ(r.check_new(o, 0, 'B', 100, mid, 0, 1), Verdict::kNoReference);
  EXPECT_EQ(r.check_new(o, 0, 'B', 100, 1'010'000, mid, 1), Verdict::kOk);
  auto l = limits();
  l.max_order_notional = 100ULL * 1'000'000 - 1;
  risk::Risk tight{l, kTicksPerSecond};
  EXPECT_EQ(tight.check_new(o, 0, 'B', 100, mid, mid, 1), Verdict::kNotional);
}

TEST(Risk, WorstCasePositionCountsWorkingOrders) {
  auto l = limits();
  l.max_open_orders = 100;
  l.burst = 100;
  risk::Risk r{l, kTicksPerSecond};
  oms::Oms o{1};
  const std::uint32_t px = 1'000'000;
  for (int i = 0; i < 2; ++i) o.enter(0, 'B', 500, px);  // 1000 working to buy
  EXPECT_EQ(r.check_new(o, 0, 'B', 1, px, px, 1), Verdict::kPosition);
  EXPECT_EQ(r.check_new(o, 0, 'S', 500, px, px, 1), Verdict::kOk);  // sells are separate
  for (int i = 0; i < 2; ++i) o.enter(0, 'S', 500, px);
  EXPECT_EQ(r.check_new(o, 0, 'S', 1, px, px, 1), Verdict::kPosition);
}

TEST(Risk, ReplaceOnlyCountsGrowth) {
  auto l = limits();
  l.burst = 100;
  l.max_order_qty = 1'000;
  risk::Risk r{l, kTicksPerSecond};
  oms::Oms o{1};
  const std::uint32_t px = 1'000'000;
  const auto ref = o.enter(0, 'B', 500, px);
  o.enter(0, 'B', 500, px);
  oms::OrderResponse a{};
  a.type = 'A';
  a.user_ref = ref;
  a.qty = 500;
  o.apply(a);
  const oms::Order* found = o.find(ref);
  ASSERT_NE(found, nullptr);
  const oms::Order& old = *found;
  EXPECT_EQ(r.check_replace(o, old, 500, px + 100, px, 1), Verdict::kOk);  // same size
  EXPECT_EQ(r.check_replace(o, old, 501, px, px, 1), Verdict::kPosition);  // grows past
}

TEST(Risk, OpenOrdersRateAndKillSwitch) {
  risk::Risk r{limits(), kTicksPerSecond};
  oms::Oms o{1};
  const std::uint32_t px = 1'000'000;
  std::uint64_t t = 1;
  EXPECT_EQ(r.check_new(o, 0, 'B', 1, px, px, t), Verdict::kOk);
  EXPECT_EQ(r.check_new(o, 0, 'B', 1, px, px, t), Verdict::kOk);
  EXPECT_EQ(r.check_new(o, 0, 'B', 1, px, px, t), Verdict::kRate);  // burst of 2 used up
  t += 100'000'000;                                                 // 0.1 s: one token
  EXPECT_EQ(r.check_new(o, 0, 'B', 1, px, px, t), Verdict::kOk);
  for (int i = 0; i < 3; ++i) o.enter(0, 'B', 1, px);
  t += 1'000'000'000;
  EXPECT_EQ(r.check_new(o, 0, 'B', 1, px, px, t), Verdict::kOpenOrders);
  r.kill();
  EXPECT_EQ(r.check_new(o, 0, 'S', 1, px, px, t), Verdict::kKill);
  EXPECT_EQ(r.blocked(Verdict::kRate), 1u);
}

TEST(Risk, NotionalDoesNotWrapForLargeOrders) {
  auto l = limits();
  l.max_order_qty = 1'000;
  risk::Risk r{l, kTicksPerSecond};
  oms::Oms o{1};
  // 900 shares at $5,000 is $4.5M: qty * price does not fit in 32 bits.
  const std::uint32_t px = 50'000'000;
  EXPECT_EQ(r.check_new(o, 0, 'B', 900, px, px, 1), Verdict::kNotional);
}

}  // namespace
