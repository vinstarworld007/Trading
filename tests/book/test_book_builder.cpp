#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <vector>

#include "lab/book/book_builder.hpp"
#include "lab/book/report.hpp"
#include "lab/itch/synthetic.hpp"

namespace {

using namespace lab::itch;
using lab::book::BookBuilder;
using lab::book::Level;
using lab::book::Side;

// Copy of a level, or an all-zero level when it is missing.
Level at(const Level* l) { return l != nullptr ? *l : Level{}; }

// Feeds hand-made messages to a builder with increasing timestamps.
class Feed {
 public:
  Feed() : builder_{std::make_unique<BookBuilder>(1024)} {}

  template <class M>
  void send(M m, std::uint16_t locate = 1) {
    m.header.stock_locate = locate;
    m.header.timestamp = ++ts_;
    std::array<std::byte, kMaxMessageSize> buf{};
    encode(m, buf.data());
    builder_->apply(buf.data(), M::kSize);
  }

  void list(std::uint16_t locate, const char* symbol) {
    StockDirectory r;
    r.stock = make_symbol(symbol);
    r.round_lot_size = 100;
    send(r, locate);
  }

  void add(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price,
           const char* symbol = "TEST", std::uint16_t locate = 1) {
    AddOrder a;
    a.order_reference_number = ref;
    a.buy_sell_indicator = side;
    a.shares = shares;
    a.stock = make_symbol(symbol);
    a.price = price;
    send(a, locate);
  }

  void rewind_clock(std::uint64_t by) { ts_ -= by; }
  BookBuilder& b() { return *builder_; }

 private:
  std::unique_ptr<BookBuilder> builder_;
  std::uint64_t ts_ = 1'000'000;
};

TEST(BookBuilder, AppliesTheFullOrderLifecycle) {
  Feed f;
  f.list(1, "TEST");
  f.add(1, 'B', 300, 100'000);
  f.add(2, 'B', 200, 100'000);
  f.add(3, 'S', 500, 101'000);

  OrderExecuted e;
  e.order_reference_number = 1;
  e.executed_shares = 100;
  f.send(e);
  OrderCancel x;
  x.order_reference_number = 2;
  x.cancelled_shares = 50;
  f.send(x);

  const auto& bids = f.b().book(1).bids();
  ASSERT_NE(bids.best(), nullptr);
  EXPECT_EQ(at(bids.best()).shares, 350u);
  EXPECT_EQ(at(bids.best()).orders, 2u);

  // Executing the rest of order 1 removes it without a delete message.
  e.executed_shares = 200;
  f.send(e);
  EXPECT_EQ(f.b().orders().find(1), nullptr);
  EXPECT_EQ(at(bids.best()).orders, 1u);

  // Replace moves order 3 to a new reference, price and size, same side.
  OrderReplace u;
  u.original_order_reference_number = 3;
  u.new_order_reference_number = 30;
  u.shares = 100;
  u.price = 100'500;
  f.send(u);
  const auto& asks = f.b().book(1).asks();
  EXPECT_EQ(asks.depth(), 1u);
  EXPECT_EQ(at(asks.best()).price, 100'500u);
  EXPECT_EQ(at(asks.best()).shares, 100u);
  EXPECT_EQ(f.b().orders().find(3), nullptr);
  ASSERT_NE(f.b().orders().find(30), nullptr);

  OrderDelete d;
  d.order_reference_number = 30;
  f.send(d);
  EXPECT_TRUE(asks.empty());

  const auto& s = f.b().instrument(1).stats;
  EXPECT_EQ(s.adds, 3u);
  EXPECT_EQ(s.executions, 2u);
  EXPECT_EQ(s.executed_shares, 300u);
  EXPECT_EQ(s.last_trade_price, 100'000u);
  EXPECT_EQ(s.cancels, 1u);
  EXPECT_EQ(s.replaces, 1u);
  EXPECT_EQ(s.deletes, 1u);
  EXPECT_EQ(f.b().checks().error_count(), 0u);
}

TEST(BookBuilder, CountsTradesThatDoNotTouchTheBook) {
  Feed f;
  f.list(1, "TEST");
  f.add(1, 'S', 100, 50'000);
  OrderExecutedWithPrice c;
  c.order_reference_number = 1;
  c.executed_shares = 40;
  c.printable = 'N';
  c.execution_price = 49'900;
  f.send(c);
  Trade p;
  p.shares = 70;
  p.price = 50'100;
  p.stock = make_symbol("TEST");
  f.send(p);
  CrossTrade q;
  q.shares = 1000;
  q.cross_price = 50'000;
  q.cross_type = 'C';
  q.stock = make_symbol("TEST");
  f.send(q);

  const auto& s = f.b().instrument(1).stats;
  EXPECT_EQ(s.non_printable_shares, 40u);
  EXPECT_EQ(s.executed_shares, 0u);
  EXPECT_EQ(s.hidden_trade_shares, 70u);
  EXPECT_EQ(s.cross_shares, 1000u);
  EXPECT_EQ(s.closing_cross_price, 50'000u);
  EXPECT_EQ(s.matched_volume(), 1070u);
  EXPECT_EQ(at(f.b().book(1).asks().best()).shares, 60u);
}

TEST(BookBuilder, FlagsDataQualityProblems) {
  Feed f;
  f.list(1, "TEST");
  f.add(1, 'B', 100, 10'000);

  OrderDelete d;
  d.order_reference_number = 999;  // never added
  f.send(d);
  f.add(1, 'B', 100, 10'000);  // duplicate reference
  OrderExecuted e;
  e.order_reference_number = 1;
  e.executed_shares = 150;  // more than the 100 remaining
  f.send(e);
  f.add(2, 'B', 100, 10'000, "TEST", 7);  // locate 7 has no directory message
  f.add(3, 'B', 100, 10'000, "OTHER");    // symbol does not match locate 1
  f.rewind_clock(100);
  f.add(4, 'S', 100, 20'000);  // timestamp goes backwards

  const auto& c = f.b().checks();
  EXPECT_EQ(c.unknown_order_reference, 1u);
  EXPECT_EQ(c.duplicate_order_reference, 1u);
  EXPECT_EQ(c.over_execution, 1u);
  EXPECT_EQ(c.locate_without_directory, 1u);
  EXPECT_EQ(c.locate_symbol_mismatch, 1u);
  EXPECT_EQ(c.timestamp_regressions, 1u);
  EXPECT_EQ(c.book_inconsistency, 0u);
}

TEST(BookBuilder, CountsBooksBecomingLockedOrCrossed) {
  Feed f;
  f.list(1, "TEST");
  f.add(1, 'B', 100, 10'000);
  f.add(2, 'S', 100, 10'000);  // locked
  f.add(3, 'B', 100, 10'100);  // crossed
  OrderDelete d;
  d.order_reference_number = 3;
  f.send(d);  // back to locked
  d.order_reference_number = 2;
  f.send(d);                  // clear
  f.add(4, 'S', 100, 9'900);  // crossed again
  EXPECT_EQ(f.b().checks().locked_book_events, 2u);
  EXPECT_EQ(f.b().checks().crossed_book_events, 2u);
}

TEST(BookBuilder, ChecksSystemEventOrder) {
  Feed f;
  for (char code : {'O', 'S', 'Q', 'M', 'E', 'C'}) {
    SystemEvent s;
    s.event_code = code;
    f.send(s, 0);
  }
  EXPECT_EQ(f.b().system_events(), "OSQMEC");
  EXPECT_EQ(f.b().checks().system_event_out_of_order, 0u);

  Feed g;
  for (char code : {'O', 'Q'}) {  // start of system hours skipped
    SystemEvent s;
    s.event_code = code;
    g.send(s, 0);
  }
  EXPECT_EQ(g.b().checks().system_event_out_of_order, 1u);
}

// End to end: a synthetic day's rebuilt books must match the generator's own
// model of every live order exactly, with zero integrity errors.
TEST(BookBuilder, RebuildsSyntheticDayExactly) {
  SyntheticConfig config;
  config.symbols = 25;
#if defined(LAB_SANITIZED)
  config.book_messages = 60'000;
#else
  config.book_messages = 400'000;
#endif
  SyntheticDay day{config};
  auto builder = std::make_unique<BookBuilder>(1u << 16);
  day.generate([&](std::span<const std::byte> m) { builder->apply(m); });

  const auto& c = builder->checks();
  EXPECT_EQ(c.error_count(), 0u);
  EXPECT_EQ(c.crossed_book_events, 0u);
  EXPECT_EQ(builder->system_events(), "OSQMEC");
  EXPECT_EQ(builder->messages(), day.messages_written());
  EXPECT_EQ(builder->orders().size(), day.live_orders().size());

  // Aggregate the model into levels: (locate, side) -> price -> {shares, orders}.
  std::map<std::pair<std::uint16_t, char>,
           std::map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>>>
      expected;
  for (const auto& [ref, o] : day.live_orders()) {
    auto& lvl = expected[{o.locate, o.side}][o.price];
    lvl.first += o.shares;
    lvl.second += 1;
  }

  for (std::uint16_t l = 1; l <= config.symbols; ++l) {
    for (char side : {'B', 'S'}) {
      const auto& book_side = builder->book(l).side(side == 'B' ? Side::kBuy : Side::kSell);
      const auto& want = expected[{l, side}];
      ASSERT_EQ(book_side.depth(), want.size()) << "locate " << l << " side " << side;
      // Walk from best to worst and compare with the model in the same order.
      std::vector<std::pair<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>>> ordered(
          want.begin(), want.end());
      if (side == 'B') std::reverse(ordered.begin(), ordered.end());  // highest bid first
      for (std::size_t d = 0; d < ordered.size(); ++d) {
        const auto* lvl = book_side.level(d);
        ASSERT_NE(lvl, nullptr);
        EXPECT_EQ(at(lvl).price, ordered[d].first);
        EXPECT_EQ(at(lvl).shares, ordered[d].second.first);
        EXPECT_EQ(at(lvl).orders, ordered[d].second.second);
      }
    }
    const auto& stats = builder->instrument(l).stats;
    const auto& totals = day.totals(l);
    EXPECT_EQ(stats.executed_shares, totals.executed_shares) << "locate " << l;
    EXPECT_EQ(stats.hidden_trade_shares, totals.hidden_trade_shares) << "locate " << l;
    EXPECT_EQ(stats.cross_shares, totals.cross_shares) << "locate " << l;
  }
}

TEST(BookBuilder, SyntheticScenarioDayHasNoErrors) {
  SyntheticConfig config;
  config.symbols = 25;
  config.book_messages = 60'000;
  config.scenarios = true;
  SyntheticDay day{config};
  auto builder = std::make_unique<BookBuilder>(1u << 16);
  day.generate([&](std::span<const std::byte> m) { builder->apply(m); });

  const auto& c = builder->checks();
  EXPECT_EQ(c.error_count(), 0u);
  EXPECT_EQ(builder->system_events(), "OSQMEC");
  EXPECT_EQ(builder->orders().size(), day.live_orders().size());
  // Opening and closing crosses for 25 symbols, the IPO, the news halt and
  // the LULD reopening: one imbalance message and one Q each.
  EXPECT_EQ(c.crosses, 53u);
  EXPECT_EQ(c.noii, 53u);
  EXPECT_EQ(c.cross_overfill, 0u);
  EXPECT_EQ(builder->mwcb_breached(), '1');
  EXPECT_EQ(builder->mwcb_levels()[0], 465'000'000'000u);

  const auto& news = builder->instrument(1);
  EXPECT_EQ(news.imbalance.cross_type, 'C');  // the close came after the halt cross
  const auto& luld = builder->instrument(2);
  EXPECT_NE(luld.collar.reference, 0u);
  EXPECT_LT(luld.collar.lower, luld.collar.reference);
  EXPECT_GT(luld.collar.upper, luld.collar.reference);
  EXPECT_EQ(builder->instrument(3).reg_sho_action, '1');
  EXPECT_EQ(builder->instrument(4).operational_halt, 'T');
  const auto& ipo = builder->instrument(26);
  EXPECT_TRUE(ipo.listed);
  EXPECT_EQ(ipo.ipo_qualifier, 'A');
  EXPECT_EQ(ipo.trading_state, 'T');
  EXPECT_EQ(ipo.stats.cross_shares, day.totals(26).cross_shares);
  std::uint64_t broken = 0;
  for (std::uint16_t l = 1; l <= config.symbols + 1; ++l) {
    const auto& inst = builder->instrument(l);
    EXPECT_EQ(inst.trading_state, 'T') << "locate " << l;
    EXPECT_EQ(inst.stats.cross_shares, day.totals(l).cross_shares) << "locate " << l;
    broken += inst.stats.broken_trades;
  }
  EXPECT_EQ(broken, 1u);
}

}  // namespace

TEST(BookBuilder, DigestMatchesOnlyForTheSameBooks) {
  Feed a;
  Feed b;
  for (Feed* f : {&a, &b}) {
    f->list(1, "TEST");
    f->add(1, 'B', 100, 1'000'000);
    f->add(2, 'S', 200, 1'010'000);
  }
  EXPECT_EQ(a.b().digest(), b.b().digest());

  b.add(3, 'B', 100, 1'000'000);  // one more order at an existing level
  EXPECT_NE(a.b().digest(), b.b().digest());
  a.add(3, 'B', 100, 1'000'000);
  EXPECT_EQ(a.b().digest(), b.b().digest());

  Feed empty;
  EXPECT_NE(empty.b().digest(), a.b().digest());
}

TEST(Report, FormatsPricesWithFourDecimals) {
  EXPECT_EQ(lab::book::format_price(160'600), "16.0600");
  EXPECT_EQ(lab::book::format_price(1'000'001), "100.0001");
  EXPECT_EQ(lab::book::format_price(2'345'000), "234.5000");
  EXPECT_EQ(lab::book::format_price(0), "-");
}
