#include "lab/net/udp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace lab::net {
namespace {

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::system_category(), what);
}

in_addr parse_ip(std::string_view text) {
  const std::string s{text};
  in_addr a{};
  if (inet_pton(AF_INET, s.c_str(), &a) != 1) {
    throw std::invalid_argument("not an IPv4 address: " + s);
  }
  return a;
}

int open_udp() {
  const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) throw_errno("socket");
  return fd;
}

void set_opt(int fd, int level, int name, const void* value, socklen_t len, const char* what) {
  if (setsockopt(fd, level, name, value, len) != 0) throw_errno(what);
}

void set_int(int fd, int level, int name, int value, const char* what) {
  set_opt(fd, level, name, &value, sizeof value, what);
}

const sockaddr* as_sockaddr(const sockaddr_in* a) noexcept {
  return reinterpret_cast<const sockaddr*>(a);
}

}  // namespace

bool Endpoint::multicast() const noexcept {
  return (ntohl(addr.sin_addr.s_addr) & 0xF0000000u) == 0xE0000000u;  // 224.0.0.0/4
}

std::uint16_t Endpoint::port() const noexcept { return ntohs(addr.sin_port); }

std::string Endpoint::str() const {
  char ip[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
  return std::string{ip} + ":" + std::to_string(port());
}

bool Endpoint::operator==(const Endpoint& o) const noexcept {
  return addr.sin_addr.s_addr == o.addr.sin_addr.s_addr && addr.sin_port == o.addr.sin_port;
}

Endpoint parse_endpoint(std::string_view text) {
  const auto colon = text.rfind(':');
  if (colon == std::string_view::npos) {
    throw std::invalid_argument("expected host:port, got " + std::string{text});
  }
  unsigned port = 0;
  const auto p = text.substr(colon + 1);
  const auto [end, ec] = std::from_chars(p.data(), p.data() + p.size(), port);
  if (ec != std::errc{} || end != p.data() + p.size() || port > 65535) {
    throw std::invalid_argument("bad port in " + std::string{text});
  }
  Endpoint e;
  e.addr.sin_family = AF_INET;
  e.addr.sin_port = htons(static_cast<std::uint16_t>(port));
  e.addr.sin_addr = parse_ip(text.substr(0, colon));
  return e;
}

UdpSocket::~UdpSocket() {
  if (fd_ >= 0) close(fd_);
}

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
  if (this != &o) {
    if (fd_ >= 0) close(fd_);
    fd_ = o.fd_;
    o.fd_ = -1;
  }
  return *this;
}

UdpSocket UdpSocket::sender(const Endpoint& dest, std::string_view iface, int ttl) {
  UdpSocket s{open_udp()};
  if (dest.multicast()) {
    const in_addr ifaddr = parse_ip(iface);
    set_opt(s.fd_, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof ifaddr, "IP_MULTICAST_IF");
    set_int(s.fd_, IPPROTO_IP, IP_MULTICAST_TTL, ttl, "IP_MULTICAST_TTL");
    set_int(s.fd_, IPPROTO_IP, IP_MULTICAST_LOOP, 1, "IP_MULTICAST_LOOP");
  }
  set_int(s.fd_, SOL_SOCKET, SO_SNDBUF, 4 << 20, "SO_SNDBUF");
  return s;
}

UdpSocket UdpSocket::receiver(const Endpoint& listen, std::string_view iface, int rcvbuf_bytes) {
  UdpSocket s{open_udp()};
  set_int(s.fd_, SOL_SOCKET, SO_REUSEADDR, 1, "SO_REUSEADDR");
  set_int(s.fd_, SOL_SOCKET, SO_RCVBUF, rcvbuf_bytes, "SO_RCVBUF");
  // Binding the group address (not INADDR_ANY) keeps other groups on the same
  // port out of this socket.
  if (bind(s.fd_, as_sockaddr(&listen.addr), sizeof listen.addr) != 0) {
    throw_errno("bind " + listen.str());
  }
  if (listen.multicast()) {
    ip_mreq mreq{};
    mreq.imr_multiaddr = listen.addr.sin_addr;
    mreq.imr_interface = parse_ip(iface);
    set_opt(s.fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq,
            "IP_ADD_MEMBERSHIP (is multicast enabled on the interface?)");
  }
  return s;
}

UdpSocket UdpSocket::bound(const Endpoint& local, int rcvbuf_bytes) {
  UdpSocket s{open_udp()};
  set_int(s.fd_, SOL_SOCKET, SO_REUSEADDR, 1, "SO_REUSEADDR");
  set_int(s.fd_, SOL_SOCKET, SO_RCVBUF, rcvbuf_bytes, "SO_RCVBUF");
  if (bind(s.fd_, as_sockaddr(&local.addr), sizeof local.addr) != 0) {
    throw_errno("bind " + local.str());
  }
  return s;
}

ssize_t UdpSocket::send_to(std::span<const std::byte> data, const Endpoint& dest) const noexcept {
  return sendto(fd_, data.data(), data.size(), MSG_DONTWAIT, as_sockaddr(&dest.addr),
                sizeof dest.addr);
}

ssize_t UdpSocket::recv_from(std::span<std::byte> buf, Endpoint* from) const noexcept {
  sockaddr_in a{};
  socklen_t len = sizeof a;
  const ssize_t n =
      recvfrom(fd_, buf.data(), buf.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&a), &len);
  if (n >= 0 && from != nullptr) from->addr = a;
  return n;
}

Endpoint UdpSocket::local() const {
  Endpoint e;
  socklen_t len = sizeof e.addr;
  if (getsockname(fd_, reinterpret_cast<sockaddr*>(&e.addr), &len) != 0) {
    throw_errno("getsockname");
  }
  return e;
}

BatchReceiver::BatchReceiver(std::size_t batch, std::size_t max_datagram)
    : stride_{max_datagram}, buf_(batch * max_datagram), iov_(batch), from_(batch), msgs_(batch) {
  for (std::size_t i = 0; i < batch; ++i) {
    iov_[i].iov_base = buf_.data() + i * stride_;
    iov_[i].iov_len = stride_;
    std::memset(&msgs_[i], 0, sizeof msgs_[i]);
    msgs_[i].msg_hdr.msg_iov = &iov_[i];
    msgs_[i].msg_hdr.msg_iovlen = 1;
    msgs_[i].msg_hdr.msg_name = &from_[i];
  }
}

int BatchReceiver::receive(const UdpSocket& s) noexcept {
  for (auto& m : msgs_) m.msg_hdr.msg_namelen = sizeof(sockaddr_in);
  const int n =
      recvmmsg(s.fd(), msgs_.data(), static_cast<unsigned>(msgs_.size()), MSG_DONTWAIT, nullptr);
  if (n <= 0) return 0;
  for (int i = 0; i < n; ++i) {
    if (msgs_[i].msg_hdr.msg_flags & MSG_TRUNC) ++truncated_;
  }
  return n;
}

}  // namespace lab::net
