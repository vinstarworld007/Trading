#pragma once

// Non-blocking TCP for order entry sessions: a listener that accepts without
// blocking, and a stream with TCP_NODELAY and a send buffer for the bytes the
// kernel could not take at once. Like udp.hpp, every call here is a syscall
// and belongs on a session's I/O thread.
//
//   auto server = TcpListener::listen(parse_endpoint("127.0.0.1:15000"));
//   if (auto s = server.accept()) ...
//   auto client = TcpStream::connect(parse_endpoint("127.0.0.1:15000"));
//   client.send(bytes);      // queues what the kernel does not take
//   client.flush();          // call regularly to drain the queue
//   client.recv(buffer);     // >0 bytes, 0 nothing yet, -1 closed or failed

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "lab/net/udp.hpp"

namespace lab::net {

class TcpStream {
 public:
  TcpStream() = default;
  explicit TcpStream(int fd);
  ~TcpStream();
  TcpStream(TcpStream&& o) noexcept;
  TcpStream& operator=(TcpStream&& o) noexcept;
  TcpStream(const TcpStream&) = delete;
  TcpStream& operator=(const TcpStream&) = delete;

  // Connects, waiting up to `timeout`; then switches to non-blocking. Throws
  // std::system_error on failure.
  static TcpStream connect(const Endpoint& to,
                           std::chrono::milliseconds timeout = std::chrono::seconds{5});

  // Sends what the kernel takes now and queues the rest. Returns false once
  // the connection has failed.
  bool send(std::span<const std::byte> data) noexcept;
  // Pushes queued bytes. Returns false once the connection has failed.
  bool flush() noexcept;
  // Bytes received (>0), 0 if nothing is waiting, -1 if closed or failed.
  ssize_t recv(std::span<std::byte> buf) noexcept;

  void close() noexcept;
  bool open() const noexcept { return fd_ >= 0 && !failed_; }
  std::size_t queued() const noexcept { return out_.size() - out_pos_; }
  Endpoint peer() const;
  int fd() const noexcept { return fd_; }

 private:
  int fd_ = -1;
  bool failed_ = false;
  std::vector<std::byte> out_;
  std::size_t out_pos_ = 0;
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();
  TcpListener(TcpListener&& o) noexcept : fd_{o.fd_} { o.fd_ = -1; }
  TcpListener& operator=(TcpListener&& o) noexcept;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  // Port 0 picks a free port; see local(). Throws std::system_error.
  static TcpListener listen(const Endpoint& at, int backlog = 64);

  // A new connection if one is waiting.
  std::optional<TcpStream> accept() noexcept;
  Endpoint local() const;

 private:
  int fd_ = -1;
};

}  // namespace lab::net
