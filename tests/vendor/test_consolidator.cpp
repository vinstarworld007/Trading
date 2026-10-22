#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "lab/itch/codec.hpp"
#include "lab/vendor/consolidator.hpp"

namespace {

using namespace lab::itch;
using lab::vendor::Consolidator;
using lab::vendor::Record;
namespace field = lab::vendor::field;

struct Change {
  Record r;
  std::uint32_t mask;
};

class Feed {
 public:
  template <class M>
  std::vector<Change> send(M m, std::uint16_t locate = 7) {
    m.header.stock_locate = locate;
    m.header.timestamp = ++ts_;
    std::array<std::byte, kMaxMessageSize> buf{};
    encode(m, buf.data());
    std::vector<Change> out;
    c.apply({buf.data(), M::kSize},
            [&](const Record& r, std::uint32_t mask) { out.push_back({r, mask}); });
    return out;
  }
  std::vector<Change> add(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t px) {
    AddOrder a;
    a.order_reference_number = ref;
    a.buy_sell_indicator = side;
    a.shares = shares;
    a.stock = make_symbol("ACME");
    a.price = px;
    return send(a);
  }
  Consolidator c{1024};
  std::uint64_t ts_ = 34'200'000'000'000;
};

TEST(Consolidator, ListingAloneSaysNothing) {
  Feed f;
  StockDirectory d;
  d.stock = make_symbol("ACME");
  d.round_lot_size = 100;
  EXPECT_TRUE(f.send(d).empty());
  EXPECT_EQ(f.c.find("ACME"), 7);
  EXPECT_EQ(f.c.find("NOPE"), 0);
  EXPECT_TRUE(f.c.record(7).listed);
}

TEST(Consolidator, ReportsQuoteTradeAndStatusChanges) {
  Feed f;
  StockDirectory d;
  d.stock = make_symbol("ACME");
  f.send(d);

  auto ch = f.add(1, 'B', 300, 1'000'000);  // $100.00
  ASSERT_EQ(ch.size(), 1u);
  EXPECT_EQ(ch[0].mask, field::kBid | field::kBidSize);
  EXPECT_EQ(ch[0].r.bid, 1'000'000u);
  EXPECT_EQ(ch[0].r.bid_size, 300u);
  EXPECT_EQ(ch[0].r.quote_ns, f.ts_);

  ch = f.add(2, 'S', 200, 1'000'500);
  ASSERT_EQ(ch.size(), 1u);
  EXPECT_EQ(ch[0].mask, field::kAsk | field::kAskSize);

  // A deeper bid changes no top-of-book field: no update.
  EXPECT_TRUE(f.add(3, 'B', 100, 999'900).empty());

  OrderExecuted e;
  e.order_reference_number = 2;
  e.executed_shares = 50;
  e.match_number = 1;
  ch = f.send(e);
  ASSERT_EQ(ch.size(), 1u);
  EXPECT_TRUE(ch[0].mask & field::kAskSize);
  EXPECT_TRUE(ch[0].mask & field::kVolume);
  EXPECT_TRUE(ch[0].mask & field::kOpen);
  EXPECT_EQ(lab::vendor::kind_of(ch[0].mask), lab::vendor::Kind::kTrade);
  EXPECT_EQ(ch[0].r.last, 1'000'500u);
  EXPECT_EQ(ch[0].r.last_size, 50u);
  EXPECT_EQ(ch[0].r.volume, 50u);
  EXPECT_EQ(ch[0].r.open, 1'000'500u);
  EXPECT_EQ(ch[0].r.high, 1'000'500u);
  EXPECT_EQ(ch[0].r.low, 1'000'500u);
  EXPECT_EQ(ch[0].r.trades, 1u);
  EXPECT_EQ(ch[0].r.ask_size, 150u);

  StockTradingAction h;
  h.stock = make_symbol("ACME");
  h.trading_state = 'H';
  ch = f.send(h);
  ASSERT_EQ(ch.size(), 1u);
  EXPECT_EQ(ch[0].mask, field::kStatus);
  EXPECT_EQ(ch[0].r.status, 'H');
  EXPECT_EQ(lab::vendor::kind_of(ch[0].mask), lab::vendor::Kind::kStatus);

  OrderDelete del;
  del.order_reference_number = 1;
  ch = f.send(del);
  ASSERT_EQ(ch.size(), 1u);
  EXPECT_EQ(ch[0].r.bid, 999'900u);  // the next level up
  EXPECT_EQ(ch[0].r.bid_size, 100u);
  EXPECT_EQ(ch[0].r.updates, 5u);
  EXPECT_EQ(f.c.books().checks().error_count(), 0u);
}

}  // namespace
