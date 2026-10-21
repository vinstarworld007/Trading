#include "lab/book/report.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace lab::book {

std::string format_price(std::uint32_t p) {
  if (p == 0) return "-";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%u.%04u", p / 10000, p % 10000);
  return buf;
}

std::string format_feed_time(std::uint64_t ns_since_midnight) {
  const std::uint64_t s = ns_since_midnight / 1'000'000'000ULL;
  char buf[32];
  std::snprintf(
      buf, sizeof buf, "%02llu:%02llu:%02llu.%09llu", static_cast<unsigned long long>(s / 3600),
      static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60),
      static_cast<unsigned long long>(ns_since_midnight % 1'000'000'000ULL));
  return buf;
}

const char* message_type_name(char t) {
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

void print_message_types(std::FILE* out, const BookBuilder& b) {
  std::fprintf(out, "\nMessage types:\n");
  for (char t : std::string{"SRHYLVWKJhAFECXDUPQBINO"}) {
    if (const auto c = b.type_count(t)) {
      std::fprintf(out, "  %c  %-22s %12llu\n", t, message_type_name(t),
                   static_cast<unsigned long long>(c));
    }
  }
  std::fprintf(out, "System events seen: %s\n",
               b.system_events().empty() ? "none" : b.system_events().c_str());
}

void print_integrity(std::FILE* out, const IntegrityChecks& c) {
  std::fprintf(out, "\nIntegrity checks:\n");
  auto line = [out](const char* name, std::uint64_t v, bool is_error) {
    std::fprintf(out, "  %-6s %-32s %llu\n", v == 0 ? "OK" : (is_error ? "ERROR" : "INFO"), name,
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
}

void print_book(std::FILE* out, const BookBuilder& b, std::uint16_t locate, std::size_t depth) {
  const Instrument& inst = b.instrument(locate);
  std::fprintf(out, "\nBook for %.*s (locate %u), trading state %c:\n",
               static_cast<int>(lab::itch::trimmed(inst.symbol).size()),
               lab::itch::trimmed(inst.symbol).data(), locate, inst.trading_state);
  std::fprintf(out, "  %10s %8s %12s | %-12s %-8s %-10s\n", "bid shares", "orders", "bid", "ask",
               "orders", "ask shares");
  for (std::size_t d = 0; d < depth; ++d) {
    const auto* bid = inst.book.bids().level(d);
    const auto* ask = inst.book.asks().level(d);
    if (bid == nullptr && ask == nullptr) break;
    std::fprintf(
        out, "  %10s %8s %12s | %-12s %-8s %-10s\n", bid ? std::to_string(bid->shares).c_str() : "",
        bid ? std::to_string(bid->orders).c_str() : "", bid ? format_price(bid->price).c_str() : "",
        ask ? format_price(ask->price).c_str() : "", ask ? std::to_string(ask->orders).c_str() : "",
        ask ? std::to_string(ask->shares).c_str() : "");
  }
}

void print_top(std::FILE* out, const BookBuilder& b, std::size_t n) {
  if (n == 0) return;
  std::vector<std::uint16_t> locates = b.listed_locates();
  std::sort(locates.begin(), locates.end(), [&](std::uint16_t x, std::uint16_t y) {
    return b.instrument(x).stats.matched_volume() > b.instrument(y).stats.matched_volume();
  });
  std::fprintf(out, "\nTop %zu by matched volume:\n", std::min(n, locates.size()));
  std::fprintf(out, "  %-8s %12s %10s %10s %10s %10s %10s %10s\n", "symbol", "volume", "adds",
               "execs", "open", "close", "best bid", "best ask");
  for (std::size_t i = 0; i < n && i < locates.size(); ++i) {
    const Instrument& inst = b.instrument(locates[i]);
    const auto sym = lab::itch::trimmed(inst.symbol);
    const auto* bid = inst.book.bids().best();
    const auto* ask = inst.book.asks().best();
    std::fprintf(
        out, "  %-8.*s %12llu %10llu %10llu %10s %10s %10s %10s\n", static_cast<int>(sym.size()),
        sym.data(), static_cast<unsigned long long>(inst.stats.matched_volume()),
        static_cast<unsigned long long>(inst.stats.adds),
        static_cast<unsigned long long>(inst.stats.executions),
        format_price(inst.stats.opening_cross_price).c_str(),
        format_price(inst.stats.closing_cross_price).c_str(),
        bid ? format_price(bid->price).c_str() : "-", ask ? format_price(ask->price).c_str() : "-");
  }
}

}  // namespace lab::book
