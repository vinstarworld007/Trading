#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "lab/itch/codec.hpp"

namespace {

using namespace lab::itch;

// Builds raw bytes field by field from the spec tables, independently of the
// codec's own field lists.
struct Bytes {
  std::vector<std::byte> b;
  explicit Bytes(std::size_t n) : b(n) {}
  Bytes& u8(std::size_t off, char v) {
    b[off] = static_cast<std::byte>(v);
    return *this;
  }
  Bytes& be(std::size_t off, std::size_t len, std::uint64_t v) {
    for (std::size_t i = 0; i < len; ++i) {
      b[off + len - 1 - i] = static_cast<std::byte>(v & 0xff);
      v >>= 8;
    }
    return *this;
  }
  Bytes& text(std::size_t off, const char* s, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) b[off + i] = static_cast<std::byte>(s[i]);
    return *this;
  }
  Bytes& header(char type, std::uint16_t locate, std::uint16_t tracking, std::uint64_t ts) {
    return u8(0, type).be(1, 2, locate).be(3, 2, tracking).be(5, 6, ts);
  }
};

constexpr std::uint64_t kTs = 34'200'000'000'123ULL;  // 09:30:00.000000123

TEST(ItchCodec, DecodesAddOrderFromSpecLayout) {
  Bytes raw{36};
  raw.header('A', 13, 7, kTs)
      .be(11, 8, 0x1122334455667788ULL)
      .u8(19, 'B')
      .be(20, 4, 300)
      .text(24, "AAPL    ", 8)
      .be(32, 4, 1'577'500);  // $157.75
  AddOrder m;
  decode(raw.b.data(), m);
  EXPECT_EQ(m.header.stock_locate, 13);
  EXPECT_EQ(m.header.tracking_number, 7);
  EXPECT_EQ(m.header.timestamp, kTs);
  EXPECT_EQ(m.order_reference_number, 0x1122334455667788ULL);
  EXPECT_EQ(m.buy_sell_indicator, 'B');
  EXPECT_EQ(m.shares, 300u);
  EXPECT_EQ(trimmed(m.stock), "AAPL");
  EXPECT_EQ(m.price, 1'577'500u);
}

TEST(ItchCodec, DecodesOrderReplaceFromSpecLayout) {
  Bytes raw{35};
  raw.header('U', 2, 0, kTs).be(11, 8, 1001).be(19, 8, 2002).be(27, 4, 500).be(31, 4, 99'900);
  OrderReplace m;
  decode(raw.b.data(), m);
  EXPECT_EQ(m.original_order_reference_number, 1001u);
  EXPECT_EQ(m.new_order_reference_number, 2002u);
  EXPECT_EQ(m.shares, 500u);
  EXPECT_EQ(m.price, 99'900u);
}

TEST(ItchCodec, DecodesCrossTradeFromSpecLayout) {
  Bytes raw{40};
  raw.header('Q', 9, 0, kTs)
      .be(11, 8, 1'234'567)
      .text(19, "MSFT    ", 8)
      .be(27, 4, 1'050'000)
      .be(31, 8, 77)
      .u8(39, 'C');
  CrossTrade m;
  decode(raw.b.data(), m);
  EXPECT_EQ(m.shares, 1'234'567u);
  EXPECT_EQ(trimmed(m.stock), "MSFT");
  EXPECT_EQ(m.cross_price, 1'050'000u);
  EXPECT_EQ(m.match_number, 77u);
  EXPECT_EQ(m.cross_type, 'C');
}

TEST(ItchCodec, DecodesNoiiFromSpecLayout) {
  Bytes raw{50};
  raw.header('I', 4, 0, kTs)
      .be(11, 8, 1000)
      .be(19, 8, 250)
      .u8(27, 'S')
      .text(28, "QQQ     ", 8)
      .be(36, 4, 3'000'000)
      .be(40, 4, 3'010'000)
      .be(44, 4, 3'005'000)
      .u8(48, 'O')
      .u8(49, 'L');
  Noii m;
  decode(raw.b.data(), m);
  EXPECT_EQ(m.paired_shares, 1000u);
  EXPECT_EQ(m.imbalance_shares, 250u);
  EXPECT_EQ(m.imbalance_direction, 'S');
  EXPECT_EQ(trimmed(m.stock), "QQQ");
  EXPECT_EQ(m.far_price, 3'000'000u);
  EXPECT_EQ(m.near_price, 3'010'000u);
  EXPECT_EQ(m.current_reference_price, 3'005'000u);
  EXPECT_EQ(m.cross_type, 'O');
  EXPECT_EQ(m.price_variation_indicator, 'L');
}

TEST(ItchCodec, DecodesOrderExecutedWithPriceFromSpecLayout) {
  Bytes raw{36};
  raw.header('C', 1, 0, kTs).be(11, 8, 55).be(19, 4, 40).be(23, 8, 9001).u8(31, 'N').be(32, 4, 123);
  OrderExecutedWithPrice m;
  decode(raw.b.data(), m);
  EXPECT_EQ(m.order_reference_number, 55u);
  EXPECT_EQ(m.executed_shares, 40u);
  EXPECT_EQ(m.match_number, 9001u);
  EXPECT_EQ(m.printable, 'N');
  EXPECT_EQ(m.execution_price, 123u);
}

template <class M>
void expect_round_trip(const M& in) {
  std::array<std::byte, kMaxMessageSize> buf{};
  encode(in, buf.data());
  EXPECT_EQ(static_cast<char>(buf[0]), M::kType);
  M out;
  decode(buf.data(), out);
  std::array<std::byte, kMaxMessageSize> again{};
  encode(out, again.data());
  EXPECT_EQ(buf, again) << "type " << M::kType;
}

TEST(ItchCodec, EveryMessageTypeRoundTrips) {
  Header h{42, 3, kTs};
  SystemEvent s;
  s.header = h;
  s.event_code = 'Q';
  expect_round_trip(s);

  StockDirectory r;
  r.header = h;
  r.stock = make_symbol("ZVZZT");
  r.round_lot_size = 100;
  r.etp_leverage_factor = 3;
  r.issue_sub_type = {'Z', 'A'};
  expect_round_trip(r);

  StockTradingAction ha;
  ha.header = h;
  ha.stock = make_symbol("ZVZZT");
  ha.trading_state = 'H';
  ha.reason = {'L', 'U', 'D', 'P'};
  expect_round_trip(ha);

  RegShoRestriction y;
  y.header = h;
  y.reg_sho_action = '1';
  expect_round_trip(y);

  MarketParticipantPosition l;
  l.header = h;
  l.mpid = {'G', 'S', 'C', 'O'};
  expect_round_trip(l);

  MwcbDeclineLevel v;
  v.header = h;
  v.level1 = 0x0102030405060708ULL;
  v.level2 = 2;
  v.level3 = 3;
  expect_round_trip(v);

  MwcbStatus w;
  w.header = h;
  w.breached_level = '2';
  expect_round_trip(w);

  IpoQuotingPeriod k;
  k.header = h;
  k.ipo_quotation_release_time = 36'000;
  k.ipo_price = 250'000;
  expect_round_trip(k);

  LuldAuctionCollar j;
  j.header = h;
  j.reference_price = 100;
  j.upper_collar_price = 110;
  j.lower_collar_price = 90;
  j.collar_extension = 2;
  expect_round_trip(j);

  OperationalHalt oh;
  oh.header = h;
  oh.market_code = 'Q';
  oh.operational_halt_action = 'H';
  expect_round_trip(oh);

  AddOrder a;
  a.header = h;
  a.order_reference_number = 77;
  a.shares = 100;
  a.price = 1;
  expect_round_trip(a);

  AddOrderMpid f;
  f.header = h;
  f.order_reference_number = 78;
  f.attribution = {'X', 'Y', 'Z', 'W'};
  expect_round_trip(f);

  OrderExecuted e;
  e.header = h;
  e.order_reference_number = 1;
  e.executed_shares = 2;
  e.match_number = 3;
  expect_round_trip(e);

  OrderExecutedWithPrice c;
  c.header = h;
  c.execution_price = 99;
  expect_round_trip(c);

  OrderCancel x;
  x.header = h;
  x.cancelled_shares = 5;
  expect_round_trip(x);

  OrderDelete d;
  d.header = h;
  d.order_reference_number = 9;
  expect_round_trip(d);

  OrderReplace u;
  u.header = h;
  u.original_order_reference_number = 1;
  u.new_order_reference_number = 2;
  expect_round_trip(u);

  Trade p;
  p.header = h;
  p.match_number = 11;
  expect_round_trip(p);

  CrossTrade q;
  q.header = h;
  q.shares = 1ULL << 40;
  expect_round_trip(q);

  BrokenTrade b;
  b.header = h;
  b.match_number = 12;
  expect_round_trip(b);

  Noii i;
  i.header = h;
  i.imbalance_shares = 1;
  expect_round_trip(i);

  Rpii n;
  n.header = h;
  n.interest_flag = 'B';
  expect_round_trip(n);

  DirectListingCapitalRaise o;
  o.header = h;
  o.near_execution_time = 1ULL << 33;
  expect_round_trip(o);
}

struct Recorder : HandlerBase {
  using HandlerBase::on;
  int adds = 0;
  int deletes = 0;
  void on(const AddOrder&) { ++adds; }
  void on(const OrderDelete&) { ++deletes; }
};

TEST(ItchCodec, DispatchCallsMatchingHandlerAndIgnoresOthers) {
  std::array<std::byte, kMaxMessageSize> buf{};
  Recorder r;
  AddOrder a;
  encode(a, buf.data());
  EXPECT_EQ(dispatch(buf.data(), AddOrder::kSize, r), DispatchResult::kOk);
  SystemEvent s;
  encode(s, buf.data());
  EXPECT_EQ(dispatch(buf.data(), SystemEvent::kSize, r), DispatchResult::kOk);
  EXPECT_EQ(r.adds, 1);
  EXPECT_EQ(r.deletes, 0);
}

TEST(ItchCodec, DispatchRejectsShortAndUnknownButAcceptsLonger) {
  std::array<std::byte, 64> buf{};
  Recorder r;
  OrderDelete d;
  encode(d, buf.data());
  EXPECT_EQ(dispatch(buf.data(), OrderDelete::kSize - 1, r), DispatchResult::kTooShort);
  EXPECT_EQ(dispatch(buf.data(), OrderDelete::kSize + 4, r), DispatchResult::kOk)
      << "spec allows fields appended in future versions";
  EXPECT_EQ(r.deletes, 1);
  buf[0] = static_cast<std::byte>('z');
  EXPECT_EQ(dispatch(buf.data(), 20, r), DispatchResult::kUnknownType);
  EXPECT_EQ(dispatch(buf.data(), 0, r), DispatchResult::kTooShort);
}

TEST(ItchCodec, SymbolHelpers) {
  const Symbol s = make_symbol("BRK.A");
  EXPECT_EQ(std::string(s.data(), 8), "BRK.A   ");
  EXPECT_EQ(trimmed(s), "BRK.A");
  EXPECT_EQ(trimmed(make_symbol("TOOLONGNAME")), "TOOLONGN");
}

}  // namespace
