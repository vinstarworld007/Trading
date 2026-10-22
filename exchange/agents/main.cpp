// agents: agent-based liquidity for simex. Instead of replaying a historical
// day, a population of simple trading agents makes the market the firm
// trades in. Every symbol has a hidden fundamental value that moves one tick
// at a time at random; the agents react to it, to each other and to whatever
// else trades on simex (the firm's trader, for one).
//
//   simex --symbols SYM0001,SYM0002 --accounts FIRM01:lab,AGENT1:lab,AGENT2:lab
//   agents --symbols SYM0001,SYM0002 --duration 20
//
// Populations (docs/core-plan.md, phase 7):
//   makers     account 1: quote a bid and an ask around a noisy estimate of
//              value, wider for some than others, skewed by inventory.
//   noise      account 2: marketable IOC orders on random sides at random times.
//   value      account 2: know the value; buy when trades print below it by
//              more than a threshold, sell when above. They pick off makers
//              whose estimate is stale.
//   momentum   account 2: compare fast and slow averages of trade prices and
//              chase the move.
// Trade prices come from the agents' own executions (they are on one side of
// almost every trade), so agents need no market data feed.
//
// Checks, any failure exits 1: every OUCH answer names an order the agents
// sent; the market traded; at the end every agent order is gone. Prints per
// population orders, fills, inventory and PnL marked at the final value, and
// the market's spread and volatility.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lab/core/clock.hpp"
#include "lab/core/histogram.hpp"
#include "lab/net/tcp.hpp"
#include "lab/ouch/codec.hpp"
#include "lab/ouch/messages.hpp"
#include "lab/soup/soup.hpp"

namespace {

namespace ouch = lab::ouch;
namespace soup = lab::soup;
using Clock = std::chrono::steady_clock;

constexpr std::int64_t kTick = 100;  // $0.01 in Price(4)

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

struct Options {
  std::string connect = "127.0.0.1:15000";
  std::pair<std::string, std::string> maker_account{"AGENT1", "lab"};
  std::pair<std::string, std::string> taker_account{"AGENT2", "lab"};
  std::vector<std::string> symbols{"SYM0001", "SYM0002", "SYM0003", "SYM0004", "SYM0005"};
  std::int64_t start_value = 1'000'000;  // $100.00
  double duration = 20;
  std::uint64_t seed = 1;
  int makers = 4;                 // per symbol
  double value_rate = 5;          // value steps per second per symbol
  double noise_rate = 10;         // noise orders per second per symbol
  double value_threshold = 3;     // ticks between value and the last trade before value traders act
  double momentum_threshold = 2;  // ticks between fast and slow averages
  double agent_rate = 20;         // max orders per second per symbol for value and momentum agents
  std::int64_t maker_limit = 3'000;  // shares of inventory per maker before it quotes one side
};

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: agents [--connect IP:PORT] [--symbols A,B] [--makers N] [--value DOLLARS]\n"
               "              [--maker-account USER:PASS] [--taker-account USER:PASS]\n"
               "              [--value-rate N] [--noise-rate N] [--agent-rate N]\n"
               "              [--duration S] [--seed S]\n");
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

std::pair<std::string, std::string> user_pass(const std::string& s) {
  const auto c = s.find(':');
  if (c == std::string::npos) usage();
  return {s.substr(0, c), s.substr(c + 1)};
}

enum Population : int { kMakers, kNoise, kValue, kMomentum, kPopulations };
constexpr const char* kNames[kPopulations] = {"makers", "noise", "value", "momentum"};

struct PopStats {
  std::uint64_t orders = 0;
  std::uint64_t replaces = 0;
  std::uint64_t fills = 0;
  std::uint64_t shares = 0;
  std::int64_t cash = 0;  // Price(4) * shares; + sells, - buys
  std::vector<std::int64_t> position;
};

struct Order {
  int population = 0;
  int agent = -1;  // maker index, or -1
  int side_index = 0;
  std::size_t symbol = 0;
  char side = 'B';
  std::uint32_t price = 0;
  std::uint32_t open = 0;
  bool pending = true;  // a request for it is unanswered
};

class Agents;

// One SoupBinTCP/OUCH session to simex and the orders sent through it.
class Account {
 public:
  Account(Agents& a, std::string user, std::string pass)
      : agents_{a}, user_{std::move(user)}, pass_{std::move(pass)} {}

  void login(const lab::net::Endpoint& to) {
    for (int attempt = 0;; ++attempt) {
      try {
        sock_ = lab::net::TcpStream::connect(to);
        break;
      } catch (const std::system_error&) {
        if (attempt == 300) throw;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
      }
    }
    std::array<std::byte, soup::kLoginRequestSize> login;
    soup::write_login_request(login.data(), user_, pass_, "", 0);
    sock_.send(login);
    std::array<std::byte, ouch::AccountQuery::kSize> q;
    ouch::encode(ouch::AccountQuery{}, q.data());
    send(q);
    const auto deadline = Clock::now() + std::chrono::seconds{30};
    while (next_ref_ == 0) {
      if (Clock::now() > deadline) throw std::runtime_error(user_ + ": no answer to login");
      sock_.flush();
      const auto n = sock_.recv(in_.write_area());
      if (n < 0) throw std::runtime_error(user_ + ": simex closed the connection");
      if (n == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds{200});
        continue;
      }
      in_.commit(static_cast<std::size_t>(n));
      while (auto p = in_.next()) {
        if (p->type == soup::kLoginRejected) throw std::runtime_error(user_ + ": login rejected");
        if (p->type == soup::kSequencedData && !p->payload.empty() &&
            static_cast<char>(p->payload[0]) == ouch::AccountQueryResponse::kType &&
            p->payload.size() >= ouch::AccountQueryResponse::kSize) {
          ouch::AccountQueryResponse r;
          ouch::decode(p->payload.data(), r);
          next_ref_ = std::max<std::uint32_t>(r.next_user_ref_num, 1);
        }
      }
    }
  }

  std::uint32_t enter(const Order& o, const ouch::Symbol& symbol, char tif) {
    const std::uint32_t ref = next_ref_++;
    ouch::EnterOrder e;
    e.user_ref_num = ref;
    e.side = o.side;
    e.quantity = o.open;
    e.symbol = symbol;
    e.price = o.price;
    e.time_in_force = tif;
    std::array<std::byte, ouch::EnterOrder::kSize> buf;
    ouch::encode(e, buf.data());
    send(buf);
    orders_.emplace(ref, o);
    return ref;
  }

  std::uint32_t replace(std::uint32_t ref, std::uint32_t qty, std::uint32_t price) {
    auto it = orders_.find(ref);
    if (it == orders_.end() || it->second.pending) return 0;
    const std::uint32_t n = next_ref_++;
    ouch::ReplaceOrder r;
    r.orig_user_ref_num = ref;
    r.user_ref_num = n;
    r.quantity = qty;
    r.price = price;
    std::array<std::byte, ouch::ReplaceOrder::kSize> buf;
    ouch::encode(r, buf.data());
    send(buf);
    it->second.pending = true;
    pending_replace_.emplace(n, ref);
    return n;
  }

  void cancel(std::uint32_t ref) {
    auto it = orders_.find(ref);
    if (it == orders_.end() || it->second.pending) return;
    ouch::CancelOrder c;
    c.user_ref_num = ref;
    c.quantity = 0;
    std::array<std::byte, ouch::CancelOrder::kSize> buf;
    ouch::encode(c, buf.data());
    send(buf);
    it->second.pending = true;
  }

  void cancel_all() {
    for (auto& [ref, o] : orders_) {
      if (!o.pending) cancel(ref);
    }
  }

  void poll();

  bool ended() const { return ended_; }
  std::size_t open_orders() const { return orders_.size(); }
  std::uint64_t unknown() const { return unknown_; }
  const Order* find(std::uint32_t ref) const {
    const auto it = orders_.find(ref);
    return it == orders_.end() ? nullptr : &it->second;
  }

 private:
  struct Handler;

  void send(std::span<const std::byte> msg) {
    std::array<std::byte, soup::kHeaderSize + ouch::kMaxMessageSize> pkt;
    sock_.send({pkt.data(), soup::write_data(pkt.data(), soup::kUnsequencedData, msg)});
    last_tx_ = Clock::now();
  }

  Agents& agents_;
  std::string user_, pass_;
  lab::net::TcpStream sock_;
  soup::StreamParser in_;
  std::uint32_t next_ref_ = 0;
  std::unordered_map<std::uint32_t, Order> orders_;
  std::unordered_map<std::uint32_t, std::uint32_t> pending_replace_;  // new ref -> old ref
  Clock::time_point last_tx_ = Clock::now();
  std::uint64_t unknown_ = 0;
  bool ended_ = false;

  friend class Agents;
};

struct Maker {
  std::size_t symbol = 0;
  int half_spread = 1;  // ticks from its estimate to each quote
  std::uint32_t lots = 1;
  int noise = 0;                       // ticks its estimate is off by
  std::array<std::uint32_t, 2> ref{};  // live bid, ask (0: none)
  std::int64_t position = 0;
};

struct SymbolState {
  ouch::Symbol symbol{};
  std::int64_t value = 0;     // hidden fundamental, Price(4)
  std::int64_t last_px = 0;   // last trade the agents saw
  double fast = 0, slow = 0;  // trade price averages, in Price(4)
  Clock::time_point next_value_agent{}, next_momentum_agent{};
  std::uint64_t trades = 0;
  std::vector<std::int64_t> returns;  // value steps, in ticks, for volatility
};

class Agents {
 public:
  explicit Agents(const Options& opt)
      : opt_{opt},
        rng_{opt.seed},
        makers_acc_{*this, opt.maker_account.first, opt.maker_account.second},
        takers_acc_{*this, opt.taker_account.first, opt.taker_account.second} {
    for (const auto& s : opt.symbols) {
      SymbolState st;
      st.symbol = ouch::make_alpha<8>(s);
      st.value = st.last_px = opt.start_value;
      st.fast = st.slow = static_cast<double>(opt.start_value);
      symbols_.push_back(st);
    }
    std::uniform_int_distribution<int> spread{1, 4}, lots{1, 5};
    for (std::size_t s = 0; s < symbols_.size(); ++s) {
      for (int m = 0; m < opt.makers; ++m) {
        Maker mk;
        mk.symbol = s;
        mk.half_spread = spread(rng_);
        mk.lots = static_cast<std::uint32_t>(lots(rng_));
        makers_.push_back(mk);
      }
    }
    for (auto& p : stats_) p.position.assign(symbols_.size(), 0);
  }

  void run() {
    const auto to = lab::net::parse_endpoint(opt_.connect);
    makers_acc_.login(to);
    takers_acc_.login(to);
    std::fprintf(stderr, "agents: %zu makers, noise, value and momentum on %zu symbols\n",
                 makers_.size(), symbols_.size());
    const auto start = Clock::now();
    const auto end = start + std::chrono::duration<double>{opt_.duration};
    auto last = start;
    auto next_sample = start;
    while (!g_stop.load() && Clock::now() < end && !makers_acc_.ended() && !takers_acc_.ended()) {
      makers_acc_.poll();
      takers_acc_.poll();
      const auto now = Clock::now();
      const double dt = std::chrono::duration<double>(now - last).count();
      if (dt < 0.001) continue;
      last = now;
      step(now, dt);
      if (now >= next_sample) {
        sample();
        next_sample = now + std::chrono::milliseconds{10};
      }
    }
    // Cancel everything and wait for the answers.
    makers_acc_.cancel_all();
    takers_acc_.cancel_all();
    const auto until = Clock::now() + std::chrono::seconds{5};
    while (Clock::now() < until &&
           (makers_acc_.open_orders() != 0 || takers_acc_.open_orders() != 0) &&
           !(makers_acc_.ended() && takers_acc_.ended())) {
      makers_acc_.poll();
      takers_acc_.poll();
      makers_acc_.cancel_all();  // orders accepted since: cancel those too
      takers_acc_.cancel_all();
    }
    elapsed_ = std::chrono::duration<double>(Clock::now() - start).count();
  }

  bool report(std::FILE* out) const;

  // From an Account: fills and finished orders.
  void on_fill(const Order& o, std::uint32_t q, std::uint32_t px) {
    PopStats& p = stats_[o.population];
    ++p.fills;
    p.shares += q;
    const std::int64_t signed_q = o.side == 'B' ? q : -static_cast<std::int64_t>(q);
    p.position[o.symbol] += signed_q;
    p.cash -= signed_q * px;
    if (o.agent >= 0) makers_[static_cast<std::size_t>(o.agent)].position += signed_q;
    SymbolState& s = symbols_[o.symbol];
    s.last_px = px;
    s.fast += 0.3 * (px - s.fast);
    s.slow += 0.03 * (px - s.slow);
    ++s.trades;
  }
  void on_gone(const Order& o, std::uint32_t ref) {
    if (o.agent < 0) return;
    auto& r =
        makers_[static_cast<std::size_t>(o.agent)].ref[static_cast<std::size_t>(o.side_index)];
    if (r == ref) r = 0;
  }
  void on_replaced(const Order& o, std::uint32_t old_ref, std::uint32_t new_ref) {
    if (o.agent < 0) return;
    auto& r =
        makers_[static_cast<std::size_t>(o.agent)].ref[static_cast<std::size_t>(o.side_index)];
    if (r == old_ref) r = new_ref;
  }

 private:
  void step(Clock::time_point now, double dt) {
    std::uniform_real_distribution<double> u{0, 1};
    for (auto& s : symbols_) {
      if (u(rng_) < opt_.value_rate * dt) {
        const std::int64_t move = u(rng_) < 0.5 ? -kTick : kTick;
        s.value = std::max<std::int64_t>(s.value + move, 100 * kTick);
        s.returns.push_back(move / kTick);
      }
    }
    for (std::size_t i = 0; i < makers_.size(); ++i) quote(static_cast<int>(i));
    for (std::size_t i = 0; i < symbols_.size(); ++i) {
      SymbolState& s = symbols_[i];
      if (u(rng_) < opt_.noise_rate * dt) {
        const char side = u(rng_) < 0.5 ? 'B' : 'S';
        const std::int64_t px = s.last_px + (side == 'B' ? 3 : -3) * kTick;
        take(kNoise, i, side, px, 1 + static_cast<std::uint32_t>(u(rng_) * 3));
      }
      const double gap = static_cast<double>(s.value - s.last_px) / kTick;
      if (now >= s.next_value_agent && std::abs(gap) >= opt_.value_threshold) {
        const char side = gap > 0 ? 'B' : 'S';
        take(kValue, i, side, s.value + (side == 'B' ? -kTick : kTick), 2);
        s.next_value_agent = now + std::chrono::duration_cast<Clock::duration>(
                                       std::chrono::duration<double>{1.0 / opt_.agent_rate});
      }
      const double trend = (s.fast - s.slow) / kTick;
      if (now >= s.next_momentum_agent && std::abs(trend) >= opt_.momentum_threshold) {
        const char side = trend > 0 ? 'B' : 'S';
        take(kMomentum, i, side, s.last_px + (side == 'B' ? 2 : -2) * kTick, 1);
        s.next_momentum_agent = now + std::chrono::duration_cast<Clock::duration>(
                                          std::chrono::duration<double>{1.0 / opt_.agent_rate});
      }
    }
  }

  // An IOC limit order from a taking population.
  void take(Population pop, std::size_t sym, char side, std::int64_t px, std::uint32_t lots) {
    if (px <= 0) return;
    Order o;
    o.population = pop;
    o.symbol = sym;
    o.side = side;
    o.price = static_cast<std::uint32_t>(px);
    o.open = lots * 100;
    takers_acc_.enter(o, symbols_[sym].symbol, ouch::tif::kIoc);
    ++stats_[pop].orders;
  }

  // Moves maker i's two quotes toward its targets: enter, replace or cancel.
  void quote(int i) {
    Maker& m = makers_[static_cast<std::size_t>(i)];
    SymbolState& s = symbols_[m.symbol];
    std::uniform_real_distribution<double> u{0, 1};
    if (u(rng_) < 0.01) m.noise = static_cast<int>(std::lround((u(rng_) - 0.5) * 4));
    const std::int64_t skew = m.position / 500;  // a tick per 500 shares held
    const std::int64_t estimate = s.value + (m.noise - skew) * kTick;
    for (int side = 0; side < 2; ++side) {
      const bool bid = side == 0;
      const bool too_long = bid && m.position >= opt_.maker_limit;
      const bool too_short = !bid && m.position <= -opt_.maker_limit;
      const std::int64_t px = estimate + (bid ? -m.half_spread : m.half_spread) * kTick;
      std::uint32_t& ref = m.ref[static_cast<std::size_t>(side)];
      if (ref != 0) {
        const Order* o = makers_acc_.find(ref);
        if (o == nullptr) {
          ref = 0;
        } else if (o->pending) {
          continue;
        } else if (too_long || too_short || px <= 0) {
          makers_acc_.cancel(ref);
          continue;
        } else if (o->price != static_cast<std::uint32_t>(px)) {
          if (makers_acc_.replace(ref, m.lots * 100, static_cast<std::uint32_t>(px)) != 0) {
            ++stats_[kMakers].replaces;
          }
          continue;
        } else {
          continue;
        }
      }
      if (too_long || too_short || px <= 0) continue;
      Order o;
      o.population = kMakers;
      o.agent = i;
      o.side_index = side;
      o.symbol = m.symbol;
      o.side = bid ? 'B' : 'S';
      o.price = static_cast<std::uint32_t>(px);
      o.open = m.lots * 100;
      ref = makers_acc_.enter(o, s.symbol, ouch::tif::kDay);
      ++stats_[kMakers].orders;
    }
  }

  // Touch spread the makers show, per symbol, in ticks.
  void sample() {
    for (std::size_t i = 0; i < symbols_.size(); ++i) {
      std::int64_t bid = 0, ask = 0;
      for (const auto& m : makers_) {
        if (m.symbol != i) continue;
        if (const Order* o = m.ref[0] ? makers_acc_.find(m.ref[0]) : nullptr) {
          bid = std::max<std::int64_t>(bid, o->price);
        }
        if (const Order* o = m.ref[1] ? makers_acc_.find(m.ref[1]) : nullptr) {
          ask = ask == 0 ? o->price : std::min<std::int64_t>(ask, o->price);
        }
      }
      ++samples_;
      if (bid == 0 || ask == 0) continue;
      ++two_sided_;
      spread_.record(static_cast<std::uint64_t>(std::max<std::int64_t>(ask - bid, 0) / kTick));
    }
  }

  Options opt_;
  std::mt19937_64 rng_;
  Account makers_acc_;
  Account takers_acc_;
  std::vector<SymbolState> symbols_;
  std::vector<Maker> makers_;
  std::array<PopStats, kPopulations> stats_;
  lab::LatencyHistogram spread_;  // ticks
  std::uint64_t samples_ = 0;
  std::uint64_t two_sided_ = 0;
  double elapsed_ = 0;
};

struct Account::Handler : ouch::HandlerBase {
  Account& a;
  explicit Handler(Account& acc) : a{acc} {}
  using HandlerBase::on;

  Order* find(std::uint32_t ref) {
    const auto it = a.orders_.find(ref);
    if (it == a.orders_.end()) {
      ++a.unknown_;
      return nullptr;
    }
    return &it->second;
  }
  void gone(std::uint32_t ref) {
    const auto it = a.orders_.find(ref);
    if (it == a.orders_.end()) return;
    const Order o = it->second;
    a.orders_.erase(it);
    a.agents_.on_gone(o, ref);
  }

  void on(const ouch::OrderAccepted& m) {
    if (Order* o = find(m.user_ref_num)) {
      o->pending = false;
      o->open = m.quantity;
      if (o->open == 0) gone(m.user_ref_num);
    }
  }
  void on(const ouch::OrderExecuted& m) {
    if (Order* o = find(m.user_ref_num)) {
      const std::uint32_t q = std::min(m.quantity, o->open);
      o->open -= q;
      a.agents_.on_fill(*o, q, static_cast<std::uint32_t>(m.price));
      if (o->open == 0) gone(m.user_ref_num);
    }
  }
  void on(const ouch::OrderCanceled& m) {
    if (Order* o = find(m.user_ref_num)) {
      o->open -= std::min(m.quantity, o->open);
      o->pending = false;
      if (o->open == 0) gone(m.user_ref_num);
    }
  }
  void on(const ouch::OrderReplaced& m) {
    a.pending_replace_.erase(m.user_ref_num);
    const auto it = a.orders_.find(m.orig_user_ref_num);
    if (it == a.orders_.end()) {
      ++a.unknown_;
      return;
    }
    Order o = it->second;
    a.orders_.erase(it);
    o.pending = false;
    o.price = static_cast<std::uint32_t>(m.price);
    o.open = m.quantity;
    a.agents_.on_replaced(o, m.orig_user_ref_num, m.user_ref_num);
    if (o.open == 0) {
      a.agents_.on_gone(o, m.user_ref_num);
    } else {
      a.orders_.emplace(m.user_ref_num, o);
    }
  }
  void on(const ouch::Rejected& m) {
    const auto rep = a.pending_replace_.find(m.user_ref_num);
    if (rep != a.pending_replace_.end()) {  // a replace refused: the old order stands, if live
      const auto old = a.orders_.find(rep->second);
      a.pending_replace_.erase(rep);
      if (old != a.orders_.end()) old->second.pending = false;
      return;
    }
    if (find(m.user_ref_num) != nullptr) gone(m.user_ref_num);
  }
};

void Account::poll() {
  if (ended_) return;
  for (;;) {
    const auto n = sock_.recv(in_.write_area());
    if (n < 0) {
      ended_ = true;
      return;
    }
    if (n == 0) break;
    in_.commit(static_cast<std::size_t>(n));
    while (auto p = in_.next()) {
      if (p->type == soup::kSequencedData) {
        Handler h{*this};
        ouch::dispatch_outbound(p->payload.data(), p->payload.size(), h);
      } else if (p->type == soup::kEndOfSession) {
        ended_ = true;
      }
    }
  }
  sock_.flush();
  if (Clock::now() - last_tx_ > std::chrono::seconds{1}) {
    std::array<std::byte, soup::kHeaderSize> hb;
    soup::write_empty(hb.data(), soup::kClientHeartbeat);
    sock_.send(hb);
    last_tx_ = Clock::now();
  }
}

bool Agents::report(std::FILE* out) const {
  bool ok = true;
  std::uint64_t trades = 0;
  std::fprintf(out, "agents: %.1f s, %zu symbols\n", elapsed_, symbols_.size());
  std::fprintf(out, "%-9s %9s %9s %8s %10s %12s %14s\n", "agents", "orders", "replaces", "fills",
               "shares", "inventory", "PnL at value");
  for (int p = 0; p < kPopulations; ++p) {
    const PopStats& s = stats_[p];
    std::int64_t inventory = 0, pnl = s.cash;
    for (std::size_t i = 0; i < symbols_.size(); ++i) {
      inventory += s.position[i];
      pnl += s.position[i] * symbols_[i].value;
    }
    std::fprintf(
        out, "%-9s %9llu %9llu %8llu %10llu %12lld %14.2f\n", kNames[p],
        static_cast<unsigned long long>(s.orders), static_cast<unsigned long long>(s.replaces),
        static_cast<unsigned long long>(s.fills), static_cast<unsigned long long>(s.shares),
        static_cast<long long>(inventory), static_cast<double>(pnl) / 10'000.0);
  }
  std::fprintf(out, "%-9s %10s %10s %10s %8s %10s\n", "symbol", "start", "value", "last trade",
               "trades", "vol/step");
  for (const auto& s : symbols_) {
    double var = 0;
    for (const auto r : s.returns) var += static_cast<double>(r * r);
    const double vol =
        s.returns.empty() ? 0 : std::sqrt(var / static_cast<double>(s.returns.size()));
    std::fprintf(out, "%-9.8s %10.2f %10.2f %10.2f %8llu %10.2f\n", s.symbol.data(),
                 static_cast<double>(opt_.start_value) / 10'000.0,
                 static_cast<double>(s.value) / 10'000.0, static_cast<double>(s.last_px) / 10'000.0,
                 static_cast<unsigned long long>(s.trades), vol);
    trades += s.trades;
  }
  std::fprintf(
      out, "Makers two-sided in %.1f%% of samples; touch spread ticks p50=%llu p99=%llu\n",
      samples_ ? 100.0 * static_cast<double>(two_sided_) / static_cast<double>(samples_) : 0.0,
      static_cast<unsigned long long>(spread_.percentile(50)),
      static_cast<unsigned long long>(spread_.percentile(99)));
  const std::uint64_t unknown = makers_acc_.unknown() + takers_acc_.unknown();
  const std::size_t left = makers_acc_.open_orders() + takers_acc_.open_orders();
  std::fprintf(out, "Checks: unknown refs %llu, orders left %zu, trades %llu\n",
               static_cast<unsigned long long>(unknown), left,
               static_cast<unsigned long long>(trades));
  if (unknown != 0 || left != 0 || trades == 0) ok = false;
  std::fprintf(out, "%s\n", ok ? "agents: OK" : "agents: FAILED");
  return ok;
}

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
    } else if (a == "--symbols") {
      opt.symbols = split(value(), ',');
    } else if (a == "--makers") {
      opt.makers = std::stoi(value());
    } else if (a == "--value") {
      opt.start_value = static_cast<std::int64_t>(std::llround(std::stod(value()) * 100)) * kTick;
    } else if (a == "--maker-account") {
      opt.maker_account = user_pass(value());
    } else if (a == "--taker-account") {
      opt.taker_account = user_pass(value());
    } else if (a == "--value-rate") {
      opt.value_rate = std::stod(value());
    } else if (a == "--noise-rate") {
      opt.noise_rate = std::stod(value());
    } else if (a == "--agent-rate") {
      opt.agent_rate = std::stod(value());
    } else if (a == "--duration") {
      opt.duration = std::stod(value());
    } else if (a == "--seed") {
      opt.seed = std::stoull(value());
    } else {
      usage();
    }
  }
  if (opt.symbols.empty() || opt.makers < 1 || opt.start_value < 100 * kTick) usage();
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  try {
    auto agents = std::make_unique<Agents>(opt);
    agents->run();
    return agents->report(stdout) ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "agents: %s\n", e.what());
    return 1;
  }
}
