#pragma once

// UDP sockets for the lab's feeds: multicast or unicast senders, receivers
// that join a group, and batched non-blocking receive with recvmmsg.
//
// Setup functions throw std::system_error or std::invalid_argument. The send
// and receive calls are noexcept and non-blocking; they are syscalls, so they
// belong on a feed's I/O thread, never on a book or strategy thread.
//
//   auto tx = UdpSocket::sender(parse_endpoint("239.1.1.1:30001"), "127.0.0.1");
//   tx.send_to(bytes, dest);
//   auto rx = UdpSocket::receiver(parse_endpoint("239.1.1.1:30001"), "127.0.0.1");
//   BatchReceiver batch{32, 1500};
//   for (int i = 0, n = batch.receive(rx); i < n; ++i) use(batch.datagram(i));

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lab::net {

struct Endpoint {
  sockaddr_in addr{};

  bool multicast() const noexcept;
  std::uint16_t port() const noexcept;
  std::string str() const;
  bool operator==(const Endpoint& o) const noexcept;
};

// "a.b.c.d:port". Throws std::invalid_argument.
Endpoint parse_endpoint(std::string_view text);

class UdpSocket {
 public:
  UdpSocket() = default;
  explicit UdpSocket(int fd) noexcept : fd_{fd} {}
  ~UdpSocket();
  UdpSocket(UdpSocket&& o) noexcept : fd_{o.fd_} { o.fd_ = -1; }
  UdpSocket& operator=(UdpSocket&& o) noexcept;
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // A socket for sending to `dest`. For a multicast group, `iface` (an IPv4
  // address) picks the outgoing interface and loopback delivery is on.
  static UdpSocket sender(const Endpoint& dest, std::string_view iface = "127.0.0.1", int ttl = 1);

  // Receives datagrams sent to `listen`. A multicast group is joined on
  // `iface`; a unicast address is bound directly. Non-blocking.
  static UdpSocket receiver(const Endpoint& listen, std::string_view iface = "127.0.0.1",
                            int rcvbuf_bytes = 16 << 20);

  // A unicast socket bound to `local` (port 0 picks one), non-blocking. Used
  // for request/response traffic such as MoldUDP64 retransmission.
  static UdpSocket bound(const Endpoint& local, int rcvbuf_bytes = 4 << 20);

  // Returns bytes sent, or -1 with errno set.
  ssize_t send_to(std::span<const std::byte> data, const Endpoint& dest) const noexcept;
  // Returns bytes received, or -1 with errno EAGAIN when nothing is waiting.
  ssize_t recv_from(std::span<std::byte> buf, Endpoint* from = nullptr) const noexcept;

  Endpoint local() const;
  int fd() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

// Receives up to `batch` datagrams per call with one recvmmsg. Buffers are
// allocated once, at construction.
class BatchReceiver {
 public:
  BatchReceiver(std::size_t batch, std::size_t max_datagram);

  // Non-blocking. Returns the number of datagrams received (0 when none).
  int receive(const UdpSocket& s) noexcept;

  std::span<const std::byte> datagram(int i) const noexcept {
    return {buf_.data() + static_cast<std::size_t>(i) * stride_, msgs_[i].msg_len};
  }
  Endpoint from(int i) const noexcept {
    Endpoint e;
    e.addr = from_[i];
    return e;
  }
  // Datagrams cut short because they were longer than max_datagram.
  std::uint64_t truncated() const noexcept { return truncated_; }

 private:
  std::size_t stride_;
  std::vector<std::byte> buf_;
  std::vector<iovec> iov_;
  std::vector<sockaddr_in> from_;
  std::vector<mmsghdr> msgs_;
  std::uint64_t truncated_ = 0;
};

}  // namespace lab::net
