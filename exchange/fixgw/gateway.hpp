#pragma once

// fixgw's single I/O thread: FIX 4.4 sessions in front, one OUCH 5.0
// session per FIX session behind, to simex. Each configured FIX CompID owns
// one simex account. NewOrderSingle, OrderCancelRequest and
// OrderCancelReplaceRequest become OUCH Enter, Cancel and Replace; OUCH
// responses become ExecutionReports and OrderCancelRejects
// (docs/specs/fix44-subset.md, "How fixgw maps FIX onto OUCH").
//
// Order state, ClOrdID lookup and the FIX resend store are sized at startup;
// nothing is allocated per message.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lab/core/histogram.hpp"
#include "lab/fix/session.hpp"
#include "lab/net/tcp.hpp"
#include "lab/ouch/messages.hpp"
#include "lab/soup/soup.hpp"

namespace fixgw {

struct LinkConfig {
  std::string comp_id;    // the firm's SenderCompID
  std::string ouch_user;  // its simex account
  std::string ouch_password;
};

struct GatewayConfig {
  std::string listen = "127.0.0.1:16000";
  std::string simex = "127.0.0.1:15000";
  std::string comp_id = "SIMEX";  // ours
  std::vector<LinkConfig> links;
  std::size_t max_orders = 1u << 18;  // per link, for the day
  bool cancel_on_disconnect = true;
};

struct LinkStats {
  std::uint64_t new_orders = 0;
  std::uint64_t cancels = 0;
  std::uint64_t replaces = 0;
  std::uint64_t exec_reports = 0;
  std::uint64_t fills = 0;
  std::uint64_t order_rejects = 0;   // ExecutionReport 150=8
  std::uint64_t cancel_rejects = 0;  // OrderCancelReject
  std::uint64_t session_rejects = 0;
  std::uint64_t business_rejects = 0;
  std::uint64_t disconnect_cancels = 0;
  std::uint64_t unknown_ouch_refs = 0;
};

class Gateway {
 public:
  explicit Gateway(const GatewayConfig& cfg);
  ~Gateway();

  // Connects every link's OUCH session to simex. Throws on failure.
  void connect_simex();

  // Thread body: serves until `stop` is set or simex ends the session, then
  // logs every FIX session out.
  void run(const std::atomic<bool>& stop);

  std::string address() const;
  bool report(std::FILE* out) const;  // false if a check failed

 private:
  struct Link;
  struct Order;
  struct Pending;
  struct FixHandler;
  struct OuchHandler;

  void accept_new(std::uint64_t now);
  void route_pending(std::uint64_t now);
  void on_app(Link& l, const lab::fix::Message& m);
  void on_new(Link& l, const lab::fix::Message& m);
  void on_cancel(Link& l, const lab::fix::Message& m);
  void on_replace(Link& l, const lab::fix::Message& m);
  void on_fix_disconnect(Link& l);
  void read_ouch(Link& l, std::uint64_t now);
  void ouch_message(Link& l, std::span<const std::byte> msg, std::uint64_t rx_tsc);
  void send_ouch(Link& l, std::span<const std::byte> msg);

  // FIX out.
  void exec_report(Link& l, const Order& o, char exec_type, char ord_status,
                   std::string_view cl_ord_id, std::string_view orig_cl_ord_id,
                   std::uint32_t last_qty = 0, std::uint32_t last_px = 0);
  void reject_order(Link& l, const lab::fix::Message& m, int reason, std::string_view text);
  void cancel_reject(Link& l, std::string_view cl_ord_id, std::string_view orig_cl_ord_id,
                     const Order* o, char response_to, int reason, std::string_view text);
  void session_reject(Link& l, const lab::fix::Message& m, int ref_tag, int reason,
                      std::string_view text);

  // Orders and ClOrdIDs.
  Order* order(Link& l, std::uint32_t ref);
  Order* find_cl_ord_id(Link& l, std::string_view id);
  bool index_cl_ord_id(Link& l, std::uint32_t ref);
  Order* new_order_slot(Link& l, std::uint32_t& ref);

  GatewayConfig cfg_;
  lab::net::TcpListener listener_;
  std::vector<std::unique_ptr<Link>> links_;
  std::vector<std::unique_ptr<Pending>> pending_;
  bool simex_ended_ = false;
  std::uint64_t connections_ = 0;
  std::uint64_t refused_ = 0;
  lab::LatencyHistogram fix_to_ouch_;  // FIX request decoded to OUCH written, ticks
  lab::LatencyHistogram ouch_to_fix_;  // OUCH response read to FIX written, ticks
};

}  // namespace fixgw
