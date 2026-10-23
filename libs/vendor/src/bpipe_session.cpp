// The Bloomberg-style vendor's client session: SessionStart, Authorize,
// OpenService //blp/mktdata, and subscriptions by correlation ID.

#include <string>
#include <unordered_map>

#include "lab/vendor/bpipe.hpp"
#include "lab/vendor/sessions.hpp"

namespace lab::vendor {

namespace {

using bpipe::EventType;

class BpipeSession final : public Session {
 public:
  explicit BpipeSession(const BpipeConfig&) {}

  void on_bytes(std::span<const std::byte> in, Link& link) override {
    parser_.feed(in);
    out_.clear();
    while (auto body = parser_.next()) request(*body, link);
    if (!out_.empty()) link.send(out_);
    if (parser_.malformed()) link.close();
  }

  void on_update(std::uint32_t, const Subscription& s, const Record& r, std::uint32_t mask,
                 Link& link) override {
    const auto it = subs_.find(s.handle);
    if (it == subs_.end()) return;
    out_.clear();
    bpipe::Writer w{out_};
    bpipe::market_data(w, s.handle, r, mask, it->second.want, false);
    link.send(out_);
    link.published(r);
  }

  void on_end(Link& link) override {
    out_.clear();
    bpipe::Writer w{out_};
    w.event(EventType::kSessionStatus, 0, "SessionTerminated");
    w.string("description", "The lab exchange session has ended.");
    w.end_event();
    link.send(out_);
    link.close();
  }

 private:
  struct Sub {
    std::uint32_t sub = 0;
    std::uint32_t want = 0;
  };

  void request(std::span<const std::byte> body, Link& link) {
    bpipe::Writer w{out_};
    bpipe::Reader r{body.subspan(1)};
    switch (static_cast<char>(body[0])) {
      case bpipe::request::kSessionStart: {
        const std::string app = r.str();
        started_ = r.ok();
        w.event(EventType::kSessionStatus, 0, "SessionStarted");
        w.string("initialEndpoints", "lab-bpipe");
        w.end_event();
        break;
      }
      case bpipe::request::kAuthorize: {
        const std::uint64_t cid = r.u64();
        const std::string user = r.str();
        const std::string ip = r.str();
        if (!started_ || user.empty()) {
          w.event(EventType::kAuthorizationStatus, cid, "AuthorizationFailure");
          w.string("reason.category", "NO_AUTH");
          w.string("reason.description", "Start the session and name a user.");
        } else {
          authorized_ = true;
          w.event(EventType::kAuthorizationStatus, cid, "AuthorizationSuccess");
        }
        w.end_event();
        break;
      }
      case bpipe::request::kOpenService: {
        const std::uint64_t cid = r.u64();
        const std::string service = r.str();
        if (service == bpipe::kMktdata && started_) {
          mktdata_ = true;
          w.event(EventType::kServiceStatus, cid, "ServiceOpened");
        } else {
          w.event(EventType::kServiceStatus, cid, "ServiceOpenFailure");
          w.string("reason.description", "Unknown service.");
        }
        w.string("serviceName", service);
        w.end_event();
        break;
      }
      case bpipe::request::kSubscribe: {
        const std::uint64_t cid = r.u64();
        const std::string topic = r.str();
        subscribe(w, cid, topic, link);
        break;
      }
      case bpipe::request::kUnsubscribe: {
        const std::uint64_t cid = r.u64();
        if (const auto it = subs_.find(cid); it != subs_.end()) {
          link.unsubscribe(it->second.sub);
          subs_.erase(it);
          w.event(EventType::kSubscriptionStatus, cid, "SubscriptionTerminated");
          w.string("reason.description", "Unsubscribed by the client.");
          w.end_event();
        }
        break;
      }
      default:
        link.close();
    }
  }

  void failure(bpipe::Writer& w, std::uint64_t cid, std::string_view category,
               std::string_view description) {
    w.event(EventType::kSubscriptionStatus, cid, "SubscriptionFailure");
    w.string("reason.source", "lab-bpipe");
    w.integer("reason.errorCode", 2);
    w.string("reason.category", category);
    w.string("reason.description", description);
    w.end_event();
  }

  void subscribe(bpipe::Writer& w, std::uint64_t cid, const std::string& topic, Link& link) {
    if (!authorized_ || !mktdata_) {
      failure(w, cid, "NOT_AUTHORIZED", "Authorize and open //blp/mktdata first.");
      return;
    }
    if (subs_.contains(cid)) {
      failure(w, cid, "DUPLICATE_CORRELATIONID", "Correlation ID already in use.");
      return;
    }
    const bpipe::Topic t = bpipe::parse_topic(topic);
    const std::uint16_t locate = t.ok ? link.data().find(t.symbol) : 0;
    if (locate == 0) {
      failure(w, cid, "BAD_SEC", "Invalid security");
      return;
    }
    std::uint32_t want = 0;
    std::string bad;
    for (const auto& f : t.fields) {
      const int i = bpipe::field_index(f);
      if (i < 0) {
        bad += bad.empty() ? f : "," + f;
      } else {
        want |= 1u << i;
      }
    }
    if (t.fields.empty()) want = ~0u;
    if (want == 0) {
      failure(w, cid, "BAD_FLD", "No valid fields: " + bad);
      return;
    }
    w.event(EventType::kSubscriptionStatus, cid, "SubscriptionStarted");
    if (!bad.empty()) w.string("exceptions.fieldId", bad);
    w.end_event();
    bpipe::market_data(w, cid, link.data().record(locate), field::kAll, want, true);
    const auto interval = static_cast<std::uint64_t>(t.interval_s * 1e9);
    subs_[cid] = Sub{link.subscribe(locate, cid, interval), want};
  }

  bpipe::FrameParser parser_;
  std::string out_;
  bool started_ = false;
  bool authorized_ = false;
  bool mktdata_ = false;
  std::unordered_map<std::uint64_t, Sub> subs_;
};

}  // namespace

std::unique_ptr<Session> make_bpipe_session(const BpipeConfig& cfg) {
  return std::make_unique<BpipeSession>(cfg);
}

}  // namespace lab::vendor
