#pragma once

// The trader's order entry thread: one OUCH 5.0 session over SoupBinTCP. It
// owns the socket, so the trading thread never makes a syscall: requests
// arrive on a ring, are encoded and sent; responses are decoded, timestamped
// and pushed back on another ring.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lab/core/histogram.hpp"
#include "lab/core/spsc_ring.hpp"
#include "lab/net/tcp.hpp"
#include "lab/oms/messages.hpp"
#include "lab/soup/soup.hpp"

namespace trader {

using TopRing = lab::SpscRing<lab::oms::TopOfBook, 1u << 14>;
using RequestRing = lab::SpscRing<lab::oms::OrderRequest, 1u << 14>;
using ResponseRing = lab::SpscRing<lab::oms::OrderResponse, 1u << 16>;

struct OrderEntryConfig {
  std::string connect = "127.0.0.1:15000";
  std::string user = "FIRM01";
  std::string password = "lab";
  int cpu = -1;
};

struct OrderEntryStats {
  std::uint64_t requests = 0;
  std::uint64_t responses = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t undecodable = 0;
  bool session_ended = false;  // the exchange sent End of Session
  bool connection_lost = false;
  lab::LatencyHistogram tick_to_wire;    // feed packet rx to order handed to the socket
  lab::LatencyHistogram decide_to_wire;  // trading decision to order handed to the socket
  lab::LatencyHistogram round_trip;      // order sent to its first response
};

class OrderEntry {
 public:
  OrderEntry(const OrderEntryConfig& cfg, RequestRing& requests, ResponseRing& responses);

  // Connects and logs in, retrying a refused connect for up to 30 s. Throws
  // std::runtime_error if the exchange refuses the login.
  void login();

  // Thread body: serves the session until `stop` is set and every request
  // has been sent, then logs out.
  void run(const std::atomic<bool>& stop);

  const OrderEntryStats& stats() const noexcept { return stats_; }

 private:
  void send(const lab::oms::OrderRequest& r);
  void receive();
  void on_sequenced(std::span<const std::byte> msg, std::uint64_t rx_tsc);
  void push(const lab::oms::OrderResponse& r);

  OrderEntryConfig cfg_;
  RequestRing& requests_;
  ResponseRing& responses_;
  lab::net::TcpStream sock_;
  std::unique_ptr<lab::soup::StreamParser> in_;
  std::vector<std::uint64_t> sent_tsc_;  // by UserRefNum & mask, for round trips
  std::uint64_t last_tx_ns_ = 0;
  std::uint64_t next_seq_ = 1;
  OrderEntryStats stats_;
};

}  // namespace trader
