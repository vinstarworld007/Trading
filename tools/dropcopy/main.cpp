// dropcopy: the firm's back-office view of its executions, straight from the
// exchange. Logs in to simex's drop copy session, records every Execution
// Report until End of Session, and prints each account's positions.
//
//   dropcopy --connect 127.0.0.1:15000 --account DROP01:lab --csv executions.csv
//
// The CSV is what research/report.py reconciles the trader's fills against.
// Exits 0 when the session ended normally and every message decoded.

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "lab/net/tcp.hpp"
#include "lab/ouch/codec.hpp"
#include "lab/ouch/drop_copy.hpp"
#include "lab/soup/soup.hpp"

namespace {

namespace soup = lab::soup;
using lab::dropcopy::ExecutionReport;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string connect = "127.0.0.1:15000";
  std::string user = "DROP01";
  std::string password = "lab";
  std::string csv;
  double idle_seconds = 30;  // give up when nothing arrives for this long
};

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: dropcopy [--connect IP:PORT] [--account USER:PASS] [--csv FILE] "
               "[--idle-seconds S]\n");
  std::exit(2);
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) usage();
    const std::string v = argv[++i];
    if (a == "--connect") {
      o.connect = v;
    } else if (a == "--account") {
      const auto p = v.find(':');
      if (p == std::string::npos) usage();
      o.user = v.substr(0, p);
      o.password = v.substr(p + 1);
    } else if (a == "--csv") {
      o.csv = v;
    } else if (a == "--idle-seconds") {
      o.idle_seconds = std::strtod(v.c_str(), nullptr);
    } else {
      usage();
    }
  }
  return o;
}

struct Book {                                  // one account's activity
  std::map<std::string, std::int64_t> shares;  // by symbol
  std::map<std::string, std::int64_t> cash;    // Price(4) x shares
  std::uint64_t executions = 0;
  std::uint64_t volume = 0;
};

std::string trim(const char* p, std::size_t n) {
  while (n > 0 && p[n - 1] == ' ') --n;
  return {p, n};
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  lab::net::TcpStream sock;
  for (int attempt = 0;; ++attempt) {
    try {
      sock = lab::net::TcpStream::connect(lab::net::parse_endpoint(opt.connect));
      break;
    } catch (const std::system_error& e) {
      if (attempt == 300) {
        std::fprintf(stderr, "dropcopy: %s\n", e.what());
        return 2;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
  }
  std::array<std::byte, soup::kLoginRequestSize> login;
  soup::write_login_request(login.data(), opt.user, opt.password, "", 1);  // the whole day
  sock.send(login);

  std::vector<ExecutionReport> reports;
  reports.reserve(1u << 16);
  auto parser = std::make_unique<soup::StreamParser>();
  bool logged_in = false, eos = false, failed = false;
  std::uint64_t undecodable = 0, sequenced = 0;
  auto last_rx = Clock::now();
  auto last_tx = Clock::now();
  while (!eos && !failed) {
    sock.flush();
    const auto area = parser->write_area();
    const auto n = sock.recv(area);
    const auto now = Clock::now();
    if (n < 0) {
      failed = true;
      break;
    }
    if (n == 0) {
      if (std::chrono::duration<double>(now - last_rx).count() > opt.idle_seconds) break;
      if (logged_in && now - last_tx > std::chrono::seconds{1}) {
        std::array<std::byte, soup::kHeaderSize> hb;
        soup::write_empty(hb.data(), soup::kClientHeartbeat);
        sock.send(hb);
        last_tx = now;
      }
      std::this_thread::sleep_for(std::chrono::microseconds{200});
      continue;
    }
    last_rx = now;
    parser->commit(static_cast<std::size_t>(n));
    while (auto p = parser->next()) {
      switch (p->type) {
        case soup::kLoginAccepted:
          logged_in = true;
          break;
        case soup::kLoginRejected:
          std::fprintf(stderr, "dropcopy: login rejected\n");
          return 2;
        case soup::kEndOfSession:
          eos = true;
          break;
        case soup::kSequencedData: {
          ++sequenced;
          const auto& m = p->payload;
          if (m.empty()) break;
          if (static_cast<char>(m[0]) == ExecutionReport::kType) {
            if (m.size() < ExecutionReport::kSize) {
              ++undecodable;
              break;
            }
            ExecutionReport r;
            lab::ouch::decode(m.data(), r);
            reports.push_back(r);
          } else if (static_cast<char>(m[0]) != lab::ouch::SystemEvent::kType) {
            ++undecodable;
          }
          break;
        }
        default:
          break;
      }
    }
  }

  std::map<std::string, Book> books;
  for (const auto& r : reports) {
    Book& b = books[trim(r.account.data(), r.account.size())];
    const std::string sym = trim(r.symbol.data(), r.symbol.size());
    const auto q = static_cast<std::int64_t>(r.quantity);
    const auto px = static_cast<std::int64_t>(r.price);
    b.shares[sym] += r.side == 'B' ? q : -q;
    b.cash[sym] += r.side == 'B' ? -q * px : q * px;
    ++b.executions;
    b.volume += r.quantity;
  }
  std::printf("dropcopy %s: %zu Execution Reports in %llu sequenced messages, %s\n",
              opt.user.c_str(), reports.size(), static_cast<unsigned long long>(sequenced),
              eos      ? "End of Session"
              : failed ? "connection lost"
                       : "idle timeout");
  for (const auto& [account, b] : books) {
    std::printf("  %-6s %llu executions, %llu shares; positions:", account.c_str(),
                static_cast<unsigned long long>(b.executions),
                static_cast<unsigned long long>(b.volume));
    for (const auto& [sym, sh] : b.shares) {
      if (sh != 0) std::printf(" %s %+lld", sym.c_str(), static_cast<long long>(sh));
    }
    std::printf("\n");
  }

  if (!opt.csv.empty()) {
    std::FILE* f = std::fopen(opt.csv.c_str(), "w");
    if (f == nullptr) {
      std::fprintf(stderr, "dropcopy: cannot write %s\n", opt.csv.c_str());
      return 1;
    }
    std::fprintf(f,
                 "exchange_ns,account,user_ref,side,qty,symbol,price,match,liquidity,order_ref\n");
    for (const auto& r : reports) {
      std::fprintf(f, "%llu,%s,%u,%c,%u,%s,%llu,%llu,%c,%llu\n",
                   static_cast<unsigned long long>(r.timestamp),
                   trim(r.account.data(), r.account.size()).c_str(), r.user_ref_num, r.side,
                   r.quantity, trim(r.symbol.data(), r.symbol.size()).c_str(),
                   static_cast<unsigned long long>(r.price),
                   static_cast<unsigned long long>(r.match_number), r.liquidity_flag,
                   static_cast<unsigned long long>(r.order_reference_number));
    }
    std::fclose(f);
    std::printf("Wrote %zu executions to %s\n", reports.size(), opt.csv.c_str());
  }
  if (undecodable != 0)
    std::printf("ERROR: %llu undecodable messages\n", static_cast<unsigned long long>(undecodable));
  return eos && undecodable == 0 ? 0 : 1;
}
