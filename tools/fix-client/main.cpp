// fix-client: drives fixgw with FIX 4.4 sessions that trade with each other
// and checks every answer.
//
//   fix-client --connect 127.0.0.1:16000 --sessions FIXA,FIXB --symbols AAPL,MSFT
//              --orders 5000 --rate 1000 --reconnect-at 2000
//
// Each session sends NewOrderSingle, OrderCancelRequest and
// OrderCancelReplaceRequest around a mid price, so the sessions cross each
// other. Checks, any failure exits 1:
//   - every request is answered by the right message for it, once;
//   - fills add up: CumQty grows by LastQty, LeavesQty = OrderQty - CumQty,
//     and a limit order never fills through its price;
//   - ExecIDs are unique per session; nothing names an unknown ClOrdID;
//   - with --reconnect-at N, after N orders the first session drops its TCP
//     connection without a Logout and logs in again without resetting
//     sequence numbers: what fixgw sent meanwhile must arrive by resend.
// At the end it cancels what is left, waits for the answers and logs out.
// Prints fills per session (one per execution side, comparable with the
// drop copy) and round-trip latency.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lab/core/clock.hpp"
#include "lab/core/histogram.hpp"
#include "lab/fix/session.hpp"
#include "lab/net/tcp.hpp"

namespace {

namespace fix = lab::fix;
namespace tag = lab::fix::tag;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string connect = "127.0.0.1:16000";
  std::string target = "SIMEX";
  std::vector<std::string> sessions{"FIXA", "FIXB"};
  std::vector<std::string> symbols{"AAPL", "MSFT"};
  std::uint64_t mid = 1'000'000;  // Price(4): $100.00
  std::uint64_t orders = 5'000;
  double rate = 1'000;  // requests per second, all sessions together
  double cancel = 0.25;
  double replace = 0.25;
  double ioc = 0.1;
  double market = 0.02;
  std::uint32_t max_lots = 5;
  std::uint64_t seed = 1;
  std::uint64_t reconnect_at = 0;
  std::uint32_t heartbeat = 5;
  double answer_timeout = 10;
};

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: fix-client [--connect IP:PORT] [--target COMPID] [--sessions A,B]\n"
               "                  [--symbols X,Y] [--mid DOLLARS] [--orders N] [--rate N]\n"
               "                  [--cancel F] [--replace F] [--ioc F] [--market F]\n"
               "                  [--max-lots N] [--seed S] [--reconnect-at N]\n"
               "                  [--heartbeat S] [--answer-timeout S]\n");
  std::exit(2);
}

std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> out;
  while (!s.empty()) {
    const auto p = s.find(sep);
    if (p != 0) out.emplace_back(s.substr(0, p));
    if (p == std::string_view::npos) break;
    s.remove_prefix(p + 1);
  }
  return out;
}

enum class Pending { kNone, kNew, kCancel, kReplace };

struct Order {
  std::string id;  // current ClOrdID
  std::string symbol;
  char side = '1';
  char ord_type = '2';
  char tif = '0';
  std::uint32_t qty = 0;
  std::uint32_t price = 0;
  std::uint32_t cum = 0;
  bool live = false;  // accepted and not finished
  bool done = false;
  Pending pending = Pending::kNone;
  std::string pending_id;  // ClOrdID of the pending request
  std::uint64_t sent_tsc = 0;
  std::uint32_t new_qty = 0;  // of a pending replace
  std::uint32_t new_price = 0;
};

struct Stats {
  std::uint64_t news = 0, cancels = 0, replaces = 0;
  std::uint64_t accepted = 0, rejected = 0, fills = 0, filled_shares = 0, canceled = 0;
  std::uint64_t replaced = 0, cancel_rejects = 0, session_rejects = 0, errors = 0;
};

class Client {
 public:
  Client(const Options& opt, const std::string& comp_id, std::uint64_t seed)
      : opt_{opt}, comp_id_{comp_id}, rng_{seed}, fix_{config(opt, comp_id)} {
    orders_.reserve(opt.orders * 2);
  }

  static fix::SessionConfig config(const Options& opt, const std::string& comp_id) {
    fix::SessionConfig c;
    c.sender = comp_id;
    c.target = opt.target;
    c.heartbeat_s = opt.heartbeat;
    return c;
  }

  void connect(bool reset) {
    const auto to = lab::net::parse_endpoint(opt_.connect);
    for (int attempt = 0;; ++attempt) {
      try {
        fix_.connect(lab::net::TcpStream::connect(to), lab::mono_ns(), reset);
        break;
      } catch (const std::system_error&) {
        if (attempt == 300) throw;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
      }
    }
    const auto until = Clock::now() + std::chrono::seconds{10};
    while (!fix_.active() && Clock::now() < until) {
      poll();
      if (fix_.state() == fix::Session::State::kDisconnected) break;
    }
    if (!fix_.active()) throw std::runtime_error(comp_id_ + ": logon failed");
  }

  void drop() { fix_.close(*this); }

  void poll() { fix_.poll(lab::mono_ns(), *this); }

  // One random request.
  void act(std::uint64_t n) {
    std::uniform_real_distribution<double> u{0, 1};
    const double r = u(rng_);
    if (r < opt_.cancel && cancel_one()) return;
    if (r < opt_.cancel + opt_.replace && replace_one()) return;
    new_order(n);
  }

  void cancel_all() {
    for (auto& [id, o] : orders_) {
      (void)id;
      if (o.live && o.pending == Pending::kNone) send_cancel(o);
    }
  }

  bool quiet() const { return outstanding_ == 0; }
  std::size_t outstanding() const { return outstanding_; }
  std::size_t live() const {
    return static_cast<std::size_t>(std::count_if(orders_.begin(), orders_.end(),
                                                  [](const auto& kv) { return kv.second.live; }));
  }
  fix::Session& session() { return fix_; }
  const Stats& stats() const { return stats_; }
  const std::string& name() const { return comp_id_; }
  lab::LatencyHistogram& round_trip() { return round_trip_; }

  // fix::Session handler.
  void on_logon() {}
  void on_disconnect() {}
  void on_app(const fix::Message& m) {
    switch (m.type()) {
      case fix::msg_type::kExecutionReport:
        exec_report(m);
        break;
      case fix::msg_type::kOrderCancelReject:
        cancel_reject(m);
        break;
      case fix::msg_type::kReject:
      case fix::msg_type::kBusinessMessageReject:
        ++stats_.session_rejects;
        error("reject", m);
        break;
      default:
        error("unexpected message", m);
        break;
    }
  }

 private:
  void error(const char* what, const fix::Message& m) {
    ++stats_.errors;
    if (stats_.errors <= 10) {
      const auto id = m.get(tag::kClOrdID).value_or("-");
      const auto text = m.get(tag::kText).value_or("");
      std::fprintf(stderr, "%s: %s (35=%c 11=%.*s 58=%.*s)\n", comp_id_.c_str(), what, m.type(),
                   static_cast<int>(id.size()), id.data(), static_cast<int>(text.size()),
                   text.data());
    }
  }

  Order* find(std::string_view id) {
    const auto it = orders_.find(std::string{id});
    return it == orders_.end() ? nullptr : &it->second;
  }

  void answered(Order& o) {
    if (o.pending == Pending::kNone) return;
    round_trip_.record(lab::rdtsc() - o.sent_tsc);
    o.pending = Pending::kNone;
    o.pending_id.clear();
    --outstanding_;
  }

  std::string next_id() { return comp_id_ + "-" + std::to_string(++ids_); }

  void new_order(std::uint64_t n) {
    std::uniform_real_distribution<double> u{0, 1};
    std::uniform_int_distribution<int> lots{1, static_cast<int>(opt_.max_lots)};
    std::uniform_int_distribution<int> ticks{-2, 6};  // negative: crosses the mid
    std::uniform_int_distribution<std::size_t> sym{0, opt_.symbols.size() - 1};
    Order o;
    o.id = next_id();
    o.symbol = opt_.symbols[sym(rng_)];
    o.side = (n + rng_()) % 2 == 0 ? '1' : '2';
    o.qty = static_cast<std::uint32_t>(lots(rng_)) * 100;
    const double kind = u(rng_);
    o.ord_type = kind < opt_.market ? '1' : '2';
    o.tif = o.ord_type == '1' || kind < opt_.market + opt_.ioc ? '3' : '0';
    const std::int64_t away = static_cast<std::int64_t>(ticks(rng_)) * 100;  // $0.01 ticks
    o.price = static_cast<std::uint32_t>(static_cast<std::int64_t>(opt_.mid) +
                                         (o.side == '1' ? -away : away));
    fix::Writer& w = fix_.begin(fix::msg_type::kNewOrderSingle);
    w.add(tag::kClOrdID, o.id);
    w.add(tag::kSymbol, o.symbol);
    w.add(tag::kSide, o.side);
    w.add_time(tag::kTransactTime, lab::wall_ns());
    w.add_uint(tag::kOrderQty, o.qty);
    w.add(tag::kOrdType, o.ord_type);
    if (o.ord_type == '2') w.add_price(tag::kPrice, o.price);
    w.add(tag::kTimeInForce, o.tif);
    o.pending = Pending::kNew;
    o.pending_id = o.id;
    o.sent_tsc = lab::rdtsc();
    fix_.send();
    ++outstanding_;
    ++stats_.news;
    orders_.emplace(o.id, std::move(o));
  }

  Order* pick() {
    // A live order with nothing pending, from a random starting point.
    if (orders_.empty()) return nullptr;
    std::uniform_int_distribution<std::size_t> b{0, orders_.bucket_count() - 1};
    const std::size_t start = b(rng_);
    for (std::size_t i = 0; i < orders_.bucket_count(); ++i) {
      const std::size_t k = (start + i) % orders_.bucket_count();
      for (auto it = orders_.begin(k); it != orders_.end(k); ++it) {
        if (it->second.live && it->second.pending == Pending::kNone) return &it->second;
      }
    }
    return nullptr;
  }

  void send_cancel(Order& o) {
    o.pending = Pending::kCancel;
    o.pending_id = next_id();
    o.sent_tsc = lab::rdtsc();
    fix::Writer& w = fix_.begin(fix::msg_type::kOrderCancelRequest);
    w.add(tag::kOrigClOrdID, o.id);
    w.add(tag::kClOrdID, o.pending_id);
    w.add(tag::kSymbol, o.symbol);
    w.add(tag::kSide, o.side);
    w.add_time(tag::kTransactTime, lab::wall_ns());
    w.add_uint(tag::kOrderQty, o.qty);
    fix_.send();
    ++outstanding_;
    ++stats_.cancels;
  }

  bool cancel_one() {
    Order* o = pick();
    if (o == nullptr) return false;
    send_cancel(*o);
    return true;
  }

  bool replace_one() {
    Order* o = pick();
    if (o == nullptr || o->ord_type != '2') return false;
    std::uniform_int_distribution<int> lots{1, static_cast<int>(opt_.max_lots)};
    std::uniform_int_distribution<int> move{-2, 2};
    o->new_qty = o->cum + static_cast<std::uint32_t>(lots(rng_)) * 100;
    const std::int64_t px = static_cast<std::int64_t>(o->price) + move(rng_) * 100;
    o->new_price = static_cast<std::uint32_t>(std::max<std::int64_t>(px, 100));
    o->pending = Pending::kReplace;
    o->pending_id = next_id();
    o->sent_tsc = lab::rdtsc();
    fix::Writer& w = fix_.begin(fix::msg_type::kOrderCancelReplaceRequest);
    w.add(tag::kOrigClOrdID, o->id);
    w.add(tag::kClOrdID, o->pending_id);
    w.add(tag::kSymbol, o->symbol);
    w.add(tag::kSide, o->side);
    w.add_time(tag::kTransactTime, lab::wall_ns());
    w.add_uint(tag::kOrderQty, o->new_qty);
    w.add(tag::kOrdType, '2');
    w.add_price(tag::kPrice, o->new_price);
    w.add(tag::kTimeInForce, o->tif);
    fix_.send();
    ++outstanding_;
    ++stats_.replaces;
    return true;
  }

  void finish(Order& o) {
    o.live = false;
    o.done = true;
  }

  void exec_report(const fix::Message& m) {
    const auto exec_id = m.get(tag::kExecID);
    if (!exec_id || !exec_ids_.insert(std::string{*exec_id}).second) {
      error("missing or repeated ExecID", m);
      return;
    }
    const auto id = m.get(tag::kClOrdID).value_or("");
    const auto orig = m.get(tag::kOrigClOrdID);
    const char type = m.get_char(tag::kExecType).value_or(0);
    const auto cum = m.get_uint(tag::kCumQty);
    const auto leaves = m.get_uint(tag::kLeavesQty);
    switch (type) {
      case '0': {
        Order* o = find(id);
        if (o == nullptr || o->pending != Pending::kNew)
          return error("New for no pending order", m);
        answered(*o);
        o->live = true;
        ++stats_.accepted;
        return;
      }
      case '8': {
        Order* o = find(id);
        if (o == nullptr || o->pending != Pending::kNew) return error("Rejected for no order", m);
        answered(*o);
        finish(*o);
        ++stats_.rejected;
        return error("order rejected", m);
      }
      case 'F': {
        Order* o = find(id);
        if (o == nullptr) return error("fill for an unknown order", m);
        const auto last = m.get_uint(tag::kLastQty).value_or(0);
        const auto px = fix::parse_price(m.get(tag::kLastPx).value_or("")).value_or(0);
        if (o->pending == Pending::kNew) {  // a fill can beat the New to us? never: must follow it
          return error("fill before New", m);
        }
        o->cum += static_cast<std::uint32_t>(last);
        if (last == 0 || cum != o->cum || leaves != o->qty - std::min(o->qty, o->cum) ||
            o->cum > o->qty) {
          error("fill quantities do not add up", m);
        }
        if (o->ord_type == '2' &&
            ((o->side == '1' && px > o->price) || (o->side != '1' && px < o->price))) {
          error("filled through the limit price", m);
        }
        ++stats_.fills;
        stats_.filled_shares += last;
        if (o->cum >= o->qty) finish(*o);
        return;
      }
      case '4': {
        if (orig) {
          Order* o = find(*orig);
          if (o == nullptr || o->pending_id != id ||
              (o->pending != Pending::kCancel && o->pending != Pending::kReplace)) {
            return error("Canceled answers no pending request", m);
          }
          answered(*o);
          finish(*o);
        } else {
          Order* o = find(id);
          if (o == nullptr) return error("Canceled for an unknown order", m);
          finish(*o);  // IOC remainder, or canceled when the session dropped
        }
        ++stats_.canceled;
        return;
      }
      case '5': {
        Order* o = orig ? find(*orig) : nullptr;
        if (o == nullptr || o->pending != Pending::kReplace || o->pending_id != id) {
          return error("Replaced answers no pending replace", m);
        }
        answered(*o);
        Order n = *o;
        orders_.erase(n.id);
        n.id = std::string{id};
        n.qty = n.new_qty;
        n.price = n.new_price;
        if (cum != n.cum || leaves != n.qty - n.cum) error("Replaced quantities do not add up", m);
        ++stats_.replaced;
        orders_.emplace(n.id, std::move(n));
        return;
      }
      default:
        return error("unexpected ExecType", m);
    }
  }

  void cancel_reject(const fix::Message& m) {
    const auto id = m.get(tag::kClOrdID).value_or("");
    const auto orig = m.get(tag::kOrigClOrdID).value_or("");
    const char to = m.get_char(tag::kCxlRejResponseTo).value_or(0);
    Order* o = find(orig);
    const Pending want = to == '1' ? Pending::kCancel : Pending::kReplace;
    if (o == nullptr || o->pending != want || o->pending_id != id) {
      return error("OrderCancelReject answers no pending request", m);
    }
    answered(*o);
    ++stats_.cancel_rejects;
    // Too late is fine (it filled or was canceled meanwhile); anything else is not.
    if (m.get(tag::kCxlRejReason) != "0") error("cancel or replace rejected", m);
  }

  Options opt_;
  std::string comp_id_;
  std::mt19937_64 rng_;
  fix::Session fix_;
  std::unordered_map<std::string, Order> orders_;
  std::unordered_set<std::string> exec_ids_;
  std::uint64_t ids_ = 0;
  std::size_t outstanding_ = 0;
  Stats stats_;
  lab::LatencyHistogram round_trip_;
};

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (a == "--connect") {
      opt.connect = value();
    } else if (a == "--target") {
      opt.target = value();
    } else if (a == "--sessions") {
      opt.sessions = split(value(), ',');
    } else if (a == "--symbols") {
      opt.symbols = split(value(), ',');
    } else if (a == "--mid") {
      opt.mid = static_cast<std::uint64_t>(fix::parse_price(value()).value_or(0));
    } else if (a == "--orders") {
      opt.orders = std::stoull(value());
    } else if (a == "--rate") {
      opt.rate = std::stod(value());
    } else if (a == "--cancel") {
      opt.cancel = std::stod(value());
    } else if (a == "--replace") {
      opt.replace = std::stod(value());
    } else if (a == "--ioc") {
      opt.ioc = std::stod(value());
    } else if (a == "--market") {
      opt.market = std::stod(value());
    } else if (a == "--max-lots") {
      opt.max_lots = static_cast<std::uint32_t>(std::stoul(value()));
    } else if (a == "--seed") {
      opt.seed = std::stoull(value());
    } else if (a == "--reconnect-at") {
      opt.reconnect_at = std::stoull(value());
    } else if (a == "--heartbeat") {
      opt.heartbeat = static_cast<std::uint32_t>(std::stoul(value()));
    } else if (a == "--answer-timeout") {
      opt.answer_timeout = std::stod(value());
    } else {
      usage();
    }
  }
  if (opt.sessions.empty() || opt.symbols.empty() || opt.mid < 10'000 || opt.max_lots == 0) {
    usage();
  }
  try {
    std::vector<std::unique_ptr<Client>> clients;
    for (std::size_t i = 0; i < opt.sessions.size(); ++i) {
      clients.push_back(std::make_unique<Client>(opt, opt.sessions[i], opt.seed * 1000 + i));
      clients.back()->connect(true);
    }
    auto poll_all = [&] {
      for (auto& c : clients) c->poll();
    };
    const auto start = Clock::now();
    const auto clock = lab::TscClock::calibrate(std::chrono::milliseconds{20});
    std::uint64_t sent = 0;
    std::uint64_t requests = 0;
    bool reconnected = false;
    while (sent < opt.orders) {
      poll_all();
      const double due = std::chrono::duration<double>(Clock::now() - start).count() * opt.rate;
      while (static_cast<double>(requests) < due && sent < opt.orders) {
        Client& c = *clients[requests % clients.size()];
        const auto before = c.stats().news;
        c.act(sent);
        sent += c.stats().news - before;
        ++requests;
      }
      if (opt.reconnect_at != 0 && !reconnected && sent >= opt.reconnect_at) {
        // Drop without Logout, let fixgw notice and cancel, then come back.
        clients[0]->drop();
        const auto until = Clock::now() + std::chrono::milliseconds{200};
        while (Clock::now() < until) {
          for (std::size_t i = 1; i < clients.size(); ++i) clients[i]->poll();
        }
        clients[0]->connect(false);
        reconnected = true;
        std::fprintf(stderr, "%s: reconnected after %llu orders\n", clients[0]->name().c_str(),
                     static_cast<unsigned long long>(sent));
      }
    }
    // Wait for answers, cancel what is left, wait again.
    auto settle = [&] {
      const auto until = Clock::now() + std::chrono::duration<double>{opt.answer_timeout};
      while (Clock::now() < until) {
        poll_all();
        if (std::all_of(clients.begin(), clients.end(), [](auto& c) { return c->quiet(); })) {
          return true;
        }
      }
      return false;
    };
    bool ok = settle();
    for (auto& c : clients) c->cancel_all();
    ok = settle() && ok;
    for (auto& c : clients) c->session().logout("done", lab::mono_ns());
    const auto until = Clock::now() + std::chrono::seconds{3};
    while (Clock::now() < until && std::any_of(clients.begin(), clients.end(), [](auto& c) {
             return c->session().state() != fix::Session::State::kDisconnected;
           })) {
      poll_all();
    }

    const double secs = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("fix-client: %llu orders in %.1f s\n", static_cast<unsigned long long>(sent), secs);
    std::printf("%-8s %7s %7s %7s %7s %7s %7s %9s %7s %7s %7s %7s %7s %6s\n", "session", "new",
                "cancel", "replace", "accept", "reject", "fills", "shares", "cxld", "rplcd",
                "cxl_rej", "resent", "gapfill", "errors");
    std::uint64_t fills = 0;
    lab::LatencyHistogram rt;
    for (auto& c : clients) {
      const Stats& s = c->stats();
      const auto& f = c->session().stats();
      std::printf(
          "%-8s %7llu %7llu %7llu %7llu %7llu %7llu %9llu %7llu %7llu %7llu %7llu %7llu %6llu\n",
          c->name().c_str(), static_cast<unsigned long long>(s.news),
          static_cast<unsigned long long>(s.cancels), static_cast<unsigned long long>(s.replaces),
          static_cast<unsigned long long>(s.accepted), static_cast<unsigned long long>(s.rejected),
          static_cast<unsigned long long>(s.fills),
          static_cast<unsigned long long>(s.filled_shares),
          static_cast<unsigned long long>(s.canceled), static_cast<unsigned long long>(s.replaced),
          static_cast<unsigned long long>(s.cancel_rejects),
          static_cast<unsigned long long>(f.resent),
          static_cast<unsigned long long>(f.gap_fills_sent),
          static_cast<unsigned long long>(s.errors));
      fills += s.fills;
      rt.merge(c->round_trip());
      if (s.errors != 0) ok = false;
      if (c->outstanding() != 0) {
        std::fprintf(stderr, "%s: %zu requests unanswered\n", c->name().c_str(), c->outstanding());
        ok = false;
      }
      if (c->live() != 0) {
        std::fprintf(stderr, "%s: %zu orders still live\n", c->name().c_str(), c->live());
        ok = false;
      }
    }
    std::printf("fills %llu\n", static_cast<unsigned long long>(fills));
    rt.print(stdout, "request to first answer", 1.0 / clock.ticks_per_ns());
    if (opt.reconnect_at != 0 && !reconnected) ok = false;
    std::printf("%s\n", ok ? "fix-client: OK" : "fix-client: FAILED");
    return ok ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fix-client: %s\n", e.what());
    return 1;
  }
}
