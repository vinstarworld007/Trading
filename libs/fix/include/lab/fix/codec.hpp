#pragma once

// FIX 4.4 tag=value framing (docs/specs/fix44-subset.md): a Writer that builds
// one message in a caller-owned buffer and fills in BodyLength and CheckSum,
// a StreamParser that cuts complete, checksummed messages out of a TCP byte
// stream, and a Message that indexes one message's fields. No allocation
// after construction; values are views into the parser's buffer.
//
//   std::array<char, 512> buf;
//   fix::Writer w{buf.data(), buf.size()};
//   w.begin("FIX.4.4", '0');
//   w.add(fix::tag::kSenderCompID, "FIRM");
//   ...
//   std::string_view wire = w.finish();
//
//   fix::StreamParser in;
//   recv into in.write_area(), then in.commit(n);
//   while (auto m = in.next()) { fix::Message msg; if (msg.parse(*m)) ... }

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace lab::fix {

inline constexpr char kSoh = '\x01';
inline constexpr std::string_view kBeginString = "FIX.4.4";

namespace tag {
inline constexpr int kAccount = 1;
inline constexpr int kAvgPx = 6;
inline constexpr int kBeginSeqNo = 7;
inline constexpr int kBeginString = 8;
inline constexpr int kBodyLength = 9;
inline constexpr int kCheckSum = 10;
inline constexpr int kClOrdID = 11;
inline constexpr int kCumQty = 14;
inline constexpr int kEndSeqNo = 16;
inline constexpr int kExecID = 17;
inline constexpr int kLastPx = 31;
inline constexpr int kLastQty = 32;
inline constexpr int kMsgSeqNum = 34;
inline constexpr int kMsgType = 35;
inline constexpr int kNewSeqNo = 36;
inline constexpr int kOrderID = 37;
inline constexpr int kOrderQty = 38;
inline constexpr int kOrdStatus = 39;
inline constexpr int kOrdType = 40;
inline constexpr int kOrigClOrdID = 41;
inline constexpr int kPossDupFlag = 43;
inline constexpr int kPrice = 44;
inline constexpr int kRefSeqNum = 45;
inline constexpr int kSenderCompID = 49;
inline constexpr int kSendingTime = 52;
inline constexpr int kSide = 54;
inline constexpr int kSymbol = 55;
inline constexpr int kTargetCompID = 56;
inline constexpr int kText = 58;
inline constexpr int kTimeInForce = 59;
inline constexpr int kTransactTime = 60;
inline constexpr int kEncryptMethod = 98;
inline constexpr int kCxlRejReason = 102;
inline constexpr int kOrdRejReason = 103;
inline constexpr int kHeartBtInt = 108;
inline constexpr int kTestReqID = 112;
inline constexpr int kOrigSendingTime = 122;
inline constexpr int kGapFillFlag = 123;
inline constexpr int kResetSeqNumFlag = 141;
inline constexpr int kExecType = 150;
inline constexpr int kLeavesQty = 151;
inline constexpr int kRefTagID = 371;
inline constexpr int kRefMsgType = 372;
inline constexpr int kSessionRejectReason = 373;
inline constexpr int kBusinessRejectRefID = 379;
inline constexpr int kBusinessRejectReason = 380;
inline constexpr int kCxlRejResponseTo = 434;
inline constexpr int kUsername = 553;
inline constexpr int kPassword = 554;
}  // namespace tag

namespace msg_type {
inline constexpr char kHeartbeat = '0';
inline constexpr char kTestRequest = '1';
inline constexpr char kResendRequest = '2';
inline constexpr char kReject = '3';
inline constexpr char kSequenceReset = '4';
inline constexpr char kLogout = '5';
inline constexpr char kExecutionReport = '8';
inline constexpr char kOrderCancelReject = '9';
inline constexpr char kLogon = 'A';
inline constexpr char kNewOrderSingle = 'D';
inline constexpr char kOrderCancelRequest = 'F';
inline constexpr char kOrderCancelReplaceRequest = 'G';
inline constexpr char kBusinessMessageReject = 'j';

// Session-level messages are never resent; a gap fill covers them.
constexpr bool is_session(char t) noexcept {
  return t == kHeartbeat || t == kTestRequest || t == kResendRequest || t == kReject ||
         t == kSequenceReset || t == kLogout || t == kLogon;
}
}  // namespace msg_type

// ---- Values ----------------------------------------------------------------

inline std::optional<std::uint64_t> parse_uint(std::string_view s) noexcept {
  if (s.empty() || s.size() > 19) return std::nullopt;
  std::uint64_t v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

// "100.25" -> 1002500: Price(4), 4 implied decimals. No floating point; more
// than four decimal places, a sign, or an exponent is rejected.
inline std::optional<std::uint64_t> parse_price(std::string_view s) noexcept {
  const std::size_t dot = s.find('.');
  const std::string_view whole = s.substr(0, dot);
  const std::string_view frac = dot == std::string_view::npos ? "" : s.substr(dot + 1);
  if (frac.size() > 4 || (whole.empty() && frac.empty())) return std::nullopt;
  std::uint64_t w = 0;
  if (!whole.empty()) {
    const auto v = parse_uint(whole);
    if (!v || *v > 1'000'000'000'000ULL) return std::nullopt;
    w = *v;
  }
  std::uint64_t f = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    f *= 10;
    if (i < frac.size()) {
      if (frac[i] < '0' || frac[i] > '9') return std::nullopt;
      f += static_cast<std::uint64_t>(frac[i] - '0');
    }
  }
  return w * 10'000 + f;
}

// Price(4) -> shortest decimal: 1002500 -> "100.25", 1000000 -> "100".
// Returns characters written (at most 25).
inline std::size_t format_price(char* out, std::uint64_t price4) noexcept {
  char* p = std::to_chars(out, out + 20, price4 / 10'000).ptr;
  std::uint64_t f = price4 % 10'000;
  if (f != 0) {
    *p++ = '.';
    char digits[4];
    for (int i = 3; i >= 0; --i) {
      digits[i] = static_cast<char>('0' + f % 10);
      f /= 10;
    }
    int n = 4;
    while (digits[n - 1] == '0') --n;
    std::memcpy(p, digits, static_cast<std::size_t>(n));
    p += n;
  }
  return static_cast<std::size_t>(p - out);
}

inline constexpr std::size_t kTimestampSize = 21;  // YYYYMMDD-HH:MM:SS.sss

// Nanoseconds since the Unix epoch -> UTCTimestamp with milliseconds.
inline void format_utc_timestamp(char* out, std::uint64_t unix_ns) noexcept {
  const std::uint64_t ms_total = unix_ns / 1'000'000;
  const std::uint64_t secs = ms_total / 1000;
  const auto ms = static_cast<unsigned>(ms_total % 1000);
  const auto sod = static_cast<unsigned>(secs % 86400);
  // Civil date from days since 1970-01-01 (H. Hinnant's days_from_civil inverse).
  const std::int64_t z = static_cast<std::int64_t>(secs / 86400) + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const auto y = static_cast<unsigned>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2));
  auto put = [&](std::size_t at, unsigned v, int width) {
    for (int i = width - 1; i >= 0; --i) {
      out[at + static_cast<std::size_t>(i)] = static_cast<char>('0' + v % 10);
      v /= 10;
    }
  };
  put(0, y, 4);
  put(4, m, 2);
  put(6, d, 2);
  out[8] = '-';
  put(9, sod / 3600, 2);
  out[11] = ':';
  put(12, sod / 60 % 60, 2);
  out[14] = ':';
  put(15, sod % 60, 2);
  out[17] = '.';
  put(18, ms, 3);
}

// True for YYYYMMDD-HH:MM:SS with optional .sss.
inline bool valid_utc_timestamp(std::string_view s) noexcept {
  if (s.size() != 17 && s.size() != 21) return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    const bool digit = c >= '0' && c <= '9';
    if (i == 8) {
      if (c != '-') return false;
    } else if (i == 11 || i == 14) {
      if (c != ':') return false;
    } else if (i == 17) {
      if (c != '.') return false;
    } else if (!digit) {
      return false;
    }
  }
  return true;
}

// ---- Writer ----------------------------------------------------------------

// Builds one message. The body is written after a reserved gap, so finish()
// can put "8=...|9=len|" right in front of it without moving anything.
class Writer {
 public:
  static constexpr std::size_t kHeadRoom = 32;  // "8=FIX.4.4|9=" + up to 10 digits + "|"

  Writer(char* buf, std::size_t capacity) noexcept : buf_{buf}, cap_{capacity} {}

  void begin(std::string_view begin_string, char msg_type) noexcept {
    begin_string_ = begin_string.substr(0, 12);  // with "9=" and 10 digits, fits kHeadRoom
    pos_ = kHeadRoom;
    ok_ = cap_ > kHeadRoom + 8;
    char t[1] = {msg_type};
    add(tag::kMsgType, std::string_view{t, 1});
  }

  void add(int tag, std::string_view value) noexcept {
    char key[12];
    char* k = std::to_chars(key, key + sizeof key, tag).ptr;
    const auto klen = static_cast<std::size_t>(k - key);
    if (!ok_ || pos_ + klen + value.size() + 2 + 7 > cap_) {
      ok_ = false;
      return;
    }
    std::memcpy(buf_ + pos_, key, klen);
    pos_ += klen;
    buf_[pos_++] = '=';
    std::memcpy(buf_ + pos_, value.data(), value.size());
    pos_ += value.size();
    buf_[pos_++] = kSoh;
  }
  void add(int tag, char value) noexcept { add(tag, std::string_view{&value, 1}); }
  void add_uint(int tag, std::uint64_t value) noexcept {
    char v[20];
    add(tag,
        std::string_view{v, static_cast<std::size_t>(std::to_chars(v, v + 20, value).ptr - v)});
  }
  void add_price(int tag, std::uint64_t price4) noexcept {
    char v[32];
    add(tag, std::string_view{v, format_price(v, price4)});
  }
  void add_time(int tag, std::uint64_t unix_ns) noexcept {
    char v[kTimestampSize];
    format_utc_timestamp(v, unix_ns);
    add(tag, std::string_view{v, kTimestampSize});
  }
  // Fields already encoded as tag=value|..., copied as they are.
  void add_raw(std::string_view fields) noexcept {
    if (!ok_ || pos_ + fields.size() + 7 > cap_) {
      ok_ = false;
      return;
    }
    std::memcpy(buf_ + pos_, fields.data(), fields.size());
    pos_ += fields.size();
  }

  // Where the next field will go: lets a caller remember where a run of
  // fields starts, to read it back with written_since().
  std::size_t mark() const noexcept { return pos_; }
  std::string_view written_since(std::size_t mark) const noexcept {
    return {buf_ + mark, pos_ - mark};
  }

  // Writes BeginString, BodyLength and CheckSum. Empty if the buffer was too
  // small; the message is then unusable.
  std::string_view finish() noexcept {
    if (!ok_) return {};
    const std::size_t body = pos_ - kHeadRoom;
    char head[2 * kHeadRoom];
    std::size_t h = 0;
    std::memcpy(head, "8=", 2);
    h += 2;
    const std::size_t bs = begin_string_.size() < 12 ? begin_string_.size() : 12;
    std::memcpy(head + h, begin_string_.data(), bs);
    h += bs;
    head[h++] = kSoh;
    std::memcpy(head + h, "9=", 2);
    h += 2;
    h = static_cast<std::size_t>(std::to_chars(head + h, head + sizeof head, body).ptr - head);
    head[h++] = kSoh;
    const std::size_t start = kHeadRoom - h;
    std::memcpy(buf_ + start, head, h);
    unsigned sum = 0;
    for (std::size_t i = start; i < pos_; ++i) sum += static_cast<unsigned char>(buf_[i]);
    sum %= 256;
    buf_[pos_++] = '1';
    buf_[pos_++] = '0';
    buf_[pos_++] = '=';
    buf_[pos_++] = static_cast<char>('0' + sum / 100);
    buf_[pos_++] = static_cast<char>('0' + sum / 10 % 10);
    buf_[pos_++] = static_cast<char>('0' + sum % 10);
    buf_[pos_++] = kSoh;
    return {buf_ + start, pos_ - start};
  }

  bool ok() const noexcept { return ok_; }

 private:
  char* buf_;
  std::size_t cap_;
  std::size_t pos_ = kHeadRoom;
  std::string_view begin_string_ = kBeginString;
  bool ok_ = false;
};

// ---- Message ---------------------------------------------------------------

struct Field {
  int tag;
  std::string_view value;
};

// One message's fields in wire order. Views into the parsed bytes.
class Message {
 public:
  static constexpr std::size_t kMaxFields = 64;

  // Splits a complete message (as StreamParser::next() returns it). False if
  // a field is malformed or there are more than kMaxFields.
  bool parse(std::string_view wire) noexcept {
    n_ = 0;
    std::size_t i = 0;
    while (i < wire.size()) {
      const std::size_t eq = wire.find('=', i);
      if (eq == std::string_view::npos) return false;
      const std::size_t end = wire.find(kSoh, eq + 1);
      if (end == std::string_view::npos) return false;
      int t = 0;
      const auto [p, ec] = std::from_chars(wire.data() + i, wire.data() + eq, t);
      if (ec != std::errc{} || p != wire.data() + eq || t <= 0) return false;
      if (n_ == kMaxFields) return false;
      fields_[n_++] = Field{t, wire.substr(eq + 1, end - eq - 1)};
      i = end + 1;
    }
    if (n_ < 4 || fields_[2].tag != tag::kMsgType || fields_[2].value.size() != 1) return false;
    type_ = fields_[2].value[0];
    return true;
  }

  char type() const noexcept { return type_; }
  std::span<const Field> fields() const noexcept { return {fields_.data(), n_}; }

  std::optional<std::string_view> get(int t) const noexcept {
    for (std::size_t i = 0; i < n_; ++i) {
      if (fields_[i].tag == t) return fields_[i].value;
    }
    return std::nullopt;
  }
  std::optional<std::uint64_t> get_uint(int t) const noexcept {
    const auto v = get(t);
    return v ? parse_uint(*v) : std::nullopt;
  }
  std::optional<char> get_char(int t) const noexcept {
    const auto v = get(t);
    if (!v || v->size() != 1) return std::nullopt;
    return (*v)[0];
  }
  bool flag(int t) const noexcept { return get_char(t) == 'Y'; }

 private:
  std::array<Field, kMaxFields> fields_{};
  std::size_t n_ = 0;
  char type_ = 0;
};

// ---- StreamParser ----------------------------------------------------------

// Cuts complete messages out of a TCP byte stream. A message whose
// BodyLength or CheckSum does not add up is garbled: FIX says to drop it
// without consuming a sequence number, so it is skipped (and counted) and
// the parser resynchronizes on the next "8=".
class StreamParser {
 public:
  explicit StreamParser(std::size_t capacity = 1u << 16) : buf_(capacity) {}

  std::span<char> write_area() noexcept {
    compact();
    return {buf_.data() + end_, buf_.size() - end_};
  }
  void commit(std::size_t n) noexcept { end_ += n; }
  bool feed(std::string_view data) noexcept {
    const auto area = write_area();
    if (data.size() > area.size()) return false;
    std::memcpy(area.data(), data.data(), data.size());
    commit(data.size());
    return true;
  }

  // The next complete, checksummed message, valid until the next call.
  std::optional<std::string_view> next() noexcept {
    for (;;) {
      const std::string_view in{buf_.data() + begin_, end_ - begin_};
      if (in.size() < 2) return std::nullopt;
      if (in[0] != '8' || in[1] != '=') {
        resync(1);
        continue;
      }
      // 8=...|9=N|
      const std::size_t soh1 = in.find(kSoh);
      if (soh1 == std::string_view::npos) {
        if (in.size() > 32) resync(1);
        return std::nullopt;
      }
      if (in.size() < soh1 + 3) return std::nullopt;
      if (in[soh1 + 1] != '9' || in[soh1 + 2] != '=') {
        resync(1);
        continue;
      }
      const std::size_t soh2 = in.find(kSoh, soh1 + 3);
      if (soh2 == std::string_view::npos) {
        if (in.size() - soh1 > 16) resync(1);
        return std::nullopt;
      }
      const auto body = parse_uint(in.substr(soh1 + 3, soh2 - soh1 - 3));
      if (!body || *body > buf_.size() / 2) {
        resync(1);
        continue;
      }
      const std::size_t trailer = soh2 + 1 + *body;  // where "10=" must start
      if (in.size() < trailer + 7) return std::nullopt;
      const std::string_view t = in.substr(trailer, 7);
      if (t.substr(0, 3) != "10=" || t[6] != kSoh) {
        resync(1);
        continue;
      }
      unsigned sum = 0;
      for (std::size_t i = 0; i < trailer; ++i) sum += static_cast<unsigned char>(in[i]);
      const auto expected = parse_uint(t.substr(3, 3));
      if (!expected || *expected != sum % 256) {
        resync(trailer + 7);
        continue;
      }
      begin_ += trailer + 7;
      return in.substr(0, trailer + 7);
    }
  }

  std::uint64_t garbled() const noexcept { return garbled_; }
  // True when the buffer is full of an incomplete message: the peer is not
  // speaking FIX (or sent something larger than the buffer).
  bool stuck() const noexcept { return begin_ == 0 && end_ == buf_.size(); }

 private:
  void resync(std::size_t skip) noexcept {
    ++garbled_;
    begin_ += skip;
    // Skip to the next "8=" so one bad byte does not cost a message per byte.
    if (begin_ > end_) begin_ = end_;
    const std::string_view in{buf_.data() + begin_, end_ - begin_};
    const std::size_t at = in.find("8=FIX");
    if (at != std::string_view::npos) {
      begin_ += at;
    } else if (end_ - begin_ > 4) {
      begin_ = end_ - 4;  // keep what could be the start of "8=FIX"
    }
  }
  void compact() noexcept {
    if (begin_ == 0) return;
    std::memmove(buf_.data(), buf_.data() + begin_, end_ - begin_);
    end_ -= begin_;
    begin_ = 0;
  }

  std::vector<char> buf_;
  std::size_t begin_ = 0;
  std::size_t end_ = 0;
  std::uint64_t garbled_ = 0;
};

}  // namespace lab::fix
