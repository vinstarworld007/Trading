#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "lab/soup/soup.hpp"

namespace {

using namespace lab::soup;

std::string chars(const std::byte* p, std::size_t n) {
  return {reinterpret_cast<const char*>(p), n};
}

TEST(Soup, LoginRequestLayout) {
  std::array<std::byte, kLoginRequestSize> b;
  ASSERT_EQ(write_login_request(b.data(), "FIRM01", "lab", "", 42), 49u);
  EXPECT_EQ(lab::load_be16(b.data()), 47);  // type byte plus 46 payload bytes
  EXPECT_EQ(static_cast<char>(b[2]), 'L');
  EXPECT_EQ(chars(b.data() + 3, 6), "FIRM01");                       // username, left-justified
  EXPECT_EQ(chars(b.data() + 9, 10), "lab       ");                  // password
  EXPECT_EQ(chars(b.data() + 19, 10), "          ");                 // session: blank is current
  EXPECT_EQ(chars(b.data() + 29, 20), std::string(18, ' ') + "42");  // right-justified

  const auto req = parse_login_request({b.data() + kHeaderSize, b.size() - kHeaderSize});
  ASSERT_TRUE(req);
  EXPECT_EQ(req->user, "FIRM01");
  EXPECT_EQ(req->password, "lab");
  EXPECT_TRUE(req->session.empty());
  EXPECT_EQ(req->next_sequence, 42u);
}

TEST(Soup, LoginAcceptedRoundTrip) {
  std::array<std::byte, kLoginAcceptedSize> b;
  ASSERT_EQ(write_login_accepted(b.data(), "SIMEX00001", 1234567), 33u);
  EXPECT_EQ(static_cast<char>(b[2]), 'A');
  EXPECT_EQ(chars(b.data() + 3, 10), "SIMEX00001");
  const auto a = parse_login_accepted({b.data() + kHeaderSize, b.size() - kHeaderSize});
  ASSERT_TRUE(a);
  EXPECT_EQ(a->session, "SIMEX00001");
  EXPECT_EQ(a->next_sequence, 1234567u);
}

TEST(Soup, RejectsNonNumericSequence) {
  std::array<std::byte, kLoginRequestSize> b;
  write_login_request(b.data(), "FIRM01", "lab", "", 1);
  std::memcpy(b.data() + 40, "x", 1);
  EXPECT_FALSE(parse_login_request({b.data() + kHeaderSize, b.size() - kHeaderSize}));
  EXPECT_FALSE(parse_login_request({b.data() + kHeaderSize, 10}));
}

TEST(Soup, StreamParserReassemblesSplitPackets) {
  std::vector<std::byte> wire(256);
  std::size_t n = 0;
  const std::array<std::byte, 3> msg{std::byte{1}, std::byte{2}, std::byte{3}};
  n += write_data(wire.data() + n, kSequencedData, msg);
  n += write_empty(wire.data() + n, kServerHeartbeat);
  n += write_data(wire.data() + n, kUnsequencedData, msg);

  StreamParser p{1024};
  std::vector<char> types;
  for (std::size_t i = 0; i < n; ++i) {  // one byte at a time
    ASSERT_TRUE(p.feed({wire.data() + i, 1}));
    while (auto pkt = p.next()) {
      types.push_back(pkt->type);
      if (pkt->type != kServerHeartbeat) {
        ASSERT_EQ(pkt->payload.size(), 3u);
        EXPECT_EQ(pkt->payload[2], std::byte{3});
      }
    }
  }
  EXPECT_EQ(types, (std::vector<char>{'S', 'H', 'U'}));
  EXPECT_EQ(p.buffered(), 0u);
  EXPECT_FALSE(p.malformed());
}

TEST(Soup, ZeroLengthIsMalformed) {
  StreamParser p{64};
  const std::array<std::byte, 2> zero{};
  ASSERT_TRUE(p.feed(zero));
  EXPECT_FALSE(p.next());
  EXPECT_TRUE(p.malformed());
}

}  // namespace
