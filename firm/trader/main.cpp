// trader: the firm's trading engine. Market data in, quotes out.
//
//   trader --md 127.0.0.1:31001 --retrans 127.0.0.1:31002 --connect 127.0.0.1:15000
//          --account FIRM01:lab --symbols SYM0001,SYM0002 --duration 30
//
// Threads:
//   rx       MoldUDP64 receive and retransmission requests (FeedReceiver)
//   md       sequencing, ITCH decode, books; top-of-book changes for traded
//            symbols to the trading thread
//   trader   market maker, pre-trade risk, OMS; order requests to order entry
//   oe       OUCH 5.0 over SoupBinTCP: sends requests, decodes responses
// Only rx and oe make syscalls. At the end it cancels its orders and reports
// positions, PnL, risk blocks and latency (feed packet to order on the wire,
// order round trip). Exits 0 when the feed lost nothing, books are clean, and
// the trading checks pass.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lab/book/book_builder.hpp"
#include "lab/core/affinity.hpp"
#include "lab/core/endian.hpp"
#include "lab/feed/receiver.hpp"
#include "lab/ouch/messages.hpp"
#include "order_entry.hpp"
#include "trading.hpp"

namespace {

struct Options {
  lab::feed::ReceiverConfig feed;
  trader::OrderEntryConfig oe;
  trader::TradingConfig trading;
  int md_cpu = -1;
  double duration = 0;  // 0: until the feed or the session ends
  std::string fills_csv;
  std::string market_csv;
};

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: trader --symbols A,B [--md IP:PORT] [--iface IP] [--retrans IP:PORT]\n"
               "              [--connect IP:PORT] [--account USER:PASS] [--duration S]\n"
               "              [--quote-qty N] [--max-position N] [--skew-ticks N]\n"
               "              [--risk-max-position N] [--max-order-qty N] [--collar-bps N]\n"
               "              [--max-orders-per-sec N] [--max-open-orders N]\n"
               "              [--rx-cpu N] [--md-cpu N] [--trader-cpu N] [--oe-cpu N]\n"
               "              [--fills-csv FILE] [--market-csv FILE]\n");
  std::exit(2);
}

Options parse(int argc, char** argv) {
  Options o;
  o.feed.group = "127.0.0.1:31001";
  o.feed.retrans = "127.0.0.1:31002";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) usage();
    const char* v = argv[++i];
    if (a == "--md") {
      o.feed.group = v;
    } else if (a == "--iface") {
      o.feed.iface = v;
    } else if (a == "--retrans") {
      o.feed.retrans = v;
    } else if (a == "--connect") {
      o.oe.connect = v;
    } else if (a == "--account") {
      const std::string s = v;
      const auto p = s.find(':');
      if (p == std::string::npos) usage();
      o.oe.user = s.substr(0, p);
      o.oe.password = s.substr(p + 1);
    } else if (a == "--symbols") {
      std::string s = v;
      std::size_t b = 0;
      while (b <= s.size()) {
        const auto e = s.find(',', b);
        const std::string sym = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
        if (!sym.empty()) o.trading.symbols.push_back(lab::ouch::make_alpha<8>(sym));
        if (e == std::string::npos) break;
        b = e + 1;
      }
    } else if (a == "--duration") {
      o.duration = std::strtod(v, nullptr);
    } else if (a == "--quote-qty") {
      o.trading.maker.quote_qty = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (a == "--max-position") {
      o.trading.maker.max_position = std::strtoll(v, nullptr, 10);
    } else if (a == "--skew-ticks") {
      o.trading.maker.skew_ticks = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (a == "--risk-max-position") {
      o.trading.limits.max_position = std::strtoll(v, nullptr, 10);
    } else if (a == "--max-order-qty") {
      o.trading.limits.max_order_qty = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (a == "--collar-bps") {
      o.trading.limits.collar_bps = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (a == "--max-orders-per-sec") {
      o.trading.limits.max_orders_per_second = std::strtod(v, nullptr);
    } else if (a == "--max-open-orders") {
      o.trading.limits.max_open_orders = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (a == "--fills-csv") {
      o.fills_csv = v;
      o.trading.fill_log_capacity = 1u << 20;
    } else if (a == "--market-csv") {
      o.market_csv = v;
    } else if (a == "--rx-cpu") {
      o.feed.rx_cpu = std::atoi(v);
    } else if (a == "--md-cpu") {
      o.md_cpu = std::atoi(v);
    } else if (a == "--trader-cpu") {
      o.trading.cpu = std::atoi(v);
    } else if (a == "--oe-cpu") {
      o.oe.cpu = std::atoi(v);
    } else {
      usage();
    }
  }
  if (o.trading.symbols.empty()) usage();
  return o;
}

std::atomic<bool> g_interrupted{false};
extern "C" void on_signal(int) { g_interrupted.store(true, std::memory_order_relaxed); }

// The market data thread: books for every symbol, top-of-book changes for the
// traded ones.
class MarketData {
 public:
  MarketData(const std::vector<lab::oms::Symbol>& symbols, trader::TopRing& out,
             std::size_t log_capacity)
      : symbols_{symbols},
        out_{out},
        books_{std::make_unique<lab::book::BookBuilder>()},
        instrument_of_(65536, -1),
        last_(symbols.size()) {
    log_.reserve(log_capacity);
  }

  // After run(): every top-of-book change for the traded symbols, as CSV.
  bool write_log(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) return false;
    std::fprintf(f, "feed_ns,symbol,bid,bid_qty,ask,ask_qty\n");
    for (const auto& t : log_) {
      std::string sym(symbols_[t.instrument].data(), 8);
      while (!sym.empty() && sym.back() == ' ') sym.pop_back();
      std::fprintf(f, "%llu,%s,%u,%u,%u,%u\n", static_cast<unsigned long long>(t.feed_ns),
                   sym.c_str(), t.bid_px[0], t.bid_qty[0], t.ask_px[0], t.ask_qty[0]);
    }
    return std::fclose(f) == 0;
  }

  void run(lab::feed::FeedReceiver& rx, int cpu, const std::atomic<bool>& stop) {
    lab::set_thread_name("trader-md");
    if (cpu >= 0) (void)lab::pin_this_thread(cpu);
    auto deliver = [&](std::uint64_t, std::span<const std::byte> msg, std::uint64_t rx_tsc) {
      on_message(msg, rx_tsc);
    };
    while (!stop.load(std::memory_order_relaxed)) {
      rx.poll(deliver);
      if (rx.finished()) break;
    }
    finished_.store(true, std::memory_order_release);
  }

  bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }
  const lab::book::BookBuilder& books() const noexcept { return *books_; }
  std::uint64_t tops_sent() const noexcept { return tops_sent_; }
  std::uint64_t ring_full_spins() const noexcept { return ring_full_spins_; }
  std::size_t listed() const noexcept { return listed_; }

 private:
  void on_message(std::span<const std::byte> msg, std::uint64_t rx_tsc) {
    books_->apply(msg);
    if (msg.size() < 11) return;
    const std::uint16_t locate = lab::load_be16(msg.data() + 1);
    if (static_cast<char>(msg[0]) == 'R' && msg.size() >= 19) {  // Stock Directory: Stock at 11
      for (std::size_t i = 0; i < symbols_.size(); ++i) {
        if (std::memcmp(msg.data() + 11, symbols_[i].data(), 8) == 0) {
          instrument_of_[locate] = static_cast<int>(i);
          ++listed_;
        }
      }
      return;
    }
    const int inst = instrument_of_[locate];
    if (inst < 0) return;
    lab::oms::TopOfBook t{};
    const auto& book = books_->book(locate);
    for (std::size_t d = 0; d < 2; ++d) {
      if (const auto* l = book.bids().level(d)) {
        t.bid_px[d] = l->price;
        t.bid_qty[d] = static_cast<std::uint32_t>(l->shares);
      }
      if (const auto* l = book.asks().level(d)) {
        t.ask_px[d] = l->price;
        t.ask_qty[d] = static_cast<std::uint32_t>(l->shares);
      }
    }
    const char state = books_->instrument(locate).trading_state;
    t.trading_state = state == ' ' ? 'T' : state;
    auto& last = last_[static_cast<std::size_t>(inst)];
    if (std::memcmp(t.bid_px, last.bid_px, 32) == 0 && t.trading_state == last.trading_state)
      return;  // top two levels and state unchanged
    t.rx_tsc = rx_tsc;
    t.feed_ns = books_->last_timestamp();
    t.instrument = static_cast<std::uint16_t>(inst);
    t.locate = locate;
    last = t;
    if (log_.size() < log_.capacity()) log_.push_back(t);  // reserved: no allocation
    while (!out_.try_push(t)) ++ring_full_spins_;
    ++tops_sent_;
  }

  std::vector<lab::oms::Symbol> symbols_;
  trader::TopRing& out_;
  std::unique_ptr<lab::book::BookBuilder> books_;
  std::vector<int> instrument_of_;  // by stock locate
  std::vector<lab::oms::TopOfBook> last_;
  std::vector<lab::oms::TopOfBook> log_;
  std::uint64_t tops_sent_ = 0;
  std::uint64_t ring_full_spins_ = 0;
  std::size_t listed_ = 0;
  std::atomic<bool> finished_{false};
};

struct Rings {
  trader::TopRing tops;
  trader::RequestRing requests;
  trader::ResponseRing responses;
};

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  const auto clock = lab::TscClock::calibrate();

  // Everything is allocated before the threads start.
  auto rings = std::make_unique<Rings>();
  auto md = std::make_unique<MarketData>(opt.trading.symbols, rings->tops,
                                         opt.market_csv.empty() ? 0 : 4u << 20);
  auto oe = std::make_unique<trader::OrderEntry>(opt.oe, rings->requests, rings->responses);
  auto trading = std::make_unique<trader::Trading>(opt.trading, rings->tops, rings->requests,
                                                   rings->responses, clock);
  std::unique_ptr<lab::feed::FeedReceiver> rx;
  try {
    rx = std::make_unique<lab::feed::FeedReceiver>(opt.feed);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "trader: %s\n", e.what());
    return 2;
  }
  rx->start();
  std::atomic<bool> md_stop{false}, trading_stop{false}, oe_stop{false};
  std::thread md_thread{[&] { md->run(*rx, opt.md_cpu, md_stop); }};
  try {
    oe->login();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "trader: order entry on %s: %s\n", opt.oe.connect.c_str(), e.what());
    md_stop = true;
    md_thread.join();
    rx->stop();
    return 2;
  }
  std::fprintf(stderr, "trader: %s on %s, market data %s, %zu symbols\n", opt.oe.user.c_str(),
               opt.oe.connect.c_str(), rx->describe().c_str(), opt.trading.symbols.size());

  const auto start = std::chrono::steady_clock::now();
  std::thread oe_thread{[&] { oe->run(oe_stop); }};
  std::thread trading_thread{[&] { trading->run(trading_stop); }};

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  // Main thread: wait for the duration, the end of the feed, or Ctrl-C.
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (g_interrupted.load() || md->finished() || oe->stats().session_ended ||
        (opt.duration > 0 && secs >= opt.duration)) {
      break;
    }
  }
  trading_stop = true;
  trading_thread.join();
  oe_stop = true;
  oe_thread.join();
  md_stop = true;
  md_thread.join();
  rx->stop();
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  const double scale = 1.0 / clock.ticks_per_ns();
  const auto& oes = oe->stats();
  std::printf("trader %s ran %.2f s on %zu symbols (%zu listed on the feed)\n", opt.oe.user.c_str(),
              secs, opt.trading.symbols.size(), md->listed());
  rx->print_report(stdout);
  const auto& checks = md->books().checks();
  std::printf("Books        %llu integrity errors, %llu top-of-book updates to the trader\n",
              static_cast<unsigned long long>(checks.error_count()),
              static_cast<unsigned long long>(md->tops_sent()));
  const bool trading_ok = trading->report(stdout);
  std::printf("Session      %llu requests, %llu responses, %s\n",
              static_cast<unsigned long long>(oes.requests),
              static_cast<unsigned long long>(oes.responses),
              oes.connection_lost ? "connection lost"
              : oes.session_ended ? "ended by the exchange"
                                  : "logged out");
  oes.tick_to_wire.print(stdout, "Tick to wire", scale);
  oes.decide_to_wire.print(stdout, "Decide to wire", scale);
  oes.round_trip.print(stdout, "Order round trip", scale);

  bool ok = trading_ok;
  if (rx->sequencer().stats().lost != 0 || checks.error_count() != 0) {
    std::printf("CHECK FAILED: market data lost messages or failed integrity checks\n");
    ok = false;
  }
  if (oes.connection_lost || oes.undecodable != 0) {
    std::printf("CHECK FAILED: order entry connection lost or undecodable messages\n");
    ok = false;
  }
  if (md->listed() != opt.trading.symbols.size()) {
    std::printf("CHECK FAILED: %zu of %zu symbols were never listed on the feed\n",
                opt.trading.symbols.size() - md->listed(), opt.trading.symbols.size());
    ok = false;
  }
  if (!opt.fills_csv.empty() && !trading->write_fills(opt.fills_csv)) {
    std::printf("cannot write %s\n", opt.fills_csv.c_str());
    ok = false;
  }
  if (!opt.market_csv.empty() && !md->write_log(opt.market_csv)) {
    std::printf("cannot write %s\n", opt.market_csv.c_str());
    ok = false;
  }
  std::printf("Trader checks: %s\n", ok ? "OK" : "FAILED");
  return ok ? 0 : 1;
}
