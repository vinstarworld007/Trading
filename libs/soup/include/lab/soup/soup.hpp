#pragma once

// SoupBinTCP 3.00 framing, from docs/specs/soupbintcp-layout.md.
//
// Every packet is a 2-byte big-endian length, a 1-byte type, then a payload.
// Writers append whole packets to a caller-owned buffer; StreamParser turns
// the TCP byte stream back into packets without copying them twice.
//
//   std::array<std::byte, 64> buf;
//   std::size_t n = soup::write_login_request(buf.data(), "FIRM01", "secret", "", 1);
//
//   soup::StreamParser in;
//   in.feed(received);                              // or recv() into in.write_area()
//   while (auto p = in.next()) handle(p->type, p->payload);

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "lab/core/compiler.hpp"
#include "lab/core/endian.hpp"

namespace lab::soup {

inline constexpr std::size_t kHeaderSize = 3;
inline constexpr std::size_t kMaxPacket = 0xFFFF + 2;  // length field counts type + payload

// Packet types.
inline constexpr char kDebug = '+';
inline constexpr char kLoginRequest = 'L';
inline constexpr char kLoginAccepted = 'A';
inline constexpr char kLoginRejected = 'J';
inline constexpr char kSequencedData = 'S';
inline constexpr char kServerHeartbeat = 'H';
inline constexpr char kEndOfSession = 'Z';
inline constexpr char kUnsequencedData = 'U';
inline constexpr char kClientHeartbeat = 'R';
inline constexpr char kLogoutRequest = 'O';

// Login Rejected reason codes.
inline constexpr char kRejectNotAuthorized = 'A';
inline constexpr char kRejectSessionUnavailable = 'S';

inline constexpr std::size_t kLoginRequestSize = kHeaderSize + 6 + 10 + 10 + 20;  // 49
inline constexpr std::size_t kLoginAcceptedSize = kHeaderSize + 10 + 20;          // 33
inline constexpr std::size_t kLoginRejectedSize = kHeaderSize + 1;                // 4

// Left-justified, space-padded alphanumeric field.
inline void put_alpha(std::byte* p, std::size_t width, std::string_view s) noexcept {
  std::memset(p, ' ', width);
  std::memcpy(p, s.data(), s.size() < width ? s.size() : width);
}

inline std::string_view get_alpha(const std::byte* p, std::size_t width) noexcept {
  const char* c = reinterpret_cast<const char*>(p);
  std::size_t n = width;
  while (n > 0 && c[n - 1] == ' ') --n;
  return {c, n};
}

// Right-justified, space-padded ASCII number.
inline void put_number(std::byte* p, std::size_t width, std::uint64_t v) noexcept {
  std::memset(p, ' ', width);
  char digits[24];
  const auto r = std::to_chars(digits, digits + sizeof digits, v);
  const std::size_t n = static_cast<std::size_t>(r.ptr - digits);
  std::memcpy(p + width - n, digits, n);
}

// Parses an ASCII number padded with spaces on either side. nullopt if bad.
inline std::optional<std::uint64_t> get_number(const std::byte* p, std::size_t width) noexcept {
  const char* c = reinterpret_cast<const char*>(p);
  std::size_t b = 0;
  std::size_t e = width;
  while (b < e && c[b] == ' ') ++b;
  while (e > b && c[e - 1] == ' ') --e;
  if (b == e) return std::uint64_t{0};
  std::uint64_t v = 0;
  const auto r = std::from_chars(c + b, c + e, v);
  if (r.ec != std::errc{} || r.ptr != c + e) return std::nullopt;
  return v;
}

inline std::size_t write_header(std::byte* p, char type, std::size_t payload) noexcept {
  store_be16(p, static_cast<std::uint16_t>(payload + 1));
  p[2] = static_cast<std::byte>(type);
  return kHeaderSize;
}

// A packet with no payload (heartbeats, End of Session, Logout).
inline std::size_t write_empty(std::byte* p, char type) noexcept {
  return write_header(p, type, 0);
}

// Sequenced or Unsequenced Data wrapping `msg`.
inline std::size_t write_data(std::byte* p, char type, std::span<const std::byte> msg) noexcept {
  write_header(p, type, msg.size());
  std::memcpy(p + kHeaderSize, msg.data(), msg.size());
  return kHeaderSize + msg.size();
}

inline std::size_t write_login_request(std::byte* p, std::string_view user,
                                       std::string_view password, std::string_view session,
                                       std::uint64_t next_sequence) noexcept {
  write_header(p, kLoginRequest, kLoginRequestSize - kHeaderSize);
  put_alpha(p + 3, 6, user);
  put_alpha(p + 9, 10, password);
  put_alpha(p + 19, 10, session);
  put_number(p + 29, 20, next_sequence);
  return kLoginRequestSize;
}

inline std::size_t write_login_accepted(std::byte* p, std::string_view session,
                                        std::uint64_t next_sequence) noexcept {
  write_header(p, kLoginAccepted, kLoginAcceptedSize - kHeaderSize);
  put_alpha(p + 3, 10, session);
  put_number(p + 13, 20, next_sequence);
  return kLoginAcceptedSize;
}

inline std::size_t write_login_rejected(std::byte* p, char reason) noexcept {
  write_header(p, kLoginRejected, 1);
  p[3] = static_cast<std::byte>(reason);
  return kLoginRejectedSize;
}

struct LoginRequest {
  std::string_view user;
  std::string_view password;
  std::string_view session;  // empty: the current session
  std::uint64_t next_sequence = 0;
};

// `payload` is everything after the type byte.
inline std::optional<LoginRequest> parse_login_request(
    std::span<const std::byte> payload) noexcept {
  if (payload.size() < kLoginRequestSize - kHeaderSize) return std::nullopt;
  const std::byte* p = payload.data();  // spec offsets minus the 3-byte header
  const auto seq = get_number(p + 26, 20);
  if (!seq) return std::nullopt;
  return LoginRequest{get_alpha(p, 6), get_alpha(p + 6, 10), get_alpha(p + 16, 10), *seq};
}

struct LoginAccepted {
  std::string_view session;
  std::uint64_t next_sequence = 0;
};

inline std::optional<LoginAccepted> parse_login_accepted(
    std::span<const std::byte> payload) noexcept {
  if (payload.size() < kLoginAcceptedSize - kHeaderSize) return std::nullopt;
  const std::byte* p = payload.data();  // spec offsets minus the 3-byte header
  const auto seq = get_number(p + 10, 20);
  if (!seq) return std::nullopt;
  return LoginAccepted{get_alpha(p, 10), *seq};
}

struct Packet {
  char type;
  std::span<const std::byte> payload;  // after the type byte
};

// Reassembles packets from a TCP byte stream. Holds at most one maximum-size
// packet plus whatever arrived after it; allocated once.
class StreamParser {
 public:
  explicit StreamParser(std::size_t capacity = 4 * kMaxPacket) : buf_(capacity) {}

  // Where the next recv() may write, and how much. Call commit() after.
  std::span<std::byte> write_area() noexcept {
    compact();
    return {buf_.data() + end_, buf_.size() - end_};
  }
  void commit(std::size_t n) noexcept { end_ += n; }

  // Copies `data` in. Returns false if it does not fit (the peer is
  // sending faster than packets are consumed, or a packet is malformed).
  bool feed(std::span<const std::byte> data) noexcept {
    const auto area = write_area();
    if (data.size() > area.size()) return false;
    std::memcpy(area.data(), data.data(), data.size());
    commit(data.size());
    return true;
  }

  // The next complete packet, valid until the next call to any method.
  std::optional<Packet> next() noexcept {
    if (end_ - begin_ < 2) return std::nullopt;
    const std::size_t len = load_be16(buf_.data() + begin_);
    if (LAB_UNLIKELY(len == 0)) {  // no type byte: malformed
      bad_ = true;
      return std::nullopt;
    }
    if (end_ - begin_ < 2 + len) return std::nullopt;
    const std::byte* p = buf_.data() + begin_;
    begin_ += 2 + len;
    return Packet{static_cast<char>(p[2]), {p + kHeaderSize, len - 1}};
  }

  bool malformed() const noexcept { return bad_; }
  std::size_t buffered() const noexcept { return end_ - begin_; }

 private:
  void compact() noexcept {
    if (begin_ == 0) return;
    std::memmove(buf_.data(), buf_.data() + begin_, end_ - begin_);
    end_ -= begin_;
    begin_ = 0;
  }

  std::vector<std::byte> buf_;
  std::size_t begin_ = 0;
  std::size_t end_ = 0;
  bool bad_ = false;
};

}  // namespace lab::soup
