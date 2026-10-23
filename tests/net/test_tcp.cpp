#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "lab/net/tcp.hpp"
#include "lab/net/udp.hpp"

namespace {

using namespace lab::net;

// Polls until `want` bytes arrive or a second passes.
std::string read_bytes(TcpStream& s, std::size_t want) {
  std::string got;
  std::array<std::byte, 4096> buf;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (got.size() < want && std::chrono::steady_clock::now() < until) {
    const auto n = s.recv(buf);
    if (n < 0) break;
    got.append(reinterpret_cast<const char*>(buf.data()), static_cast<std::size_t>(n));
    s.flush();
  }
  return got;
}

struct Pair {
  TcpListener listener = TcpListener::listen(parse_endpoint("127.0.0.1:0"));
  TcpStream client = TcpStream::connect(listener.local());
  TcpStream server = accept_one();

  TcpStream accept_one() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (std::chrono::steady_clock::now() < until) {
      if (auto s = listener.accept()) return std::move(*s);
    }
    throw std::runtime_error("no connection");
  }
};

std::span<const std::byte> bytes(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

TEST(Tcp, SendsBothWays) {
  Pair p;
  ASSERT_TRUE(p.client.send(bytes("hello")));
  EXPECT_EQ(read_bytes(p.server, 5), "hello");
  ASSERT_TRUE(p.server.send(bytes("world!")));
  EXPECT_EQ(read_bytes(p.client, 6), "world!");
}

TEST(Tcp, QueuesWhatTheKernelDoesNotTake) {
  Pair p;
  const std::string big(8 << 20, 'x');  // more than socket buffers hold
  ASSERT_TRUE(p.client.send(bytes(big)));
  EXPECT_GT(p.client.queued(), 0u);
  std::thread reader{[&] { EXPECT_EQ(read_bytes(p.server, big.size()).size(), big.size()); }};
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (p.client.queued() > 0 && std::chrono::steady_clock::now() < until) p.client.flush();
  reader.join();
  EXPECT_EQ(p.client.queued(), 0u);
}

TEST(Tcp, EmptyBufferIsNotAClose) {
  Pair p;
  EXPECT_EQ(p.server.recv({}), 0);
  EXPECT_TRUE(p.server.open());
}

TEST(Tcp, ReportsPeerClose) {
  Pair p;
  p.client.close();
  std::array<std::byte, 16> buf;
  ssize_t n = 0;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (n == 0 && std::chrono::steady_clock::now() < until) n = p.server.recv(buf);
  EXPECT_EQ(n, -1);
  EXPECT_FALSE(p.server.open());
}

}  // namespace
