// ouch-client: drives simex's order entry port with random orders and checks
// what comes back.
//
//   ouch-client --connect 127.0.0.1:15000 --accounts FIRM01:lab,FIRM02:lab
//               --reference day.itch50.gz --orders 20000 --rate 5000
//
// Each account logs in over SoupBinTCP and sends OUCH 5.0 Enter, Cancel and
// Replace orders around a reference price per symbol (the first Add Order for
// it in --reference, or --mid). Prices straddle the reference so the accounts
// trade with each other and with the book. Checks, any failure exits 1:
//   - sequenced messages arrive in order with no gap;
//   - every request is answered, and the first answer is the right kind;
//   - no answer names an unknown UserRefNum;
//   - executions and cancels never exceed the order's open shares, and
//     executions are at or inside the limit price;
//   - each match number has at most one adding and one removing side;
//   - with --replay-check, logging account 1 in again from sequence 1 replays
//     the stream byte for byte.
// Round trips (request sent to first answer) go into a latency histogram.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lab/core/clock.hpp"
#include "lab/core/histogram.hpp"
#include "lab/itch/codec.hpp"
#include "lab/itch/file_reader.hpp"
#include "lab/net/tcp.hpp"
#include "lab/ouch/codec.hpp"
#include "lab/ouch/messages.hpp"
#include "lab/soup/soup.hpp"

namespace {

namespace ouch = lab::ouch;
namespace soup = lab::soup;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string connect = "127.0.0.1:15000";
  std::vector<std::pair<std::string, std::string>> accounts{{"FIRM01", "lab"}, {"FIRM02", "lab"}};
  std::vector<std::string> symbols;
  std::string reference;
  std::size_t reference_symbols = 5;
  std::uint64_t mid = 1'000'000;  // $100.00
  std::uint64_t orders = 10'000;
  double rate = 5'000;
  std::uint64_t seed = 1;
  double ioc = 0.1;
  bool replay_check = false;
  bool wait_eos = false;
  double answer_timeout = 10;
};

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: ouch-client [--connect IP:PORT] [--accounts USER:PASS,...]\n"
               "                   [--reference ITCH_FILE [--reference-symbols N] | --symbols "
               "A,B --mid DOLLARS]\n"
               "                   [--orders N] [--rate N] [--ioc FRACTION] [--seed S]\n"
               "                   [--replay-check] [--wait-eos] [--answer-timeout S]\n");
  std::exit(2);
}

std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> out;
  while (!s.empty()) {
    const auto p = s.find(sep);
    out.emplace_back(s.substr(0, p));
    if (p == std::string_view::npos) break;
    s.remove_prefix(p + 1);
  }
  return out;
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (a == "--connect") {
      o.connect = value();
    } else if (a == "--accounts") {
      o.accounts.clear();
      for (const auto& acc : split(value(), ',')) {
        const auto p = acc.find(':');
        if (p == std::string::npos) usage();
        o.accounts.emplace_back(acc.substr(0, p), acc.substr(p + 1));
      }
    } else if (a == "--symbols") {
      o.symbols = split(value(), ',');
    } else if (a == "--reference") {
      o.reference = value();
    } else if (a == "--reference-symbols") {
      o.reference_symbols = std::strtoull(value(), nullptr, 10);
    } else if (a == "--mid") {
      o.mid = static_cast<std::uint64_t>(std::strtod(value(), nullptr) * 10'000 + 0.5);
    } else if (a == "--orders") {
      o.orders = std::strtoull(value(), nullptr, 10);
    } else if (a == "--rate") {
      o.rate = std::strtod(value(), nullptr);
    } else if (a == "--ioc") {
      o.ioc = std::strtod(value(), nullptr);
    } else if (a == "--seed") {
      o.seed = std::strtoull(value(), nullptr, 10);
    } else if (a == "--replay-check") {
      o.replay_check = true;
    } else if (a == "--wait-eos") {
      o.wait_eos = true;
    } else if (a == "--answer-timeout") {
      o.answer_timeout = std::strtod(value(), nullptr);
    } else {
      usage();
    }
  }
  if (o.accounts.empty() || o.rate <= 0) usage();
  return o;
}

struct Instrument {
  ouch::Symbol symbol;
  std::uint64_t reference;  // Price(4)
};

// The first Add Order price of the first `n` symbols that have one.
std::vector<Instrument> reference_prices(const std::string& path, std::size_t n) {
  struct Scan : lab::itch::HandlerBase {
    using HandlerBase::on;
    std::map<std::uint16_t, Instrument> found;
    void add(std::uint16_t locate, const lab::itch::Symbol& s, std::uint32_t price) {
      if (found.count(locate) != 0) return;
      Instrument in;
      std::memcpy(in.symbol.data(), s.data(), in.symbol.size());
      in.reference = price;
      found.emplace(locate, in);
    }
    void on(const lab::itch::AddOrder& m) { add(m.header.stock_locate, m.stock, m.price); }
    void on(const lab::itch::AddOrderMpid& m) { add(m.header.stock_locate, m.stock, m.price); }
  } scan;
  lab::itch::FileReader reader{path};
  while (scan.found.size() < n && reader.messages_read() < 5'000'000) {
    const auto msg = reader.next();
    if (!msg) break;
    lab::itch::dispatch(msg->data(), msg->size(), scan);
  }
  if (!reader.ok()) {
    std::fprintf(stderr, "ouch-client: %s: %s\n", path.c_str(), reader.error().c_str());
    std::exit(1);
  }
  std::vector<Instrument> out;
  for (const auto& [locate, in] : scan.found) out.push_back(in);
  return out;
}

enum class Want : std::uint8_t { kNone, kAccept, kReplace, kCancel };

struct Order {
  std::size_t instrument;
  char side;
  std::uint64_t price;
  std::uint32_t open = 0;  // shares still on the book or about to be
  std::uint32_t executed = 0;
  std::uint32_t cancel_to = 0;  // remaining size a pending cancel asks for
  bool live = false;            // accepted and not finished
  Want want = Want::kNone;
  std::uint64_t sent_tsc = 0;
};

struct Stats {
  std::uint64_t enters = 0, cancels = 0, replaces = 0;
  std::uint64_t accepted = 0, rejected = 0, replaced = 0, canceled = 0;
  std::uint64_t executions = 0, executed_shares = 0;
  std::uint64_t sequenced = 0, heartbeats = 0, replayed = 0;
  std::map<std::uint16_t, std::uint64_t> reject_reasons;
};

class Checker {
 public:
  void fail(std::string what) {
    if (errors_.size() < 20) errors_.push_back(std::move(what));
    ++count_;
  }
  std::uint64_t count() const { return count_; }
  void print() const {
    for (const auto& e : errors_) std::printf("  ERROR  %s\n", e.c_str());
  }
  // Each match has one adding and one removing side.
  void match(std::uint64_t number, char flag) {
    auto& seen = matches_[number];
    const int bit = flag == ouch::liquidity::kAdded ? 1 : 2;
    if ((seen & bit) != 0) fail("match " + std::to_string(number) + " has two sides of one kind");
    seen |= bit;
  }

 private:
  std::vector<std::string> errors_;
  std::uint64_t count_ = 0;
  std::unordered_map<std::uint64_t, int> matches_;
};

class Session {
 public:
  Session(std::string user, std::string password, Checker& check, lab::LatencyHistogram& rtt)
      : user_{std::move(user)}, password_{std::move(password)}, check_{check}, rtt_{rtt} {}

  void connect(const lab::net::Endpoint& to, std::uint64_t from_seq) {
    // The exchange may still be starting: retry a refused connect for a while.
    for (int attempt = 0;; ++attempt) {
      try {
        sock_ = lab::net::TcpStream::connect(to);
        break;
      } catch (const std::system_error& e) {
        if (attempt == 100) {
          std::fprintf(stderr, "ouch-client: %s\n", e.what());
          std::exit(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
      }
    }
    in_ = std::make_unique<soup::StreamParser>();
    std::array<std::byte, soup::kLoginRequestSize> buf;
    soup::write_login_request(buf.data(), user_, password_, "", from_seq);
    sock_.send(buf);
    logged_in_ = false;
    // Generous: an exchange built with sanitizers can take a while to start.
    const auto deadline = Clock::now() + std::chrono::seconds{30};
    while (!logged_in_ && Clock::now() < deadline && !failed_) {
      poll();
      std::this_thread::sleep_for(std::chrono::microseconds{50});
    }
    if (!logged_in_) {
      std::fprintf(stderr, "ouch-client: %s: login failed\n", user_.c_str());
      std::exit(1);
    }
    if (login_seq_ != from_seq) {
      check_.fail(user_ + ": login accepted at sequence " + std::to_string(login_seq_) +
                  ", asked for " + std::to_string(from_seq));
    }
  }

  // Logs in again from sequence 1; the stream must replay what was received.
  void replay(const lab::net::Endpoint& to) {
    replay_pos_ = 0;
    replaying_ = true;
    sock_.close();
    connect(to, 1);
  }

  void poll() {
    sock_.flush();
    for (;;) {
      const auto area = in_->write_area();
      const auto n = sock_.recv(area);
      if (n < 0) {
        if (!eos_ && !logged_out_) failed_ = true;
        break;
      }
      if (n == 0) break;
      in_->commit(static_cast<std::size_t>(n));
      while (auto p = in_->next()) on_packet(*p);
    }
    if (in_->malformed()) {
      check_.fail(user_ + ": malformed SoupBinTCP packet");
      failed_ = true;
    }
    const auto now = Clock::now();
    if (logged_in_ && now - last_tx_ > std::chrono::seconds{1}) {
      std::array<std::byte, soup::kHeaderSize> hb;
      soup::write_empty(hb.data(), soup::kClientHeartbeat);
      sock_.send(hb);
      last_tx_ = now;
    }
  }

  void act(std::mt19937_64& rng, const std::vector<Instrument>& instruments, double ioc) {
    std::uniform_real_distribution<double> u{0, 1};
    std::vector<std::uint32_t> idle;
    if (live_count_ > 8) {
      for (const auto& [ref, o] : orders_) {
        if (o.live && o.want == Want::kNone && o.open > 0) idle.push_back(ref);
        if (idle.size() >= 64) break;
      }
    }
    const double r = u(rng);
    if (idle.empty() || r < 0.55) {
      enter(rng, instruments, ioc);
    } else if (r < 0.8) {
      const std::uint32_t ref = idle[rng() % idle.size()];
      Order& o = orders_[ref];
      ouch::CancelOrder m;
      m.user_ref_num = ref;
      m.quantity = u(rng) < 0.7 ? 0 : o.open / 2;  // full or partial
      o.cancel_to = m.quantity;
      o.want = Want::kCancel;
      o.sent_tsc = lab::rdtsc();
      send(m);
      ++stats_.cancels;
    } else {
      const std::uint32_t ref = idle[rng() % idle.size()];
      Order& o = orders_[ref];
      const std::uint32_t new_ref = next_ref_++;
      ouch::ReplaceOrder m;
      m.orig_user_ref_num = ref;
      m.user_ref_num = new_ref;
      m.price = jitter(rng, instruments[o.instrument].reference, o.side);
      m.quantity = o.executed + 100 * static_cast<std::uint32_t>(1 + rng() % 5);
      m.cl_ord_id = ouch::make_alpha<14>("R" + std::to_string(new_ref));
      o.want = Want::kReplace;
      o.sent_tsc = lab::rdtsc();
      Order n{o.instrument, o.side, m.price};
      n.want = Want::kReplace;
      n.sent_tsc = o.sent_tsc;
      orders_.emplace(new_ref, n);
      replacing_[new_ref] = ref;
      send(m);
      ++stats_.replaces;
    }
  }

  void logout() {
    std::array<std::byte, soup::kHeaderSize> o;
    soup::write_empty(o.data(), soup::kLogoutRequest);
    sock_.send(o);
    logged_out_ = true;
    const auto deadline = Clock::now() + std::chrono::milliseconds{500};
    while (sock_.queued() > 0 && Clock::now() < deadline) sock_.flush();
  }

  // One line per unanswered request, for the report.
  void describe_awaiting(Checker& check) const {
    static constexpr const char* kNames[] = {"none", "enter", "replace", "cancel"};
    for (const auto& [ref, o] : orders_) {
      if (o.want == Want::kNone) continue;
      check.fail(user_ + ": no answer to " + kNames[static_cast<int>(o.want)] + " for UserRefNum " +
                 std::to_string(ref) + " (open " + std::to_string(o.open) + ", live " +
                 std::to_string(o.live) + ")");
    }
  }

  std::size_t awaiting() const {
    std::size_t n = 0;
    for (const auto& [ref, o] : orders_) n += o.want != Want::kNone;
    return n;
  }
  bool failed() const { return failed_; }
  bool eos() const { return eos_; }
  bool replay_done() const { return !replaying_; }
  const Stats& stats() const { return stats_; }
  const std::string& user() const { return user_; }

 private:
  static std::uint64_t jitter(std::mt19937_64& rng, std::uint64_t ref, char side) {
    // Buys from 10 cents under to 4 over, sells mirrored: some cross.
    const auto ticks = static_cast<std::int64_t>(rng() % 15);
    const std::int64_t off = side == ouch::side::kBuy ? ticks - 10 : 10 - ticks;
    const std::int64_t p = static_cast<std::int64_t>(ref) + off * 100;
    return static_cast<std::uint64_t>(std::max<std::int64_t>(p, 100));
  }

  void enter(std::mt19937_64& rng, const std::vector<Instrument>& instruments, double ioc) {
    std::uniform_real_distribution<double> u{0, 1};
    const std::size_t i = rng() % instruments.size();
    const std::uint32_t ref = next_ref_++;
    ouch::EnterOrder m;
    m.user_ref_num = ref;
    m.side = (rng() & 1) != 0 ? ouch::side::kBuy : ouch::side::kSell;
    m.quantity = 100 * static_cast<std::uint32_t>(1 + rng() % 5);
    m.symbol = instruments[i].symbol;
    m.price = jitter(rng, instruments[i].reference, m.side);
    m.time_in_force = u(rng) < ioc ? ouch::tif::kIoc : ouch::tif::kDay;
    m.cl_ord_id = ouch::make_alpha<14>("E" + std::to_string(ref));
    Order o{i, m.side, m.price};
    o.want = Want::kAccept;
    o.sent_tsc = lab::rdtsc();
    orders_.emplace(ref, o);
    send(m);
    ++stats_.enters;
  }

  template <class M>
  void send(const M& m) {
    std::array<std::byte, M::kSize> msg;
    ouch::encode(m, msg.data());
    std::array<std::byte, soup::kHeaderSize + M::kSize> pkt;
    soup::write_data(pkt.data(), soup::kUnsequencedData, msg);
    sock_.send(pkt);
    last_tx_ = Clock::now();
  }

  void on_packet(const soup::Packet& p) {
    switch (p.type) {
      case soup::kLoginAccepted: {
        const auto a = soup::parse_login_accepted(p.payload);
        if (!a) {
          check_.fail(user_ + ": bad Login Accepted");
          failed_ = true;
          return;
        }
        login_seq_ = next_seq_ = a->next_sequence;
        logged_in_ = true;
        return;
      }
      case soup::kLoginRejected:
        failed_ = true;
        return;
      case soup::kServerHeartbeat:
        ++stats_.heartbeats;
        return;
      case soup::kEndOfSession:
        eos_ = true;
        return;
      case soup::kSequencedData:
        on_sequenced(p.payload);
        return;
      case soup::kDebug:
        return;
      default:
        check_.fail(user_ + ": unexpected packet type " + std::string(1, p.type));
    }
  }

  void on_sequenced(std::span<const std::byte> msg) {
    ++next_seq_;
    if (replaying_) {
      if (replay_pos_ < offsets_.size()) {
        const std::size_t begin = offsets_[replay_pos_];
        const std::size_t end =
            replay_pos_ + 1 < offsets_.size() ? offsets_[replay_pos_ + 1] : stream_.size();
        if (end - begin != msg.size() ||
            std::memcmp(stream_.data() + begin, msg.data(), msg.size()) != 0) {
          check_.fail(user_ + ": replayed message " + std::to_string(replay_pos_ + 1) +
                      " differs from the original");
        }
        ++replay_pos_;
        ++stats_.replayed;
        return;
      }
      replaying_ = false;  // caught up: everything from here is new
    }
    offsets_.push_back(stream_.size());
    stream_.insert(stream_.end(), msg.begin(), msg.end());
    ++stats_.sequenced;
    struct H : ouch::HandlerBase {
      using HandlerBase::on;
      Session& s;
      explicit H(Session& ss) : s{ss} {}
      void on(const ouch::OrderAccepted& m) { s.on_accepted(m); }
      void on(const ouch::OrderReplaced& m) { s.on_replaced(m); }
      void on(const ouch::OrderCanceled& m) { s.on_canceled(m); }
      void on(const ouch::OrderExecuted& m) { s.on_executed(m); }
      void on(const ouch::Rejected& m) { s.on_rejected(m); }
      void on(const ouch::SystemEvent&) {}
    } h{*this};
    if (ouch::dispatch_outbound(msg.data(), msg.size(), h) != ouch::DispatchResult::kOk) {
      check_.fail(user_ + ": undecodable OUCH message");
    }
  }

  Order* find(std::uint32_t ref, const char* what) {
    const auto it = orders_.find(ref);
    if (it == orders_.end()) {
      check_.fail(user_ + ": " + what + " for unknown UserRefNum " + std::to_string(ref));
      return nullptr;
    }
    return &it->second;
  }

  void answered(Order& o, Want got, std::uint32_t ref, const char* what) {
    if (o.want == Want::kNone) return;
    if (o.want != got) {
      check_.fail(user_ + ": " + what + " answered a different request for UserRefNum " +
                  std::to_string(ref));
    }
    rtt_.record(lab::rdtsc() - o.sent_tsc);
    o.want = Want::kNone;
  }

  void finish(std::uint32_t ref, Order& o) {
    if (o.live) --live_count_;
    o.live = false;
    if (o.want == Want::kNone) orders_.erase(ref);
  }

  void on_accepted(const ouch::OrderAccepted& m) {
    ++stats_.accepted;
    Order* o = find(m.user_ref_num, "Accepted");
    if (o == nullptr) return;
    if (o->want != Want::kAccept) check_.fail(user_ + ": unexpected Accepted");
    answered(*o, Want::kAccept, m.user_ref_num, "Accepted");
    o->open = m.quantity;
    o->live = true;
    ++live_count_;
  }

  void on_rejected(const ouch::Rejected& m) {
    ++stats_.rejected;
    ++stats_.reject_reasons[m.reason];
    Order* o = find(m.user_ref_num, "Rejected");
    if (o == nullptr) return;
    if (o->want == Want::kReplace) {  // the replacement was refused; the original stands
      const auto r = replacing_.find(m.user_ref_num);
      if (r != replacing_.end()) {
        if (auto* orig = find(r->second, "Rejected replace")) {
          orig->want = Want::kNone;
          if (!orig->live) orders_.erase(r->second);
        }
        replacing_.erase(r);
      }
    }
    answered(*o, o->want, m.user_ref_num, "Rejected");
    finish(m.user_ref_num, *o);
  }

  void on_replaced(const ouch::OrderReplaced& m) {
    ++stats_.replaced;
    const auto r = replacing_.find(m.user_ref_num);
    if (r == replacing_.end() || r->second != m.orig_user_ref_num) {
      check_.fail(user_ + ": Replaced for a replace never sent");
      return;
    }
    replacing_.erase(r);
    Order* orig = find(m.orig_user_ref_num, "Replaced");
    Order* n = find(m.user_ref_num, "Replaced");
    if (orig == nullptr || n == nullptr) return;
    answered(*orig, Want::kReplace, m.orig_user_ref_num, "Replaced");
    n->want = Want::kNone;
    n->open = m.quantity;
    n->executed = orig->executed;
    n->live = true;
    ++live_count_;
    finish(m.orig_user_ref_num, *orig);
  }

  void on_canceled(const ouch::OrderCanceled& m) {
    ++stats_.canceled;
    Order* o = find(m.user_ref_num, "Canceled");
    if (o == nullptr) return;
    if (m.quantity > o->open) check_.fail(user_ + ": canceled more shares than open");
    o->open -= std::min(m.quantity, o->open);
    if (o->want == Want::kCancel) answered(*o, Want::kCancel, m.user_ref_num, "Canceled");
    if (o->want == Want::kReplace && o->open == 0 && m.reason == ouch::cancel_reason::kUser) {
      // A replace for no more than the executed shares cancels the order:
      // this is its answer, and the replacement never exists.
      for (auto it = replacing_.begin(); it != replacing_.end(); ++it) {
        if (it->second != m.user_ref_num) continue;
        orders_.erase(it->first);
        replacing_.erase(it);
        break;
      }
      answered(*o, Want::kReplace, m.user_ref_num, "Canceled");
      finish(m.user_ref_num, *o);
    } else if (o->want == Want::kReplace && o->open == 0) {
      // The order finished (IOC) while a replace was in flight; its reject finishes this.
      if (o->live) --live_count_;
      o->live = false;
    } else if (o->open == 0) {
      finish(m.user_ref_num, *o);
    }
  }

  void on_executed(const ouch::OrderExecuted& m) {
    ++stats_.executions;
    stats_.executed_shares += m.quantity;
    check_.match(m.match_number, m.liquidity_flag);
    Order* o = find(m.user_ref_num, "Executed");
    if (o == nullptr) return;
    if (m.quantity > o->open) check_.fail(user_ + ": executed more shares than open");
    const bool inside = o->side == ouch::side::kBuy ? m.price <= o->price : m.price >= o->price;
    if (!inside) check_.fail(user_ + ": execution outside the limit price");
    o->open -= std::min(m.quantity, o->open);
    o->executed += m.quantity;
    // A cancel asking for more than is now left changes nothing and is not
    // answered: the fill answered it.
    if (o->want == Want::kCancel && o->open <= o->cancel_to) o->want = Want::kNone;
    if (o->open == 0 && o->want == Want::kNone) {
      finish(m.user_ref_num, *o);
    } else if (o->open == 0) {  // a replace is in flight; its reject finishes this
      if (o->live) --live_count_;
      o->live = false;
    }
  }

  std::string user_;
  std::string password_;
  Checker& check_;
  lab::LatencyHistogram& rtt_;
  lab::net::TcpStream sock_;
  std::unique_ptr<soup::StreamParser> in_;
  Clock::time_point last_tx_ = Clock::now();
  std::uint64_t next_seq_ = 0;
  std::uint64_t login_seq_ = 0;
  std::uint32_t next_ref_ = 1;
  std::unordered_map<std::uint32_t, Order> orders_;
  std::unordered_map<std::uint32_t, std::uint32_t> replacing_;  // new ref -> original
  std::size_t live_count_ = 0;
  std::vector<std::byte> stream_;  // every sequenced message, for the replay check
  std::vector<std::size_t> offsets_;
  std::size_t replay_pos_ = 0;
  bool replaying_ = false;
  bool logged_in_ = false;
  bool logged_out_ = false;
  bool eos_ = false;
  bool failed_ = false;
  Stats stats_;
};

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  std::vector<Instrument> instruments;
  if (!opt.reference.empty()) {
    instruments = reference_prices(opt.reference, opt.reference_symbols);
  } else {
    for (const auto& s :
         opt.symbols.empty() ? std::vector<std::string>{"AAPL", "MSFT", "SPY"} : opt.symbols) {
      instruments.push_back({ouch::make_alpha<8>(s), opt.mid});
    }
  }
  if (instruments.empty()) {
    std::fprintf(stderr, "ouch-client: no symbols to trade\n");
    return 1;
  }

  const auto clock = lab::TscClock::calibrate();
  const auto to = lab::net::parse_endpoint(opt.connect);
  Checker check;
  lab::LatencyHistogram rtt;
  std::vector<std::unique_ptr<Session>> sessions;
  for (const auto& [user, pass] : opt.accounts) {
    sessions.push_back(std::make_unique<Session>(user, pass, check, rtt));
    sessions.back()->connect(to, 1);
  }
  std::printf("ouch-client: %zu accounts on %s, %zu symbols, %llu orders at %.0f/s\n",
              sessions.size(), opt.connect.c_str(), instruments.size(),
              static_cast<unsigned long long>(opt.orders), opt.rate);

  std::mt19937_64 rng{opt.seed};
  auto poll_all = [&] {
    bool ok = true;
    for (auto& s : sessions) {
      s->poll();
      ok = ok && !s->failed();
    }
    return ok;
  };

  const auto start = Clock::now();
  const auto interval = std::chrono::duration<double>(1.0 / opt.rate);
  std::uint64_t sent = 0;
  bool ok = true;
  while (sent < opt.orders && ok) {
    const auto due = start + std::chrono::duration_cast<Clock::duration>(interval * sent);
    if (Clock::now() >= due) {
      sessions[sent % sessions.size()]->act(rng, instruments, opt.ioc);
      ++sent;
    }
    ok = poll_all();
  }
  const double send_seconds = std::chrono::duration<double>(Clock::now() - start).count();

  auto pending = [&] {
    std::size_t n = 0;
    for (const auto& s : sessions) n += s->awaiting();
    return n;
  };
  const auto answer_deadline =
      Clock::now() + std::chrono::duration_cast<Clock::duration>(
                         std::chrono::duration<double>(opt.answer_timeout));
  while (ok && pending() > 0 && Clock::now() < answer_deadline) ok = poll_all();
  const std::size_t unanswered = pending();
  if (unanswered > 0) {
    check.fail(std::to_string(unanswered) + " requests never answered");
    for (const auto& s : sessions) s->describe_awaiting(check);
  }

  if (ok && opt.replay_check) {
    sessions.front()->replay(to);
    const auto deadline = Clock::now() + std::chrono::seconds{10};
    while (ok && !sessions.front()->replay_done() && Clock::now() < deadline) ok = poll_all();
    if (!sessions.front()->replay_done()) check.fail("replay did not finish");
  }

  if (opt.wait_eos) {
    while (ok) {
      ok = poll_all();
      if (std::all_of(sessions.begin(), sessions.end(), [](const auto& s) { return s->eos(); })) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
  } else {
    for (auto& s : sessions) s->logout();
  }
  for (const auto& s : sessions) {
    if (s->failed() && !s->eos()) check.fail(s->user() + ": connection lost");
  }

  std::printf("ouch-client sent %llu requests in %.2f s\n", static_cast<unsigned long long>(sent),
              send_seconds);
  for (const auto& s : sessions) {
    const Stats& t = s->stats();
    std::printf(
        "  %-6s enter %llu cancel %llu replace %llu | accepted %llu rejected %llu replaced %llu "
        "canceled %llu executions %llu (%llu shares) | %llu sequenced, %llu replayed\n",
        s->user().c_str(), static_cast<unsigned long long>(t.enters),
        static_cast<unsigned long long>(t.cancels), static_cast<unsigned long long>(t.replaces),
        static_cast<unsigned long long>(t.accepted), static_cast<unsigned long long>(t.rejected),
        static_cast<unsigned long long>(t.replaced), static_cast<unsigned long long>(t.canceled),
        static_cast<unsigned long long>(t.executions),
        static_cast<unsigned long long>(t.executed_shares),
        static_cast<unsigned long long>(t.sequenced), static_cast<unsigned long long>(t.replayed));
  }
  std::map<std::uint16_t, std::uint64_t> reasons;
  for (const auto& s : sessions) {
    for (const auto& [r, n] : s->stats().reject_reasons) reasons[r] += n;
  }
  if (!reasons.empty()) {
    std::printf("  rejects by reason:");
    for (const auto& [r, n] : reasons) {
      std::printf(" 0x%04X=%llu", r, static_cast<unsigned long long>(n));
    }
    std::printf("\n");
  }
  rtt.print(stdout, "Round trip", 1.0 / clock.ticks_per_ns());
  if (check.count() == 0) {
    std::printf("Protocol checks: OK\n");
    return 0;
  }
  std::printf("Protocol checks: %llu errors\n", static_cast<unsigned long long>(check.count()));
  check.print();
  return 1;
}
