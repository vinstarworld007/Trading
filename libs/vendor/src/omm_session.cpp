// The Reuters-style vendor's client session: a WebSocket (tr_json2) carrying
// OMM JSON. Login, MarketPrice item streams (single, batch, view, snapshot),
// Close, and Ping/Pong.

#include <string>
#include <unordered_map>

#include "lab/vendor/json.hpp"
#include "lab/vendor/omm.hpp"
#include "lab/vendor/sessions.hpp"
#include "lab/vendor/ws.hpp"

namespace lab::vendor {

namespace {

class OmmSession final : public Session {
 public:
  explicit OmmSession(const OmmConfig& cfg) : cfg_{cfg} {}

  void on_bytes(std::span<const std::byte> in, Link& link) override {
    last_rx_ = link.now_ns();
    if (!upgraded_) {
      head_.append(reinterpret_cast<const char*>(in.data()), in.size());
      const auto end = head_.find("\r\n\r\n");
      if (end == std::string::npos) {
        if (head_.size() > 16384) link.close();
        return;
      }
      const std::string reply = ws::server_handshake(head_.substr(0, end + 4), omm::kSubprotocol);
      if (reply.empty()) {
        link.send(std::string_view{"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"});
        link.close();
        return;
      }
      link.send(reply);
      upgraded_ = true;
      decoder_.feed(std::string_view{head_}.substr(end + 4));
      head_.clear();
    } else {
      decoder_.feed(in);
    }
    while (auto m = decoder_.next()) {
      switch (m->opcode) {
        case ws::kText:
          text(m->payload, link);
          break;
        case ws::kPing:
          frame(ws::kPong, m->payload, link);
          break;
        case ws::kClose:
          frame(ws::kClose, m->payload.substr(0, 2), link);
          link.close();
          return;
        default:
          break;
      }
    }
    if (decoder_.malformed()) link.close();
  }

  void on_update(std::uint32_t, const Subscription& s, const Record& r, std::uint32_t mask,
                 Link& link) override {
    const auto it = streams_.find(static_cast<std::int64_t>(s.handle));
    if (it == streams_.end()) return;
    out_.clear();
    out_ += '[';
    omm::update(out_, it->first, r, mask, it->second.view);
    out_ += ']';
    frame(ws::kText, out_, link);
    link.published(r);
  }

  void on_timer(std::uint64_t now, Link& link) override {
    if (!upgraded_) return;
    if (now - last_tx_ > cfg_.ping_interval_ns) frame(ws::kText, R"([{"Type":"Ping"}])", link);
  }

  void on_end(Link& link) override {
    if (upgraded_) {
      out_ = "[";
      bool first = true;
      for (const auto& [id, st] : streams_) {
        if (!first) out_ += ',';
        first = false;
        omm::status(out_, id, "Closed", "Suspect", "", "The lab exchange session has ended.");
      }
      out_ += ']';
      if (!streams_.empty()) frame(ws::kText, out_, link);
      frame(ws::kClose, std::string_view{"\x03\xe8", 2}, link);  // 1000 normal closure
    }
    link.close();
  }

 private:
  struct Stream {
    std::uint32_t sub = 0;
    std::uint32_t view = ~0u;
  };

  void frame(ws::Opcode op, std::string_view payload, Link& link) {
    std::string f;
    ws::append_frame(f, op, payload, false);
    link.send(f);
    last_tx_ = link.now_ns();
  }

  void text(const std::string& payload, Link& link) {
    const auto v = json::parse(payload);
    if (!v) {
      error("Invalid JSON", link);
      return;
    }
    out_ = "[";
    first_ = true;
    try {
      if (v->is_array()) {
        for (const auto& m : v->items) message(m, link);
      } else {
        message(*v, link);
      }
    } catch (const std::exception&) {  // an ID that is not an integer
      error("Invalid message", link);
    }
    out_ += ']';
    if (!first_) frame(ws::kText, out_, link);
  }

  void next() {
    if (!first_) out_ += ',';
    first_ = false;
  }

  void error(std::string_view text, Link& link) {
    std::string e = R"([{"Type":"Error","Text":)";
    json::quote(e, text);
    e += "}]";
    frame(ws::kText, e, link);
  }

  void message(const json::Value& m, Link& link) {
    if (!m.is_object()) return;
    const std::string_view type = m.str("Type");
    if (type == "Ping") {
      next();
      out_ += R"({"Type":"Pong"})";
      return;
    }
    if (type == "Pong") return;
    const json::Value* idv = m.get("ID");
    if (idv == nullptr || !idv->is_number()) return;
    const std::int64_t id = std::stoll(idv->text);
    if (type == "Close") {
      close_stream(id, link);
      return;
    }
    if (!type.empty() && type != "Request") return;
    const std::string_view domain = m.str("Domain");
    const json::Value* key = m.get("Key");
    if (domain == "Login") {
      login_ = true;
      next();
      omm::login_refresh(out_, id, key != nullptr ? key->str("Name") : "");
      return;
    }
    if (!domain.empty() && domain != "MarketPrice") {
      next();
      omm::status(out_, id, "Closed", "Suspect", "UnsupportedDomain", "Domain not supported.");
      return;
    }
    if (!login_) {
      next();
      omm::status(out_, id, "Closed", "Suspect", "NotEntitled", "Login first.");
      return;
    }
    const json::Value* streaming = m.get("Streaming");
    const bool stream =
        streaming == nullptr || streaming->type != json::Value::Type::kBool || streaming->b;
    std::uint32_t view = ~0u;
    if (const json::Value* vw = m.get("View"); vw != nullptr && vw->is_array()) {
      view = 0;
      for (const auto& f : vw->items) {
        const int i = omm::field_index(f.text);
        if (i >= 0) view |= 1u << i;
      }
    }
    const json::Value* name = key != nullptr ? key->get("Name") : nullptr;
    if (name != nullptr && name->is_array()) {  // batch: streams ID+1, ID+2, ...
      std::int64_t sid = id;
      for (const auto& n : name->items) item(++sid, n.text, view, stream, link);
      next();
      omm::status(
          out_, id, "Closed", "Ok", "",
          "Processed " + std::to_string(name->items.size()) + " total items from Batch Request.");
      return;
    }
    if (name == nullptr || !name->is_string()) {
      next();
      omm::status(out_, id, "Closed", "Suspect", "UsageError", "Key Name is required.");
      return;
    }
    item(id, name->text, view, stream, link);
  }

  void item(std::int64_t id, std::string_view ric, std::uint32_t view, bool stream, Link& link) {
    close_stream(id, link);  // a reissue replaces the old stream
    const std::uint16_t locate = link.data().find(omm::symbol_of(ric));
    next();
    if (locate == 0) {
      omm::status(out_, id, "Closed", "Suspect", "NotFound", "The record could not be found");
      return;
    }
    omm::refresh(out_, id, cfg_.service, link.data().record(locate), view, stream, false);
    if (stream)
      streams_[id] = Stream{link.subscribe(locate, static_cast<std::uint64_t>(id), 0), view};
  }

  void close_stream(std::int64_t id, Link& link) {
    const auto it = streams_.find(id);
    if (it == streams_.end()) return;
    link.unsubscribe(it->second.sub);
    streams_.erase(it);
  }

  OmmConfig cfg_;
  bool upgraded_ = false;
  bool login_ = false;
  std::string head_;
  ws::Decoder decoder_;
  std::unordered_map<std::int64_t, Stream> streams_;
  std::string out_;
  bool first_ = true;
  std::uint64_t last_tx_ = 0;
  std::uint64_t last_rx_ = 0;
};

}  // namespace

std::unique_ptr<Session> make_omm_session(const OmmConfig& cfg) {
  return std::make_unique<OmmSession>(cfg);
}

}  // namespace lab::vendor
