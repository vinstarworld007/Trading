#include <gtest/gtest.h>

#include <array>

#include "lab/vendor/icf.hpp"

namespace {

using namespace lab::vendor;

TEST(Icf, MarketDataRoundTripsAndHasTheDocumentedLayout) {
  Record r;
  r.symbol = lab::itch::make_symbol("ACME");
  r.bid = 1'000'000;
  r.bid_size = 300;
  r.ask = 1'000'500;
  r.ask_size = 200;
  r.quote_ns = 34'200'000'000'123;
  icf::MarketData m;
  m.event = icf::kQuote;
  m.source = 1;
  m.request_id = 42;
  m.symbol = r.symbol;
  m.exch_ns = r.quote_ns;
  m.vendor_ns = r.quote_ns + 900;
  m.record_seq = 3;
  icf::fill(m, r, field::kQuote);
  ASSERT_EQ(m.count, 5u);  // bid, bid size, ask, ask size, quote time

  std::array<std::byte, icf::kMaxMessage> b{};
  const std::size_t n = icf::encode(m, b.data());
  EXPECT_EQ(n, icf::kHeaderSize + 5 * icf::kTokenSize);
  EXPECT_EQ(static_cast<char>(b[0]), 'M');
  EXPECT_EQ(static_cast<char>(b[1]), 'Q');
  EXPECT_EQ(lab::load_be16(b.data() + 2), 1);
  EXPECT_EQ(lab::load_be32(b.data() + 4), 42u);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(b.data() + 8), 8), "ACME    ");
  EXPECT_EQ(lab::load_be64(b.data() + 16), r.quote_ns);
  EXPECT_EQ(lab::load_be32(b.data() + 32), 3u);
  EXPECT_EQ(static_cast<std::uint8_t>(b[36]), 5);
  // First token: QUOTE.BID.PRICE, type P, value.
  EXPECT_EQ(lab::load_be16(b.data() + 37), icf::token::kBidPrice);
  EXPECT_EQ(static_cast<char>(b[39]), icf::kPrice);
  EXPECT_EQ(lab::load_be64(b.data() + 40), 1'000'000u);

  const auto d = icf::decode({b.data(), n});
  ASSERT_TRUE(d);
  EXPECT_EQ(d->request_id, 42u);
  EXPECT_EQ(d->vendor_ns, m.vendor_ns);
  ASSERT_EQ(d->count, 5u);
  for (std::size_t i = 0; i < d->count; ++i) {
    EXPECT_EQ(d->tokens[i].id, m.tokens[i].id);
    EXPECT_EQ(d->tokens[i].value, m.tokens[i].value);
  }
  EXPECT_FALSE(icf::decode({b.data(), n - 1}));
}

TEST(Icf, ResponseRoundTrips) {
  std::array<std::byte, 600> b{};
  const std::size_t n = icf::encode_response(b.data(), 7, false, "unknown SYMBOL_TICKER XYZ");
  const auto r = icf::decode_response({b.data(), n});
  ASSERT_TRUE(r);
  EXPECT_EQ(r->request_id, 7u);
  EXPECT_FALSE(r->ok);
  EXPECT_EQ(r->text, "unknown SYMBOL_TICKER XYZ");
}

TEST(Icf, ParsesCommandsWithParametersAndRegex) {
  const auto cs = icf::parse_commands(
      "SUBSCRIBE,SYMBOL_TICKER:AAPL,ENUM_SRC_ID:1 ; "
      "SUBSCRIBEWILDCARD,SYMBOL_TICKER:{^SYM00(0[1-3]|1,2)$},CONFLATION_INTERVAL:250;;");
  ASSERT_EQ(cs.size(), 2u);
  EXPECT_EQ(cs[0].verb, "SUBSCRIBE");
  EXPECT_EQ(cs[0].get("SYMBOL_TICKER"), "AAPL");
  EXPECT_EQ(cs[0].get("ENUM_SRC_ID"), "1");
  EXPECT_EQ(cs[0].get("MISSING"), "");
  EXPECT_EQ(cs[1].verb, "SUBSCRIBEWILDCARD");
  EXPECT_EQ(cs[1].get("SYMBOL_TICKER"), "^SYM00(0[1-3]|1,2)$");
  EXPECT_EQ(cs[1].get("CONFLATION_INTERVAL"), "250");
}

TEST(Icf, ScenarioTokens) {
  Record r;
  r.status = 'P';
  r.halt_reason = {'L', 'U', 'D', 'P'};
  r.reg_sho = '1';
  r.auction_type = 'H';
  r.indicative_price = 1'000'000;
  r.paired_shares = 500;
  r.imbalance_shares = 200;
  r.imbalance_side = 'S';
  r.band_lower = 950'000;
  r.band_upper = 1'050'000;
  icf::MarketData m;
  icf::fill(m, r, field::kAll);
  EXPECT_LE(m.count, icf::kMaxTokens);
  std::array<std::byte, icf::kMaxMessage> b{};
  const auto d = icf::decode({b.data(), icf::encode(m, b.data())});
  ASSERT_TRUE(d);
  int seen = 0;
  for (std::size_t i = 0; i < d->count; ++i) {
    const icf::Token& t = d->tokens[i];
    switch (t.id) {
      case icf::token::kHaltReason: {
        const auto a = icf::alpha_chars(t.value);
        EXPECT_EQ(t.type, icf::kAlpha);
        EXPECT_EQ(std::string_view(a.data(), 8), "LUDP    ");
        ++seen;
        break;
      }
      case icf::token::kRegSho:
        EXPECT_EQ(t.value, '1');
        ++seen;
        break;
      case icf::token::kAuctionType:
        EXPECT_EQ(t.value, 'H');
        ++seen;
        break;
      case icf::token::kIndicativePrice:
        EXPECT_EQ(t.value, 1'000'000);
        ++seen;
        break;
      case icf::token::kImbalanceSide:
        EXPECT_EQ(t.value, 'S');
        ++seen;
        break;
      case icf::token::kLuldLower:
        EXPECT_EQ(t.value, 950'000);
        ++seen;
        break;
      case icf::token::kLuldUpper:
        EXPECT_EQ(t.value, 1'050'000);
        ++seen;
        break;
      default:
        break;
    }
  }
  EXPECT_EQ(seen, 7);
  EXPECT_STREQ(icf::token_name(icf::token::kImbalanceShares), "AUCTION.IMBALANCE.SIZE");
}

}  // namespace
