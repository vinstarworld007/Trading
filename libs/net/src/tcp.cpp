#include "lab/net/tcp.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>

namespace lab::net {
namespace {

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::system_category(), what);
}

const sockaddr* as_sockaddr(const sockaddr_in* a) noexcept {
  return reinterpret_cast<const sockaddr*>(a);
}

void set_nodelay(int fd) noexcept {
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

}  // namespace

TcpStream::TcpStream(int fd) : fd_{fd} {
  out_.reserve(1 << 16);
  set_nodelay(fd_);
}

TcpStream::~TcpStream() { close(); }

TcpStream::TcpStream(TcpStream&& o) noexcept
    : fd_{o.fd_}, failed_{o.failed_}, out_{std::move(o.out_)}, out_pos_{o.out_pos_} {
  o.fd_ = -1;
}

TcpStream& TcpStream::operator=(TcpStream&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = o.fd_;
    failed_ = o.failed_;
    out_ = std::move(o.out_);
    out_pos_ = o.out_pos_;
    o.fd_ = -1;
  }
  return *this;
}

TcpStream TcpStream::connect(const Endpoint& to, std::chrono::milliseconds timeout) {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) throw_errno("socket");
  TcpStream s{fd};
  if (::connect(fd, as_sockaddr(&to.addr), sizeof to.addr) != 0) {
    if (errno != EINPROGRESS) throw_errno("connect " + to.str());
    pollfd p{fd, POLLOUT, 0};
    const int r = poll(&p, 1, static_cast<int>(timeout.count()));
    if (r == 0) {
      errno = ETIMEDOUT;
      throw_errno("connect " + to.str());
    }
    if (r < 0) throw_errno("poll");
    int err = 0;
    socklen_t len = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
      errno = err;
      throw_errno("connect " + to.str());
    }
  }
  return s;
}

bool TcpStream::flush() noexcept {
  if (!open()) return false;
  while (out_pos_ < out_.size()) {
    const ssize_t n =
        ::send(fd_, out_.data() + out_pos_, out_.size() - out_pos_, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      out_pos_ += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    failed_ = true;
    return false;
  }
  if (out_pos_ == out_.size()) {
    out_.clear();
    out_pos_ = 0;
  }
  return true;
}

bool TcpStream::send(std::span<const std::byte> data) noexcept {
  if (!open()) return false;
  std::size_t sent = 0;
  if (out_pos_ == out_.size()) {  // nothing queued: try the kernel directly
    const ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n >= 0) {
      sent = static_cast<std::size_t>(n);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      failed_ = true;
      return false;
    }
  }
  if (sent < data.size()) out_.insert(out_.end(), data.begin() + sent, data.end());
  return true;
}

ssize_t TcpStream::recv(std::span<std::byte> buf) noexcept {
  if (!open()) return -1;
  if (buf.empty()) return 0;  // recv() would return 0, which means closed
  const ssize_t n = ::recv(fd_, buf.data(), buf.size(), MSG_DONTWAIT);
  if (n > 0) return n;
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  failed_ = true;  // 0 is an orderly close
  return -1;
}

void TcpStream::close() noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

Endpoint TcpStream::peer() const {
  Endpoint e;
  socklen_t len = sizeof e.addr;
  if (getpeername(fd_, reinterpret_cast<sockaddr*>(&e.addr), &len) != 0) throw_errno("getpeername");
  return e;
}

TcpListener::~TcpListener() {
  if (fd_ >= 0) ::close(fd_);
}

TcpListener& TcpListener::operator=(TcpListener&& o) noexcept {
  if (this != &o) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = o.fd_;
    o.fd_ = -1;
  }
  return *this;
}

TcpListener TcpListener::listen(const Endpoint& at, int backlog) {
  TcpListener l;
  l.fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (l.fd_ < 0) throw_errno("socket");
  const int one = 1;
  setsockopt(l.fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(l.fd_, as_sockaddr(&at.addr), sizeof at.addr) != 0) throw_errno("bind " + at.str());
  if (::listen(l.fd_, backlog) != 0) throw_errno("listen " + at.str());
  return l;
}

std::optional<TcpStream> TcpListener::accept() noexcept {
  const int fd = accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd < 0) return std::nullopt;
  return TcpStream{fd};
}

Endpoint TcpListener::local() const {
  Endpoint e;
  socklen_t len = sizeof e.addr;
  if (getsockname(fd_, reinterpret_cast<sockaddr*>(&e.addr), &len) != 0) throw_errno("getsockname");
  return e;
}

}  // namespace lab::net
