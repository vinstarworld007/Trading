#pragma once

// simex's order entry gateway: OUCH 5.0 over SoupBinTCP, one thread.
//
// Each account has one sequenced outbound stream for the day. Every OUCH
// response is appended to it and sent if the account is connected, so a client
// that reconnects and asks for sequence N gets everything from N on, as
// SoupBinTCP promises. Inbound orders are validated (UserRefNum strictly
// increasing, side, quantity, price), turned into Commands and pushed to the
// matching thread; responses come back as SessionEvents.
//
// All socket work happens here, never on the matching thread.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "lab/net/tcp.hpp"
#include "lab/ouch/messages.hpp"
#include "lab/soup/soup.hpp"
#include "matcher.hpp"

namespace simex {

struct AccountConfig {
  std::string user;
  std::string password;
};

struct GatewayConfig {
  std::string listen = "127.0.0.1:15000";
  std::string session = "SIMEX00001";
  std::vector<AccountConfig> accounts;
  // Receives a copy of every firm execution (docs/specs/dropcopy-layout.md);
  // an empty user turns it off.
  AccountConfig drop_copy{"DROP01", "lab"};
  bool cancel_on_disconnect = true;
  std::uint64_t heartbeat_ms = 1000;
  std::uint64_t idle_timeout_ms = 15000;
  std::uint64_t login_timeout_ms = 30000;
};

struct GatewayStats {
  std::uint64_t connections = 0;
  std::uint64_t logins = 0;
  std::uint64_t login_rejects = 0;
  std::uint64_t inbound = 0;          // OUCH messages received
  std::uint64_t outbound = 0;         // OUCH messages sequenced
  std::uint64_t gateway_rejects = 0;  // rejected before reaching the matcher
  std::uint64_t stale_refs = 0;       // UserRefNum not above the last one: ignored
  std::uint64_t bad_messages = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t drop_copies = 0;  // Execution Reports sequenced to the drop copy account
};

class Gateway {
 public:
  Gateway(const GatewayConfig& cfg, CommandRing& to_matcher, SessionRing& from_matcher);

  // Thread body. Serves sessions until `stop` is set; while `accepting` is
  // false it stops taking new orders. Before returning it sends every
  // remaining response, End of Day, and SoupBinTCP End of Session.
  void run(const std::atomic<bool>& accepting, std::atomic<bool>& intake_stopped,
           const std::atomic<bool>& stop);

  std::string address() const;
  const GatewayStats& stats() const noexcept { return stats_; }

 private:
  struct OrderInfo {
    lab::ouch::ClOrdId cl_ord_id;
    char side;  // as entered: B, S, T or E
    char display;
    char capacity;
  };
  struct Conn;
  struct Account {
    AccountConfig cfg;
    std::uint16_t owner;
    std::uint32_t last_ref = 0;
    std::vector<std::byte> stream;       // sequenced OUCH messages, back to back
    std::vector<std::uint32_t> offsets;  // start of message i (sequence i + 1)
    std::unordered_map<std::uint32_t, OrderInfo> orders;
    Conn* conn = nullptr;    // logged-in connection, if any
    bool drop_copy = false;  // receives Execution Reports, sends no orders
  };
  struct Conn {
    lab::net::TcpStream sock;
    std::unique_ptr<lab::soup::StreamParser> in;
    Account* account = nullptr;
    std::uint64_t opened_ns = 0;
    std::uint64_t last_rx_ns = 0;
    std::uint64_t last_tx_ns = 0;
    bool dead = false;
  };

  void accept_new(std::uint64_t now);
  void read(Conn& c, std::uint64_t now, bool accepting);
  void login(Conn& c, std::span<const std::byte> payload, std::uint64_t now);
  void inbound(Account& a, std::span<const std::byte> msg);
  void drain_events();
  void sequence(Account& a, std::span<const std::byte> msg);
  void send_from(Conn& c, const Account& a, std::uint64_t first_seq);
  void send_packet(Conn& c, std::span<const std::byte> bytes);
  void housekeeping(std::uint64_t now);
  void close(Conn& c);
  void reject(Account& a, std::uint32_t ref, std::uint16_t reason, const lab::ouch::ClOrdId& id);
  void push(const Command& c);
  void drop_copy(const Account& a, const SessionEvent& e, const OrderInfo& oi);
  std::uint64_t feed_time() const;

  GatewayConfig cfg_;
  CommandRing& to_matcher_;
  SessionRing& from_matcher_;
  lab::net::TcpListener listener_;
  std::vector<Account> accounts_;
  Account* drop_ = nullptr;
  std::vector<std::unique_ptr<Conn>> conns_;
  bool open_ = true;  // still sending commands to the matcher
  GatewayStats stats_;
};

}  // namespace simex
