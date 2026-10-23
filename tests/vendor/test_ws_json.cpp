#include <gtest/gtest.h>

#include "lab/vendor/json.hpp"
#include "lab/vendor/ws.hpp"

namespace {

using namespace lab::vendor;

std::string hex(std::span<const std::uint8_t> b) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (auto x : b) {
    s += d[x >> 4];
    s += d[x & 15];
  }
  return s;
}

TEST(WebSocket, Sha1MatchesRfc3174) {
  EXPECT_EQ(hex(ws::sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(hex(ws::sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(hex(ws::sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(WebSocket, AcceptKeyMatchesRfc6455Example) {
  EXPECT_EQ(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocket, HandshakeAgreesOnTheSubprotocol) {
  const auto hs = ws::client_handshake("127.0.0.1:17002", "/WebSocket", "tr_json2");
  const std::string reply = ws::server_handshake(hs.request, "tr_json2");
  ASSERT_FALSE(reply.empty());
  EXPECT_TRUE(reply.starts_with("HTTP/1.1 101"));
  EXPECT_EQ(ws::header(reply, "Sec-WebSocket-Accept"), hs.expected_accept);
  EXPECT_EQ(ws::header(reply, "sec-websocket-protocol"), "tr_json2");
  EXPECT_TRUE(ws::server_handshake(hs.request, "other").empty());
}

TEST(WebSocket, FramesRoundTripMaskedFragmentedAndLong) {
  std::string wire;
  ws::append_frame(wire, ws::kText, "hello", true);
  const std::string big(70'000, 'x');  // 64-bit length form
  ws::append_frame(wire, ws::kBinary, big, false);
  ws::append_frame(wire, ws::kPing, "p", true);
  ws::Decoder d;
  // Feed a byte at a time to exercise partial frames.
  std::vector<ws::Message> got;
  for (char c : wire) {
    d.feed(std::string_view{&c, 1});
    while (auto m = d.next()) got.push_back(*m);
  }
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0].opcode, ws::kText);
  EXPECT_EQ(got[0].payload, "hello");
  EXPECT_EQ(got[1].payload.size(), big.size());
  EXPECT_EQ(got[2].opcode, ws::kPing);
  EXPECT_FALSE(d.malformed());

  // A message in two fragments, a control frame between them.
  std::string frag;
  frag += static_cast<char>(0x01);  // text, FIN clear
  frag += static_cast<char>(3);
  frag += "abc";
  ws::append_frame(frag, ws::kPing, "", false);
  frag += static_cast<char>(0x80);  // continuation, FIN
  frag += static_cast<char>(2);
  frag += "de";
  ws::Decoder d2;
  d2.feed(frag);
  auto m1 = d2.next();
  ASSERT_TRUE(m1);
  EXPECT_EQ(m1->opcode, ws::kPing);
  auto m2 = d2.next();
  ASSERT_TRUE(m2);
  EXPECT_EQ(m2->opcode, ws::kText);
  EXPECT_EQ(m2->payload, "abcde");
}

TEST(Json, ParsesNestedValues) {
  const auto v = json::parse(R"([{"ID":2,"Key":{"Name":["A.O","B.O"]},"Streaming":false,)"
                             R"("Px":101.25,"S":"a\"bA","N":null}])");
  ASSERT_TRUE(v);
  ASSERT_TRUE(v->is_array());
  const json::Value& m = v->items.at(0);
  EXPECT_EQ(m.get("ID")->text, "2");
  EXPECT_EQ(m.get("Key")->get("Name")->items.size(), 2u);
  EXPECT_EQ(m.get("Key")->get("Name")->items[1].text, "B.O");
  EXPECT_EQ(m.get("Streaming")->type, json::Value::Type::kBool);
  EXPECT_FALSE(m.get("Streaming")->b);
  EXPECT_EQ(m.get("Px")->text, "101.25");  // number text kept exact
  EXPECT_EQ(m.str("S"), "a\"bA");
  EXPECT_TRUE(m.get("N")->is_null());
  EXPECT_FALSE(json::parse("{\"a\":1"));
  EXPECT_FALSE(json::parse("[1,]"));
}

TEST(Json, PricesAreExactDecimals) {
  auto p = [](std::int64_t v) {
    std::string s;
    json::price(s, v);
    return s;
  };
  EXPECT_EQ(p(1'000'000), "100");
  EXPECT_EQ(p(1'012'500), "101.25");
  EXPECT_EQ(p(1), "0.0001");
  EXPECT_EQ(p(-25'000), "-2.5");
  EXPECT_EQ(json::parse_price("101.25"), 1'012'500);
  EXPECT_EQ(json::parse_price("0.0001"), 1);
  EXPECT_EQ(json::parse_price("100"), 1'000'000);
  EXPECT_EQ(json::parse_price("-2.5"), -25'000);
  EXPECT_FALSE(json::parse_price("1.00001"));  // finer than Price(4)
  EXPECT_FALSE(json::parse_price("abc"));
}

TEST(Json, TimesAreNanosecondsSinceMidnight) {
  std::string s;
  json::time_ns(s, 34'200'000'000'123);
  EXPECT_EQ(s, "\"09:30:00.000000123\"");
  EXPECT_EQ(json::parse_time_ns("09:30:00.000000123"), 34'200'000'000'123u);
  EXPECT_EQ(json::parse_time_ns("09:30:00.5"), 34'200'500'000'000u);
}

}  // namespace
