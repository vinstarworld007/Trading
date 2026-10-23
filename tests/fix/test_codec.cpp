#include <gtest/gtest.h>

#include <array>
#include <string>

#include "lab/fix/codec.hpp"

namespace {

namespace fix = lab::fix;

// '|' for SOH, for readable expectations.
std::string bars(std::string_view s) {
  std::string out{s};
  for (auto& c : out) {
    if (c == fix::kSoh) c = '|';
  }
  return out;
}
std::string soh(std::string_view s) {
  std::string out{s};
  for (auto& c : out) {
    if (c == '|') c = fix::kSoh;
  }
  return out;
}

TEST(FixWriter, MatchesThePublishedExample) {
  // The Logon example from the FIX article: BodyLength 65, CheckSum 062.
  std::array<char, 256> buf;
  fix::Writer w{buf.data(), buf.size()};
  w.begin("FIX.4.2", 'A');
  w.add(fix::tag::kSenderCompID, "SERVER");
  w.add(fix::tag::kTargetCompID, "CLIENT");
  w.add_uint(fix::tag::kMsgSeqNum, 177);
  w.add(fix::tag::kSendingTime, "20090107-18:15:16");
  w.add(fix::tag::kEncryptMethod, '0');
  w.add_uint(fix::tag::kHeartBtInt, 30);
  EXPECT_EQ(bars(w.finish()),
            "8=FIX.4.2|9=65|35=A|49=SERVER|56=CLIENT|34=177|52=20090107-18:15:16|98=0|108=30|"
            "10=062|");
}

TEST(FixWriter, ReportsOverflow) {
  std::array<char, 48> buf;
  fix::Writer w{buf.data(), buf.size()};
  w.begin(fix::kBeginString, '0');
  w.add(fix::tag::kText, "a text that cannot fit in what is left of this buffer");
  EXPECT_FALSE(w.ok());
  EXPECT_TRUE(w.finish().empty());
}

TEST(FixValues, Prices) {
  EXPECT_EQ(fix::parse_price("100.25"), 1'002'500u);
  EXPECT_EQ(fix::parse_price("100"), 1'000'000u);
  EXPECT_EQ(fix::parse_price("0.0001"), 1u);
  EXPECT_EQ(fix::parse_price(".5"), 5'000u);
  EXPECT_EQ(fix::parse_price("12."), 120'000u);
  EXPECT_FALSE(fix::parse_price("1.00001"));
  EXPECT_FALSE(fix::parse_price("-1"));
  EXPECT_FALSE(fix::parse_price("1e3"));
  EXPECT_FALSE(fix::parse_price(""));
  EXPECT_FALSE(fix::parse_price("."));
  char out[32];
  EXPECT_EQ(std::string(out, fix::format_price(out, 1'002'500)), "100.25");
  EXPECT_EQ(std::string(out, fix::format_price(out, 1'000'000)), "100");
  EXPECT_EQ(std::string(out, fix::format_price(out, 1)), "0.0001");
  EXPECT_EQ(std::string(out, fix::format_price(out, 1'999'999'900)), "199999.99");
}

TEST(FixValues, Integers) {
  EXPECT_EQ(fix::parse_uint("0"), 0u);
  EXPECT_EQ(fix::parse_uint("18446744073709551"), 18446744073709551u);
  EXPECT_FALSE(fix::parse_uint("12a"));
  EXPECT_FALSE(fix::parse_uint(""));
  EXPECT_FALSE(fix::parse_uint("+1"));
}

TEST(FixValues, Timestamps) {
  char out[fix::kTimestampSize];
  fix::format_utc_timestamp(out, 0);
  EXPECT_EQ(std::string(out, sizeof out), "19700101-00:00:00.000");
  // 2009-01-07 18:15:16.789 UTC
  fix::format_utc_timestamp(out, 1'231'352'116'789'000'000ULL);
  EXPECT_EQ(std::string(out, sizeof out), "20090107-18:15:16.789");
  // 2024-02-29 23:59:59.999 UTC: a leap day
  fix::format_utc_timestamp(out, 1'709'251'199'999'000'000ULL);
  EXPECT_EQ(std::string(out, sizeof out), "20240229-23:59:59.999");
  EXPECT_TRUE(fix::valid_utc_timestamp("20090107-18:15:16"));
  EXPECT_TRUE(fix::valid_utc_timestamp("20090107-18:15:16.789"));
  EXPECT_FALSE(fix::valid_utc_timestamp("20090107 18:15:16"));
  EXPECT_FALSE(fix::valid_utc_timestamp("2009010-18:15:16.7"));
}

std::string make_message(char type) {
  std::array<char, 256> buf;
  fix::Writer w{buf.data(), buf.size()};
  w.begin(fix::kBeginString, type);
  w.add(fix::tag::kSenderCompID, "A");
  w.add(fix::tag::kTargetCompID, "B");
  w.add_uint(fix::tag::kMsgSeqNum, 7);
  w.add(fix::tag::kSendingTime, "20240229-23:59:59.999");
  w.add(fix::tag::kClOrdID, "ord-1");
  w.add_price(fix::tag::kPrice, 1'002'500);
  return std::string{w.finish()};
}

TEST(FixMessage, ParsesFields) {
  const std::string wire = make_message('D');
  fix::Message m;
  ASSERT_TRUE(m.parse(wire));
  EXPECT_EQ(m.type(), 'D');
  EXPECT_EQ(m.get(fix::tag::kClOrdID), "ord-1");
  EXPECT_EQ(m.get_uint(fix::tag::kMsgSeqNum), 7u);
  EXPECT_EQ(fix::parse_price(*m.get(fix::tag::kPrice)), 1'002'500u);
  EXPECT_FALSE(m.get(fix::tag::kSymbol));
  EXPECT_EQ(m.fields().front().tag, fix::tag::kBeginString);
  EXPECT_EQ(m.fields().back().tag, fix::tag::kCheckSum);
}

TEST(FixMessage, RejectsMalformed) {
  fix::Message m;
  EXPECT_FALSE(m.parse(soh("8=FIX.4.4|9=5|35=0|x=1|10=000|")));
  EXPECT_FALSE(m.parse(soh("8=FIX.4.4|9=5|35=0|11|10=000|")));
  EXPECT_FALSE(m.parse(soh("8=FIX.4.4|9=5|35=0|11=a")));
  EXPECT_FALSE(m.parse(soh("8=FIX.4.4|9=5|34=1|35=0|10=000|")));
}

TEST(FixStreamParser, SplitsAStreamInAnyChunking) {
  const std::string a = make_message('D');
  const std::string b = make_message('8');
  const std::string stream = a + b + a;
  for (std::size_t chunk = 1; chunk <= stream.size(); chunk += 7) {
    fix::StreamParser in;
    std::vector<std::string> got;
    for (std::size_t i = 0; i < stream.size(); i += chunk) {
      ASSERT_TRUE(in.feed(std::string_view{stream}.substr(i, chunk)));
      while (auto m = in.next()) got.emplace_back(*m);
    }
    ASSERT_EQ(got.size(), 3u) << "chunk " << chunk;
    EXPECT_EQ(got[0], a);
    EXPECT_EQ(got[1], b);
    EXPECT_EQ(got[2], a);
    EXPECT_EQ(in.garbled(), 0u);
  }
}

TEST(FixStreamParser, DropsGarbledMessagesAndResynchronizes) {
  const std::string good = make_message('D');
  std::string bad_sum = good;
  bad_sum[bad_sum.size() - 2] = bad_sum[bad_sum.size() - 2] == '0' ? '1' : '0';
  std::string bad_len = good;
  bad_len.replace(bad_len.find("9=") + 2, 2, "99");
  fix::StreamParser in;
  ASSERT_TRUE(in.feed("noise" + bad_sum + good + bad_len + good));
  std::vector<std::string> got;
  while (auto m = in.next()) got.emplace_back(*m);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], good);
  EXPECT_EQ(got[1], good);
  EXPECT_GE(in.garbled(), 3u);
}

}  // namespace
