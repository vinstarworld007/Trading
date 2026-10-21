#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "lab/mold/retransmit_store.hpp"

namespace {

using lab::mold::make_session;
using lab::mold::Packet;
using lab::mold::PacketBuilder;
using lab::mold::RetransmitStore;

std::vector<std::byte> msg(std::size_t len, int fill) {
  return std::vector<std::byte>(len, static_cast<std::byte>(fill));
}

TEST(RetransmitStore, StoresMessagesBySequenceNumber) {
  RetransmitStore s{1 << 20, 1024};
  EXPECT_EQ(s.next_sequence(), 1u);
  EXPECT_EQ(s.append(msg(10, 1)), 1u);
  EXPECT_EQ(s.append(msg(20, 2)), 2u);
  const auto m = s.get(2);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->size(), 20u);
  EXPECT_EQ((*m)[0], std::byte{2});
  EXPECT_FALSE(s.get(0));
  EXPECT_FALSE(s.get(3));  // not sent yet
}

TEST(RetransmitStore, ForgetsMessagesPastTheIndexWindow) {
  RetransmitStore s{1 << 20, 4};
  for (int i = 1; i <= 6; ++i) s.append(msg(8, i));
  EXPECT_FALSE(s.get(2));
  ASSERT_TRUE(s.get(3));
  EXPECT_EQ((*s.get(3))[0], std::byte{3});
  ASSERT_TRUE(s.get(6));
}

TEST(RetransmitStore, ForgetsMessagesWhoseBytesWereOverwritten) {
  const std::size_t arena = 1 << 17;  // the smallest allowed
  RetransmitStore s{arena, 1 << 16};
  const std::size_t len = 1000;
  const std::size_t total = 3 * arena / len;  // wraps the byte ring about three times
  for (std::size_t i = 1; i <= total; ++i) s.append(msg(len, static_cast<int>(i & 0xFF)));
  EXPECT_FALSE(s.get(1));
  std::size_t kept = 0;
  for (std::size_t i = 1; i <= total; ++i) {
    if (const auto m = s.get(i)) {
      ++kept;
      ASSERT_EQ(m->size(), len);
      EXPECT_EQ((*m)[0], static_cast<std::byte>(i & 0xFF)) << i;
      EXPECT_EQ((*m)[len - 1], static_cast<std::byte>(i & 0xFF)) << i;
    }
  }
  EXPECT_GT(kept, arena / len - 2);  // about one arena's worth survives
  EXPECT_LE(kept, arena / len);
}

TEST(RetransmitStore, FillsOnePacketAtMost) {
  RetransmitStore s{1 << 20, 1024};
  for (int i = 1; i <= 100; ++i) s.append(msg(30, i));
  PacketBuilder<> b{200};  // room for 5 blocks of 32 bytes after the header
  const auto session = make_session("S");
  EXPECT_EQ(s.fill(b, session, 10, 50), 5u);
  Packet p;
  const auto pkt = b.finish();
  ASSERT_TRUE(lab::mold::parse(pkt.data(), pkt.size(), p));
  EXPECT_EQ(p.header.sequence, 10u);
  EXPECT_EQ(p.header.count, 5u);
  EXPECT_EQ(s.fill(b, session, 99, 50), 2u);  // only 99 and 100 exist
  EXPECT_EQ(s.fill(b, session, 101, 5), 0u);  // nothing to send
}

}  // namespace
