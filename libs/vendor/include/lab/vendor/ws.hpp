#pragma once

// The WebSocket protocol (RFC 6455) as far as the OMM JSON vendor needs it:
// the HTTP Upgrade handshake on both sides (with SHA-1 from RFC 3174 and
// base64 for Sec-WebSocket-Accept), frame encoding with and without a mask,
// and a stream decoder that reassembles fragmented messages and surfaces
// control frames. Control path code: it allocates.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lab::vendor::ws {

// ---------------------------------------------------------------- SHA-1 (RFC 3174)

inline std::array<std::uint8_t, 20> sha1(std::string_view data) {
  std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::string msg{data};
  const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back('\0');
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
  auto rol = [](std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  for (std::size_t off = 0; off < msg.size(); off += 64) {
    std::uint32_t w[80];
    for (int t = 0; t < 16; ++t) {
      const auto* b = reinterpret_cast<const unsigned char*>(msg.data() + off + 4 * t);
      w[t] = (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) |
             (std::uint32_t{b[2]} << 8) | std::uint32_t{b[3]};
    }
    for (int t = 16; t < 80; ++t) w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; ++t) {
      std::uint32_t f, k;
      if (t < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (t < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (t < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const std::uint32_t tmp = rol(a, 5) + f + e + k + w[t];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  std::array<std::uint8_t, 20> out{};
  for (int i = 0; i < 5; ++i) {
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
  }
  return out;
}

inline std::string base64(std::span<const std::uint8_t> in) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t i = 0;
  for (; i + 3 <= in.size(); i += 3) {
    const std::uint32_t v =
        (std::uint32_t{in[i]} << 16) | (std::uint32_t{in[i + 1]} << 8) | in[i + 2];
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  if (i + 1 == in.size()) {
    const std::uint32_t v = std::uint32_t{in[i]} << 16;
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    const std::uint32_t v = (std::uint32_t{in[i]} << 16) | (std::uint32_t{in[i + 1]} << 8);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

// Sec-WebSocket-Accept for a client's Sec-WebSocket-Key (RFC 6455 4.2.2).
inline std::string accept_key(std::string_view client_key) {
  const auto digest = sha1(std::string{client_key} + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  return base64(digest);
}

// ---------------------------------------------------------------- handshake

// Header value from an HTTP request or response head (case-insensitive name).
inline std::string_view header(std::string_view head, std::string_view name) {
  std::size_t pos = head.find("\r\n");
  while (pos != std::string_view::npos && pos + 2 < head.size()) {
    const std::size_t start = pos + 2;
    const std::size_t end = head.find("\r\n", start);
    const std::string_view line = head.substr(start, end - start);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos && colon == name.size()) {
      bool same = true;
      for (std::size_t i = 0; i < colon; ++i) {
        const char a = static_cast<char>(line[i] | 0x20), b = static_cast<char>(name[i] | 0x20);
        if (a != b) same = false;
      }
      if (same) {
        std::string_view v = line.substr(colon + 1);
        while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
        while (!v.empty() && v.back() == ' ') v.remove_suffix(1);
        return v;
      }
    }
    pos = end;
  }
  return {};
}

// The server's 101 response to a client's request head, or an empty string if
// the request is not a WebSocket upgrade offering `subprotocol`.
inline std::string server_handshake(std::string_view request, std::string_view subprotocol) {
  const std::string_view key = header(request, "Sec-WebSocket-Key");
  const std::string_view protocols = header(request, "Sec-WebSocket-Protocol");
  if (key.empty() || request.substr(0, 4) != "GET " ||
      protocols.find(subprotocol) == std::string_view::npos) {
    return {};
  }
  return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Accept: " +
         accept_key(key) + "\r\nSec-WebSocket-Protocol: " + std::string{subprotocol} + "\r\n\r\n";
}

struct ClientHandshake {
  std::string request;
  std::string expected_accept;
};

inline ClientHandshake client_handshake(std::string_view host, std::string_view path,
                                        std::string_view subprotocol) {
  std::random_device rd;
  std::array<std::uint8_t, 16> nonce{};
  for (auto& b : nonce) b = static_cast<std::uint8_t>(rd());
  const std::string key = base64(nonce);
  ClientHandshake h;
  h.request =
      "GET " + std::string{path} + " HTTP/1.1\r\nHost: " + std::string{host} +
      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
      "\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: " + std::string{subprotocol} +
      "\r\n\r\n";
  h.expected_accept = accept_key(key);
  return h;
}

// ---------------------------------------------------------------- frames

enum Opcode : std::uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

// One frame with FIN set. Clients must mask (RFC 6455 5.3); servers must not.
inline void append_frame(std::string& out, Opcode op, std::string_view payload, bool mask) {
  out.push_back(static_cast<char>(0x80 | op));
  const std::uint8_t mbit = mask ? 0x80 : 0;
  const std::uint64_t n = payload.size();
  if (n < 126) {
    out.push_back(static_cast<char>(mbit | n));
  } else if (n <= 0xFFFF) {
    out.push_back(static_cast<char>(mbit | 126));
    out.push_back(static_cast<char>(n >> 8));
    out.push_back(static_cast<char>(n & 0xFF));
  } else {
    out.push_back(static_cast<char>(mbit | 127));
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
  }
  if (!mask) {
    out.append(payload);
    return;
  }
  static thread_local std::mt19937 rng{std::random_device{}()};
  const std::uint32_t k = rng();
  const char key[4] = {static_cast<char>(k >> 24), static_cast<char>(k >> 16),
                       static_cast<char>(k >> 8), static_cast<char>(k)};
  out.append(key, 4);
  const std::size_t base = out.size();
  out.append(payload);
  for (std::size_t i = 0; i < payload.size(); ++i) out[base + i] ^= key[i & 3];
}

struct Message {
  Opcode opcode;        // kText, kBinary, kClose, kPing or kPong
  std::string payload;  // reassembled and unmasked
};

// Cuts messages out of a byte stream. Data messages may span several frames;
// control frames arrive whole, possibly between fragments.
class Decoder {
 public:
  void feed(std::span<const std::byte> data) {
    buf_.append(reinterpret_cast<const char*>(data.data()), data.size());
  }
  void feed(std::string_view data) { buf_.append(data); }

  std::optional<Message> next() {
    for (;;) {
      if (buf_.size() - pos_ < 2) return compact();
      const auto* p = reinterpret_cast<const unsigned char*>(buf_.data() + pos_);
      const bool fin = (p[0] & 0x80) != 0;
      const auto op = static_cast<Opcode>(p[0] & 0x0F);
      const bool masked = (p[1] & 0x80) != 0;
      std::uint64_t n = p[1] & 0x7F;
      std::size_t h = 2;
      if (n == 126) {
        if (buf_.size() - pos_ < 4) return compact();
        n = (std::uint64_t{p[2]} << 8) | p[3];
        h = 4;
      } else if (n == 127) {
        if (buf_.size() - pos_ < 10) return compact();
        n = 0;
        for (int i = 0; i < 8; ++i) n = (n << 8) | p[2 + i];
        h = 10;
      }
      const std::size_t mk = masked ? 4 : 0;
      if (n > (64u << 20)) {  // a peer sending this is broken
        bad_ = true;
        return std::nullopt;
      }
      if (buf_.size() - pos_ < h + mk + n) return compact();
      std::string payload = buf_.substr(pos_ + h + mk, n);
      if (masked) {
        for (std::size_t i = 0; i < n; ++i) payload[i] ^= static_cast<char>(p[h + (i & 3)]);
      }
      pos_ += h + mk + n;
      if (op >= kClose) return Message{op, std::move(payload)};
      if (op != kContinuation) partial_op_ = op;
      partial_ += payload;
      if (fin) {
        Message m{partial_op_, std::move(partial_)};
        partial_.clear();
        return m;
      }
    }
  }

  bool malformed() const noexcept { return bad_; }

 private:
  std::optional<Message> compact() {
    if (pos_ > 0) {
      buf_.erase(0, pos_);
      pos_ = 0;
    }
    return std::nullopt;
  }

  std::string buf_;
  std::size_t pos_ = 0;
  std::string partial_;
  Opcode partial_op_ = kText;
  bool bad_ = false;
};

}  // namespace lab::vendor::ws
