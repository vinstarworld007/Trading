// itch-dump: replays a Nasdaq ITCH 5.0 day from a file, rebuilds every order
// book and reports data quality.
//
//   itch-dump ~/data/itch/01302019.NASDAQ_ITCH50.gz
//   itch-dump FILE --symbol AAPL --depth 10      print AAPL's final book
//   itch-dump FILE --top 20 --csv stats.csv      top symbols, all stats to CSV
//   itch-dump FILE --limit 10000000              stop after 10M messages
//
// Exits 0 when every integrity check is zero, 1 when any is not, 2 on usage
// or read errors, so it can gate scripts and CI.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "lab/book/book_builder.hpp"
#include "lab/core/clock.hpp"
#include "lab/core/histogram.hpp"
#include "lab/itch/file_reader.hpp"

namespace {

using lab::book::BookBuilder;
using lab::book::Instrument;

struct Options {
  std::string path;
  std::string symbol;
  std::string csv;
  std::uint64_t limit = 0;
  std::size_t depth = 10;
  std::size_t top = 10;
  bool progress = true;
};

void usage() {
  std::fprintf(stderr,
               "usage: itch-dump FILE [--symbol SYM] [--depth N] [--top N] [--csv OUT] "
               "[--limit N] [--quiet]\n");
}

bool parse(int argc, char** argv, Options& o) {
  if (argc < 2) return false;
  o.path = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--quiet") {
      o.progress = false;
      continue;
    }
    const char* v = value();
    if (v == nullptr) return false;
    if (a == "--symbol")
      o.symbol = v;
    else if (a == "--csv")
      o.csv = v;
    else if (a == "--limit")
      o.limit = std::strtoull(v, nullptr, 10);
    else if (a == "--depth")
      o.depth = std::strtoul(v, nullptr, 10);
    else if (a == "--top")
      o.top = std::strtoul(v, nullptr, 10);
    else
      return false;
  }
  return true;
}

std::string price(std::uint32_t p) {
  if (p == 0) return "-";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%u.%04u", p / 10000, p % 10000);
  return buf;
}

std::string clock_time(std::uint64_t ns_since_midnight) {
  const std::uint64_t s = ns_since_midnight / 1'000'000'000ULL;
  char buf[32];
  std::snprintf(
      buf, sizeof buf, "%02llu:%02llu:%02llu.%09llu", static_cast<unsigned long long>(s / 3600),
      static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60),
      static_cast<unsigned long long>(ns_since_midnight % 1'000'000'000ULL));
  return buf;
}

const char* type_name(char t) {
  switch (t) {
    case 'S':
      return "system event";
    case 'R':
      return "stock directory";
    case 'H':
      return "trading action";
    case 'Y':
      return "reg SHO";
    case 'L':
      return "participant position";
    case 'V':
      return "MWCB decline level";
    case 'W':
      return "MWCB status";
    case 'K':
      return "IPO quoting period";
    case 'J':
      return "LULD auction collar";
    case 'h':
      return "operational halt";
    case 'A':
      return "add order";
    case 'F':
      return "add order (MPID)";
    case 'E':
      return "order executed";
    case 'C':
      return "executed with price";
    case 'X':
      return "order cancel";
    case 'D':
      return "order delete";
    case 'U':
      return "order replace";
    case 'P':
      return "trade (hidden)";
    case 'Q':
      return "cross trade";
    case 'B':
      return "broken trade";
    case 'I':
      return "NOII";
    case 'N':
      return "RPII";
    case 'O':
      return "direct listing";
    default:
      return "unknown";
  }
}

void print_book(const BookBuilder& b, std::uint16_t locate, std::size_t depth) {
  const Instrument& inst = b.instrument(locate);
  std::printf("\nBook for %.*s (locate %u), trading state %c:\n",
              static_cast<int>(lab::itch::trimmed(inst.symbol).size()),
              lab::itch::trimmed(inst.symbol).data(), locate, inst.trading_state);
  std::printf("  %10s %8s %12s | %-12s %-8s %-10s\n", "bid shares", "orders", "bid", "ask",
              "orders", "ask shares");
  for (std::size_t d = 0; d < depth; ++d) {
    const auto* bid = inst.book.bids().level(d);
    const auto* ask = inst.book.asks().level(d);
    if (bid == nullptr && ask == nullptr) break;
    std::printf(
        "  %10s %8s %12s | %-12s %-8s %-10s\n", bid ? std::to_string(bid->shares).c_str() : "",
        bid ? std::to_string(bid->orders).c_str() : "", bid ? price(bid->price).c_str() : "",
        ask ? price(ask->price).c_str() : "", ask ? std::to_string(ask->orders).c_str() : "",
        ask ? std::to_string(ask->shares).c_str() : "");
  }
}

bool write_csv(const BookBuilder& b, const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (f == nullptr) return false;
  std::fprintf(f,
               "symbol,locate,adds,executions,cancels,deletes,replaces,executed_shares,"
               "non_printable_shares,hidden_trade_shares,cross_shares,matched_volume,"
               "broken_trades,opening_cross,closing_cross,last_trade,best_bid,best_ask,"
               "bid_levels,ask_levels,trading_state\n");
  for (std::uint16_t l : b.listed_locates()) {
    const Instrument& inst = b.instrument(l);
    const auto& s = inst.stats;
    const auto* bid = inst.book.bids().best();
    const auto* ask = inst.book.asks().best();
    const auto sym = lab::itch::trimmed(inst.symbol);
    std::fprintf(
        f,
        "%.*s,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%s,%s,%s,%s,%s,%zu,%zu,%"
        "c\n",
        static_cast<int>(sym.size()), sym.data(), l, static_cast<unsigned long long>(s.adds),
        static_cast<unsigned long long>(s.executions), static_cast<unsigned long long>(s.cancels),
        static_cast<unsigned long long>(s.deletes), static_cast<unsigned long long>(s.replaces),
        static_cast<unsigned long long>(s.executed_shares),
        static_cast<unsigned long long>(s.non_printable_shares),
        static_cast<unsigned long long>(s.hidden_trade_shares),
        static_cast<unsigned long long>(s.cross_shares),
        static_cast<unsigned long long>(s.matched_volume()),
        static_cast<unsigned long long>(s.broken_trades), price(s.opening_cross_price).c_str(),
        price(s.closing_cross_price).c_str(), price(s.last_trade_price).c_str(),
        bid ? price(bid->price).c_str() : "-", ask ? price(ask->price).c_str() : "-",
        inst.book.bids().depth(), inst.book.asks().depth(), inst.trading_state);
  }
  return std::fclose(f) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse(argc, argv, opt)) {
    usage();
    return 2;
  }

  lab::itch::FileReader reader{opt.path};
  if (!reader.ok()) {
    std::fprintf(stderr, "itch-dump: %s\n", reader.error().c_str());
    return 2;
  }

  auto builder = std::make_unique<BookBuilder>();
  lab::LatencyHistogram apply_ticks;  // sampled cost of decode + book update
  const auto clock = lab::TscClock::calibrate(std::chrono::milliseconds{20});
  const auto wall_start = std::chrono::steady_clock::now();
  std::uint64_t n = 0;

  while (auto msg = reader.next()) {
    if ((n & 63) == 0) {
      const std::uint64_t t0 = lab::rdtsc();
      builder->apply(*msg);
      apply_ticks.record(lab::rdtscp() - t0);
    } else {
      builder->apply(*msg);
    }
    ++n;
    if (opt.progress && (n % 50'000'000) == 0) {
      std::fprintf(stderr, "  %llu M messages, feed time %s\n",
                   static_cast<unsigned long long>(n / 1'000'000),
                   clock_time(builder->last_timestamp()).c_str());
    }
    if (opt.limit != 0 && n >= opt.limit) break;
  }
  if (!reader.ok()) {
    std::fprintf(stderr, "itch-dump: %s after %llu messages\n", reader.error().c_str(),
                 static_cast<unsigned long long>(n));
  }
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
  const BookBuilder& b = *builder;

  std::printf("File        %s\n", opt.path.c_str());
  std::printf("Messages    %llu (%.1f MB decompressed), last feed time %s\n",
              static_cast<unsigned long long>(n), static_cast<double>(reader.bytes_read()) / 1e6,
              clock_time(b.last_timestamp()).c_str());
  std::printf("Elapsed     %.2f s, %.2f M msgs/s including decompression\n", secs,
              secs > 0 ? static_cast<double>(n) / secs / 1e6 : 0.0);
  apply_ticks.print(stdout, "Apply cost (1 in 64)", 1.0 / clock.ticks_per_ns());
  std::printf("Live orders %zu at end of input\n", b.orders().size());

  std::printf("\nMessage types:\n");
  for (char t : std::string{"SRHYLVWKJhAFECXDUPQBINO"}) {
    if (const auto c = b.type_count(t)) {
      std::printf("  %c  %-22s %12llu\n", t, type_name(t), static_cast<unsigned long long>(c));
    }
  }
  std::printf("System events seen: %s\n",
              b.system_events().empty() ? "none" : b.system_events().c_str());

  const auto& c = b.checks();
  std::printf("\nIntegrity checks:\n");
  auto line = [](const char* name, std::uint64_t v, bool is_error) {
    std::printf("  %-6s %-32s %llu\n", v == 0 ? "OK" : (is_error ? "ERROR" : "INFO"), name,
                static_cast<unsigned long long>(v));
  };
  line("unknown message type", c.unknown_message_type, true);
  line("truncated message", c.truncated_message, true);
  line("unknown order reference", c.unknown_order_reference, true);
  line("duplicate order reference", c.duplicate_order_reference, true);
  line("over-execution", c.over_execution, true);
  line("locate without directory", c.locate_without_directory, true);
  line("locate/symbol mismatch", c.locate_symbol_mismatch, true);
  line("book inconsistency", c.book_inconsistency, true);
  line("timestamp regressions", c.timestamp_regressions, true);
  line("system events out of order", c.system_event_out_of_order, true);
  line("books became crossed", c.crossed_book_events, false);
  line("books became locked", c.locked_book_events, false);
  if (opt.limit != 0) {
    std::printf("  (stopped early at --limit: end-of-day checks are incomplete)\n");
  }

  std::vector<std::uint16_t> locates = b.listed_locates();
  std::sort(locates.begin(), locates.end(), [&](std::uint16_t x, std::uint16_t y) {
    return b.instrument(x).stats.matched_volume() > b.instrument(y).stats.matched_volume();
  });
  std::printf("\nTop %zu by matched volume:\n", std::min(opt.top, locates.size()));
  std::printf("  %-8s %12s %10s %10s %10s %10s %10s %10s\n", "symbol", "volume", "adds", "execs",
              "open", "close", "best bid", "best ask");
  for (std::size_t i = 0; i < opt.top && i < locates.size(); ++i) {
    const Instrument& inst = b.instrument(locates[i]);
    const auto sym = lab::itch::trimmed(inst.symbol);
    const auto* bid = inst.book.bids().best();
    const auto* ask = inst.book.asks().best();
    std::printf("  %-8.*s %12llu %10llu %10llu %10s %10s %10s %10s\n", static_cast<int>(sym.size()),
                sym.data(), static_cast<unsigned long long>(inst.stats.matched_volume()),
                static_cast<unsigned long long>(inst.stats.adds),
                static_cast<unsigned long long>(inst.stats.executions),
                price(inst.stats.opening_cross_price).c_str(),
                price(inst.stats.closing_cross_price).c_str(),
                bid ? price(bid->price).c_str() : "-", ask ? price(ask->price).c_str() : "-");
  }

  if (!opt.symbol.empty()) {
    if (const auto l = b.find_locate(opt.symbol))
      print_book(b, l, opt.depth);
    else
      std::printf("\nSymbol %s not found in the stock directory\n", opt.symbol.c_str());
  }

  if (!opt.csv.empty()) {
    if (write_csv(b, opt.csv))
      std::printf("\nWrote per-symbol stats to %s\n", opt.csv.c_str());
    else
      std::fprintf(stderr, "itch-dump: cannot write %s\n", opt.csv.c_str());
  }

  if (!reader.ok()) return 2;
  return c.error_count() == 0 ? 0 : 1;
}
