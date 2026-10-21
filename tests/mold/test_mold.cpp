#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <vector>

#include "lab/mold/mold.hpp"

namespace {

using namespace lab::mold;

std::vector<std::byte> bytes(std::initializer_list<int> v) {
  std::vector<std::byte> out;
  for (int b : v) out.push_back(static_cast<std::byte>(b));
  return out;
}

TEST(Mold, SessionIsSpacePaddedAndTrimmed) {
  const Session s = make_session("ABC");
  EXPECT_EQ(std::string(s.data(), s.size()), "ABC       ");
  EXPECT_EQ(trimmed(s), "ABC");
  EXPECT_EQ(trimmed(make_session("0123456789XYZ")), "0123456789");  // cut to 10
}

// Field offsets from docs/specs/moldudp64-layout.md: session 0:10,
// sequence 10:8, count 18:2, all big-endian.
TEST(Mold, HeaderMatchesTheSpecLayout) {
  PacketBuilder<64> b;
  b.begin(make_session("SESS01"), 0x0102030405060708ULL);
  ASSERT_TRUE(b.add(bytes({0xAA, 0xBB, 0xCC})));
  const auto pkt = b.finish();
  ASSERT_EQ(pkt.size(), 20u + 2 + 3);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(pkt.data()), 10), "SESS01    ");
  for (int i = 0; i < 8; ++i) EXPECT_EQ(pkt[10 + i], std::byte(i + 1));
  EXPECT_EQ(pkt[18], std::byte{0});
  EXPECT_EQ(pkt[19], std::byte{1});
  EXPECT_EQ(pkt[20], std::byte{0});  // block length 3, big-endian
  EXPECT_EQ(pkt[21], std::byte{3});
  EXPECT_EQ(pkt[22], std::byte{0xAA});
  EXPECT_EQ(pkt[24], std::byte{0xCC});
}

TEST(Mold, RoundTripsMessagesWithConsecutiveSequenceNumbers) {
  PacketBuilder<> b;
  b.begin(make_session("S"), 41);
  ASSERT_TRUE(b.add(bytes({1})));
  ASSERT_TRUE(b.add(bytes({})));  // zero-length messages are legal
  ASSERT_TRUE(b.add(bytes({2, 3})));
  EXPECT_EQ(b.next_sequence(), 44u);
  const auto pkt = b.finish();

  Packet p;
  ASSERT_TRUE(parse(pkt.data(), pkt.size(), p));
  EXPECT_EQ(p.header.sequence, 41u);
  EXPECT_EQ(p.header.count, 3u);
  std::vector<std::pair<std::uint64_t, std::size_t>> seen;
  EXPECT_TRUE(p.for_each(
      [&](std::uint64_t seq, std::span<const std::byte> m) { seen.emplace_back(seq, m.size()); }));
  const std::vector<std::pair<std::uint64_t, std::size_t>> want{{41, 1}, {42, 0}, {43, 2}};
  EXPECT_EQ(seen, want);
}

TEST(Mold, BuilderStopsAtThePayloadLimit) {
  PacketBuilder<> b{100};  // 20 header + 80 for blocks
  b.begin(make_session("S"), 1);
  const std::vector<std::byte> msg(18);  // 20 bytes with its length prefix
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(b.add(msg)) << i;
  EXPECT_FALSE(b.add(msg));
  EXPECT_EQ(b.count(), 4u);
  EXPECT_EQ(b.size(), 100u);
  EXPECT_FALSE(b.add(bytes({})));  // even an empty block needs 2 bytes
}

TEST(Mold, HeartbeatAndEndOfSession) {
  PacketBuilder<> b;
  b.heartbeat(make_session("S"), 7);
  Packet p;
  auto pkt = b.finish();
  ASSERT_TRUE(parse(pkt.data(), pkt.size(), p));
  EXPECT_TRUE(p.header.heartbeat());
  EXPECT_FALSE(p.header.end_of_session());
  EXPECT_EQ(p.header.sequence, 7u);
  EXPECT_EQ(pkt.size(), kHeaderSize);

  b.end_of_session(make_session("S"), 9);
  pkt = b.finish();
  ASSERT_TRUE(parse(pkt.data(), pkt.size(), p));
  EXPECT_TRUE(p.header.end_of_session());
  EXPECT_EQ(p.header.count, 0xFFFFu);
  EXPECT_EQ(p.header.messages(), 0u);
  EXPECT_EQ(p.header.sequence, 9u);
  int calls = 0;
  EXPECT_TRUE(p.for_each([&](auto, auto) { ++calls; }));
  EXPECT_EQ(calls, 0);
}

TEST(Mold, DetectsTruncation) {
  PacketBuilder<> b;
  b.begin(make_session("S"), 1);
  ASSERT_TRUE(b.add(bytes({1, 2, 3})));
  ASSERT_TRUE(b.add(bytes({4, 5, 6})));
  const auto pkt = b.finish();

  Packet p;
  EXPECT_FALSE(parse(pkt.data(), kHeaderSize - 1, p));
  ASSERT_TRUE(parse(pkt.data(), pkt.size() - 1, p));  // last byte missing
  int delivered = 0;
  EXPECT_FALSE(p.for_each([&](auto, auto) { ++delivered; }));
  EXPECT_EQ(delivered, 1);  // the complete first message still arrives
}

TEST(Mold, RequestRoundTrip) {
  std::array<std::byte, kRequestSize> buf{};
  Request r;
  r.session = make_session("FEED");
  r.sequence = 123456789;
  r.count = 500;
  encode_request(buf.data(), r);
  Request back;
  ASSERT_TRUE(decode_request(buf.data(), buf.size(), back));
  EXPECT_EQ(back.session, r.session);
  EXPECT_EQ(back.sequence, r.sequence);
  EXPECT_EQ(back.count, r.count);
  EXPECT_FALSE(decode_request(buf.data(), kRequestSize - 1, back));
}

}  // namespace
