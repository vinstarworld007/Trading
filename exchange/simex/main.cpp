// simex: the lab's exchange. Firms send OUCH 5.0 orders over SoupBinTCP; a
// single matching thread sequences and matches them in price-time priority;
// the results go back to each firm and out to everyone as an ITCH 5.0 feed
// over MoldUDP64. A Nasdaq ITCH day can be replayed in as historical
// liquidity, so firm orders meet a realistic book.
//
//   simex --replay ~/data/itch/01302019.NASDAQ_ITCH50.gz --rate 1000000
//   simex --symbols AAPL,MSFT,SPY                 empty books, firms only
//
// Threads (see docs/core-plan.md for cores):
//
//   gateway    TCP sessions, OUCH decode and validation, sequenced responses
//   matcher    the sequencer and matching engine; no syscalls
//   md         ITCH encode is done by the matcher; this thread packs MoldUDP64,
//              serves re-requests and checks the feed by rebuilding its books
//   journal    writes every sequenced command to --journal
//   liquidity  replays --replay into the matcher
//
// Runs until Ctrl-C, --duration seconds, or the end of --replay plus --linger
// seconds. Then it closes the market (ITCH M, E, C), sends End of Day and
// End of Session, prints a report and checks that the matching engine's books
// equal the books rebuilt from its own feed. Exit 0 when they do.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gateway.hpp"
#include "lab/book/book_builder.hpp"
#include "lab/book/report.hpp"
#include "lab/core/affinity.hpp"
#include "lab/core/clock.hpp"
#include "lab/feed/publisher.hpp"
#include "liquidity.hpp"
#include "matcher.hpp"

namespace {

using namespace simex;

struct Options {
  GatewayConfig gateway;
  lab::feed::PublisherConfig md;
  LiquidityConfig liquidity;
  std::vector<std::string> symbols;
  ScenarioConfig scenarios;
  std::vector<lab::book::StripDef> strips;
  bool scenario_all = false;
  std::string journal;
  double duration = 0;  // seconds; 0 = until Ctrl-C or end of replay
  double linger = 2;    // seconds after the replay ends
  double eos_seconds = 2;
  int matcher_cpu = -1;
  int gateway_cpu = -1;
  int md_cpu = -1;
};

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::stringstream in{s};
  for (std::string item; std::getline(in, item, sep);) {
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

void usage() {
  std::fprintf(
      stderr,
      "usage: simex [--replay FILE [--rate N | --speed X] [--limit N] [--linger S]]\n"
      "             [--symbols A,B,C] [--accounts USER:PASS,...] [--listen IP:PORT]\n"
      "             [--drop-copy USER:PASS | --no-drop-copy]\n"
      "             [--md-dest IP:PORT] [--md-iface IP] [--md-retrans-port N]\n"
      "             [--md-session NAME] [--journal FILE] [--duration S]\n"
      "             [--eos-seconds S] [--no-cancel-on-disconnect]\n"
      "             [--matcher-cpu N] [--gateway-cpu N] [--md-cpu N]\n"
      "  scenarios:  [--scenario all] [--open-auction S] [--close-at S --close-auction S]\n"
      "             [--noii-interval S] [--quote-period S] [--halt SYM@T+D[:REASON]]\n"
      "             [--op-halt SYM@T+D] [--ipo SYM@T+D] [--mwcb LEVEL@T+D]\n"
      "             [--break-at T] [--luld] [--luld-band-pct N] [--luld-limit S]\n"
      "             [--luld-pause S] [--reg-sho] [--strip NAME=A+B]\n");
}

bool parse(int argc, char** argv, Options& o) {
  o.md.dest = "239.1.1.2:31001";
  o.md.retrans_port = 31002;
  o.md.session = "SIMEX00001";
  o.md.max_delay_us = 5;
  std::string accounts = "FIRM01:lab,FIRM02:lab,FIRM03:lab,AGENT1:lab,AGENT2:lab";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--no-cancel-on-disconnect") {
      o.gateway.cancel_on_disconnect = false;
      continue;
    }
    if (a == "--no-drop-copy") {
      o.gateway.drop_copy = {};
      continue;
    }
    if (a == "--luld") {
      o.scenarios.luld = true;
      continue;
    }
    if (a == "--reg-sho") {
      o.scenarios.reg_sho = true;
      continue;
    }
    if (i + 1 >= argc) return false;
    const char* v = argv[++i];
    if (a == "--replay")
      o.liquidity.path = v;
    else if (a == "--rate")
      o.liquidity.rate = std::strtod(v, nullptr);
    else if (a == "--speed")
      o.liquidity.speed = std::strtod(v, nullptr);
    else if (a == "--limit")
      o.liquidity.limit = std::strtoull(v, nullptr, 10);
    else if (a == "--linger")
      o.linger = std::strtod(v, nullptr);
    else if (a == "--symbols")
      o.symbols = split(v, ',');
    else if (a == "--accounts")
      accounts = v;
    else if (a == "--drop-copy") {
      const std::string d = v;
      const auto colon = d.find(':');
      if (colon == std::string::npos) return false;
      o.gateway.drop_copy = {d.substr(0, colon), d.substr(colon + 1)};
    } else if (a == "--listen")
      o.gateway.listen = v;
    else if (a == "--md-dest")
      o.md.dest = v;
    else if (a == "--md-iface")
      o.md.iface = v;
    else if (a == "--md-retrans-port")
      o.md.retrans_port = std::atoi(v);
    else if (a == "--md-session")
      o.md.session = v;
    else if (a == "--journal")
      o.journal = v;
    else if (a == "--duration")
      o.duration = std::strtod(v, nullptr);
    else if (a == "--eos-seconds")
      o.eos_seconds = std::strtod(v, nullptr);
    else if (a == "--matcher-cpu")
      o.matcher_cpu = std::atoi(v);
    else if (a == "--gateway-cpu")
      o.gateway_cpu = std::atoi(v);
    else if (a == "--md-cpu")
      o.md_cpu = std::atoi(v);
    else if (a == "--scenario") {
      if (std::string{v} != "all") return false;
      o.scenario_all = true;
    } else if (a == "--open-auction")
      o.scenarios.open_auction_s = std::strtod(v, nullptr);
    else if (a == "--close-at")
      o.scenarios.close_at_s = std::strtod(v, nullptr);
    else if (a == "--close-auction")
      o.scenarios.close_auction_s = std::strtod(v, nullptr);
    else if (a == "--noii-interval")
      o.scenarios.noii_interval_s = std::strtod(v, nullptr);
    else if (a == "--quote-period")
      o.scenarios.quote_period_s = std::strtod(v, nullptr);
    else if (a == "--luld-band-pct")
      o.scenarios.luld_band_pct = static_cast<std::uint32_t>(std::atoi(v));
    else if (a == "--luld-limit")
      o.scenarios.luld_limit_s = std::strtod(v, nullptr);
    else if (a == "--luld-pause")
      o.scenarios.luld_pause_s = std::strtod(v, nullptr);
    else if (a == "--break-at")
      o.scenarios.breaks.push_back(std::strtod(v, nullptr));
    else if (a == "--halt" || a == "--op-halt" || a == "--ipo") {
      const auto t = parse_timed(v);
      if (!t) return false;
      (a == "--halt"      ? o.scenarios.halts
       : a == "--op-halt" ? o.scenarios.op_halts
                          : o.scenarios.ipos)
          .push_back(*t);
    } else if (a == "--strip") {
      const auto d = lab::book::parse_strip(v);
      if (!d) return false;
      o.strips.push_back(*d);
    } else if (a == "--mwcb") {
      const auto t = parse_timed(v);
      if (!t || (t->symbol != "1" && t->symbol != "2" && t->symbol != "3")) return false;
      o.scenarios.mwcb = MwcbEvent{t->symbol[0] - '0', t->at_s, t->for_s};
    } else
      return false;
  }
  for (const auto& acc : split(accounts, ',')) {
    const auto colon = acc.find(':');
    if (colon == std::string::npos) return false;
    o.gateway.accounts.push_back({acc.substr(0, colon), acc.substr(colon + 1)});
  }
  if (o.liquidity.path.empty() && o.symbols.empty()) o.symbols = {"AAPL", "MSFT", "SPY"};
  if (o.scenario_all) o.scenarios = all_scenarios(o.duration, o.symbols);
  for (const auto& ipo : o.scenarios.ipo_symbols()) o.symbols.push_back(ipo);
  for (const auto& st : o.strips) o.symbols.push_back(st.name);
  return !o.gateway.accounts.empty();
}

void pin(int cpu, const char* name) {
  lab::set_thread_name(name);
  if (cpu < 0) return;
  if (auto ec = lab::pin_this_thread(cpu)) {
    std::fprintf(stderr, "simex: cannot pin %s to cpu %d: %s\n", name, cpu, ec.message().c_str());
  }
}

std::atomic<bool> g_interrupted{false};
extern "C" void on_signal(int) { g_interrupted.store(true, std::memory_order_relaxed); }

struct Rings {
  CommandRing gateway;
  CommandRing liquidity;
  SessionRing sessions;
  MdRing md;
  JournalRing journal;
};

struct Flags {
  std::atomic<bool> accepting{true};       // gateway takes new orders
  std::atomic<bool> gateway_quiet{false};  // gateway acknowledged: no more commands
  std::atomic<bool> stop_liquidity{false};
  std::atomic<bool> liquidity_done{false};
  std::atomic<bool> closing{false};  // matcher: drain inputs and close the market
  std::atomic<bool> matcher_done{false};
  std::atomic<bool> stop_gateway{false};
};

// Market data thread: publishes the matcher's ITCH messages and rebuilds the
// books from them, as a receiver would, to check the feed.
void md_loop(const Options& opt, MdRing& ring, lab::feed::MoldPublisher& pub,
             lab::book::BookBuilder& books, const Flags& f) {
  pin(opt.md_cpu, "simex-md");
  for (;;) {
    const std::uint64_t now = lab::mono_ns();
    bool any = false;
    for (int i = 0; i < 64; ++i) {
      const MdEvent* e = ring.front();
      if (e == nullptr) break;
      const std::span<const std::byte> msg{e->data, e->len};
      pub.add(msg, now);
      books.apply(msg);
      ring.pop();
      any = true;
    }
    pub.service(now);
    if (!any && f.matcher_done.load(std::memory_order_acquire) && ring.empty_approx()) break;
  }
  pub.finish(opt.eos_seconds);
}

// Journal thread: appends each sequenced command as a raw 64-byte record.
void journal_loop(std::FILE* out, JournalRing& ring, const Flags& f, std::uint64_t& written) {
  lab::set_thread_name("simex-journal");
  Command c;
  for (;;) {
    bool any = false;
    while (ring.try_pop(c)) {
      std::fwrite(&c, sizeof c, 1, out);
      ++written;
      any = true;
    }
    if (!any) {
      if (f.matcher_done.load(std::memory_order_acquire) && ring.empty_approx()) break;
      std::this_thread::sleep_for(std::chrono::microseconds{200});
    }
  }
  std::fflush(out);
}

// Compares every instrument's engine book with the book rebuilt from the feed.
std::size_t check_books(const Matcher& m, const lab::book::BookBuilder& feed) {
  std::size_t bad = 0;
  for (std::uint16_t l : m.locates()) {
    for (auto side : {lab::match::Side::kBuy, lab::match::Side::kSell}) {
      const auto want = m.engine().depth(l, side, ~std::size_t{0});
      const auto& got = side == lab::match::Side::kBuy ? feed.book(l).bids() : feed.book(l).asks();
      bool same = want.size() == got.depth();
      for (std::size_t d = 0; same && d < want.size(); ++d) {
        const auto* lv = got.level(d);
        if (lv == nullptr) {
          same = false;
          break;
        }
        same = lv->price == want[d].price && lv->shares == want[d].shares &&
               lv->orders == want[d].orders;
      }
      if (!same) ++bad;
    }
  }
  return bad;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse(argc, argv, opt)) {
    usage();
    return 2;
  }

  // Everything is allocated and faulted in before the threads start.
  auto rings = std::make_unique<Rings>();
  auto flags = std::make_unique<Flags>();
  auto feed_books = std::make_unique<lab::book::BookBuilder>();
  const auto clock = lab::TscClock::calibrate(std::chrono::milliseconds{50});
  lab::match::EngineConfig ec;
  ec.max_orders = 1u << 22;
  ec.levels_per_side = 256;
  ec.max_quantity =
      0x7FFFFFFF;  // historical orders can exceed OUCH's limit; the gateway checks firms
  std::unique_ptr<Matcher> matcher;
  std::unique_ptr<Gateway> gateway;
  std::unique_ptr<lab::feed::MoldPublisher> pub;
  std::FILE* journal = nullptr;
  try {
    matcher = std::make_unique<Matcher>(
        ec,
        Matcher::Rings{&rings->gateway, &rings->liquidity, &rings->sessions, &rings->md,
                       opt.journal.empty() ? nullptr : &rings->journal},
        clock);
    gateway = std::make_unique<Gateway>(opt.gateway, rings->gateway, rings->sessions);
    pub = std::make_unique<lab::feed::MoldPublisher>(opt.md);
    if (!opt.journal.empty()) {
      journal = std::fopen(opt.journal.c_str(), "wb");
      if (journal == nullptr) throw std::runtime_error("cannot open journal " + opt.journal);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "simex: %s\n", e.what());
    return 2;
  }
  if (!opt.symbols.empty()) matcher->list_symbols(opt.symbols);
  matcher->set_scenarios(opt.scenarios);
  matcher->set_strips(opt.strips);
  std::fprintf(stderr, "simex: orders on %s (%zu accounts), market data %s\n",
               gateway->address().c_str(), opt.gateway.accounts.size(), pub->describe().c_str());

  const auto wall_start = std::chrono::steady_clock::now();
  std::thread md{md_loop,        std::cref(opt),        std::ref(rings->md),
                 std::ref(*pub), std::ref(*feed_books), std::cref(*flags)};
  std::uint64_t journaled = 0;
  std::thread jr;
  if (journal != nullptr) {
    jr = std::thread{journal_loop, journal, std::ref(rings->journal), std::cref(*flags),
                     std::ref(journaled)};
  }
  std::thread mt{[&] {
    pin(opt.matcher_cpu, "simex-match");
    matcher->run(flags->closing);
    flags->matcher_done.store(true, std::memory_order_release);
  }};
  std::thread gw{[&] {
    pin(opt.gateway_cpu, "simex-gateway");
    gateway->run(flags->accepting, flags->gateway_quiet, flags->stop_gateway);
  }};
  LiquidityStats liq;
  std::thread lq;
  if (!opt.liquidity.path.empty()) {
    lq = std::thread{[&] {
      lab::set_thread_name("simex-liquidity");
      liq = replay_liquidity(opt.liquidity, rings->liquidity, flags->stop_liquidity);
      flags->liquidity_done.store(true, std::memory_order_release);
    }};
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::chrono::steady_clock::time_point replay_end{};
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const auto now = std::chrono::steady_clock::now();
    if (g_interrupted.load()) break;
    if (opt.duration > 0 && std::chrono::duration<double>(now - wall_start).count() >= opt.duration)
      break;
    if (lq.joinable() && flags->liquidity_done.load(std::memory_order_acquire)) {
      if (replay_end == std::chrono::steady_clock::time_point{}) replay_end = now;
      if (std::chrono::duration<double>(now - replay_end).count() >= opt.linger) break;
    }
  }

  // Close: stop intake, let the matcher drain and close the market, then the
  // gateway sends the last responses and End of Session.
  flags->stop_liquidity.store(true);
  if (lq.joinable()) lq.join();
  flags->accepting.store(false, std::memory_order_release);
  while (!flags->gateway_quiet.load(std::memory_order_acquire)) std::this_thread::yield();
  flags->closing.store(true, std::memory_order_release);
  mt.join();
  flags->stop_gateway.store(true, std::memory_order_release);
  gw.join();
  md.join();
  if (jr.joinable()) jr.join();
  if (journal != nullptr) std::fclose(journal);
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();

  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  const auto& es = matcher->engine().stats();
  const auto& ms = matcher->stats();
  const auto& gs = gateway->stats();
  std::printf("simex ran %.2f s\n", secs);
  if (!opt.liquidity.path.empty()) {
    std::printf("Replay       %s: %llu messages, %llu commands%s%s\n", opt.liquidity.path.c_str(),
                u(liq.messages), u(liq.commands), liq.ok ? "" : ", error: ", liq.error.c_str());
  }
  std::printf(
      "Matcher      %llu commands, %llu accepted, %llu rejected, %llu executions "
      "(%llu shares), %llu canceled, %llu replaced, %zu live orders\n",
      u(ms.commands), u(es.accepted), u(es.rejected), u(es.executions), u(es.executed_shares),
      u(es.canceled), u(es.replaced), matcher->engine().live_orders());
  std::printf(
      "             %llu historical orders already gone, %llu unknown cancels, "
      "%llu backpressure spins\n",
      u(ms.hist_orphans), u(ms.unknown_cancels), u(ms.backpressure_spins));
  ms.command_ticks.print(stdout, "Command      producer to matched", 1.0 / clock.ticks_per_ns());
  if (opt.scenarios.any() || !opt.strips.empty()) {
    const auto& ss = matcher->scenario_stats();
    std::printf(
        "Scenarios    %llu crosses (%llu shares), %llu imbalance messages, %llu halts, "
        "%llu LULD limit states, %llu pauses, %llu extensions, %llu operational halts, "
        "%llu broken trades, %llu Reg SHO, %llu orders refused while halted, "
        "%llu implied strip fills (%llu shares)\n",
        u(ss.crosses), u(ss.cross_shares), u(ss.noii), u(ss.halts), u(ss.limit_states),
        u(ss.pauses), u(ss.extensions), u(ss.op_halts), u(ss.broken_trades), u(ss.reg_sho),
        u(ss.halted_rejects), u(ss.implied_fills), u(ss.implied_shares));
  }
  std::printf(
      "Gateway      %llu connections, %llu logins (%llu rejected), %llu OUCH in, "
      "%llu out, %llu rejected at the gateway, %llu stale refs, %llu bad, %llu timeouts\n",
      u(gs.connections), u(gs.logins), u(gs.login_rejects), u(gs.inbound), u(gs.outbound),
      u(gs.gateway_rejects), u(gs.stale_refs), u(gs.bad_messages), u(gs.timeouts));
  if (!opt.gateway.drop_copy.user.empty()) {
    std::printf("Drop copy    %llu Execution Reports to %s\n", u(gs.drop_copies),
                opt.gateway.drop_copy.user.c_str());
  }
  if (journal != nullptr) {
    std::printf("Journal      %llu commands to %s\n", u(journaled), opt.journal.c_str());
  }
  pub->print_report(stdout, secs);
  const auto& fb = *feed_books;
  std::printf("Book digest  %016llx\n", static_cast<unsigned long long>(fb.digest()));
  lab::book::print_integrity(stdout, fb.checks());
  const std::size_t mismatched = check_books(*matcher, fb);
  std::printf("\nEngine books vs feed books: %zu instruments, %zu sides differ\n",
              matcher->locates().size(), mismatched);
  if (!liq.ok) return 2;
  return mismatched == 0 && fb.checks().error_count() == 0 ? 0 : 1;
}
