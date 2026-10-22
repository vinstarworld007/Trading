#pragma once

// A lab market data vendor's distribution server: one thread that takes a
// venue's MoldUDP64 feed through feed::FeedReceiver, keeps the consolidated
// records (consolidator.hpp), serves TCP clients through a protocol, and
// conflates per subscription. The three vendor look-alikes (ICF, OMM JSON
// over WebSocket, B-PIPE) differ only in their Session class; subscriptions,
// conflation, latency accounting and the end-of-day flush live here.
//
//   Server server{cfg, [] { return std::make_unique<MySession>(); }};
//   server.run(stop);
//   server.report(stdout);
//
// A vendor is not on the firm's hot path: requests may allocate. Market data
// fan-out does not.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lab/core/histogram.hpp"
#include "lab/feed/receiver.hpp"
#include "lab/net/tcp.hpp"
#include "lab/vendor/consolidator.hpp"

namespace lab::vendor {

// Wall-clock nanoseconds since local midnight, the clock ITCH timestamps use.
std::uint64_t ns_since_midnight() noexcept;

struct Subscription {
  std::uint16_t locate = 0;
  std::uint64_t handle = 0;       // the protocol's own id (stream ID, correlation ID, request ID)
  std::uint64_t interval_ns = 0;  // 0: every change; otherwise at most one update per interval
  std::uint32_t pending = 0;      // changes held back by conflation
  std::uint64_t last_ns = 0;      // when the last update went out
  std::uint32_t link = 0;
  bool active = false;
};

class Server;

// One client connection, as a protocol Session sees it.
class Link {
 public:
  void send(std::span<const std::byte> bytes);
  void send(std::string_view text) {
    send({reinterpret_cast<const std::byte*>(text.data()), text.size()});
  }
  // Starts updates for `locate`; returns the subscription's id.
  std::uint32_t subscribe(std::uint16_t locate, std::uint64_t handle, std::uint64_t interval_ns);
  void unsubscribe(std::uint32_t sub);
  const Subscription& subscription(std::uint32_t sub) const;
  const Consolidator& data() const;
  std::uint64_t now_ns() const;
  // Closes the connection once queued bytes are sent.
  void close() noexcept { closing_ = true; }
  bool closing() const noexcept { return closing_; }
  std::uint32_t id() const noexcept { return id_; }
  // For latency accounting: the record whose update is being sent.
  void published(const Record& r);

 private:
  friend class Server;
  Server* server_ = nullptr;
  std::uint32_t id_ = 0;
  net::TcpStream stream_;
  bool closing_ = false;
  std::vector<std::uint32_t> subs_;
};

class Session {
 public:
  virtual ~Session() = default;
  // New bytes from the client. The session keeps any partial message.
  virtual void on_bytes(std::span<const std::byte> in, Link& link) = 0;
  // A subscribed record changed (mask: fields changed since the last update
  // on this subscription).
  virtual void on_update(std::uint32_t sub, const Subscription& s, const Record& r,
                         std::uint32_t mask, Link& link) = 0;
  // Called about every millisecond, for heartbeats and timeouts.
  virtual void on_timer(std::uint64_t /*now_ns*/, Link& /*link*/) {}
  // The venue's feed ended: say goodbye in the protocol's own way.
  virtual void on_end(Link& link) { link.close(); }
};

// A vendor is off the firm's hot path: its feed thread and its server
// thread sleep briefly when idle instead of spinning.
inline feed::ReceiverConfig idle_feed() {
  feed::ReceiverConfig c;
  c.idle_sleep_us = 20;
  return c;
}

struct ServerConfig {
  std::string name = "vendor";
  std::string listen = "127.0.0.1:0";
  feed::ReceiverConfig feed = idle_feed();
  double linger_s = 1.0;            // after the feed ends, time for clients to read the last bytes
  std::uint32_t luld_band_pct = 0;  // the venue's LULD percentage above $3.00 (0: 5%)
};

struct ServerStats {
  std::uint64_t messages = 0;   // ITCH messages consolidated
  std::uint64_t changes = 0;    // record changes
  std::uint64_t updates = 0;    // updates handed to sessions
  std::uint64_t conflated = 0;  // changes merged into a pending update
  std::uint64_t connections = 0;
  std::uint64_t subscriptions = 0;  // ever opened
  std::uint64_t bytes_out = 0;
  LatencyHistogram exch_to_publish;  // ITCH timestamp to the update leaving, ns
};

class Server {
 public:
  using Factory = std::function<std::unique_ptr<Session>()>;

  Server(const ServerConfig& cfg, Factory factory);
  ~Server();

  std::string address() const;
  // Thread body: serves until the feed ends (then flushes, notifies clients
  // and lingers) or `stop` is set.
  void run(const std::atomic<bool>& stop);
  // Prints the report; false if the venue's books had integrity errors.
  bool report(std::FILE* out) const;
  const ServerStats& stats() const noexcept { return stats_; }

 private:
  friend class Link;
  struct Conn {
    Link link;
    std::unique_ptr<Session> session;
    std::uint64_t opened_ns = 0;
  };

  void on_change(const Record& r, std::uint32_t mask);
  void deliver(std::uint32_t sub_id, std::uint32_t mask);
  void flush_conflated(bool all);
  void accept_new();
  void serve_links();
  Conn* conn(std::uint32_t id) noexcept;

  ServerConfig cfg_;
  Factory factory_;
  net::TcpListener listener_;
  feed::FeedReceiver rx_;
  Consolidator data_;
  std::vector<std::unique_ptr<Conn>> conns_;
  std::uint32_t next_link_ = 1;
  std::vector<Subscription> subs_;
  std::vector<std::uint32_t> free_subs_;
  std::vector<std::vector<std::uint32_t>> by_locate_;
  std::vector<std::uint32_t> conflating_;  // subscriptions with an interval
  std::uint64_t now_ns_ = 0;
  ServerStats stats_;
  bool feed_ended_ = false;
};

}  // namespace lab::vendor
