// The ICF vendor's client session: SoupBinTCP login, text commands in
// Unsequenced Data, responses and token messages in Sequenced Data.

#include <regex>
#include <string>
#include <unordered_map>

#include "lab/soup/soup.hpp"
#include "lab/vendor/icf.hpp"
#include "lab/vendor/sessions.hpp"

namespace lab::vendor {

namespace {

class IcfSession final : public Session {
 public:
  explicit IcfSession(const IcfConfig& cfg) : cfg_{cfg} {}

  void on_bytes(std::span<const std::byte> in, Link& link) override {
    if (!parser_.feed(in)) {
      link.close();
      return;
    }
    last_rx_ = link.now_ns();
    while (auto p = parser_.next()) {
      switch (p->type) {
        case soup::kLoginRequest:
          login(p->payload, link);
          break;
        case soup::kUnsequencedData:
          if (logged_in_) {
            try {
              commands({reinterpret_cast<const char*>(p->payload.data()), p->payload.size()}, link);
            } catch (const std::exception&) {  // a malformed number in a parameter
              respond(0, false, "malformed command", link);
            }
          }
          break;
        case soup::kLogoutRequest:
          link.close();
          break;
        default:  // client heartbeats and debug packets
          break;
      }
    }
    if (parser_.malformed()) link.close();
  }

  void on_update(std::uint32_t, const Subscription& s, const Record& r, std::uint32_t mask,
                 Link& link) override {
    char event = icf::kStatus;
    switch (kind_of(mask)) {
      case Kind::kTrade:
        event = icf::kTrade;
        break;
      case Kind::kQuote:
        event = icf::kQuote;
        break;
      case Kind::kStatus:
        break;
    }
    send_md(event, static_cast<std::uint32_t>(s.handle), r, mask, link);
    link.published(r);
  }

  void on_timer(std::uint64_t now, Link& link) override {
    if (logged_in_ && now - last_tx_ > 1'000'000'000ULL) {
      std::byte b[3];
      link.send({b, soup::write_empty(b, soup::kServerHeartbeat)});
      last_tx_ = now;
    }
    if (last_rx_ == 0) last_rx_ = now;
    if (now - last_rx_ > 15'000'000'000ULL) link.close();  // silent client
  }

  void on_end(Link& link) override {
    std::byte b[3];
    link.send({b, soup::write_empty(b, soup::kEndOfSession)});
    link.close();
  }

 private:
  void login(std::span<const std::byte> payload, Link& link) {
    std::byte b[64];
    const auto req = soup::parse_login_request(payload);
    if (!req || req->user.empty()) {
      link.send({b, soup::write_login_rejected(b, soup::kRejectNotAuthorized)});
      link.close();
      return;
    }
    logged_in_ = true;
    link.send({b, soup::write_login_accepted(b, cfg_.session, 1)});
    last_tx_ = link.now_ns();
  }

  void sequenced(std::span<const std::byte> msg, Link& link) {
    std::byte b[3 + icf::kMaxMessage + 520];
    link.send({b, soup::write_data(b, soup::kSequencedData, msg)});
    last_tx_ = link.now_ns();
  }

  void respond(std::uint32_t id, bool ok, std::string_view text, Link& link) {
    std::byte b[600];
    sequenced({b, icf::encode_response(b, id, ok, text)}, link);
  }

  void send_md(char event, std::uint32_t request_id, const Record& r, std::uint32_t mask,
               Link& link) {
    icf::MarketData m;
    m.event = event;
    m.source = cfg_.source_id;
    m.request_id = request_id;
    m.symbol = r.symbol;
    m.exch_ns = r.exch_ns;
    m.vendor_ns = link.now_ns();
    m.record_seq = static_cast<std::uint32_t>(r.updates);
    icf::fill(m, r, mask);
    std::byte b[icf::kMaxMessage];
    sequenced({b, icf::encode(m, b)}, link);
  }

  void commands(std::string_view text, Link& link) {
    for (const icf::Command& c : icf::parse_commands(text)) {
      const std::string_view rid = c.get("REQUEST_ID");
      const std::uint32_t id =
          rid.empty() ? next_request_++ : static_cast<std::uint32_t>(std::stoul(std::string{rid}));
      const std::string_view src = c.get("ENUM_SRC_ID");
      if (!src.empty() && std::stoul(std::string{src}) != cfg_.source_id) {
        respond(id, false, "unknown ENUM_SRC_ID", link);
        continue;
      }
      const std::string_view conf = c.get("CONFLATION_INTERVAL");  // milliseconds
      const std::uint64_t interval =
          conf.empty() ? 0 : std::stoull(std::string{conf}) * 1'000'000ULL;
      const std::string_view sym = c.get("SYMBOL_TICKER");
      if (c.verb == "SUBSCRIBE" || c.verb == "QUERYSNAP" || c.verb == "QUERYSNAPANDSUBSCRIBE") {
        const std::uint16_t locate = link.data().find(sym);
        if (locate == 0) {
          respond(id, false, "unknown SYMBOL_TICKER " + std::string{sym}, link);
          continue;
        }
        respond(id, true, c.verb, link);
        send_md(icf::kImage, id, link.data().record(locate), field::kAll, link);
        if (c.verb != "QUERYSNAP") subscribe(std::string{sym}, locate, id, interval, link);
      } else if (c.verb == "SUBSCRIBEWILDCARD") {
        std::regex re;
        try {
          re = std::regex{std::string{sym}};
        } catch (const std::regex_error&) {
          respond(id, false, "bad SYMBOL_TICKER pattern", link);
          continue;
        }
        std::vector<std::uint16_t> hits;
        link.data().for_each([&](const Record& r) {
          if (std::regex_search(std::string{itch::trimmed(r.symbol)}, re)) hits.push_back(r.locate);
        });
        respond(id, true, "SUBSCRIBEWILDCARD " + std::to_string(hits.size()) + " symbols", link);
        for (const std::uint16_t l : hits) {
          const Record& r = link.data().record(l);
          send_md(icf::kImage, id, r, field::kAll, link);
          subscribe(std::string{itch::trimmed(r.symbol)}, l, id, interval, link);
        }
      } else if (c.verb == "UNSUBSCRIBE") {
        const auto it = subs_.find(std::string{sym});
        if (it == subs_.end()) {
          respond(id, false, "not subscribed", link);
          continue;
        }
        link.unsubscribe(it->second);
        subs_.erase(it);
        respond(id, true, "UNSUBSCRIBE", link);
      } else {
        respond(id, false, "unknown command " + c.verb, link);
      }
    }
  }

  void subscribe(const std::string& sym, std::uint16_t locate, std::uint32_t id,
                 std::uint64_t interval, Link& link) {
    if (const auto it = subs_.find(sym); it != subs_.end()) link.unsubscribe(it->second);
    subs_[sym] = link.subscribe(locate, id, interval);
  }

  IcfConfig cfg_;
  soup::StreamParser parser_;
  bool logged_in_ = false;
  std::uint32_t next_request_ = 1;
  std::uint64_t last_tx_ = 0;
  std::uint64_t last_rx_ = 0;
  std::unordered_map<std::string, std::uint32_t> subs_;
};

}  // namespace

std::unique_ptr<Session> make_icf_session(const IcfConfig& cfg) {
  return std::make_unique<IcfSession>(cfg);
}

}  // namespace lab::vendor
