#include <gtest/gtest.h>

#include "lab/vendor/bpipe.hpp"
#include "lab/vendor/json.hpp"
#include "lab/vendor/omm.hpp"

namespace {

using namespace lab::vendor;

Record sample() {
  Record r;
  r.locate = 7;
  r.symbol = lab::itch::make_symbol("ACME");
  r.listed = true;
  r.bid = 1'000'000;
  r.bid_size = 300;
  r.ask = 1'000'500;
  r.ask_size = 200;
  r.last = 1'000'500;
  r.last_size = 50;
  r.volume = 50;
  r.open = r.high = r.low = 1'000'500;
  r.trades = 1;
  r.quote_ns = 34'200'000'000'000;
  r.trade_ns = 34'200'000'000'500;
  r.exch_ns = r.trade_ns;
  r.updates = 4;
  return r;
}

TEST(Omm, FieldIdsComeFromRdmFieldDictionary) {
  auto fid = [](std::string_view n) { return omm::kFields[omm::field_index(n)].fid; };
  EXPECT_EQ(fid("TRDPRC_1"), 6);
  EXPECT_EQ(fid("BID"), 22);
  EXPECT_EQ(fid("ASK"), 25);
  EXPECT_EQ(fid("BIDSIZE"), 30);
  EXPECT_EQ(fid("ASKSIZE"), 31);
  EXPECT_EQ(fid("ACVOL_1"), 32);
  EXPECT_EQ(fid("TRDVOL_1"), 178);
  EXPECT_EQ(fid("TRD_STATUS"), 6614);
  EXPECT_EQ(fid("QUOTIM_NS"), 14265);
  EXPECT_EQ(fid("SALTIM_NS"), 14266);
  EXPECT_EQ(omm::field_index("NOPE"), -1);
  EXPECT_EQ(omm::ric("ACME"), "ACME.O");
  EXPECT_EQ(omm::symbol_of("ACME.O"), "ACME");
}

TEST(Omm, RefreshIsValidJsonWithEveryField) {
  std::string out;
  omm::refresh(out, 3, "LAB_RT", sample(), ~0u, true, false);
  const auto v = json::parse(out);
  ASSERT_TRUE(v) << out;
  EXPECT_EQ(v->str("Type"), "Refresh");
  EXPECT_EQ(v->get("Key")->str("Name"), "ACME.O");
  EXPECT_EQ(v->get("State")->str("Stream"), "Open");
  EXPECT_EQ(v->get("Qos")->str("Rate"), "TickByTick");
  const json::Value* f = v->get("Fields");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->members.size(), omm::kFieldCount);
  EXPECT_EQ(f->get("BID")->text, "100");
  EXPECT_EQ(f->get("ASK")->text, "100.05");
  EXPECT_EQ(f->get("TRD_STATUS")->text, "1");
  EXPECT_EQ(f->str("QUOTIM_NS"), "09:30:00.000000000");
}

TEST(Omm, UpdateCarriesOnlyChangedFieldsInTheView) {
  std::string out;
  const std::uint32_t view = (1u << omm::field_index("BID")) | (1u << omm::field_index("ASK"));
  omm::update(out, 3, sample(), field::kBid | field::kBidSize, view);
  const auto v = json::parse(out);
  ASSERT_TRUE(v) << out;
  EXPECT_EQ(v->str("UpdateType"), "Quote");
  const json::Value* f = v->get("Fields");
  ASSERT_EQ(f->members.size(), 1u);
  EXPECT_EQ(f->members[0].first, "BID");
}

TEST(Omm, StatusNamesTheState) {
  std::string out;
  omm::status(out, 9, "Closed", "Suspect", "NotFound", "The record could not be found");
  const auto v = json::parse(out);
  ASSERT_TRUE(v);
  EXPECT_EQ(v->get("State")->str("Code"), "NotFound");
}

TEST(Bpipe, EventsRoundTrip) {
  std::string out;
  bpipe::Writer w{out};
  bpipe::market_data(w, 101, sample(), field::kAll, ~0u, true);
  bpipe::FrameParser p;
  // Feed in two pieces to exercise reassembly.
  const auto* bytes = reinterpret_cast<const std::byte*>(out.data());
  p.feed({bytes, 5});
  EXPECT_FALSE(p.next());
  p.feed({bytes + 5, out.size() - 5});
  const auto body = p.next();
  ASSERT_TRUE(body);
  const auto e = bpipe::decode_event(*body);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->type, bpipe::EventType::kSubscriptionData);
  EXPECT_EQ(e->cid, 101u);
  EXPECT_EQ(e->message_type, "MarketDataEvents");
  EXPECT_EQ(e->get("MKTDATA_EVENT_SUBTYPE")->text, "INITPAINT");
  EXPECT_EQ(e->get("BID")->price4(), 1'000'000);
  EXPECT_EQ(e->get("BID")->exponent, 0);  // 100, not 1000000e-4
  EXPECT_EQ(e->get("ASK")->price4(), 1'000'500);
  EXPECT_EQ(e->get("ASK")->value, 10005);
  EXPECT_EQ(e->get("ASK")->exponent, -2);
  EXPECT_EQ(e->get("VOLUME")->value, 50);
  EXPECT_EQ(e->get("TRADING_STATUS")->text, "TRADING");
  EXPECT_EQ(static_cast<std::uint64_t>(e->get("EVENT_TIME")->value), sample().exch_ns);
  EXPECT_FALSE(p.malformed());
}

TEST(Bpipe, UpdatesCarryOnlyWantedChangedFields) {
  std::string out;
  bpipe::Writer w{out};
  const std::uint32_t want = 1u << bpipe::field_index("ASK");
  bpipe::market_data(w, 5, sample(), field::kBid | field::kAsk, want, false);
  const auto e =
      bpipe::decode_event({reinterpret_cast<const std::byte*>(out.data()) + 4, out.size() - 4});
  ASSERT_TRUE(e);
  EXPECT_EQ(e->get("MKTDATA_EVENT_TYPE")->text, "QUOTE");
  EXPECT_NE(e->get("ASK"), nullptr);
  EXPECT_EQ(e->get("BID"), nullptr);
}

TEST(Bpipe, ParsesTopics) {
  auto t = bpipe::parse_topic("//blp/mktdata/ticker/AAPL US Equity?fields=BID,ASK&interval=1.5");
  EXPECT_TRUE(t.ok);
  EXPECT_EQ(t.symbol, "AAPL");
  EXPECT_EQ(t.security, "AAPL US Equity");
  ASSERT_EQ(t.fields.size(), 2u);
  EXPECT_EQ(t.fields[1], "ASK");
  EXPECT_DOUBLE_EQ(t.interval_s, 1.5);
  EXPECT_TRUE(bpipe::parse_topic("IBM US Equity").ok);
  EXPECT_FALSE(bpipe::parse_topic("IBM").ok);
  EXPECT_FALSE(bpipe::parse_topic("IBM US Comdty").ok);
}

TEST(Bpipe, RejectsOversizeFrames) {
  bpipe::FrameParser p;
  const std::byte huge[4] = {std::byte{0x7f}, std::byte{0}, std::byte{0}, std::byte{0}};
  p.feed(huge);
  EXPECT_FALSE(p.next());
  EXPECT_TRUE(p.malformed());
}

}  // namespace
