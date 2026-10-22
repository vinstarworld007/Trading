#include "gateway.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "lab/core/clock.hpp"
#include "lab/core/compiler.hpp"
#include "lab/ouch/codec.hpp"
#include "lab/ouch/drop_copy.hpp"

namespace simex {

namespace ouch = lab::ouch;
namespace soup = lab::soup;

namespace {

bool same_ignoring_case(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::toupper(static_cast<unsigned char>(x)) ==
                  std::toupper(static_cast<unsigned char>(y));
         });
}

}  // namespace

Gateway::Gateway(const GatewayConfig& cfg, CommandRing& to_matcher, SessionRing& from_matcher)
    : cfg_{cfg},
      to_matcher_{to_matcher},
      from_matcher_{from_matcher},
      listener_{lab::net::TcpListener::listen(lab::net::parse_endpoint(cfg.listen))} {
  accounts_.reserve(cfg.accounts.size() + 1);
  std::uint16_t owner = 1;
  for (const auto& a : cfg.accounts) {
    Account acc;
    acc.cfg = a;
    acc.owner = owner++;
    acc.stream.reserve(64u << 20);
    acc.offsets.reserve(1u << 20);
    acc.orders.reserve(1u << 16);
    accounts_.push_back(std::move(acc));
  }
  if (!cfg.drop_copy.user.empty()) {
    Account acc;
    acc.cfg = cfg.drop_copy;
    acc.owner = owner++;  // never on an order: drop copy sends none
    acc.drop_copy = true;
    acc.stream.reserve(64u << 20);
    acc.offsets.reserve(1u << 20);
    accounts_.push_back(std::move(acc));
    drop_ = &accounts_.back();
  }
  // Every account's stream opens with Start of Day.
  for (auto& a : accounts_) {
    ouch::SystemEvent s;
    s.timestamp = feed_time();
    s.event_code = 'S';
    std::array<std::byte, ouch::SystemEvent::kSize> buf;
    ouch::encode(s, buf.data());
    sequence(a, buf);
  }
}

std::string Gateway::address() const { return listener_.local().str(); }

std::uint64_t Gateway::feed_time() const {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_r(&t, &local);
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  return (static_cast<std::uint64_t>(local.tm_hour) * 3600 +
          static_cast<std::uint64_t>(local.tm_min) * 60 +
          static_cast<std::uint64_t>(local.tm_sec)) *
             1'000'000'000ULL +
         static_cast<std::uint64_t>(ns % 1'000'000'000);
}

void Gateway::push(const Command& c) {
  while (!to_matcher_.try_push(c)) {
  }
}

void Gateway::run(const std::atomic<bool>& accepting, std::atomic<bool>& intake_stopped,
                  const std::atomic<bool>& stop) {
  while (!stop.load(std::memory_order_acquire)) {
    const std::uint64_t now = lab::mono_ns();
    open_ = accepting.load(std::memory_order_acquire);
    accept_new(now);
    for (auto& c : conns_) read(*c, now, open_);
    if (!open_) intake_stopped.store(true, std::memory_order_release);
    drain_events();
    housekeeping(now);
  }
  // Last responses, End of Day, End of Session.
  drain_events();
  for (auto& a : accounts_) {
    ouch::SystemEvent e;
    e.timestamp = feed_time();
    e.event_code = 'E';
    std::array<std::byte, ouch::SystemEvent::kSize> buf;
    ouch::encode(e, buf.data());
    sequence(a, buf);
  }
  for (auto& c : conns_) {
    if (c->account == nullptr || c->dead) continue;
    std::array<std::byte, soup::kHeaderSize> z;
    soup::write_empty(z.data(), soup::kEndOfSession);
    send_packet(*c, z);
    // Give the kernel a moment to take what is queued.
    for (int i = 0; i < 1000 && c->sock.queued() > 0 && c->sock.flush(); ++i) {
    }
  }
  intake_stopped.store(true, std::memory_order_release);
}

void Gateway::accept_new(std::uint64_t now) {
  while (auto s = listener_.accept()) {
    auto c = std::make_unique<Conn>();
    c->sock = std::move(*s);
    c->in = std::make_unique<soup::StreamParser>();
    c->opened_ns = c->last_rx_ns = c->last_tx_ns = now;
    conns_.push_back(std::move(c));
    ++stats_.connections;
  }
}

void Gateway::read(Conn& c, std::uint64_t now, bool accepting) {
  if (c.dead) return;
  const auto area = c.in->write_area();
  const ssize_t n = c.sock.recv(area);
  if (n < 0) {
    close(c);
    return;
  }
  if (n > 0) {
    c.in->commit(static_cast<std::size_t>(n));
    c.last_rx_ns = now;
  }
  while (auto p = c.in->next()) {
    if (c.account == nullptr) {
      if (p->type == soup::kLoginRequest) {
        login(c, p->payload, now);
        if (c.dead) return;
      } else if (p->type != soup::kDebug) {
        close(c);  // anything but a login first is a protocol error
        return;
      }
      continue;
    }
    switch (p->type) {
      case soup::kUnsequencedData:
        if (accepting) inbound(*c.account, p->payload);
        break;
      case soup::kLogoutRequest:
        close(c);
        return;
      default:  // heartbeats and debug
        break;
    }
  }
  if (c.in->malformed()) close(c);
}

void Gateway::login(Conn& c, std::span<const std::byte> payload, std::uint64_t now) {
  const auto req = soup::parse_login_request(payload);
  Account* acc = nullptr;
  char reject = soup::kRejectNotAuthorized;
  if (req) {
    for (auto& a : accounts_) {
      if (same_ignoring_case(req->user, a.cfg.user) &&
          same_ignoring_case(req->password, a.cfg.password)) {
        acc = &a;
      }
    }
    if (acc != nullptr && !req->session.empty() && req->session != cfg_.session) {
      acc = nullptr;
      reject = soup::kRejectSessionUnavailable;
    }
  }
  if (acc == nullptr) {
    std::array<std::byte, soup::kLoginRejectedSize> buf;
    soup::write_login_rejected(buf.data(), reject);
    send_packet(c, buf);
    c.sock.flush();
    ++stats_.login_rejects;
    close(c);
    return;
  }
  if (acc->conn != nullptr) {  // a new login takes the account over, orders and all
    Conn* old = acc->conn;
    acc->conn = nullptr;
    old->account = nullptr;
    close(*old);
  }
  c.account = acc;
  acc->conn = &c;
  ++stats_.logins;
  const std::uint64_t count = acc->offsets.size();
  const std::uint64_t first =
      req->next_sequence == 0 ? count + 1 : std::min<std::uint64_t>(req->next_sequence, count + 1);
  std::array<std::byte, soup::kLoginAcceptedSize> buf;
  soup::write_login_accepted(buf.data(), cfg_.session, first);
  send_packet(c, buf);
  send_from(c, *acc, first);
  c.last_rx_ns = now;
}

void Gateway::reject(Account& a, std::uint32_t ref, std::uint16_t reason, const ouch::ClOrdId& id) {
  ouch::Rejected j;
  j.timestamp = feed_time();
  j.user_ref_num = ref;
  j.reason = reason;
  j.cl_ord_id = id;
  std::array<std::byte, ouch::Rejected::kSize> buf;
  ouch::encode(j, buf.data());
  sequence(a, buf);
  ++stats_.gateway_rejects;
}

void Gateway::inbound(Account& a, std::span<const std::byte> msg) {
  if (a.drop_copy) return;  // a drop copy session only listens
  ++stats_.inbound;
  struct Handler : ouch::HandlerBase {
    Gateway& g;
    Account& a;
    Handler(Gateway& gw, Account& acc) : g{gw}, a{acc} {}
    using HandlerBase::on;

    // Common checks for a new order or replacement. Returns false if rejected.
    bool admit(std::uint32_t ref, std::uint32_t qty, std::uint64_t price, const ouch::ClOrdId& id) {
      if (ref <= a.last_ref) {  // not new: a retransmission, ignored per the spec
        ++g.stats_.stale_refs;
        return false;
      }
      a.last_ref = ref;
      if (qty == 0 || qty > lab::match::kMaxQuantity) {
        g.reject(a, ref, ouch::reject::kInvalidQuantity, id);
        return false;
      }
      if (price == 0 || (price > ouch::kMaxPrice && price != ouch::kMarketPrice)) {
        g.reject(a, ref, ouch::reject::kInvalidPrice, id);
        return false;
      }
      return true;
    }

    void on(const ouch::EnterOrder& m) {
      if (!admit(m.user_ref_num, m.quantity, m.price, m.cl_ord_id)) return;
      char side;
      switch (m.side) {
        case ouch::side::kBuy:
          side = 'B';
          break;
        case ouch::side::kSell:
        case ouch::side::kSellShort:
        case ouch::side::kSellShortExempt:
          side = 'S';
          break;
        default:
          g.reject(a, m.user_ref_num, ouch::reject::kInvalidSide, m.cl_ord_id);
          return;
      }
      Command c{};
      c.type = Command::kEnter;
      c.owner = a.owner;
      c.ref = m.user_ref_num;
      c.symbol = m.symbol;
      c.side = side;
      c.qty = m.quantity;
      c.tif = m.time_in_force == ouch::tif::kIoc ? ouch::tif::kIoc : ouch::tif::kDay;
      if (m.price == ouch::kMarketPrice) {  // market orders never rest
        c.price = side == 'B' ? lab::match::kMarketBuyPrice : lab::match::kMarketSellPrice;
        c.tif = ouch::tif::kIoc;
      } else {
        c.price = static_cast<std::uint32_t>(m.price);
      }
      c.rx_tsc = lab::rdtsc();
      a.orders[m.user_ref_num] = OrderInfo{m.cl_ord_id, m.side, m.display, m.capacity};
      g.push(c);
    }

    void on(const ouch::ReplaceOrder& m) {
      if (!admit(m.user_ref_num, m.quantity, m.price, m.cl_ord_id)) return;
      if (m.price == ouch::kMarketPrice) {
        g.reject(a, m.user_ref_num, ouch::reject::kInvalidPrice, m.cl_ord_id);
        return;
      }
      const auto orig = a.orders.find(m.orig_user_ref_num);
      if (orig == a.orders.end()) {
        g.reject(a, m.user_ref_num, ouch::reject::kReplaceNotAllowed, m.cl_ord_id);
        return;
      }
      Command c{};
      c.type = Command::kReplace;
      c.owner = a.owner;
      c.ref = m.user_ref_num;
      c.orig_ref = m.orig_user_ref_num;
      c.qty = m.quantity;
      c.price = static_cast<std::uint32_t>(m.price);
      c.tif = m.time_in_force == ouch::tif::kIoc ? ouch::tif::kIoc : ouch::tif::kDay;
      c.rx_tsc = lab::rdtsc();
      a.orders[m.user_ref_num] =
          OrderInfo{m.cl_ord_id, orig->second.side, m.display, orig->second.capacity};
      g.push(c);
    }

    void on(const ouch::CancelOrder& m) {
      Command c{};
      c.type = Command::kCancel;
      c.owner = a.owner;
      c.ref = m.user_ref_num;
      c.qty = m.quantity;
      c.rx_tsc = lab::rdtsc();
      g.push(c);
    }

    void on(const ouch::AccountQuery&) {
      ouch::AccountQueryResponse q;
      q.timestamp = g.feed_time();
      q.next_user_ref_num = a.last_ref + 1;
      std::array<std::byte, ouch::AccountQueryResponse::kSize> buf;
      ouch::encode(q, buf.data());
      g.sequence(a, buf);
    }
  } h{*this, a};
  if (ouch::dispatch_inbound(msg.data(), msg.size(), h) != ouch::DispatchResult::kOk) {
    ++stats_.bad_messages;
  }
}

void Gateway::drain_events() {
  SessionEvent e;
  while (from_matcher_.try_pop(e)) {
    if (e.owner == 0 || e.owner > accounts_.size()) continue;
    Account& a = accounts_[e.owner - 1];
    const auto info = a.orders.find(e.user_ref);
    const OrderInfo oi = info != a.orders.end()
                             ? info->second
                             : OrderInfo{ouch::make_alpha<14>(""), e.side, 'Y', 'P'};
    std::array<std::byte, ouch::kMaxMessageSize> buf;
    std::size_t n = 0;
    bool done = false;  // the order is finished: forget its ClOrdID
    switch (e.type) {
      case SessionEvent::kAccepted: {
        ouch::OrderAccepted m;
        m.timestamp = e.ts_ns;
        m.user_ref_num = e.user_ref;
        m.side = oi.side;
        m.quantity = e.qty;
        m.symbol = e.symbol;
        m.price = e.price;
        m.time_in_force = e.tif;
        m.display = oi.display;
        m.order_reference_number = e.orn;
        m.capacity = oi.capacity;
        m.order_state = 'L';
        m.cl_ord_id = oi.cl_ord_id;
        n = ouch::encode(m, buf.data());
        break;
      }
      case SessionEvent::kReplaced: {
        ouch::OrderReplaced m;
        m.timestamp = e.ts_ns;
        m.orig_user_ref_num = e.orig_user_ref;
        m.user_ref_num = e.user_ref;
        m.side = oi.side;
        m.quantity = e.qty;
        m.symbol = e.symbol;
        m.price = e.price;
        m.time_in_force = e.tif;
        m.display = oi.display;
        m.order_reference_number = e.orn;
        m.capacity = oi.capacity;
        m.order_state = 'L';
        m.cl_ord_id = oi.cl_ord_id;
        n = ouch::encode(m, buf.data());
        a.orders.erase(e.orig_user_ref);
        break;
      }
      case SessionEvent::kCanceled: {
        ouch::OrderCanceled m;
        m.timestamp = e.ts_ns;
        m.user_ref_num = e.user_ref;
        m.quantity = e.qty;
        m.reason = e.reason;
        n = ouch::encode(m, buf.data());
        done = e.remaining == 0;
        break;
      }
      case SessionEvent::kExecuted: {
        ouch::OrderExecuted m;
        m.timestamp = e.ts_ns;
        m.user_ref_num = e.user_ref;
        m.quantity = e.qty;
        m.price = e.price;
        m.liquidity_flag = e.liquidity;
        m.match_number = e.match;
        n = ouch::encode(m, buf.data());
        done = e.remaining == 0;
        drop_copy(a, e, oi);
        break;
      }
      case SessionEvent::kRejected: {
        ouch::Rejected m;
        m.timestamp = e.ts_ns;
        m.user_ref_num = e.user_ref;
        m.reason = e.reject_reason;
        m.cl_ord_id = oi.cl_ord_id;
        n = ouch::encode(m, buf.data());
        done = true;
        break;
      }
    }
    sequence(a, {buf.data(), n});
    if (done && info != a.orders.end()) a.orders.erase(info);
  }
}

void Gateway::drop_copy(const Account& a, const SessionEvent& e, const OrderInfo& oi) {
  if (drop_ == nullptr) return;
  lab::dropcopy::ExecutionReport r;
  r.timestamp = e.ts_ns;
  r.account = ouch::make_alpha<6>(a.cfg.user);
  r.user_ref_num = e.user_ref;
  r.side = oi.side == ouch::side::kBuy ? 'B' : 'S';
  r.quantity = e.qty;
  r.symbol = e.symbol;
  r.price = e.price;
  r.match_number = e.match;
  r.liquidity_flag = e.liquidity;
  r.order_reference_number = e.orn;
  std::array<std::byte, lab::dropcopy::ExecutionReport::kSize> buf;
  ouch::encode(r, buf.data());
  sequence(*drop_, buf);
  ++stats_.drop_copies;
}

void Gateway::sequence(Account& a, std::span<const std::byte> msg) {
  a.offsets.push_back(static_cast<std::uint32_t>(a.stream.size()));
  a.stream.insert(a.stream.end(), msg.begin(), msg.end());
  ++stats_.outbound;
  if (a.conn != nullptr && !a.conn->dead) {
    std::array<std::byte, soup::kHeaderSize + ouch::kMaxMessageSize> pkt;
    send_packet(*a.conn, {pkt.data(), soup::write_data(pkt.data(), soup::kSequencedData, msg)});
  }
}

void Gateway::send_from(Conn& c, const Account& a, std::uint64_t first_seq) {
  std::array<std::byte, soup::kHeaderSize + ouch::kMaxMessageSize> pkt;
  for (std::uint64_t s = first_seq; s <= a.offsets.size(); ++s) {
    const std::size_t begin = a.offsets[s - 1];
    const std::size_t end = s < a.offsets.size() ? a.offsets[s] : a.stream.size();
    const std::span<const std::byte> msg{a.stream.data() + begin, end - begin};
    send_packet(c, {pkt.data(), soup::write_data(pkt.data(), soup::kSequencedData, msg)});
  }
}

void Gateway::send_packet(Conn& c, std::span<const std::byte> bytes) {
  if (c.dead) return;
  if (!c.sock.send(bytes)) {
    close(c);
    return;
  }
  c.last_tx_ns = lab::mono_ns();
}

void Gateway::housekeeping(std::uint64_t now) {
  for (auto& cp : conns_) {
    Conn& c = *cp;
    if (c.dead) continue;
    if (!c.sock.flush()) {
      close(c);
      continue;
    }
    if (c.account == nullptr) {
      if (now - c.opened_ns > cfg_.login_timeout_ms * 1'000'000) {
        ++stats_.timeouts;
        close(c);
      }
      continue;
    }
    if (now - c.last_rx_ns > cfg_.idle_timeout_ms * 1'000'000) {
      ++stats_.timeouts;
      close(c);
      continue;
    }
    if (now - c.last_tx_ns > cfg_.heartbeat_ms * 1'000'000) {
      std::array<std::byte, soup::kHeaderSize> hb;
      soup::write_empty(hb.data(), soup::kServerHeartbeat);
      send_packet(c, hb);
    }
  }
  std::erase_if(conns_, [](const std::unique_ptr<Conn>& c) { return c->dead; });
}

void Gateway::close(Conn& c) {
  if (c.dead) return;
  c.dead = true;
  c.sock.close();
  if (c.account != nullptr) {
    Account& a = *c.account;
    if (a.conn == &c) {
      a.conn = nullptr;
      if (cfg_.cancel_on_disconnect && open_ && !a.drop_copy) {
        Command cmd{};
        cmd.type = Command::kCancelOwner;
        cmd.owner = a.owner;
        cmd.rx_tsc = lab::rdtsc();
        push(cmd);
      }
    }
  }
}

}  // namespace simex
