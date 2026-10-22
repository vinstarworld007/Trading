#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "lab/core/endian.hpp"
#include "lab/ouch/codec.hpp"
#include "lab/ouch/messages.hpp"

namespace {

using namespace lab::ouch;

TEST(Ouch, SpecLengths) {
  EXPECT_EQ(EnterOrder::kSize, 47u);
  EXPECT_EQ(ReplaceOrder::kSize, 40u);
  EXPECT_EQ(CancelOrder::kSize, 11u);
  EXPECT_EQ(AccountQuery::kSize, 3u);
  EXPECT_EQ(SystemEvent::kSize, 10u);
  EXPECT_EQ(OrderAccepted::kSize, 64u);
  EXPECT_EQ(OrderReplaced::kSize, 68u);
  EXPECT_EQ(OrderCanceled::kSize, 20u);
  EXPECT_EQ(OrderExecuted::kSize, 36u);
  EXPECT_EQ(BrokenTrade::kSize, 38u);
  EXPECT_EQ(Rejected::kSize, 31u);
  EXPECT_EQ(AccountQueryResponse::kSize, 15u);
}

TEST(Ouch, EnterOrderWireLayout) {
  EnterOrder m;
  m.user_ref_num = 0x01020304;
  m.side = side::kSellShort;
  m.quantity = 500;
  m.symbol = make_alpha<8>("AAPL");
  m.price = 1'503'400;  // $150.34
  m.time_in_force = tif::kIoc;
  m.cl_ord_id = make_alpha<14>("abc");
  std::array<std::byte, EnterOrder::kSize> b{};
  ASSERT_EQ(encode(m, b.data()), 47u);
  EXPECT_EQ(static_cast<char>(b[0]), 'O');
  EXPECT_EQ(lab::load_be32(b.data() + 1), 0x01020304u);
  EXPECT_EQ(static_cast<char>(b[5]), 'T');
  EXPECT_EQ(lab::load_be32(b.data() + 6), 500u);
  EXPECT_EQ(std::memcmp(b.data() + 10, "AAPL    ", 8), 0);
  EXPECT_EQ(lab::load_be64(b.data() + 18), 1'503'400u);
  EXPECT_EQ(static_cast<char>(b[26]), '3');
  EXPECT_EQ(std::memcmp(b.data() + 31, "abc           ", 14), 0);
  EXPECT_EQ(lab::load_be16(b.data() + 45), 0u);
}

struct Collect : HandlerBase {
  using HandlerBase::on;
  std::vector<char> types;
  OrderExecuted exec;
  ReplaceOrder replace;
  void on(const OrderExecuted& m) {
    types.push_back('E');
    exec = m;
  }
  void on(const OrderAccepted&) { types.push_back('A'); }
  void on(const ReplaceOrder& m) {
    types.push_back('U');
    replace = m;
  }
  void on(const CancelOrder&) { types.push_back('X'); }
};

TEST(Ouch, RoundTripsThroughDispatch) {
  OrderExecuted e;
  e.timestamp = 34'200'000'000'123ULL;
  e.user_ref_num = 9;
  e.quantity = 100;
  e.price = 999'900;
  e.liquidity_flag = liquidity::kRemoved;
  e.match_number = 77;
  std::array<std::byte, 64> b{};
  encode(e, b.data());
  Collect c;
  ASSERT_EQ(dispatch_outbound(b.data(), OrderExecuted::kSize, c), DispatchResult::kOk);
  EXPECT_EQ(c.exec.timestamp, e.timestamp);
  EXPECT_EQ(c.exec.price, e.price);
  EXPECT_EQ(c.exec.match_number, 77u);
  EXPECT_EQ(c.exec.liquidity_flag, 'R');

  ReplaceOrder r;
  r.orig_user_ref_num = 5;
  r.user_ref_num = 6;
  r.quantity = 300;
  r.price = 10'000;
  encode(r, b.data());
  ASSERT_EQ(dispatch_inbound(b.data(), ReplaceOrder::kSize, c), DispatchResult::kOk);
  EXPECT_EQ(c.replace.orig_user_ref_num, 5u);
  EXPECT_EQ(c.replace.user_ref_num, 6u);
  EXPECT_EQ(c.replace.quantity, 300u);
  EXPECT_EQ(c.types, (std::vector<char>{'E', 'U'}));
}

TEST(Ouch, ChecksLengthAndAppendage) {
  CancelOrder x;
  x.user_ref_num = 1;
  std::array<std::byte, 32> b{};
  encode(x, b.data());
  Collect c;
  EXPECT_EQ(dispatch_inbound(b.data(), CancelOrder::kSize - 1, c), DispatchResult::kTooShort);
  x.appendage_length = 4;  // promises 4 more bytes
  encode(x, b.data());
  EXPECT_EQ(dispatch_inbound(b.data(), CancelOrder::kSize, c), DispatchResult::kTooShort);
  EXPECT_EQ(dispatch_inbound(b.data(), CancelOrder::kSize + 4, c), DispatchResult::kOk);
  b[0] = std::byte{'?'};
  EXPECT_EQ(dispatch_inbound(b.data(), CancelOrder::kSize, c), DispatchResult::kUnknownType);
  EXPECT_EQ(c.types, (std::vector<char>{'X'}));
}

TEST(Ouch, AlphaHelpers) {
  const auto s = make_alpha<8>("SPY");
  EXPECT_EQ(std::string(s.data(), 8), "SPY     ");
  EXPECT_EQ(trimmed(s), "SPY");
  EXPECT_EQ(trimmed(make_alpha<8>("TOOLONGSYMBOL")), "TOOLONGS");
}

}  // namespace
