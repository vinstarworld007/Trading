#include <gtest/gtest.h>

#include "lab/oms/oms.hpp"

namespace {

using namespace lab::oms;

OrderResponse resp(char type, std::uint32_t ref, std::uint32_t qty, std::uint32_t price = 0,
                   std::uint32_t orig = 0, char flag = 0) {
  OrderResponse r{};
  r.type = type;
  r.user_ref = ref;
  r.qty = qty;
  r.price = price;
  r.orig_ref = orig;
  r.flag = flag;
  return r;
}

// The order for `ref`; fails the test (and returns a blank order) if there is none.
const Order& at(const Oms& oms, std::uint32_t ref) {
  static const Order kNone{};
  const Order* o = oms.find(ref);
  if (o == nullptr) {
    ADD_FAILURE() << "no order " << ref;
    return kNone;
  }
  return *o;
}

TEST(Oms, EnterAcceptFillTracksPositionCashAndExposure) {
  Oms oms{2};
  const auto ref = oms.enter(1, 'B', 300, 100'000);
  ASSERT_NE(ref, 0u);
  EXPECT_EQ(oms.position(1).open_buy, 300u);
  EXPECT_EQ(oms.working(), 1u);
  oms.apply(resp('A', ref, 300, 100'000));
  EXPECT_EQ(at(oms, ref).state, State::kLive);
  auto a = oms.apply(resp('E', ref, 100, 99'900));
  EXPECT_TRUE(a.fill);
  EXPECT_FALSE(a.finished);
  const Position& p = oms.position(1);
  EXPECT_EQ(p.shares, 100);
  EXPECT_EQ(p.cash, -100 * 99'900);
  EXPECT_EQ(p.open_buy, 200u);
  a = oms.apply(resp('E', ref, 200, 100'000));
  EXPECT_TRUE(a.finished);
  EXPECT_EQ(p.shares, 300);
  EXPECT_EQ(p.open_buy, 0u);
  EXPECT_EQ(oms.working(), 0u);
  EXPECT_EQ(p.pnl(101'000), -100 * 99'900 - 200 * 100'000 + 300 * 101'000);
  EXPECT_EQ(oms.stats().inconsistent, 0u);
}

TEST(Oms, ReplaceSwapsOrderAndCarriesExecutedShares) {
  Oms oms{1};
  const auto ref = oms.enter(0, 'S', 500, 50'000);
  oms.apply(resp('A', ref, 500));
  oms.apply(resp('E', ref, 200, 50'000));
  const auto n = oms.replace(ref, 400, 50'100);
  ASSERT_NE(n, 0u);
  EXPECT_EQ(oms.replace_quantity(n), 600u);           // OUCH counts the chain's executed shares
  EXPECT_EQ(oms.position(0).open_sell, 300u + 400u);  // both count until the answer
  EXPECT_FALSE(oms.cancel(ref));                      // one request in flight
  oms.apply(resp('U', n, 400, 50'100, ref));
  EXPECT_EQ(at(oms, ref).state, State::kDone);
  EXPECT_EQ(at(oms, n).state, State::kLive);
  EXPECT_EQ(at(oms, n).executed, 200u);
  EXPECT_EQ(oms.position(0).open_sell, 400u);
  EXPECT_EQ(oms.working(), 1u);
  EXPECT_EQ(oms.stats().inconsistent, 0u);
}

TEST(Oms, RejectedReplaceLeavesTheOriginal) {
  Oms oms{1};
  const auto ref = oms.enter(0, 'B', 100, 10'000);
  oms.apply(resp('A', ref, 100));
  const auto n = oms.replace(ref, 100, 10'100);
  oms.apply(resp('J', n, 0));
  EXPECT_EQ(at(oms, ref).state, State::kLive);
  EXPECT_EQ(at(oms, ref).replacement, 0u);
  EXPECT_EQ(oms.position(0).open_buy, 100u);
  EXPECT_EQ(oms.working(), 1u);
}

TEST(Oms, FillDuringReplaceThenRejectFinishesTheOrder) {
  Oms oms{1};
  const auto ref = oms.enter(0, 'B', 100, 10'000);
  oms.apply(resp('A', ref, 100));
  const auto n = oms.replace(ref, 100, 10'100);
  oms.apply(resp('E', ref, 100, 10'000));  // filled before the replace arrived
  EXPECT_EQ(oms.working(), 2u);
  const auto a = oms.apply(resp('J', n, 0));
  EXPECT_TRUE(a.finished);
  EXPECT_EQ(oms.working(), 0u);
  EXPECT_EQ(oms.position(0).open_buy, 0u);
  EXPECT_EQ(oms.position(0).shares, 100);
}

TEST(Oms, ReplaceForNoMoreThanExecutedIsAnsweredByCancel) {
  Oms oms{1};
  const auto ref = oms.enter(0, 'S', 200, 10'000);
  oms.apply(resp('A', ref, 200));
  const auto n = oms.replace(ref, 100, 10'100);  // sends 0 executed + 100
  oms.apply(resp('E', ref, 150, 10'000));        // the exchange saw this first
  const auto a = oms.apply(resp('C', ref, 50, 0, 0, 'U'));
  EXPECT_TRUE(a.finished);
  EXPECT_EQ(at(oms, n).state, State::kDone);
  EXPECT_EQ(oms.working(), 0u);
  EXPECT_EQ(oms.position(0).open_sell, 0u);
}

TEST(Oms, CancelAndRejectAndInconsistentResponses) {
  Oms oms{1};
  const auto a = oms.enter(0, 'B', 100, 10'000);
  const auto b = oms.enter(0, 'B', 100, 10'000);
  oms.apply(resp('J', b, 0));
  EXPECT_EQ(at(oms, b).state, State::kDone);
  oms.apply(resp('A', a, 100));
  EXPECT_TRUE(oms.cancel(a));
  EXPECT_FALSE(oms.cancel(a));
  oms.apply(resp('C', a, 100, 0, 0, 'U'));
  EXPECT_EQ(oms.working(), 0u);
  EXPECT_EQ(oms.position(0).open_buy, 0u);
  oms.apply(resp('E', 999, 100));  // unknown order
  oms.apply(resp('E', a, 100));    // finished order
  EXPECT_EQ(oms.stats().inconsistent, 2u);
}

TEST(Oms, RefusesASlotStillInUse) {
  Oms oms{1, 4};
  for (int i = 0; i < 4; ++i) ASSERT_NE(oms.enter(0, 'B', 1, 1), 0u);
  EXPECT_EQ(oms.enter(0, 'B', 1, 1), 0u);
  EXPECT_EQ(oms.stats().full, 1u);
}

}  // namespace
