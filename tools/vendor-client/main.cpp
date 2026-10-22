// vendor-client: subscribes to a lab market data vendor and checks what it
// says against the venue's own feed.
//
//   vendor-client --vendor icf|reuters|bloomberg [--connect IP:PORT]
//                 --symbols A,B,C [--interval-ms N] [--md GROUP:PORT]
//                 [--iface IP] [--retrans IP:PORT] [--idle-us N] [--duration S]
//                 [--verbose]
//
// Speaks the vendor's protocol from the client side (ICF over SoupBinTCP,
// OMM JSON over WebSocket, B-PIPE frames), keeps a Level 1 view per symbol,
// and at the same time joins the venue's MoldUDP64 feed directly and
// consolidates it the way the vendor does. A subscription refused because
// the vendor has not seen the symbol yet is retried. When the vendor ends
// the session and the direct feed has ended, checks, any failure exits 1:
//   - every symbol was subscribed and got an image;
//   - each view's bid, ask, sizes, last price, volume and status equal the
//     direct feed's;
//   - the vendor closed the session in its protocol's own way.
// Reports exchange-to-client latency through the vendor next to the direct
// feed's, both measured from the ITCH timestamp to receipt here.

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "lab/core/histogram.hpp"
#include "lab/feed/receiver.hpp"
#include "lab/net/tcp.hpp"
#include "lab/soup/soup.hpp"
#include "lab/vendor/bpipe.hpp"
#include "lab/vendor/consolidator.hpp"
#include "lab/vendor/icf.hpp"
#include "lab/vendor/json.hpp"
#include "lab/vendor/omm.hpp"
#include "lab/vendor/server.hpp"
#include "lab/vendor/ws.hpp"

namespace {

namespace vendor = lab::vendor;
namespace soup = lab::soup;
namespace icf = lab::vendor::icf;
namespace omm = lab::vendor::omm;
namespace bpipe = lab::vendor::bpipe;
namespace ws = lab::vendor::ws;
namespace json = lab::vendor::json;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string vendor;
  std::string connect;
  std::vector<std::string> symbols;
  std::uint64_t interval_ms = 0;
  lab::feed::ReceiverConfig feed;
  double duration = 0;
  bool verbose = false;
};

// One symbol as the vendor described it.
struct View {
  bool image = false;
  std::uint32_t bid = 0, ask = 0, last = 0;
  std::uint64_t bid_size = 0, ask_size = 0, volume = 0;
  char status = 'T';
  std::uint64_t updates = 0;
};

// What every protocol client shares.
struct State {
  explicit State(const Options& o) : opt{o} {
    for (const auto& s : o.symbols) views[s];
  }
  const Options& opt;
  std::map<std::string, View> views;
  std::vector<std::string> retry;  // refused for now: the vendor has not listed it yet
  Clock::time_point retry_at{};
  lab::LatencyHistogram latency;  // ITCH timestamp to receipt through the vendor
  std::uint64_t images = 0, updates = 0, refusals = 0, retries = 0;
  bool ended = false;   // the vendor said goodbye properly
  bool failed = false;  // the protocol went wrong
  std::string why;

  View* view(std::string_view sym) {
    const auto it = views.find(std::string{sym});
    return it == views.end() ? nullptr : &it->second;
  }
  void refused(const std::string& sym) {
    ++refusals;
    retry.push_back(sym);
    retry_at = Clock::now() + std::chrono::milliseconds{200};
  }
  void received(std::uint64_t exch_ns) {
    const std::uint64_t now = vendor::ns_since_midnight();
    if (exch_ns != 0 && now > exch_ns) latency.record(now - exch_ns);
  }
  void fail(std::string w) {
    if (!failed) why = std::move(w);
    failed = true;
  }
};

class Client {
 public:
  Client(State& s, lab::net::TcpStream& tcp) : st_{s}, tcp_{tcp} {}
  virtual ~Client() = default;
  virtual void start() = 0;
  virtual void on_bytes(std::span<const std::byte> in) = 0;
  virtual void subscribe(const std::vector<std::string>& symbols) = 0;
  virtual void tick() {}
  // How the protocol shows the venue's trading state, for the comparison.
  virtual char normalize(char status) const { return status; }

 protected:
  void send(std::span<const std::byte> b) {
    if (!tcp_.send(b)) st_.fail("send failed");
  }
  void send(std::string_view s) { send({reinterpret_cast<const std::byte*>(s.data()), s.size()}); }

  State& st_;
  lab::net::TcpStream& tcp_;
};

// ---------------------------------------------------------------- ICF

class IcfClient final : public Client {
 public:
  using Client::Client;

  void start() override {
    std::byte b[soup::kLoginRequestSize];
    send({b, soup::write_login_request(b, "LABCLI", "lab", "", 1)});
  }

  void on_bytes(std::span<const std::byte> in) override {
    if (!parser_.feed(in)) return st_.fail("ICF stream overflow");
    while (auto p = parser_.next()) {
      switch (p->type) {
        case soup::kLoginAccepted:
          subscribe(st_.opt.symbols);
          break;
        case soup::kLoginRejected:
          return st_.fail("ICF login rejected");
        case soup::kSequencedData:
          message(p->payload);
          break;
        case soup::kEndOfSession:
          st_.ended = true;
          break;
        default:
          break;
      }
    }
    if (parser_.malformed()) st_.fail("ICF stream malformed");
  }

  void subscribe(const std::vector<std::string>& symbols) override {
    std::string cmds;
    for (const auto& s : symbols) {
      const std::uint32_t id = next_id_++;
      by_id_[id] = s;
      if (!cmds.empty()) cmds += ';';
      cmds += "SUBSCRIBE,SYMBOL_TICKER:" + s + ",ENUM_SRC_ID:1,REQUEST_ID:" + std::to_string(id);
      if (st_.opt.interval_ms)
        cmds += ",CONFLATION_INTERVAL:" + std::to_string(st_.opt.interval_ms);
    }
    if (cmds.size() >= soup::kMaxPacket - soup::kHeaderSize) return st_.fail("too many symbols");
    send({out_.data(),
          soup::write_data(out_.data(), soup::kUnsequencedData,
                           {reinterpret_cast<const std::byte*>(cmds.data()), cmds.size()})});
  }

  void tick() override {
    const auto now = Clock::now();
    if (now - last_hb_ > std::chrono::seconds{1}) {
      std::byte b[3];
      send({b, soup::write_empty(b, soup::kClientHeartbeat)});
      last_hb_ = now;
    }
  }

 private:
  void message(std::span<const std::byte> m) {
    if (m.empty()) return;
    if (static_cast<char>(m[0]) == icf::kResponse) {
      const auto r = icf::decode_response(m);
      if (!r) return st_.fail("bad ICF response");
      if (st_.opt.verbose) {
        std::fprintf(stderr, "ICF response %u %s %.*s\n", r->request_id, r->ok ? "ok" : "error",
                     static_cast<int>(r->text.size()), r->text.data());
      }
      if (!r->ok) {
        const auto it = by_id_.find(r->request_id);
        if (it == by_id_.end() || !r->text.starts_with("unknown SYMBOL_TICKER")) {
          return st_.fail("ICF refused: " + std::string{r->text});
        }
        st_.refused(it->second);
      }
      return;
    }
    const auto md = icf::decode(m);
    if (!md) return st_.fail("bad ICF market data");
    View* v = st_.view(lab::itch::trimmed(md->symbol));
    if (v == nullptr) return;
    if (md->event == icf::kImage) {
      v->image = true;
      ++st_.images;
    } else {
      ++st_.updates;
      st_.received(md->exch_ns);
    }
    ++v->updates;
    for (std::size_t i = 0; i < md->count; ++i) {
      const icf::Token& t = md->tokens[i];
      switch (t.id) {
        case icf::token::kBidPrice:
          v->bid = static_cast<std::uint32_t>(t.value);
          break;
        case icf::token::kBidSize:
          v->bid_size = static_cast<std::uint64_t>(t.value);
          break;
        case icf::token::kAskPrice:
          v->ask = static_cast<std::uint32_t>(t.value);
          break;
        case icf::token::kAskSize:
          v->ask_size = static_cast<std::uint64_t>(t.value);
          break;
        case icf::token::kTradePrice:
          v->last = static_cast<std::uint32_t>(t.value);
          break;
        case icf::token::kVolume:
          v->volume = static_cast<std::uint64_t>(t.value);
          break;
        case icf::token::kTradingStatus:
          v->status = static_cast<char>(t.value);
          break;
        default:
          break;
      }
    }
  }

  soup::StreamParser parser_;
  std::array<std::byte, soup::kMaxPacket> out_{};
  std::uint32_t next_id_ = 1;
  std::map<std::uint32_t, std::string> by_id_;
  Clock::time_point last_hb_ = Clock::now();
};

// ---------------------------------------------------------------- OMM JSON

class OmmClient final : public Client {
 public:
  OmmClient(State& s, lab::net::TcpStream& tcp, std::string host)
      : Client{s, tcp}, host_{std::move(host)} {}

  void start() override {
    hs_ = ws::client_handshake(host_, omm::kPath, omm::kSubprotocol);
    send(hs_.request);
  }

  void on_bytes(std::span<const std::byte> in) override {
    if (!upgraded_) {
      head_.append(reinterpret_cast<const char*>(in.data()), in.size());
      const auto end = head_.find("\r\n\r\n");
      if (end == std::string::npos) return;
      const std::string_view head{head_.data(), end + 4};
      if (!head.starts_with("HTTP/1.1 101") ||
          ws::header(head, "Sec-WebSocket-Accept") != hs_.expected_accept ||
          ws::header(head, "Sec-WebSocket-Protocol") != omm::kSubprotocol) {
        return st_.fail("WebSocket upgrade refused");
      }
      upgraded_ = true;
      decoder_.feed(std::string_view{head_}.substr(end + 4));
      head_.clear();
      text(R"({"ID":1,"Domain":"Login","Key":{"Name":"lab","Elements":)"
           R"({"ApplicationId":"256","Position":"127.0.0.1"}}})");
    } else {
      decoder_.feed(in);
    }
    while (auto m = decoder_.next()) {
      if (m->opcode == ws::kText) {
        frame_text(m->payload);
      } else if (m->opcode == ws::kPing) {
        frame(ws::kPong, m->payload);
      } else if (m->opcode == ws::kClose) {
        st_.ended = true;
        frame(ws::kClose, m->payload.substr(0, 2));
      }
    }
    if (decoder_.malformed()) st_.fail("WebSocket stream malformed");
  }

  void subscribe(const std::vector<std::string>& symbols) override {
    // A batch request: item streams get IDs request+1, request+2, ...
    const std::int64_t id = next_id_;
    std::string req = "{\"ID\":" + std::to_string(id) + ",\"Key\":{\"Name\":[";
    for (std::size_t i = 0; i < symbols.size(); ++i) {
      if (i) req += ',';
      json::quote(req, omm::ric(symbols[i]));
      by_id_[id + 1 + static_cast<std::int64_t>(i)] = symbols[i];
    }
    req += "]}}";
    next_id_ += static_cast<std::int64_t>(symbols.size()) + 1;
    text(req);
  }

  char normalize(char status) const override {
    switch (omm::trd_status_of(status)) {
      case omm::trd_status::kNormal:
        return 'T';
      case omm::trd_status::kSuspended:
        return 'P';
      default:
        return 'H';
    }
  }

 private:
  void frame(ws::Opcode op, std::string_view payload) {
    std::string f;
    ws::append_frame(f, op, payload, true);  // clients mask (RFC 6455 5.3)
    send(f);
  }
  void text(std::string_view payload) {
    if (st_.opt.verbose) {
      std::fprintf(stderr, "-> %.*s\n", static_cast<int>(payload.size()), payload.data());
    }
    frame(ws::kText, payload);
  }

  void frame_text(const std::string& payload) {
    const auto v = json::parse(payload);
    if (!v || !v->is_array()) return st_.fail("OMM message is not a JSON array");
    for (const auto& m : v->items) message(m);
  }

  void message(const json::Value& m) {
    const std::string_view type = m.str("Type");
    if (type == "Ping") return text(R"({"Type":"Pong"})");
    if (type == "Error") return st_.fail("OMM error: " + std::string{m.str("Text")});
    const json::Value* idv = m.get("ID");
    if (idv == nullptr || !idv->is_number()) return;
    const std::int64_t id = std::stoll(idv->text);
    if (id == 1) {  // login stream
      if (type == "Refresh" && !logged_in_) {
        logged_in_ = true;
        subscribe(st_.opt.symbols);
      } else if (type == "Status" && !st_.ended) {
        st_.fail("login closed");
      }
      return;
    }
    const auto it = by_id_.find(id);
    if (it == by_id_.end()) return;  // the batch request's own Status
    View* view = st_.view(it->second);
    if (type == "Status") {
      const json::Value* state = m.get("State");
      const std::string_view code = state ? state->str("Code") : "";
      if (code == "NotFound") {
        if (view != nullptr) st_.refused(it->second);
        by_id_.erase(it);
      }
      return;
    }
    if (view == nullptr || (type != "Refresh" && type != "Update")) return;
    const json::Value* fields = m.get("Fields");
    if (fields == nullptr) return;
    ++view->updates;
    if (type == "Refresh") {
      view->image = true;
      ++st_.images;
    } else {
      ++st_.updates;
    }
    std::uint64_t exch_ns = 0;
    for (const auto& [name, val] : fields->members) {
      auto px = [&]() -> std::uint32_t {
        if (val.is_null()) return 0;
        return static_cast<std::uint32_t>(json::parse_price(val.text).value_or(0));
      };
      auto num = [&]() -> std::uint64_t { return val.is_number() ? std::stoull(val.text) : 0; };
      if (name == "BID") {
        view->bid = px();
      } else if (name == "ASK") {
        view->ask = px();
      } else if (name == "BIDSIZE") {
        view->bid_size = num();
      } else if (name == "ASKSIZE") {
        view->ask_size = num();
      } else if (name == "TRDPRC_1") {
        view->last = px();
      } else if (name == "ACVOL_1") {
        view->volume = num();
      } else if (name == "TRD_STATUS") {
        const auto s = static_cast<int>(num());
        view->status = s == omm::trd_status::kNormal      ? 'T'
                       : s == omm::trd_status::kSuspended ? 'P'
                                                          : 'H';
      } else if ((name == "QUOTIM_NS" || name == "SALTIM_NS") && val.is_string()) {
        exch_ns = std::max(exch_ns, json::parse_time_ns(val.text).value_or(0));
      }
    }
    if (type == "Update") st_.received(exch_ns);
  }

  std::string host_;
  ws::ClientHandshake hs_;
  bool upgraded_ = false;
  bool logged_in_ = false;
  std::string head_;
  ws::Decoder decoder_;
  std::int64_t next_id_ = 2;
  std::map<std::int64_t, std::string> by_id_;
};

// ---------------------------------------------------------------- B-PIPE

class BpipeClient final : public Client {
 public:
  using Client::Client;

  void start() override {
    std::string out;
    bpipe::Writer w{out};
    w.begin(bpipe::request::kSessionStart);
    w.str("vendor-client");
    w.end();
    send(out);
  }

  void on_bytes(std::span<const std::byte> in) override {
    parser_.feed(in);
    while (auto body = parser_.next()) {
      const auto e = bpipe::decode_event(*body);
      if (!e) return st_.fail("bad B-PIPE event");
      event(*e);
    }
    if (parser_.malformed()) st_.fail("B-PIPE stream malformed");
  }

  void subscribe(const std::vector<std::string>& symbols) override {
    std::string out;
    bpipe::Writer w{out};
    for (const auto& s : symbols) {
      const std::uint64_t cid = next_cid_++;
      by_cid_[cid] = s;
      std::string topic = s +
                          " US Equity?fields=BID,BID_SIZE,ASK,ASK_SIZE,LAST_PRICE,VOLUME,"
                          "TRADING_STATUS";
      if (st_.opt.interval_ms) {
        char iv[32];
        std::snprintf(iv, sizeof iv, "&interval=%.3f", st_.opt.interval_ms / 1000.0);
        topic += iv;
      }
      w.begin(bpipe::request::kSubscribe);
      w.u64(cid);
      w.str(topic);
      w.end();
    }
    send(out);
  }

 private:
  void event(const bpipe::Event& e) {
    if (st_.opt.verbose && e.type != bpipe::EventType::kSubscriptionData) {
      std::fprintf(stderr, "%s %s cid %llu\n", bpipe::event_name(e.type), e.message_type.c_str(),
                   static_cast<unsigned long long>(e.cid));
    }
    std::string out;
    bpipe::Writer w{out};
    if (e.message_type == "SessionStarted") {
      w.begin(bpipe::request::kAuthorize);
      w.u64(1);
      w.str("lab");
      w.str("127.0.0.1");
      w.end();
      w.begin(bpipe::request::kOpenService);
      w.u64(2);
      w.str(bpipe::kMktdata);
      w.end();
      send(out);
    } else if (e.message_type == "AuthorizationSuccess") {
      authorized_ = true;
    } else if (e.message_type == "ServiceOpened") {
      opened_ = true;
    } else if (e.message_type == "AuthorizationFailure" || e.message_type == "ServiceOpenFailure") {
      st_.fail("B-PIPE " + e.message_type);
    } else if (e.message_type == "SubscriptionFailure") {
      const auto it = by_cid_.find(e.cid);
      const bpipe::Element* cat = e.get("reason.category");
      if (it == by_cid_.end() || cat == nullptr || cat->text != "BAD_SEC") {
        st_.fail("B-PIPE subscription failed");
      } else {
        st_.refused(it->second);
        by_cid_.erase(it);
      }
    } else if (e.message_type == "SessionTerminated") {
      st_.ended = true;
    } else if (e.message_type == "MarketDataEvents") {
      data(e);
    }
    if (authorized_ && opened_ && !subscribed_) {
      subscribed_ = true;
      subscribe(st_.opt.symbols);
    }
  }

  void data(const bpipe::Event& e) {
    const auto it = by_cid_.find(e.cid);
    if (it == by_cid_.end()) return;
    View* v = st_.view(it->second);
    if (v == nullptr) return;
    ++v->updates;
    const bpipe::Element* sub = e.get("MKTDATA_EVENT_SUBTYPE");
    const bool initial = sub != nullptr && sub->text == "INITPAINT";
    if (initial) {
      v->image = true;
      ++st_.images;
    } else {
      ++st_.updates;
      if (const auto* t = e.get("EVENT_TIME")) st_.received(static_cast<std::uint64_t>(t->value));
    }
    for (const auto& el : e.elements) {
      if (el.name == "BID") {
        v->bid = static_cast<std::uint32_t>(el.price4());
      } else if (el.name == "ASK") {
        v->ask = static_cast<std::uint32_t>(el.price4());
      } else if (el.name == "BID_SIZE") {
        v->bid_size = static_cast<std::uint64_t>(el.value);
      } else if (el.name == "ASK_SIZE") {
        v->ask_size = static_cast<std::uint64_t>(el.value);
      } else if (el.name == "LAST_PRICE") {
        v->last = static_cast<std::uint32_t>(el.price4());
      } else if (el.name == "VOLUME") {
        v->volume = static_cast<std::uint64_t>(el.value);
      } else if (el.name == "TRADING_STATUS") {
        v->status = el.text == "TRADING"          ? 'T'
                    : el.text == "HALTED"         ? 'H'
                    : el.text == "PAUSED"         ? 'P'
                    : el.text == "QUOTATION_ONLY" ? 'Q'
                                                  : '?';
      }
    }
  }

  bpipe::FrameParser parser_;
  bool authorized_ = false, opened_ = false, subscribed_ = false;
  std::uint64_t next_cid_ = 100;
  std::map<std::uint64_t, std::string> by_cid_;
};

// ---------------------------------------------------------------- main

[[noreturn]] void usage() {
  std::fprintf(
      stderr,
      "usage: vendor-client --vendor icf|reuters|bloomberg [--connect IP:PORT]\n"
      "                     --symbols A,B,C [--interval-ms N] [--md GROUP:PORT]\n"
      "                     [--iface IP] [--retrans IP:PORT] [--idle-us N] [--duration S]\n"
      "                     [--verbose]\n");
  std::exit(2);
}

std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> out;
  while (!s.empty()) {
    const auto c = s.find(sep);
    if (c != 0) out.emplace_back(s.substr(0, c));
    if (c == std::string_view::npos) break;
    s.remove_prefix(c + 1);
  }
  return out;
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--verbose") {
      o.verbose = true;
      continue;
    }
    if (i + 1 >= argc) usage();
    const std::string v = argv[++i];
    if (a == "--vendor") {
      o.vendor = v;
    } else if (a == "--connect") {
      o.connect = v;
    } else if (a == "--symbols") {
      o.symbols = split(v, ',');
    } else if (a == "--interval-ms") {
      o.interval_ms = std::stoull(v);
    } else if (a == "--md") {
      o.feed.group = v;
    } else if (a == "--iface") {
      o.feed.iface = v;
    } else if (a == "--retrans") {
      o.feed.retrans = v;
    } else if (a == "--idle-us") {
      o.feed.idle_sleep_us = std::stoull(v);
    } else if (a == "--duration") {
      o.duration = std::stod(v);
    } else {
      usage();
    }
  }
  if (o.symbols.empty()) usage();
  if (o.connect.empty()) {
    if (o.vendor == "icf") o.connect = "127.0.0.1:17001";
    if (o.vendor == "reuters") o.connect = "127.0.0.1:17002";
    if (o.vendor == "bloomberg") o.connect = "127.0.0.1:17003";
  }
  if (o.connect.empty()) usage();
  return o;
}

void print_latency(const char* name, const lab::LatencyHistogram& h) {
  if (h.count() == 0) {
    std::printf("  %-22s no samples\n", name);
    return;
  }
  std::printf(
      "  %-22s n=%-8llu p50 %8.1f us  p99 %8.1f us  p99.9 %8.1f us\n", name,
      static_cast<unsigned long long>(h.count()), static_cast<double>(h.percentile(50)) / 1e3,
      static_cast<double>(h.percentile(99)) / 1e3, static_cast<double>(h.percentile(99.9)) / 1e3);
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  State st{opt};
  try {
    // The direct feed, consolidated the way the vendor does it.
    lab::feed::FeedReceiver rx{opt.feed};
    vendor::Consolidator direct;
    lab::LatencyHistogram direct_latency;
    std::vector<bool> watched(65536, false);
    rx.start();

    lab::net::TcpStream tcp = lab::net::TcpStream::connect(lab::net::parse_endpoint(opt.connect));
    std::unique_ptr<Client> client;
    if (opt.vendor == "icf") {
      client = std::make_unique<IcfClient>(st, tcp);
    } else if (opt.vendor == "reuters") {
      client = std::make_unique<OmmClient>(st, tcp, opt.connect);
    } else if (opt.vendor == "bloomberg") {
      client = std::make_unique<BpipeClient>(st, tcp);
    } else {
      usage();
    }
    std::fprintf(stderr, "vendor-client: %s at %s, %zu symbols, direct feed %s\n",
                 opt.vendor.c_str(), opt.connect.c_str(), opt.symbols.size(),
                 opt.feed.group.c_str());
    client->start();

    const auto started = Clock::now();
    const auto deadline = opt.duration > 0
                              ? started + std::chrono::duration_cast<Clock::duration>(
                                              std::chrono::duration<double>{opt.duration})
                              : Clock::time_point::max();
    std::byte buf[65536];
    bool closed = false;
    while (Clock::now() < deadline && !st.failed) {
      bool busy = false;
      for (int i = 0;
           i < 64 && rx.poll([&](std::uint64_t, std::span<const std::byte> msg, std::uint64_t) {
             direct.apply(msg, [&](const vendor::Record& r, std::uint32_t) {
               if (!watched[r.locate]) {
                 watched[r.locate] = st.view(lab::itch::trimmed(r.symbol)) != nullptr;
                 if (!watched[r.locate]) return;
               }
               const std::uint64_t now = vendor::ns_since_midnight();
               if (now > r.exch_ns) direct_latency.record(now - r.exch_ns);
             });
           });
           ++i) {
        busy = true;
      }
      if (!closed) {
        const ssize_t n = tcp.recv(buf);
        if (n > 0) {
          client->on_bytes({buf, static_cast<std::size_t>(n)});
          busy = true;
        } else if (n < 0) {
          closed = true;
        }
        client->tick();
        tcp.flush();
      }
      if (!st.retry.empty() && Clock::now() >= st.retry_at && !st.ended) {
        std::vector<std::string> again;
        again.swap(st.retry);
        st.retries += again.size();
        client->subscribe(again);
      }
      if ((st.ended || closed) && rx.finished()) break;
      if (!busy) {
        std::this_thread::sleep_for(
            std::chrono::microseconds{std::max<std::uint64_t>(opt.feed.idle_sleep_us, 1)});
      }
    }
    // Let the last of the direct feed drain.
    while (rx.poll([&](std::uint64_t, std::span<const std::byte> msg, std::uint64_t) {
      direct.apply(msg, [](const vendor::Record&, std::uint32_t) {});
    })) {
    }
    rx.stop();

    const double secs = std::chrono::duration<double>(Clock::now() - started).count();
    std::printf("vendor-client: %s, %.1f s: %llu images, %llu updates, %llu refusals retried\n",
                opt.vendor.c_str(), secs, static_cast<unsigned long long>(st.images),
                static_cast<unsigned long long>(st.updates),
                static_cast<unsigned long long>(st.retries));
    std::printf("%-8s %12s %8s %12s %8s %12s %12s %3s\n", "symbol", "bid", "size", "ask", "size",
                "last", "volume", "st");
    int mismatches = 0;
    for (const auto& [sym, v] : st.views) {
      const std::uint16_t l = direct.find(sym);
      const vendor::Record& d = direct.record(l);
      const char want = client->normalize(d.status);
      std::printf("%-8s %12.4f %8llu %12.4f %8llu %12.4f %12llu %3c  (%llu messages)\n",
                  sym.c_str(), v.bid / 1e4, static_cast<unsigned long long>(v.bid_size),
                  v.ask / 1e4, static_cast<unsigned long long>(v.ask_size), v.last / 1e4,
                  static_cast<unsigned long long>(v.volume), v.status,
                  static_cast<unsigned long long>(v.updates));
      if (l == 0) {
        std::printf("  FAIL %s: not on the direct feed\n", sym.c_str());
        ++mismatches;
        continue;
      }
      if (!v.image) {
        std::printf("  FAIL %s: no image from the vendor\n", sym.c_str());
        ++mismatches;
        continue;
      }
      if (v.bid != d.bid || v.bid_size != d.bid_size || v.ask != d.ask ||
          v.ask_size != d.ask_size || v.last != d.last || v.volume != d.volume ||
          v.status != want) {
        std::printf(
            "  FAIL %s: direct feed says %.4f x %llu / %.4f x %llu last %.4f volume %llu %c\n",
            sym.c_str(), d.bid / 1e4, static_cast<unsigned long long>(d.bid_size), d.ask / 1e4,
            static_cast<unsigned long long>(d.ask_size), d.last / 1e4,
            static_cast<unsigned long long>(d.volume), want);
        ++mismatches;
      }
    }
    std::printf("Latency, ITCH timestamp to receipt here:\n");
    print_latency("through the vendor", st.latency);
    print_latency("direct feed", direct_latency);
    const auto integrity = direct.books().checks();
    bool ok = mismatches == 0 && !st.failed && integrity.error_count() == 0;
    if (st.failed) std::printf("FAIL: %s\n", st.why.c_str());
    if (!st.ended) {
      std::printf("FAIL: the vendor did not end the session\n");
      ok = false;
    }
    if (st.updates == 0) {
      std::printf("FAIL: no updates through the vendor\n");
      ok = false;
    }
    std::printf("vendor-client: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "vendor-client: %s\n", e.what());
    return 1;
  }
}
