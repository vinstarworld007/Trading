#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "lab/net/udp.hpp"

namespace {

using namespace lab::net;

std::vector<std::byte> text(const std::string& s) {
  std::vector<std::byte> out(s.size());
  std::memcpy(out.data(), s.data(), s.size());
  return out;
}

// Polls a non-blocking receiver until `n` datagrams arrive or a second passes.
std::vector<std::string> receive(const UdpSocket& s, std::size_t n) {
  BatchReceiver batch{8, 1500};
  std::vector<std::string> got;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (got.size() < n && std::chrono::steady_clock::now() < until) {
    const int k = batch.receive(s);
    for (int i = 0; i < k; ++i) {
      const auto d = batch.datagram(i);
      got.emplace_back(reinterpret_cast<const char*>(d.data()), d.size());
    }
    if (k == 0) std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return got;
}

TEST(Udp, ParsesEndpoints) {
  const Endpoint e = parse_endpoint("239.1.2.3:30001");
  EXPECT_TRUE(e.multicast());
  EXPECT_EQ(e.port(), 30001);
  EXPECT_EQ(e.str(), "239.1.2.3:30001");
  EXPECT_FALSE(parse_endpoint("127.0.0.1:1").multicast());
  EXPECT_THROW(parse_endpoint("127.0.0.1"), std::invalid_argument);
  EXPECT_THROW(parse_endpoint("127.0.0.1:99999"), std::invalid_argument);
  EXPECT_THROW(parse_endpoint("nope:1"), std::invalid_argument);
}

TEST(Udp, UnicastLoopbackRoundTrip) {
  UdpSocket rx = UdpSocket::bound(parse_endpoint("127.0.0.1:0"));
  const Endpoint to = rx.local();
  ASSERT_NE(to.port(), 0);
  UdpSocket tx = UdpSocket::sender(to);
  for (const char* m : {"one", "two", "three"}) ASSERT_GT(tx.send_to(text(m), to), 0);
  EXPECT_EQ(receive(rx, 3), (std::vector<std::string>{"one", "two", "three"}));

  // Nothing waiting: non-blocking receive returns at once.
  std::array<std::byte, 16> buf;
  EXPECT_EQ(rx.recv_from(buf), -1);
  EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
}

TEST(Udp, ReplyGoesBackToTheSender) {
  UdpSocket server = UdpSocket::bound(parse_endpoint("127.0.0.1:0"));
  UdpSocket client = UdpSocket::bound(parse_endpoint("127.0.0.1:0"));
  ASSERT_GT(client.send_to(text("req"), server.local()), 0);
  std::array<std::byte, 16> buf;
  Endpoint from;
  ssize_t n = -1;
  for (int i = 0; i < 1000 && n < 0; ++i) {
    n = server.recv_from(buf, &from);
    if (n < 0) std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  ASSERT_EQ(n, 3);
  EXPECT_EQ(from, client.local());
  ASSERT_GT(server.send_to(text("resp"), from), 0);
  EXPECT_EQ(receive(client, 1), std::vector<std::string>{"resp"});
}

TEST(Udp, MulticastOnLoopback) {
  const Endpoint group = parse_endpoint("239.255.77.1:30999");
  UdpSocket rx;
  try {
    rx = UdpSocket::receiver(group, "127.0.0.1");
  } catch (const std::system_error& e) {
    GTEST_SKIP() << "no loopback multicast here (" << e.what()
                 << "); scripts/wsl-setup.sh enables it";
  }
  UdpSocket tx = UdpSocket::sender(group, "127.0.0.1");
  if (tx.send_to(text("tick"), group) < 0) GTEST_SKIP() << "cannot send to a group here";
  const auto got = receive(rx, 1);
  if (got.empty()) GTEST_SKIP() << "loopback multicast route missing; scripts/wsl-setup.sh adds it";
  EXPECT_EQ(got, std::vector<std::string>{"tick"});
}

}  // namespace
