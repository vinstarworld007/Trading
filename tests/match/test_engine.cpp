#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "lab/match/engine.hpp"
#include "lab/match/key_index.hpp"

namespace {

using namespace lab::match;

constexpr std::uint16_t kSym = 7;

// Records every listener call as a short string, e.g. "exec r=1:2 a=2:1 q=100 p=1000".
struct Recorder {
  std::vector<std::string> log;
  std::vector<OrderView> rested;

  static std::string k(OrderKey key) {
    return std::to_string(key_owner(key)) + ":" + std::to_string(key_ref(key));
  }
  void on_accepted(const OrderView& o) { log.push_back("acc " + k(o.key)); }
  void on_rejected(OrderKey key, Reject r) {
    log.push_back("rej " + k(key) + " " + std::to_string(static_cast<int>(r)));
  }
  void on_execution(const OrderView& r, const OrderView& a, std::uint32_t q, std::uint32_t p,
                    std::uint64_t) {
    log.push_back("exec r=" + k(r.key) + " a=" + k(a.key) + " q=" + std::to_string(q) +
                  " p=" + std::to_string(p));
  }
  void on_rested(const OrderView& o) {
    rested.push_back(o);
    log.push_back("rest " + k(o.key) + " " + std::to_string(o.remaining) + "@" +
                  std::to_string(o.price));
  }
  void on_canceled(const OrderView& o, std::uint32_t q, CancelReason why) {
    log.push_back("cxl " + k(o.key) + " q=" + std::to_string(q) + " left=" +
                  std::to_string(o.remaining) + " why=" + std::to_string(static_cast<int>(why)));
  }
  void on_replaced(const OrderView& old, const OrderView& n) {
    log.push_back("repl " + k(old.key) + "->" + k(n.key) + " orn " + std::to_string(old.orn) +
                  "->" + std::to_string(n.orn));
  }
  std::vector<std::string> take() { return std::exchange(log, {}); }
};

NewOrder order(std::uint16_t owner, std::uint64_t ref, Side side, std::uint32_t qty,
               std::uint32_t price, Tif tif = Tif::kDay) {
  return NewOrder{make_key(owner, ref), kSym, side, tif, qty, price};
}

struct Fixture : ::testing::Test {
  Fixture() : e{std::make_unique<Engine>(EngineConfig{1024, 64, 16})} { e->add_instrument(kSym); }
  std::unique_ptr<Engine> e;
  Recorder r;
};

using EngineTest = Fixture;

TEST_F(EngineTest, RestsOrdersThatDoNotCross) {
  e->add(order(1, 1, Side::kBuy, 100, 1000), r);
  e->add(order(1, 2, Side::kSell, 50, 1010), r);
  EXPECT_EQ(r.take(), (std::vector<std::string>{"acc 1:1", "rest 1:1 100@1000", "acc 1:2",
                                                "rest 1:2 50@1010"}));
  const auto bids = e->depth(kSym, Side::kBuy, 5);
  ASSERT_EQ(bids.size(), 1u);
  EXPECT_EQ(bids[0].price, 1000u);
  EXPECT_EQ(bids[0].shares, 100u);
  EXPECT_EQ(e->live_orders(), 2u);
  EXPECT_EQ(r.rested[0].orn, 1u);
  EXPECT_EQ(r.rested[1].orn, 2u);
}

TEST_F(EngineTest, MatchesAtTheRestingPriceInTimePriority) {
  e->add(order(1, 1, Side::kSell, 100, 1010), r);
  e->add(order(1, 2, Side::kSell, 100, 1010), r);  // same price, later
  e->add(order(1, 3, Side::kSell, 100, 1005), r);  // better price
  r.take();
  e->add(order(2, 1, Side::kBuy, 250, 1020), r);
  EXPECT_EQ(r.take(), (std::vector<std::string>{
                          "acc 2:1",
                          "exec r=1:3 a=2:1 q=100 p=1005",
                          "exec r=1:1 a=2:1 q=100 p=1010",
                          "exec r=1:2 a=2:1 q=50 p=1010",
                      }));
  const auto asks = e->depth(kSym, Side::kSell, 5);
  ASSERT_EQ(asks.size(), 1u);
  EXPECT_EQ(asks[0].shares, 50u);
  EXPECT_EQ(asks[0].orders, 1u);
  EXPECT_EQ(e->live_orders(), 1u);  // the aggressor was filled, 1:2 has 50 left
  EXPECT_EQ(e->stats().executed_shares, 250u);
}

TEST_F(EngineTest, RemainderRestsAfterSweeping) {
  e->add(order(1, 1, Side::kSell, 100, 1010), r);
  r.take();
  e->add(order(2, 1, Side::kBuy, 300, 1010), r);
  EXPECT_EQ(r.take(), (std::vector<std::string>{"acc 2:1", "exec r=1:1 a=2:1 q=100 p=1010",
                                                "rest 2:1 200@1010"}));
  EXPECT_TRUE(e->depth(kSym, Side::kSell, 5).empty());
  EXPECT_EQ(e->depth(kSym, Side::kBuy, 5)[0].shares, 200u);
}

TEST_F(EngineTest, IocRemainderIsCanceledNotRested) {
  e->add(order(1, 1, Side::kBuy, 100, 1000), r);
  r.take();
  e->add(order(2, 1, Side::kSell, 150, 990, Tif::kIoc), r);
  EXPECT_EQ(r.take(), (std::vector<std::string>{"acc 2:1", "exec r=1:1 a=2:1 q=100 p=1000",
                                                "cxl 2:1 q=50 left=0 why=1"}));
  EXPECT_EQ(e->live_orders(), 0u);
}

TEST_F(EngineTest, CancelReducesOrRemoves) {
  e->add(order(1, 1, Side::kBuy, 100, 1000), r);
  e->add(order(1, 2, Side::kBuy, 100, 1000), r);
  r.take();
  EXPECT_TRUE(e->cancel(make_key(1, 1), 40, r));  // reduce to 40: keeps priority
  EXPECT_TRUE(e->cancel(make_key(1, 1), 90, r));  // larger: no change
  EXPECT_TRUE(e->cancel(make_key(1, 2), 0, r));
  EXPECT_FALSE(e->cancel(make_key(1, 9), 0, r));
  EXPECT_EQ(r.take(),
            (std::vector<std::string>{"cxl 1:1 q=60 left=40 why=0", "cxl 1:2 q=100 left=0 why=0"}));
  const auto bids = e->depth(kSym, Side::kBuy, 5);
  ASSERT_EQ(bids.size(), 1u);
  EXPECT_EQ(bids[0].shares, 40u);
  EXPECT_EQ(bids[0].orders, 1u);
}

TEST_F(EngineTest, ReplaceLosesPriorityAndCanTrade) {
  e->add(order(1, 1, Side::kBuy, 100, 1000), r);
  e->add(order(1, 2, Side::kBuy, 100, 1000), r);
  e->add(order(2, 1, Side::kSell, 100, 1010), r);
  r.take();
  // 1:1 replaced at the same price: now behind 1:2.
  e->replace(make_key(1, 1), order(1, 3, Side::kBuy, 100, 1000), r);
  EXPECT_EQ(r.take(), (std::vector<std::string>{"repl 1:1->1:3 orn 1->4", "rest 1:3 100@1000"}));
  e->add(order(3, 1, Side::kSell, 100, 1000), r);
  EXPECT_EQ(r.take()[1], "exec r=1:2 a=3:1 q=100 p=1000");

  // Replace through the offer: trades at once, remainder rests.
  e->replace(make_key(1, 3), order(1, 4, Side::kBuy, 150, 1010), r);
  EXPECT_EQ(r.take(),
            (std::vector<std::string>{"repl 1:3->1:4 orn 4->6", "exec r=2:1 a=1:4 q=100 p=1010",
                                      "rest 1:4 50@1010"}));
}

TEST_F(EngineTest, ReplaceQuantityCountsSharesAlreadyExecuted) {
  e->add(order(1, 1, Side::kBuy, 100, 1000), r);
  e->add(order(2, 1, Side::kSell, 30, 1000), r);  // 1:1 executes 30, 70 left
  r.take();
  e->replace(make_key(1, 1), order(1, 2, Side::kBuy, 100, 999), r);
  EXPECT_EQ(r.take()[1], "rest 1:2 70@999");
  e->replace(make_key(1, 2), order(1, 3, Side::kBuy, 30, 999), r);  // all 30 already done
  EXPECT_EQ(r.take(), (std::vector<std::string>{"cxl 1:2 q=70 left=0 why=0"}));
  EXPECT_EQ(e->live_orders(), 0u);
}

TEST_F(EngineTest, RejectsBadOrders) {
  e->add(order(1, 1, Side::kBuy, 0, 1000), r);
  e->add(order(1, 2, Side::kBuy, 1'000'000, 1000), r);
  e->add(order(1, 3, Side::kBuy, 100, 0), r);
  e->add(NewOrder{make_key(1, 4), 99, Side::kBuy, Tif::kDay, 100, 1000}, r);
  e->add(order(1, 5, Side::kBuy, 100, 1000), r);
  r.take();
  e->add(order(1, 5, Side::kBuy, 100, 1000), r);  // key still live
  e->replace(make_key(1, 77), order(1, 6, Side::kBuy, 100, 1000), r);
  EXPECT_EQ(r.take(), (std::vector<std::string>{"rej 1:5 0", "rej 1:6 1"}));
  EXPECT_EQ(e->stats().rejected, 6u);
}

TEST_F(EngineTest, CancelOwnerClearsOnlyThatOwner) {
  for (std::uint64_t i = 1; i <= 5; ++i) e->add(order(1, i, Side::kBuy, 10, 1000 - i), r);
  for (std::uint64_t i = 1; i <= 3; ++i) e->add(order(2, i, Side::kSell, 10, 2000 + i), r);
  EXPECT_EQ(e->cancel_owner(1, CancelReason::kSystem, r), 5u);
  EXPECT_EQ(e->live_orders(), 3u);
  EXPECT_TRUE(e->depth(kSym, Side::kBuy, 10).empty());
  EXPECT_EQ(e->depth(kSym, Side::kSell, 10).size(), 3u);
}

// Random orders against a slow, obviously correct model: the books and the
// traded volume must agree after every command.
TEST(EngineFuzz, MatchesAReferenceModel) {
  struct RefOrder {
    std::uint64_t seq;
    Side side;
    std::uint32_t price;
    std::uint32_t qty;
  };
  std::map<OrderKey, RefOrder> ref;
  std::uint64_t seq = 0;
  std::uint64_t ref_volume = 0;

  auto e = std::make_unique<Engine>(EngineConfig{4096, 16, 64});
  e->add_instrument(kSym);
  struct Null {
    void on_accepted(const OrderView&) {}
    void on_rejected(OrderKey, Reject) {}
    void on_execution(const OrderView&, const OrderView&, std::uint32_t, std::uint32_t,
                      std::uint64_t) {}
    void on_rested(const OrderView&) {}
    void on_canceled(const OrderView&, std::uint32_t, CancelReason) {}
    void on_replaced(const OrderView&, const OrderView&) {}
  } null;

  std::mt19937_64 rng{123};
  auto pick = [&](std::uint64_t n) { return rng() % n; };
  std::uint64_t next_ref = 1;
  for (int step = 0; step < 20000; ++step) {
    const int op = static_cast<int>(pick(10));
    if (op < 6 || ref.empty()) {
      const Side side = pick(2) ? Side::kBuy : Side::kSell;
      const std::uint32_t price = 1000 + static_cast<std::uint32_t>(pick(20));
      std::uint32_t qty = 1 + static_cast<std::uint32_t>(pick(500));
      const Tif tif = pick(5) == 0 ? Tif::kIoc : Tif::kDay;
      const OrderKey key = make_key(1, next_ref++);
      e->add(NewOrder{key, kSym, side, tif, qty, price}, null);
      // Reference: walk the best opposite orders in price-time order.
      while (qty > 0) {
        auto best = ref.end();
        for (auto it = ref.begin(); it != ref.end(); ++it) {
          const auto& o = it->second;
          if (o.side == side) continue;
          const bool cross = side == Side::kBuy ? o.price <= price : o.price >= price;
          if (!cross) continue;
          if (best == ref.end()) {
            best = it;
            continue;
          }
          const auto& b = best->second;
          const bool better = side == Side::kBuy ? o.price < b.price : o.price > b.price;
          if (better || (o.price == b.price && o.seq < b.seq)) best = it;
        }
        if (best == ref.end()) break;
        const std::uint32_t q = std::min(qty, best->second.qty);
        qty -= q;
        best->second.qty -= q;
        ref_volume += q;
        if (best->second.qty == 0) ref.erase(best);
      }
      if (qty > 0 && tif == Tif::kDay) ref[key] = RefOrder{++seq, side, price, qty};
    } else if (op < 8) {
      auto it = ref.begin();
      std::advance(it, static_cast<long>(pick(ref.size())));
      const std::uint32_t to = pick(2) ? 0 : static_cast<std::uint32_t>(pick(it->second.qty));
      ASSERT_TRUE(e->cancel(it->first, to, null));
      if (to == 0)
        ref.erase(it);
      else
        it->second.qty = std::min(it->second.qty, to);
    } else {
      // Replace at a new price that does not cross (keeps the reference simple).
      auto it = ref.begin();
      std::advance(it, static_cast<long>(pick(ref.size())));
      const RefOrder o = it->second;
      const std::uint32_t price = o.side == Side::kBuy
                                      ? 980 + static_cast<std::uint32_t>(pick(15))
                                      : 1025 + static_cast<std::uint32_t>(pick(15));
      const OrderKey key = make_key(1, next_ref++);
      const auto* found = e->find(it->first);
      ASSERT_NE(found, nullptr);
      const std::uint32_t executed = found->executed;
      const std::uint32_t qty = executed + 1 + static_cast<std::uint32_t>(pick(300));
      e->replace(it->first, NewOrder{key, kSym, o.side, Tif::kDay, qty, price}, null);
      ref.erase(it);
      ref[key] = RefOrder{++seq, o.side, price, qty - executed};
    }

    ASSERT_EQ(e->live_orders(), ref.size()) << "step " << step;
    ASSERT_EQ(e->stats().executed_shares, ref_volume) << "step " << step;
    if (step % 97 == 0) {
      for (Side side : {Side::kBuy, Side::kSell}) {
        std::map<std::uint32_t, std::uint64_t> want;
        for (const auto& [k, o] : ref)
          if (o.side == side) want[o.price] += o.qty;
        const auto got = e->depth(kSym, side, 1000);
        ASSERT_EQ(got.size(), want.size());
        for (const auto& l : got) EXPECT_EQ(l.shares, want[l.price]) << l.price;
      }
    }
  }
}

TEST(KeyIndex, InsertFindEraseWithCollisions) {
  KeyIndex idx{1000};
  for (std::uint32_t i = 0; i < 1000; ++i) ASSERT_TRUE(idx.insert(i * 4096ULL, i));
  EXPECT_FALSE(idx.insert(0, 5));         // present
  EXPECT_FALSE(idx.insert(99999999, 5));  // full
  for (std::uint32_t i = 0; i < 1000; i += 2) ASSERT_TRUE(idx.erase(i * 4096ULL));
  for (std::uint32_t i = 0; i < 1000; ++i) {
    EXPECT_EQ(idx.find(i * 4096ULL), i % 2 ? i : KeyIndex::kNone) << i;
  }
  EXPECT_EQ(idx.size(), 500u);
  EXPECT_FALSE(idx.erase(0));
}

}  // namespace
