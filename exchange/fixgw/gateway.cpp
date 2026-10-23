#include "gateway.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <system_error>
#include <thread>

#include "lab/core/affinity.hpp"
#include "lab/core/clock.hpp"
#include "lab/ouch/codec.hpp"

namespace fixgw {

namespace fix = lab::fix;
namespace ouch = lab::ouch;
namespace soup = lab::soup;
namespace tag = lab::fix::tag;

namespace {

constexpr std::size_t kMaxClOrdId = 20;
constexpr std::uint64_t kLogonWaitNs = 10'000'000'000;

std::uint64_t fnv1a(std::string_view s) noexcept {
  std::uint64_t h = 1469598103934665603ULL;
  for (const char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  return s;
}

char ouch_side(char fix_side) {
  switch (fix_side) {
    case '1':
      return ouch::side::kBuy;
    case '2':
      return ouch::side::kSell;
    case '5':
      return ouch::side::kSellShort;
    case '6':
      return ouch::side::kSellShortExempt;
    default:
      return 0;
  }
}

}  // namespace

struct Gateway::Order {
  enum State : std::uint8_t {
    kFree,
    kPendingNew,          // Enter sent, not yet accepted
    kLive,                // accepted, may be partially filled
    kPendingReplacement,  // the new order of a replace, not yet confirmed
    kDone,                // filled, canceled, rejected or replaced
  };
  std::array<char, kMaxClOrdId> cl_ord_id{};
  std::array<char, kMaxClOrdId> cxl_id{};  // ClOrdID of a pending cancel
  ouch::Symbol symbol{};
  std::uint64_t order_id = 0;  // ORN on simex's feed
  std::uint64_t notional = 0;  // sum of fill qty * Price(4), for AvgPx
  std::uint32_t qty = 0;       // total, executed shares included
  std::uint32_t cum = 0;
  std::uint32_t leaves = 0;
  std::uint32_t price = 0;
  std::uint32_t replace_ref = 0;  // live order: the replacement in flight
  std::uint32_t orig_ref = 0;     // replacement: the order it replaces
  std::uint8_t cl_len = 0;
  std::uint8_t cxl_len = 0;
  char side = 0;  // FIX 54
  char ord_type = 0;
  char tif = 0;
  State state = kFree;
  bool cancel_pending = false;

  std::string_view id() const noexcept { return {cl_ord_id.data(), cl_len}; }
  std::string_view cxl() const noexcept { return {cxl_id.data(), cxl_len}; }
  char status() const noexcept {
    if (state == kDone) return cum == qty ? '2' : '4';
    return cum > 0 ? '1' : '0';
  }
};

struct Gateway::Link {
  explicit Link(const LinkConfig& c, const GatewayConfig& g)
      : cfg{c}, fix{session_config(c, g)}, ouch_in{std::make_unique<soup::StreamParser>()} {
    orders.resize(g.max_orders);
    std::size_t cap = 1;
    while (cap < 2 * g.max_orders) cap <<= 1;
    ids.assign(cap, 0);
  }

  static fix::SessionConfig session_config(const LinkConfig& c, const GatewayConfig& g) {
    fix::SessionConfig s;
    s.sender = g.comp_id;
    s.target = c.comp_id;
    return s;
  }

  LinkConfig cfg;
  fix::Session fix;
  lab::net::TcpStream ouch;
  std::unique_ptr<soup::StreamParser> ouch_in;
  std::uint64_t ouch_last_tx = 0;
  bool ouch_ended = false;
  std::uint32_t base_ref = 0;
  std::uint32_t next_ref = 0;
  std::vector<Order> orders;       // index: UserRefNum - base_ref
  std::vector<std::uint32_t> ids;  // ClOrdID -> index + 1; open addressing, never deleted
  std::uint64_t exec_id = 0;
  LinkStats stats;
};

struct Gateway::Pending {
  lab::net::TcpStream sock;
  std::unique_ptr<fix::StreamParser> in = std::make_unique<fix::StreamParser>();
  std::uint64_t opened_ns = 0;
  bool done = false;
};

struct Gateway::FixHandler {
  Gateway& g;
  Link& l;
  void on_app(const fix::Message& m) { g.on_app(l, m); }
  void on_logon() {}
  void on_disconnect() { g.on_fix_disconnect(l); }
};

Gateway::Gateway(const GatewayConfig& cfg)
    : cfg_{cfg}, listener_{lab::net::TcpListener::listen(lab::net::parse_endpoint(cfg.listen))} {
  for (const auto& lc : cfg.links) links_.push_back(std::make_unique<Link>(lc, cfg_));
}

Gateway::~Gateway() = default;

std::string Gateway::address() const { return listener_.local().str(); }

// ---- simex side -------------------------------------------------------------

void Gateway::connect_simex() {
  const auto to = lab::net::parse_endpoint(cfg_.simex);
  for (auto& lp : links_) {
    Link& l = *lp;
    for (int attempt = 0;; ++attempt) {
      try {
        l.ouch = lab::net::TcpStream::connect(to);
        break;
      } catch (const std::system_error&) {
        if (attempt == 300) throw;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
      }
    }
    std::array<std::byte, soup::kLoginRequestSize> login;
    soup::write_login_request(login.data(), l.cfg.ouch_user, l.cfg.ouch_password, "", 0);
    l.ouch.send(login);
    // Then ask simex for the next UserRefNum it will take from this account.
    std::array<std::byte, soup::kHeaderSize + ouch::AccountQuery::kSize> q;
    std::array<std::byte, ouch::AccountQuery::kSize> qm;
    ouch::encode(ouch::AccountQuery{}, qm.data());
    l.ouch.send({q.data(), soup::write_data(q.data(), soup::kUnsequencedData, qm)});
    bool accepted = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (l.next_ref == 0) {
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("simex login timed out");
      l.ouch.flush();
      const auto n = l.ouch.recv(l.ouch_in->write_area());
      if (n < 0) throw std::runtime_error("simex closed the connection during login");
      if (n == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds{100});
        continue;
      }
      l.ouch_in->commit(static_cast<std::size_t>(n));
      while (auto p = l.ouch_in->next()) {
        if (p->type == soup::kLoginRejected) {
          throw std::runtime_error("simex rejected the login for " + l.cfg.ouch_user);
        }
        if (p->type == soup::kLoginAccepted) accepted = true;
        if (p->type != soup::kSequencedData || p->payload.empty()) continue;
        if (static_cast<char>(p->payload[0]) != ouch::AccountQueryResponse::kType) continue;
        if (p->payload.size() < ouch::AccountQueryResponse::kSize) continue;
        ouch::AccountQueryResponse r;
        ouch::decode(p->payload.data(), r);
        l.base_ref = l.next_ref = std::max<std::uint32_t>(r.next_user_ref_num, 1);
      }
    }
    if (!accepted) throw std::runtime_error("no Login Accepted from simex");
    l.ouch_last_tx = lab::mono_ns();
  }
}

void Gateway::send_ouch(Link& l, std::span<const std::byte> msg) {
  std::array<std::byte, soup::kHeaderSize + ouch::kMaxMessageSize> pkt;
  l.ouch.send({pkt.data(), soup::write_data(pkt.data(), soup::kUnsequencedData, msg)});
  l.ouch_last_tx = lab::mono_ns();
}

void Gateway::read_ouch(Link& l, std::uint64_t now) {
  if (l.ouch_ended) return;
  for (;;) {
    const auto n = l.ouch.recv(l.ouch_in->write_area());
    if (n < 0) {
      l.ouch_ended = true;
      return;
    }
    if (n == 0) break;
    const std::uint64_t rx_tsc = lab::rdtsc();
    l.ouch_in->commit(static_cast<std::size_t>(n));
    while (auto p = l.ouch_in->next()) {
      if (p->type == soup::kSequencedData) {
        ouch_message(l, p->payload, rx_tsc);
      } else if (p->type == soup::kEndOfSession) {
        l.ouch_ended = true;
      }
    }
  }
  l.ouch.flush();
  if (now - std::min(now, l.ouch_last_tx) > 1'000'000'000) {
    std::array<std::byte, soup::kHeaderSize> hb;
    soup::write_empty(hb.data(), soup::kClientHeartbeat);
    l.ouch.send(hb);
    l.ouch_last_tx = now;
  }
}

struct Gateway::OuchHandler : ouch::HandlerBase {
  Gateway& g;
  Link& l;
  OuchHandler(Gateway& gw, Link& link) : g{gw}, l{link} {}
  using HandlerBase::on;

  void on(const ouch::OrderAccepted& m) {
    Order* o = g.order(l, m.user_ref_num);
    if (o == nullptr) {
      ++l.stats.unknown_ouch_refs;
      return;
    }
    o->order_id = m.order_reference_number;
    if (o->state == Order::kPendingNew) o->state = Order::kLive;
    g.exec_report(l, *o, '0', o->status(), o->id(), {});
  }

  void on(const ouch::OrderExecuted& m) {
    Order* o = g.order(l, m.user_ref_num);
    if (o == nullptr) {
      ++l.stats.unknown_ouch_refs;
      return;
    }
    const std::uint32_t q = std::min(m.quantity, o->leaves);
    const auto px = static_cast<std::uint32_t>(m.price);
    o->cum += q;
    o->leaves -= q;
    o->notional += static_cast<std::uint64_t>(q) * px;
    if (o->leaves == 0) o->state = Order::kDone;
    ++l.stats.fills;
    g.exec_report(l, *o, 'F', o->status(), o->id(), {}, q, px);
    if (o->leaves == 0 && o->cancel_pending) {  // simex drops the cancel: nothing left
      o->cancel_pending = false;
      g.cancel_reject(l, o->cxl(), o->id(), o, '1', 0, "too late to cancel: filled");
    }
  }

  void on(const ouch::OrderCanceled& m) {
    Order* o = g.order(l, m.user_ref_num);
    if (o == nullptr) {
      ++l.stats.unknown_ouch_refs;
      return;
    }
    o->leaves -= std::min(m.quantity, o->leaves);
    if (o->leaves == 0) o->state = Order::kDone;
    if (o->replace_ref != 0 && m.reason == ouch::cancel_reason::kUser) {
      // A replace for no more than the executed shares cancels the rest.
      Order* r = g.order(l, o->replace_ref);
      o->replace_ref = 0;
      if (r != nullptr) {
        r->state = Order::kDone;
        g.exec_report(l, *o, '4', '4', r->id(), o->id());
        return;
      }
    }
    if (o->cancel_pending) {
      o->cancel_pending = false;
      g.exec_report(l, *o, '4', o->leaves == 0 ? '4' : o->status(), o->cxl(), o->id());
    } else {
      g.exec_report(l, *o, '4', o->leaves == 0 ? '4' : o->status(), o->id(), {});
    }
  }

  void on(const ouch::OrderReplaced& m) {
    Order* old = g.order(l, m.orig_user_ref_num);
    Order* n = g.order(l, m.user_ref_num);
    if (old == nullptr || n == nullptr) {
      ++l.stats.unknown_ouch_refs;
      return;
    }
    n->cum = old->cum;
    n->notional = old->notional;
    n->leaves = m.quantity;
    n->qty = n->cum + n->leaves;
    n->order_id = m.order_reference_number;
    n->state = Order::kLive;
    old->state = Order::kDone;
    old->leaves = 0;
    old->replace_ref = 0;
    g.exec_report(l, *n, '5', n->status(), n->id(), old->id());
  }

  void on(const ouch::Rejected& m) {
    Order* o = g.order(l, m.user_ref_num);
    if (o == nullptr) {
      ++l.stats.unknown_ouch_refs;
      return;
    }
    char text[48];
    const int n = std::snprintf(text, sizeof text, "simex reject code 0x%04X", m.reason);
    const std::string_view why{text, static_cast<std::size_t>(n)};
    if (o->state == Order::kPendingReplacement) {
      Order* old = g.order(l, o->orig_ref);
      o->state = Order::kDone;
      if (old != nullptr) old->replace_ref = 0;
      const bool late = old == nullptr || old->state == Order::kDone;
      g.cancel_reject(l, o->id(), old != nullptr ? old->id() : std::string_view{}, old, '2',
                      late ? 0 : 99, late ? "too late to replace" : why);
      return;
    }
    o->state = Order::kDone;
    o->leaves = 0;
    int reason = 99;
    if (m.reason == ouch::reject::kInvalidSymbol) reason = 1;
    if (m.reason == ouch::reject::kInvalidQuantity) reason = 13;
    ++l.stats.order_rejects;
    fix::Writer& w = l.fix.begin(fix::msg_type::kExecutionReport);
    w.add(tag::kOrderID, "NONE");
    w.add(tag::kClOrdID, o->id());
    w.add_uint(tag::kExecID, ++l.exec_id);
    w.add(tag::kExecType, '8');
    w.add(tag::kOrdStatus, '8');
    w.add(tag::kSymbol, trim({o->symbol.data(), o->symbol.size()}));
    w.add(tag::kSide, o->side);
    w.add_uint(tag::kOrderQty, o->qty);
    w.add_uint(tag::kLeavesQty, 0);
    w.add_uint(tag::kCumQty, 0);
    w.add(tag::kAvgPx, '0');
    w.add_time(tag::kTransactTime, lab::wall_ns());
    w.add_uint(tag::kOrdRejReason, static_cast<std::uint64_t>(reason));
    w.add(tag::kText, why);
    l.fix.send();
    ++l.stats.exec_reports;
  }
};

void Gateway::ouch_message(Link& l, std::span<const std::byte> msg, std::uint64_t rx_tsc) {
  OuchHandler h{*this, l};
  if (ouch::dispatch_outbound(msg.data(), msg.size(), h) != ouch::DispatchResult::kOk) return;
  if (msg.empty() || static_cast<char>(msg[0]) == ouch::SystemEvent::kType) return;
  ouch_to_fix_.record(lab::rdtsc() - rx_tsc);
}

// ---- FIX side ---------------------------------------------------------------

Gateway::Order* Gateway::order(Link& l, std::uint32_t ref) {
  if (ref < l.base_ref || ref - l.base_ref >= l.orders.size()) return nullptr;
  Order& o = l.orders[ref - l.base_ref];
  return o.state == Order::kFree ? nullptr : &o;
}

Gateway::Order* Gateway::new_order_slot(Link& l, std::uint32_t& ref) {
  if (l.next_ref - l.base_ref >= l.orders.size()) return nullptr;
  ref = l.next_ref++;
  Order& o = l.orders[ref - l.base_ref];
  o = Order{};
  return &o;
}

Gateway::Order* Gateway::find_cl_ord_id(Link& l, std::string_view id) {
  const std::size_t mask = l.ids.size() - 1;
  for (std::size_t i = fnv1a(id) & mask;; i = (i + 1) & mask) {
    const std::uint32_t v = l.ids[i];
    if (v == 0) return nullptr;
    Order& o = l.orders[v - 1];
    if (o.id() == id) return &o;
  }
}

bool Gateway::index_cl_ord_id(Link& l, std::uint32_t ref) {
  const Order& o = l.orders[ref - l.base_ref];
  const std::size_t mask = l.ids.size() - 1;
  for (std::size_t i = fnv1a(o.id()) & mask, n = 0; n < l.ids.size(); i = (i + 1) & mask, ++n) {
    if (l.ids[i] == 0) {
      l.ids[i] = ref - l.base_ref + 1;
      return true;
    }
  }
  return false;
}

void Gateway::exec_report(Link& l, const Order& o, char exec_type, char ord_status,
                          std::string_view cl_ord_id, std::string_view orig_cl_ord_id,
                          std::uint32_t last_qty, std::uint32_t last_px) {
  fix::Writer& w = l.fix.begin(fix::msg_type::kExecutionReport);
  if (o.order_id != 0) {
    w.add_uint(tag::kOrderID, o.order_id);
  } else {
    w.add(tag::kOrderID, "NONE");
  }
  w.add(tag::kClOrdID, cl_ord_id);
  if (!orig_cl_ord_id.empty()) w.add(tag::kOrigClOrdID, orig_cl_ord_id);
  w.add_uint(tag::kExecID, ++l.exec_id);
  w.add(tag::kExecType, exec_type);
  w.add(tag::kOrdStatus, ord_status);
  w.add(tag::kSymbol, trim({o.symbol.data(), o.symbol.size()}));
  w.add(tag::kSide, o.side);
  w.add_uint(tag::kOrderQty, o.qty);
  if (o.ord_type == '2') w.add_price(tag::kPrice, o.price);
  w.add(tag::kOrdType, o.ord_type);
  w.add(tag::kTimeInForce, o.tif);
  if (exec_type == 'F') {
    w.add_uint(tag::kLastQty, last_qty);
    w.add_price(tag::kLastPx, last_px);
  }
  w.add_uint(tag::kLeavesQty, o.leaves);
  w.add_uint(tag::kCumQty, o.cum);
  w.add_price(tag::kAvgPx, o.cum == 0 ? 0 : (o.notional + o.cum / 2) / o.cum);
  w.add_time(tag::kTransactTime, lab::wall_ns());
  l.fix.send();
  ++l.stats.exec_reports;
}

void Gateway::reject_order(Link& l, const fix::Message& m, int reason, std::string_view text) {
  fix::Writer& w = l.fix.begin(fix::msg_type::kExecutionReport);
  w.add(tag::kOrderID, "NONE");
  w.add(tag::kClOrdID, m.get(tag::kClOrdID).value_or(""));
  w.add_uint(tag::kExecID, ++l.exec_id);
  w.add(tag::kExecType, '8');
  w.add(tag::kOrdStatus, '8');
  w.add(tag::kSymbol, m.get(tag::kSymbol).value_or(""));
  w.add(tag::kSide, m.get(tag::kSide).value_or(""));
  if (const auto q = m.get(tag::kOrderQty)) w.add(tag::kOrderQty, *q);
  w.add_uint(tag::kLeavesQty, 0);
  w.add_uint(tag::kCumQty, 0);
  w.add(tag::kAvgPx, '0');
  w.add_time(tag::kTransactTime, lab::wall_ns());
  w.add_uint(tag::kOrdRejReason, static_cast<std::uint64_t>(reason));
  if (!text.empty()) w.add(tag::kText, text);
  l.fix.send();
  ++l.stats.exec_reports;
  ++l.stats.order_rejects;
}

void Gateway::cancel_reject(Link& l, std::string_view cl_ord_id, std::string_view orig_cl_ord_id,
                            const Order* o, char response_to, int reason, std::string_view text) {
  fix::Writer& w = l.fix.begin(fix::msg_type::kOrderCancelReject);
  if (o != nullptr && o->order_id != 0) {
    w.add_uint(tag::kOrderID, o->order_id);
  } else {
    w.add(tag::kOrderID, "NONE");
  }
  w.add(tag::kClOrdID, cl_ord_id);
  w.add(tag::kOrigClOrdID, orig_cl_ord_id);
  w.add(tag::kOrdStatus, o != nullptr ? o->status() : '8');
  w.add(tag::kCxlRejResponseTo, response_to);
  w.add_uint(tag::kCxlRejReason, static_cast<std::uint64_t>(reason));
  if (!text.empty()) w.add(tag::kText, text);
  l.fix.send();
  ++l.stats.cancel_rejects;
}

void Gateway::session_reject(Link& l, const fix::Message& m, int ref_tag, int reason,
                             std::string_view text) {
  fix::Writer& w = l.fix.begin(fix::msg_type::kReject);
  w.add_uint(tag::kRefSeqNum, m.get_uint(tag::kMsgSeqNum).value_or(0));
  w.add_uint(tag::kRefTagID, static_cast<std::uint64_t>(ref_tag));
  w.add(tag::kRefMsgType, m.type());
  w.add_uint(tag::kSessionRejectReason, static_cast<std::uint64_t>(reason));
  if (!text.empty()) w.add(tag::kText, text);
  l.fix.send();
  ++l.stats.session_rejects;
}

void Gateway::on_app(Link& l, const fix::Message& m) {
  const std::uint64_t t0 = lab::rdtsc();
  switch (m.type()) {
    case fix::msg_type::kNewOrderSingle:
      on_new(l, m);
      break;
    case fix::msg_type::kOrderCancelRequest:
      on_cancel(l, m);
      break;
    case fix::msg_type::kOrderCancelReplaceRequest:
      on_replace(l, m);
      break;
    default: {
      fix::Writer& w = l.fix.begin(fix::msg_type::kBusinessMessageReject);
      w.add_uint(tag::kRefSeqNum, m.get_uint(tag::kMsgSeqNum).value_or(0));
      w.add(tag::kRefMsgType, m.type());
      w.add(tag::kBusinessRejectReason, '3');
      w.add(tag::kText, "unsupported message type");
      l.fix.send();
      ++l.stats.business_rejects;
      return;
    }
  }
  fix_to_ouch_.record(lab::rdtsc() - t0);
}

namespace {

// The first required tag missing from `m`, or 0.
int missing(const fix::Message& m, std::initializer_list<int> tags) {
  for (const int t : tags) {
    if (!m.get(t)) return t;
  }
  return 0;
}

}  // namespace

void Gateway::on_new(Link& l, const fix::Message& m) {
  ++l.stats.new_orders;
  if (const int t = missing(m, {tag::kClOrdID, tag::kSymbol, tag::kSide, tag::kTransactTime,
                                tag::kOrderQty, tag::kOrdType})) {
    session_reject(l, m, t, 1, "required tag missing");
    return;
  }
  const std::string_view id = *m.get(tag::kClOrdID);
  const std::string_view symbol = *m.get(tag::kSymbol);
  const auto qty = m.get_uint(tag::kOrderQty);
  if (id.empty() || id.size() > kMaxClOrdId) {
    session_reject(l, m, tag::kClOrdID, 5, "ClOrdID must be 1 to 20 characters");
    return;
  }
  if (!qty) {
    session_reject(l, m, tag::kOrderQty, 6, "OrderQty must be an integer");
    return;
  }
  if (find_cl_ord_id(l, id) != nullptr) {
    reject_order(l, m, 6, "duplicate ClOrdID");
    return;
  }
  const char side = m.get_char(tag::kSide).value_or(0);
  if (ouch_side(side) == 0) {
    reject_order(l, m, 11, "Side must be 1, 2, 5 or 6");
    return;
  }
  const char type = m.get_char(tag::kOrdType).value_or(0);
  if (type != '1' && type != '2') {
    reject_order(l, m, 11, "OrdType must be 1 (market) or 2 (limit)");
    return;
  }
  std::uint64_t price = ouch::kMarketPrice;
  if (type == '2') {
    const auto p = m.get(tag::kPrice);
    if (!p) {
      reject_order(l, m, 99, "Price is required for a limit order");
      return;
    }
    const auto v = fix::parse_price(*p);
    if (!v) {
      session_reject(l, m, tag::kPrice, 6, "Price must be a decimal with at most 4 places");
      return;
    }
    price = *v;
  }
  char tif = m.get_char(tag::kTimeInForce).value_or('0');
  if (tif != '0' && tif != '3') {
    reject_order(l, m, 11, "TimeInForce must be 0 (day) or 3 (IOC)");
    return;
  }
  if (type == '1') tif = '3';
  if (symbol.empty() || symbol.size() > 8) {
    reject_order(l, m, 1, "unknown symbol");
    return;
  }
  if (l.ouch_ended) {
    reject_order(l, m, 2, "exchange closed");
    return;
  }
  std::uint32_t ref = 0;
  Order* o = new_order_slot(l, ref);
  if (o == nullptr) {
    reject_order(l, m, 0, "order table full for the day");
    return;
  }
  std::copy(id.begin(), id.end(), o->cl_ord_id.begin());
  o->cl_len = static_cast<std::uint8_t>(id.size());
  o->symbol = ouch::make_alpha<8>(symbol);
  o->qty = static_cast<std::uint32_t>(std::min<std::uint64_t>(*qty, 0xFFFFFFFFu));
  o->leaves = o->qty;
  o->price = price == ouch::kMarketPrice ? 0 : static_cast<std::uint32_t>(price);
  o->side = side;
  o->ord_type = type;
  o->tif = tif;
  o->state = Order::kPendingNew;
  index_cl_ord_id(l, ref);

  ouch::EnterOrder e;
  e.user_ref_num = ref;
  e.side = ouch_side(side);
  e.quantity = o->qty;
  e.symbol = o->symbol;
  e.price = price;
  e.time_in_force = tif;
  e.cl_ord_id = ouch::make_alpha<14>(id);
  std::array<std::byte, ouch::EnterOrder::kSize> buf;
  ouch::encode(e, buf.data());
  send_ouch(l, buf);
}

void Gateway::on_cancel(Link& l, const fix::Message& m) {
  ++l.stats.cancels;
  if (const int t = missing(m, {tag::kOrigClOrdID, tag::kClOrdID, tag::kSide})) {
    session_reject(l, m, t, 1, "required tag missing");
    return;
  }
  const std::string_view id = *m.get(tag::kClOrdID);
  const std::string_view orig = *m.get(tag::kOrigClOrdID);
  if (id.empty() || id.size() > kMaxClOrdId) {
    session_reject(l, m, tag::kClOrdID, 5, "ClOrdID must be 1 to 20 characters");
    return;
  }
  Order* o = find_cl_ord_id(l, orig);
  if (o == nullptr) {
    cancel_reject(l, id, orig, nullptr, '1', 1, "unknown order");
    return;
  }
  if (o->state == Order::kDone) {
    cancel_reject(l, id, orig, o, '1', 0, "too late to cancel");
    return;
  }
  if (o->state == Order::kPendingReplacement || o->cancel_pending || o->replace_ref != 0) {
    cancel_reject(l, id, orig, o, '1', 3, "a cancel or replace is already pending");
    return;
  }
  if (find_cl_ord_id(l, id) != nullptr) {
    cancel_reject(l, id, orig, o, '1', 6, "duplicate ClOrdID");
    return;
  }
  std::copy(id.begin(), id.end(), o->cxl_id.begin());
  o->cxl_len = static_cast<std::uint8_t>(id.size());
  o->cancel_pending = true;
  ouch::CancelOrder c;
  c.user_ref_num = static_cast<std::uint32_t>(o - l.orders.data()) + l.base_ref;
  c.quantity = 0;
  std::array<std::byte, ouch::CancelOrder::kSize> buf;
  ouch::encode(c, buf.data());
  send_ouch(l, buf);
}

void Gateway::on_replace(Link& l, const fix::Message& m) {
  ++l.stats.replaces;
  if (const int t = missing(
          m, {tag::kOrigClOrdID, tag::kClOrdID, tag::kSide, tag::kOrderQty, tag::kOrdType})) {
    session_reject(l, m, t, 1, "required tag missing");
    return;
  }
  const std::string_view id = *m.get(tag::kClOrdID);
  const std::string_view orig = *m.get(tag::kOrigClOrdID);
  if (id.empty() || id.size() > kMaxClOrdId) {
    session_reject(l, m, tag::kClOrdID, 5, "ClOrdID must be 1 to 20 characters");
    return;
  }
  const auto qty = m.get_uint(tag::kOrderQty);
  if (!qty) {
    session_reject(l, m, tag::kOrderQty, 6, "OrderQty must be an integer");
    return;
  }
  Order* o = find_cl_ord_id(l, orig);
  if (o == nullptr) {
    cancel_reject(l, id, orig, nullptr, '2', 1, "unknown order");
    return;
  }
  if (o->state == Order::kDone) {
    cancel_reject(l, id, orig, o, '2', 0, "too late to replace");
    return;
  }
  if (o->state == Order::kPendingReplacement || o->cancel_pending || o->replace_ref != 0) {
    cancel_reject(l, id, orig, o, '2', 3, "a cancel or replace is already pending");
    return;
  }
  if (find_cl_ord_id(l, id) != nullptr) {
    cancel_reject(l, id, orig, o, '2', 6, "duplicate ClOrdID");
    return;
  }
  const auto symbol = m.get(tag::kSymbol);
  if (m.get_char(tag::kSide) != o->side ||
      (symbol && *symbol != trim({o->symbol.data(), o->symbol.size()}))) {
    cancel_reject(l, id, orig, o, '2', 99, "Side and Symbol cannot change");
    return;
  }
  if (m.get_char(tag::kOrdType) != '2') {
    cancel_reject(l, id, orig, o, '2', 99, "only limit orders can be replaced");
    return;
  }
  const auto p = m.get(tag::kPrice);
  const auto price = p ? fix::parse_price(*p) : std::nullopt;
  if (!price) {
    session_reject(l, m, tag::kPrice, p ? 6 : 1, "Price must be a decimal with at most 4 places");
    return;
  }
  const char tif = m.get_char(tag::kTimeInForce).value_or('0');
  if (tif != '0' && tif != '3') {
    cancel_reject(l, id, orig, o, '2', 99, "TimeInForce must be 0 (day) or 3 (IOC)");
    return;
  }
  const std::uint32_t orig_ref = static_cast<std::uint32_t>(o - l.orders.data()) + l.base_ref;
  std::uint32_t ref = 0;
  Order* n = new_order_slot(l, ref);
  if (n == nullptr) {
    cancel_reject(l, id, orig, o, '2', 99, "order table full for the day");
    return;
  }
  std::copy(id.begin(), id.end(), n->cl_ord_id.begin());
  n->cl_len = static_cast<std::uint8_t>(id.size());
  n->symbol = o->symbol;
  n->qty = static_cast<std::uint32_t>(std::min<std::uint64_t>(*qty, 0xFFFFFFFFu));
  n->price = static_cast<std::uint32_t>(*price);
  n->side = o->side;
  n->ord_type = '2';
  n->tif = tif;
  n->orig_ref = orig_ref;
  n->state = Order::kPendingReplacement;
  index_cl_ord_id(l, ref);
  o->replace_ref = ref;

  ouch::ReplaceOrder r;
  r.orig_user_ref_num = orig_ref;
  r.user_ref_num = ref;
  r.quantity = n->qty;
  r.price = *price;
  r.time_in_force = tif;
  r.cl_ord_id = ouch::make_alpha<14>(id);
  std::array<std::byte, ouch::ReplaceOrder::kSize> buf;
  ouch::encode(r, buf.data());
  send_ouch(l, buf);
}

void Gateway::on_fix_disconnect(Link& l) {
  if (!cfg_.cancel_on_disconnect || l.ouch_ended) return;
  for (std::uint32_t i = 0; i < l.next_ref - l.base_ref; ++i) {
    const Order& o = l.orders[i];
    if (o.state != Order::kPendingNew && o.state != Order::kLive &&
        o.state != Order::kPendingReplacement) {
      continue;
    }
    ouch::CancelOrder c;
    c.user_ref_num = l.base_ref + i;
    c.quantity = 0;
    std::array<std::byte, ouch::CancelOrder::kSize> buf;
    ouch::encode(c, buf.data());
    send_ouch(l, buf);
    ++l.stats.disconnect_cancels;
  }
}

// ---- Connections ------------------------------------------------------------

void Gateway::accept_new(std::uint64_t now) {
  while (auto s = listener_.accept()) {
    auto p = std::make_unique<Pending>();
    p->sock = std::move(*s);
    p->opened_ns = now;
    pending_.push_back(std::move(p));
    ++connections_;
  }
}

void Gateway::route_pending(std::uint64_t now) {
  for (auto& pp : pending_) {
    Pending& p = *pp;
    const auto n = p.sock.recv(std::as_writable_bytes(p.in->write_area()));
    if (n < 0 || now - p.opened_ns > kLogonWaitNs || p.in->stuck()) {
      p.done = true;
      ++refused_;
      continue;
    }
    if (n > 0) p.in->commit(static_cast<std::size_t>(n));
    const auto wire = p.in->next();
    if (!wire) continue;
    p.done = true;
    fix::Message logon;
    Link* link = nullptr;
    if (logon.parse(*wire) && logon.type() == fix::msg_type::kLogon &&
        logon.get(tag::kTargetCompID) == cfg_.comp_id) {
      for (auto& l : links_) {
        if (logon.get(tag::kSenderCompID) == l->cfg.comp_id) link = l.get();
      }
    }
    // Unknown CompIDs, and a second connection for an active session, are
    // dropped without an answer: there is no session to answer in.
    if (link == nullptr || link->fix.state() != fix::Session::State::kDisconnected) {
      ++refused_;
      continue;
    }
    FixHandler h{*this, *link};
    link->fix.accept(std::move(p.sock), std::move(p.in), logon, now, h);
  }
  std::erase_if(pending_, [](const std::unique_ptr<Pending>& p) { return p->done; });
}

void Gateway::run(const std::atomic<bool>& stop) {
  lab::set_thread_name("fixgw");
  while (!stop.load(std::memory_order_acquire)) {
    const std::uint64_t now = lab::mono_ns();
    accept_new(now);
    route_pending(now);
    bool all_ended = true;
    for (auto& lp : links_) {
      Link& l = *lp;
      FixHandler h{*this, l};
      l.fix.poll(now, h);
      read_ouch(l, now);
      all_ended = all_ended && l.ouch_ended;
    }
    if (all_ended) {
      simex_ended_ = true;
      break;
    }
  }
  // Log every FIX session out and give the answers a moment.
  for (auto& lp : links_) {
    lp->fix.logout(simex_ended_ ? "simex closed" : "fixgw stopping", lab::mono_ns());
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
  while (std::chrono::steady_clock::now() < deadline) {
    bool any = false;
    for (auto& lp : links_) {
      FixHandler h{*this, *lp};
      lp->fix.poll(lab::mono_ns(), h);
      any = any || lp->fix.state() != fix::Session::State::kDisconnected;
    }
    if (!any) break;
  }
  for (auto& lp : links_) {
    FixHandler h{*this, *lp};
    lp->fix.close(h);
  }
}

bool Gateway::report(std::FILE* out) const {
  const auto clock = lab::TscClock::calibrate(std::chrono::milliseconds{20});
  bool ok = true;
  std::fprintf(out, "fixgw: %llu connections, %llu refused, simex %s\n",
               static_cast<unsigned long long>(connections_),
               static_cast<unsigned long long>(refused_),
               simex_ended_ ? "ended the session" : "up");
  std::fprintf(out, "%-10s %8s %8s %8s %8s %8s %8s %8s %8s %8s %8s\n", "session", "new", "cancel",
               "replace", "reports", "fills", "ord_rej", "cxl_rej", "sess_rej", "resent",
               "unknown");
  for (const auto& lp : links_) {
    const LinkStats& s = lp->stats;
    const auto& f = lp->fix.stats();
    std::fprintf(
        out, "%-10s %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu\n",
        lp->cfg.comp_id.c_str(), static_cast<unsigned long long>(s.new_orders),
        static_cast<unsigned long long>(s.cancels), static_cast<unsigned long long>(s.replaces),
        static_cast<unsigned long long>(s.exec_reports), static_cast<unsigned long long>(s.fills),
        static_cast<unsigned long long>(s.order_rejects),
        static_cast<unsigned long long>(s.cancel_rejects),
        static_cast<unsigned long long>(s.session_rejects),
        static_cast<unsigned long long>(f.resent),
        static_cast<unsigned long long>(s.unknown_ouch_refs));
    if (s.unknown_ouch_refs != 0) ok = false;
  }
  const double scale = 1.0 / clock.ticks_per_ns();
  fix_to_ouch_.print(out, "FIX in to OUCH out", scale);
  ouch_to_fix_.print(out, "OUCH in to FIX out", scale);
  return ok;
}

}  // namespace fixgw
