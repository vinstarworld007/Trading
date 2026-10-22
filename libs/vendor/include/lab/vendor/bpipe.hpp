#pragma once

// B-PIPE look-alike for the Bloomberg-style vendor (docs/specs/vendor-bpipe.md).
// Bloomberg's wire protocol is licensed and only reachable through its
// BLPAPI SDK, so this is a lab protocol that carries BLPAPI's public model:
// a session, an authorization, the //blp/mktdata service, subscriptions to
// topics like "AAPL US Equity?fields=BID,ASK&interval=1.0" under a
// correlation ID, and events (SESSION_STATUS, SUBSCRIPTION_STATUS,
// SUBSCRIPTION_DATA) holding named elements.
//
// Frame: u32 length (bytes after it) then a request or an event, all
// integers big-endian. Prices travel as decimals (mantissa, exponent), never
// as binary floating point.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lab/core/endian.hpp"
#include "lab/vendor/consolidator.hpp"

namespace lab::vendor::bpipe {

inline constexpr std::string_view kMktdata = "//blp/mktdata";
inline constexpr std::string_view kApiAuth = "//blp/apiauth";

// Requests, client to server.
namespace request {
inline constexpr char kSessionStart = 'S';  // string application name
inline constexpr char kAuthorize = 'A';     // u64 cid, string user, string ip
inline constexpr char kOpenService = 'O';   // u64 cid, string service
inline constexpr char kSubscribe = 'B';     // u64 cid, string topic
inline constexpr char kUnsubscribe = 'U';   // u64 cid
}  // namespace request

inline constexpr char kEvent = 'E';  // server to client

// Event types (names from BLPAPI; the codes are the lab's).
enum class EventType : std::uint8_t {
  kSessionStatus = 1,
  kServiceStatus = 2,
  kAuthorizationStatus = 3,
  kSubscriptionStatus = 4,
  kSubscriptionData = 5,
};

inline const char* event_name(EventType t) noexcept {
  switch (t) {
    case EventType::kSessionStatus:
      return "SESSION_STATUS";
    case EventType::kServiceStatus:
      return "SERVICE_STATUS";
    case EventType::kAuthorizationStatus:
      return "AUTHORIZATION_STATUS";
    case EventType::kSubscriptionStatus:
      return "SUBSCRIPTION_STATUS";
    case EventType::kSubscriptionData:
      return "SUBSCRIPTION_DATA";
  }
  return "?";
}

// Element value types.
inline constexpr char kDecimal = 'D';  // i64 mantissa, i8 exponent
inline constexpr char kInt = 'I';      // i64
inline constexpr char kString = 'S';   // u16 length, bytes
inline constexpr char kEnum = 'E';     // u16 length, bytes (an enumeration constant)
inline constexpr char kTime = 'T';     // u64 ns since midnight

// ---------------------------------------------------------------- writing

class Writer {
 public:
  explicit Writer(std::string& out) : out_{out} {}

  // Starts a frame; end() fills in its length.
  void begin(char kind) {
    start_ = out_.size();
    out_.append(4, '\0');
    out_ += kind;
  }
  void end() { put32(start_, static_cast<std::uint32_t>(out_.size() - start_ - 4)); }

  void u8(std::uint8_t v) { out_ += static_cast<char>(v); }
  void u16(std::uint16_t v) {
    out_ += static_cast<char>(v >> 8);
    out_ += static_cast<char>(v);
  }
  void u64(std::uint64_t v) {
    for (int i = 7; i >= 0; --i) out_ += static_cast<char>(v >> (8 * i));
  }
  void str(std::string_view s) {
    u16(static_cast<std::uint16_t>(s.size()));
    out_.append(s);
  }

  // Event header; elements follow, then end_event().
  void event(EventType type, std::uint64_t cid, std::string_view message_type) {
    begin(kEvent);
    u8(static_cast<std::uint8_t>(type));
    u64(cid);
    str(message_type);
    count_at_ = out_.size();
    u16(0);
    count_ = 0;
  }
  void end_event() {
    out_[count_at_] = static_cast<char>(count_ >> 8);
    out_[count_at_ + 1] = static_cast<char>(count_);
    end();
  }

  void decimal(std::string_view name, std::int64_t mantissa, std::int8_t exponent) {
    element(name, kDecimal);
    u64(static_cast<std::uint64_t>(mantissa));
    u8(static_cast<std::uint8_t>(exponent));
  }
  // Price(4) as the shortest exact decimal.
  void price(std::string_view name, std::uint32_t p4) {
    std::int64_t m = p4;
    std::int8_t e = -4;
    while (e < 0 && m != 0 && m % 10 == 0) {
      m /= 10;
      ++e;
    }
    decimal(name, m, e);
  }
  void integer(std::string_view name, std::int64_t v) {
    element(name, kInt);
    u64(static_cast<std::uint64_t>(v));
  }
  void string(std::string_view name, std::string_view v) {
    element(name, kString);
    str(v);
  }
  void enumeration(std::string_view name, std::string_view v) {
    element(name, kEnum);
    str(v);
  }
  void time(std::string_view name, std::uint64_t ns) {
    element(name, kTime);
    u64(ns);
  }

 private:
  void element(std::string_view name, char type) {
    str(name);
    out_ += type;
    ++count_;
  }
  void put32(std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out_[at + i] = static_cast<char>(v >> (24 - 8 * i));
  }

  std::string& out_;
  std::size_t start_ = 0;
  std::size_t count_at_ = 0;
  std::uint16_t count_ = 0;
};

// ---------------------------------------------------------------- reading

struct Element {
  std::string name;
  char type = kInt;
  std::int64_t value = 0;  // decimal mantissa, integer or time
  std::int8_t exponent = 0;
  std::string text;  // string and enum

  // Decimal as Price(4) (exact for up to four decimals).
  std::int64_t price4() const noexcept {
    std::int64_t v = value;
    for (int e = exponent; e < -4; ++e) v /= 10;
    for (int e = exponent; e > -4; --e) v *= 10;
    return v;
  }
};

struct Event {
  EventType type{};
  std::uint64_t cid = 0;
  std::string message_type;
  std::vector<Element> elements;

  const Element* get(std::string_view name) const noexcept {
    for (const auto& e : elements) {
      if (e.name == name) return &e;
    }
    return nullptr;
  }
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> b) : p_{b.data()}, n_{b.size()} {}
  bool ok() const noexcept { return ok_; }
  std::uint8_t u8() { return need(1) ? static_cast<std::uint8_t>(p_[i_++]) : 0; }
  std::uint16_t u16() {
    if (!need(2)) return 0;
    const std::uint16_t v = load_be16(p_ + i_);
    i_ += 2;
    return v;
  }
  std::uint64_t u64() {
    if (!need(8)) return 0;
    const std::uint64_t v = load_be64(p_ + i_);
    i_ += 8;
    return v;
  }
  std::string str() {
    const std::size_t n = u16();
    if (!need(n)) return {};
    std::string s{reinterpret_cast<const char*>(p_ + i_), n};
    i_ += n;
    return s;
  }

 private:
  bool need(std::size_t k) {
    if (n_ - i_ < k) ok_ = false;
    return ok_;
  }
  const std::byte* p_;
  std::size_t n_;
  std::size_t i_ = 0;
  bool ok_ = true;
};

// `body` is a frame after its length field: the kind byte and what follows.
inline std::optional<Event> decode_event(std::span<const std::byte> body) {
  if (body.empty() || static_cast<char>(body[0]) != kEvent) return std::nullopt;
  Reader r{body.subspan(1)};
  Event e;
  e.type = static_cast<EventType>(r.u8());
  e.cid = r.u64();
  e.message_type = r.str();
  const std::uint16_t n = r.u16();
  e.elements.reserve(n);
  for (std::uint16_t i = 0; i < n && r.ok(); ++i) {
    Element el;
    el.name = r.str();
    el.type = static_cast<char>(r.u8());
    switch (el.type) {
      case kDecimal:
        el.value = static_cast<std::int64_t>(r.u64());
        el.exponent = static_cast<std::int8_t>(r.u8());
        break;
      case kInt:
      case kTime:
        el.value = static_cast<std::int64_t>(r.u64());
        break;
      default:
        el.text = r.str();
    }
    e.elements.push_back(std::move(el));
  }
  if (!r.ok()) return std::nullopt;
  return e;
}

// Cuts frames out of a TCP byte stream.
class FrameParser {
 public:
  void feed(std::span<const std::byte> data) { buf_.insert(buf_.end(), data.begin(), data.end()); }
  // The next frame body (kind byte first), valid until the next call.
  std::optional<std::span<const std::byte>> next() {
    if (pos_ > 0 && pos_ == buf_.size()) {
      buf_.clear();
      pos_ = 0;
    }
    if (buf_.size() - pos_ < 4) return std::nullopt;
    const std::size_t n = load_be32(buf_.data() + pos_);
    if (n == 0 || n > (16u << 20)) {
      bad_ = true;
      return std::nullopt;
    }
    if (buf_.size() - pos_ < 4 + n) {
      if (pos_ > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
      }
      return std::nullopt;
    }
    std::span<const std::byte> body{buf_.data() + pos_ + 4, n};
    pos_ += 4 + n;
    return body;
  }
  bool malformed() const noexcept { return bad_; }

 private:
  std::vector<std::byte> buf_;
  std::size_t pos_ = 0;
  bool bad_ = false;
};

// ---------------------------------------------------------------- topics and fields

// A parsed subscription string.
struct Topic {
  std::string security;  // "AAPL US Equity"
  std::string symbol;    // "AAPL"
  std::vector<std::string> fields;
  double interval_s = 0;
  bool ok = false;
};

// "//blp/mktdata/ticker/AAPL US Equity?fields=BID,ASK&interval=1.0", or the
// same without the service and ticker/ prefix.
inline Topic parse_topic(std::string_view t) {
  Topic out;
  if (t.starts_with(kMktdata)) t.remove_prefix(kMktdata.size());
  if (t.starts_with("/")) t.remove_prefix(1);
  if (t.starts_with("ticker/")) t.remove_prefix(7);
  const auto q = t.find('?');
  out.security = std::string{t.substr(0, q)};
  std::string_view opts = q == std::string_view::npos ? std::string_view{} : t.substr(q + 1);
  while (!opts.empty()) {
    const auto amp = opts.find('&');
    const std::string_view kv = opts.substr(0, amp);
    opts = amp == std::string_view::npos ? std::string_view{} : opts.substr(amp + 1);
    const auto eq = kv.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string_view k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (k == "fields") {
      std::string_view f = v;
      while (!f.empty()) {
        const auto c = f.find(',');
        if (c != 0) out.fields.emplace_back(f.substr(0, c));
        f = c == std::string_view::npos ? std::string_view{} : f.substr(c + 1);
      }
    } else if (k == "interval") {
      out.interval_s = std::strtod(std::string{v}.c_str(), nullptr);
    }
  }
  // "<ticker> <exchange code> Equity"
  const auto sp1 = out.security.find(' ');
  const auto sp2 = sp1 == std::string::npos ? sp1 : out.security.find(' ', sp1 + 1);
  if (sp1 != std::string::npos && sp2 != std::string::npos &&
      std::string_view{out.security}.substr(sp2 + 1) == "Equity") {
    out.symbol = out.security.substr(0, sp1);
    out.ok = !out.symbol.empty();
  }
  return out;
}

// The //blp/mktdata fields the vendor publishes, and the record fields each
// depends on. The first twelve are Bloomberg mnemonics; the auction, band and
// halt fields after them are lab names (docs/specs/vendor-bpipe.md).
enum class Id : std::uint8_t {
  kBid,
  kBidSize,
  kAsk,
  kAskSize,
  kLast,
  kLastSize,
  kVolume,
  kOpen,
  kHigh,
  kLow,
  kTrades,
  kStatus,
  kHaltReason,
  kRegSho,
  kAuctionType,
  kIndicativePrice,
  kPairedShares,
  kImbalanceShares,
  kImbalanceSide,
  kLuldLower,
  kLuldUpper,
};
struct Field {
  std::string_view name;
  std::uint32_t mask;
  Id id;
};
inline constexpr Field kFields[] = {
    {"BID", field::kBid, Id::kBid},
    {"BID_SIZE", field::kBidSize, Id::kBidSize},
    {"ASK", field::kAsk, Id::kAsk},
    {"ASK_SIZE", field::kAskSize, Id::kAskSize},
    {"LAST_PRICE", field::kLast, Id::kLast},
    {"SIZE_LAST_TRADE", field::kLastSize, Id::kLastSize},
    {"VOLUME", field::kVolume, Id::kVolume},
    {"OPEN", field::kOpen, Id::kOpen},
    {"HIGH", field::kHigh, Id::kHigh},
    {"LOW", field::kLow, Id::kLow},
    {"NUM_TRADES", field::kTrades, Id::kTrades},
    {"TRADING_STATUS", field::kStatus, Id::kStatus},
    {"HALT_REASON", field::kStatus, Id::kHaltReason},
    {"REG_SHO_RESTRICTION", field::kStatus, Id::kRegSho},
    {"AUCTION_TYPE", field::kAuction, Id::kAuctionType},
    {"INDICATIVE_PRICE", field::kAuction, Id::kIndicativePrice},
    {"PAIRED_SHARES", field::kAuction, Id::kPairedShares},
    {"IMBALANCE_SHARES", field::kAuction, Id::kImbalanceShares},
    {"IMBALANCE_SIDE", field::kAuction, Id::kImbalanceSide},
    {"LULD_LOWER_LIMIT", field::kBand, Id::kLuldLower},
    {"LULD_UPPER_LIMIT", field::kBand, Id::kLuldUpper},
};
inline constexpr std::size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

inline int field_index(std::string_view name) noexcept {
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    if (kFields[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

inline std::string_view trading_status(char s) noexcept {
  switch (s) {
    case 'T':
      return "TRADING";
    case 'H':
      return "HALTED";
    case 'P':
      return "PAUSED";
    case 'Q':
      return "QUOTATION_ONLY";
  }
  return "UNKNOWN";
}

inline std::string_view auction_type(char t) noexcept {
  switch (t) {
    case 'O':
      return "OPENING";
    case 'C':
    case 'A':
      return "CLOSING";
    case 'H':
      return "HALT";
  }
  return "NONE";
}

inline std::string_view imbalance_side(char d) noexcept {
  switch (d) {
    case 'B':
      return "BUY";
    case 'S':
      return "SELL";
    case 'N':
      return "NONE";
    case 'O':
      return "INSUFFICIENT_ORDERS";
    case 'P':
      return "PAUSED";
  }
  return "NOT_SET";
}

// A MarketDataEvents message: event type and subtype, EVENT_TIME, then the
// fields in `want` (a bit per kFields entry) that `mask` touches (all of them
// for the initial paint).
inline void market_data(Writer& w, std::uint64_t cid, const Record& r, std::uint32_t mask,
                        std::uint32_t want, bool initial) {
  w.event(EventType::kSubscriptionData, cid, "MarketDataEvents");
  const Kind k = kind_of(mask);
  if (initial) {
    w.enumeration("MKTDATA_EVENT_TYPE", "SUMMARY");
    w.enumeration("MKTDATA_EVENT_SUBTYPE", "INITPAINT");
  } else if (k == Kind::kTrade) {
    w.enumeration("MKTDATA_EVENT_TYPE", "TRADE");
    w.enumeration("MKTDATA_EVENT_SUBTYPE", "NEW");
  } else if (k == Kind::kQuote) {
    w.enumeration("MKTDATA_EVENT_TYPE", "QUOTE");
    w.enumeration("MKTDATA_EVENT_SUBTYPE",
                  (mask & (field::kBid | field::kBidSize)) ? "BID" : "ASK");
  } else {
    w.enumeration("MKTDATA_EVENT_TYPE", "SUMMARY");
    w.enumeration("MKTDATA_EVENT_SUBTYPE", (mask & field::kAuction) ? "AUCTION"
                                           : (mask & field::kBand)  ? "PRICE_LIMITS"
                                                                    : "TRADING_STATUS");
  }
  w.time("EVENT_TIME", r.exch_ns);
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    const Field& f = kFields[i];
    if (!(want & (1u << i)) || (!initial && !(f.mask & mask))) continue;
    switch (f.id) {
      case Id::kBid:
        w.price(f.name, r.bid);  // 0: no value
        break;
      case Id::kBidSize:
        w.integer(f.name, static_cast<std::int64_t>(r.bid_size));
        break;
      case Id::kAsk:
        w.price(f.name, r.ask);  // 0: no value
        break;
      case Id::kAskSize:
        w.integer(f.name, static_cast<std::int64_t>(r.ask_size));
        break;
      case Id::kLast:
        w.price(f.name, r.last);  // 0: no value
        break;
      case Id::kLastSize:
        if (r.trades) w.integer(f.name, static_cast<std::int64_t>(r.last_size));
        break;
      case Id::kVolume:
        w.integer(f.name, static_cast<std::int64_t>(r.volume));
        break;
      case Id::kOpen:
        w.price(f.name, r.open);  // 0: no value
        break;
      case Id::kHigh:
        w.price(f.name, r.high);  // 0: no value
        break;
      case Id::kLow:
        w.price(f.name, r.low);  // 0: no value
        break;
      case Id::kTrades:
        w.integer(f.name, static_cast<std::int64_t>(r.trades));
        break;
      case Id::kStatus:
        w.enumeration(f.name, trading_status(r.status));
        break;
      case Id::kHaltReason: {
        std::string_view reason{r.halt_reason.data(), r.halt_reason.size()};
        while (!reason.empty() && reason.back() == ' ') reason.remove_suffix(1);
        w.string(f.name, reason);
        break;
      }
      case Id::kRegSho:
        w.enumeration(f.name, r.reg_sho == '1'   ? "IN_EFFECT"
                              : r.reg_sho == '2' ? "CONTINUED"
                                                 : "NONE");
        break;
      case Id::kAuctionType:
        w.enumeration(f.name, auction_type(r.auction_type));
        break;
      case Id::kIndicativePrice:
        w.price(f.name, r.indicative_price);  // 0: no value
        break;
      case Id::kPairedShares:
        w.integer(f.name, static_cast<std::int64_t>(r.paired_shares));
        break;
      case Id::kImbalanceShares:
        w.integer(f.name, static_cast<std::int64_t>(r.imbalance_shares));
        break;
      case Id::kImbalanceSide:
        w.enumeration(f.name, imbalance_side(r.imbalance_side));
        break;
      case Id::kLuldLower:
        w.price(f.name, r.band_lower);  // 0: no value
        break;
      case Id::kLuldUpper:
        w.price(f.name, r.band_upper);  // 0: no value
        break;
    }
  }
  w.end_event();
}

}  // namespace lab::vendor::bpipe
