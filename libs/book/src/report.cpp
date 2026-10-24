#include "lab/book/report.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace lab::book {

std::string format_price(std::uint32_t p) {
  if (p == 0) return "-";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%u.%02u", p / 10000, p % 10000 / 100);
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
  line("trade while halted or paused", c.trade_while_halted, true);
  line("books became crossed", c.crossed_book_events, false);
  line("books became locked", c.locked_book_events, false);
  line("crosses (Q)", c.crosses, false);
  line("halts and pauses (H)", c.halts, false);
  line("imbalance messages (I)", c.noii, false);
  line("cross executions over its print", c.cross_overfill, false);
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

void print_scenarios(std::FILE* out, const BookBuilder& b) {
  std::string body;
  char line[256];
  const auto& lv = b.mwcb_levels();
  if (lv[0] != 0 || b.mwcb_breached() != ' ') {
    std::snprintf(line, sizeof line,
                  "  Market-wide circuit breaker levels %.2f / %.2f / %.2f, breached level %c\n",
                  static_cast<double>(lv[0]) / 1e8, static_cast<double>(lv[1]) / 1e8,
                  static_cast<double>(lv[2]) / 1e8,
                  b.mwcb_breached() == ' ' ? '-' : b.mwcb_breached());
    body += line;
  }
  for (const std::uint16_t l : b.listed_locates()) {
    const Instrument& inst = b.instrument(l);
    const bool any = inst.imbalance.cross_type != ' ' || inst.collar.reference != 0 ||
                     inst.ipo_qualifier != ' ' || inst.operational_halt != ' ' ||
                     inst.stats.cross_shares != 0 || inst.reg_sho_action != ' ' ||
                     (inst.trading_state != 'T' && inst.trading_state != ' ');
    if (!any) continue;
    const auto sym = lab::itch::trimmed(inst.symbol);
    const std::string reason{inst.halt_reason.data(), inst.halt_reason.size()};
    std::snprintf(line, sizeof line, "  %-8.*s state %c %-4s open %s close %s cross shares %llu",
                  static_cast<int>(sym.size()), sym.data(), inst.trading_state, reason.c_str(),
                  format_price(inst.stats.opening_cross_price).c_str(),
                  format_price(inst.stats.closing_cross_price).c_str(),
                  static_cast<unsigned long long>(inst.stats.cross_shares));
    body += line;
    if (inst.reg_sho_action != ' ') {
      std::snprintf(line, sizeof line, ", Reg SHO %c", inst.reg_sho_action);
      body += line;
    }
    body += '\n';
    const Imbalance& i = inst.imbalance;
    if (i.cross_type != ' ') {
      std::snprintf(line, sizeof line,
                    "           last NOII %s cross %c: paired %llu, imbalance %llu %c, near %s, "
                    "ref %s, variation %c\n",
                    format_feed_time(i.timestamp).c_str(), i.cross_type,
                    static_cast<unsigned long long>(i.paired),
                    static_cast<unsigned long long>(i.imbalance), i.direction,
                    format_price(i.near_price).c_str(), format_price(i.reference_price).c_str(),
                    i.variation);
      body += line;
    }
    if (inst.collar.reference != 0) {
      std::snprintf(line, sizeof line, "           LULD collar ref %s, %s to %s, %u extension(s)\n",
                    format_price(inst.collar.reference).c_str(),
                    format_price(inst.collar.lower).c_str(),
                    format_price(inst.collar.upper).c_str(), inst.collar.extensions);
      body += line;
    }
    if (inst.ipo_qualifier != ' ') {
      std::snprintf(line, sizeof line, "           IPO %c release %02u:%02u:%02u price %s\n",
                    inst.ipo_qualifier, inst.ipo_release_time / 3600,
                    inst.ipo_release_time / 60 % 60, inst.ipo_release_time % 60,
                    format_price(inst.ipo_price).c_str());
      body += line;
    }
    if (inst.operational_halt != ' ') {
      std::snprintf(line, sizeof line, "           operational halt: %s\n",
                    inst.operational_halt == 'H' ? "halted" : "resumed");
      body += line;
    }
  }
  if (body.empty()) return;
  std::fprintf(out, "\nMarket scenarios:\n%s", body.c_str());
}

void print_implied(std::FILE* out, const BookBuilder& b, const std::vector<StripDef>& strips,
                   std::size_t depth) {
  for (const StripDef& d : strips) {
    const std::uint16_t s = b.find_locate(d.name), x = b.find_locate(d.leg_a),
                        y = b.find_locate(d.leg_b);
    if (s == 0 || x == 0 || y == 0) {
      std::fprintf(out, "\nStrip %s: not listed on this feed\n", d.name.c_str());
      continue;
    }
    const OrderBook& sb = b.book(s);
    const OrderBook& xb = b.book(x);
    const OrderBook& yb = b.book(y);
    std::fprintf(out, "\nStrip %s = %s + %s (strip volume %llu):\n", d.name.c_str(),
                 d.leg_a.c_str(), d.leg_b.c_str(),
                 static_cast<unsigned long long>(b.instrument(s).stats.matched_volume()));
    const auto ib = implied_out(xb.bids(), yb.bids(), depth);
    const auto ia = implied_out(xb.asks(), yb.asks(), depth);
    std::fprintf(out, "  %10s %12s %12s | %-12s %-12s %-10s\n", "bid shares", "implied bid",
                 "strip bid", "strip ask", "implied ask", "ask shares");
    for (std::size_t i = 0; i < depth; ++i) {
      const Level* own_b = sb.bids().level(i);
      const Level* own_a = sb.asks().level(i);
      const bool have_b = i < ib.size(), have_a = i < ia.size();
      if (!own_b && !own_a && !have_b && !have_a) break;
      std::fprintf(out, "  %10s %12s %12s | %-12s %-12s %-10s\n",
                   have_b ? std::to_string(ib[i].shares).c_str() : "",
                   have_b ? format_price(ib[i].price).c_str() : "",
                   own_b ? format_price(own_b->price).c_str() : "",
                   own_a ? format_price(own_a->price).c_str() : "",
                   have_a ? format_price(ia[i].price).c_str() : "",
                   have_a ? std::to_string(ia[i].shares).c_str() : "");
    }
    for (const auto& [leg, other] : {std::pair{x, &yb}, std::pair{y, &xb}}) {
      const ImpliedQuote q = implied_in(sb, *other);
      const auto sym = lab::itch::trimmed(b.instrument(leg).symbol);
      const Level* bb = b.book(leg).bids().best();
      const Level* ba = b.book(leg).asks().best();
      std::fprintf(out, "  %-8.*s outright %s / %s, implied in %s / %s\n",
                   static_cast<int>(sym.size()), sym.data(),
                   bb ? format_price(bb->price).c_str() : "-",
                   ba ? format_price(ba->price).c_str() : "-",
                   q.bid ? format_price(q.bid->price).c_str() : "-",
                   q.ask ? format_price(q.ask->price).c_str() : "-");
    }
  }
}

}  // namespace lab::book
