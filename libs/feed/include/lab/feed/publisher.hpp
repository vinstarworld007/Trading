#pragma once

// A MoldUDP64 market data publisher: packs messages into packets, sends them
// to a multicast group (or unicast address), keeps recent messages for its
// re-request server, sends heartbeats when idle and End of Session at the end,
// and can inject packet faults. Used by itch-replay and by simex's market data
// thread; single-threaded, one owner thread calls everything.
//
//   MoldPublisher pub{cfg};
//   pub.add(msg, lab::mono_ns());   // repeatedly
//   pub.service(lab::mono_ns());    // regularly: re-requests and heartbeats
//   pub.finish(2.0);                // End of Session for 2 s, still serving re-requests

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "lab/mold/fault_injector.hpp"
#include "lab/mold/mold.hpp"
#include "lab/mold/retransmit_store.hpp"
#include "lab/net/udp.hpp"

namespace lab::feed {

struct PublisherConfig {
  std::string dest = "239.1.1.1:30001";
  std::string iface = "127.0.0.1";
  int retrans_port = 30002;
  std::string session = "LAB0000001";
  std::size_t payload = mold::kDefaultMaxPayload;
  std::uint64_t max_delay_us = 20;  // longest a message waits for its packet to fill
  std::uint64_t heartbeat_ms = 1000;
  std::size_t store_mb = 256;  // re-request window
  mold::FaultConfig faults;
};

struct PublisherStats {
  std::uint64_t messages = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t end_of_session = 0;
  std::uint64_t requests = 0;
  std::uint64_t resent = 0;
  std::uint64_t unanswerable = 0;
  std::uint64_t send_errors = 0;
};

class MoldPublisher {
 public:
  static constexpr std::size_t kMaxPayload = 8972;  // jumbo frame; config.payload sets the limit

  // Opens the sockets. Throws std::system_error or std::invalid_argument.
  explicit MoldPublisher(const PublisherConfig& cfg);

  // Adds one message, sending the current packet first if it is full or its
  // first message has waited max_delay_us.
  void add(std::span<const std::byte> msg, std::uint64_t now_ns);
  // Sends the pending packet now.
  void flush(std::uint64_t now_ns);
  // Answers re-requests and sends a heartbeat if nothing went out for a while.
  void service(std::uint64_t now_ns);
  // Flushes, then sends End of Session every 100 ms for `eos_seconds` while
  // still answering re-requests.
  void finish(double eos_seconds);

  bool pending() const noexcept { return !builder_.empty(); }
  std::uint64_t next_sequence() const noexcept { return store_.next_sequence(); }
  const PublisherStats& stats() const noexcept { return stats_; }
  const mold::FaultStats& fault_stats() const noexcept { return faults_.stats(); }
  std::string describe() const;  // "SESSION to DEST, re-requests on ADDR"
  void print_report(std::FILE* out, double secs) const;

 private:
  using Builder = mold::PacketBuilder<kMaxPayload>;
  void send(std::span<const std::byte> pkt);
  void release_held();
  void serve_requests();

  PublisherConfig cfg_;
  mold::Session session_;
  net::Endpoint dest_;
  net::UdpSocket tx_;
  net::UdpSocket rq_;
  mold::RetransmitStore store_;
  mold::FaultInjector faults_;
  Builder builder_;
  Builder rbuilder_;
  std::vector<std::byte> held_;  // packet held back by a reorder fault
  std::size_t held_len_ = 0;
  bool holding_ = false;
  std::uint64_t sent_next_ = 1;  // next sequence number not yet sent
  std::uint64_t opened_ns_ = 0;
  std::uint64_t last_send_ns_ = 0;
  PublisherStats stats_;
};

}  // namespace lab::feed
