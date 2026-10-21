#pragma once

// MoldUDP64 packet codec, from docs/specs/moldudp64-layout.md.
//
// A downstream packet is a 20-byte header (session, first sequence number,
// message count) followed by length-prefixed message blocks. A count of 0 is a
// heartbeat and 0xFFFF is End of Session. Requests for retransmission are a
// bare 20-byte header sent to the re-request server.
//
//   lab::mold::PacketBuilder<> b;
//   b.begin(session, next_seq);
//   while (b.add(msg)) {...}        // false when the next message does not fit
//   send(b.data(), b.size());
//
//   lab::mold::Packet p;
//   if (lab::mold::parse(buf, len, p)) p.for_each([](std::uint64_t seq, auto msg) {...});

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "lab/core/compiler.hpp"
#include "lab/core/endian.hpp"

namespace lab::mold {

inline constexpr std::size_t kSessionSize = 10;
inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::size_t kRequestSize = 20;
inline constexpr std::size_t kBlockHeaderSize = 2;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;
// Largest count a data packet may carry (0xFFFF means End of Session).
inline constexpr std::uint16_t kMaxMessageCount = 0xFFFE;
// Header plus blocks within a 1500-byte MTU, leaving room for IP and UDP headers.
inline constexpr std::size_t kDefaultMaxPayload = 1400;
// Largest UDP payload over IPv4.
inline constexpr std::size_t kMaxDatagram = 65507;

// Alphanumeric, left-aligned, space-padded.
using Session = std::array<char, kSessionSize>;

inline Session make_session(std::string_view name) noexcept {
  Session s;
  s.fill(' ');
  std::memcpy(s.data(), name.data(), std::min(name.size(), s.size()));
  return s;
}

inline std::string_view trimmed(const Session& s) noexcept {
  std::size_t n = s.size();
  while (n > 0 && s[n - 1] == ' ') --n;
  return {s.data(), n};
}

struct Header {
  Session session{};
  std::uint64_t sequence = 0;  // first message in the packet, or next expected
  std::uint16_t count = 0;

  bool heartbeat() const noexcept { return count == 0; }
  bool end_of_session() const noexcept { return count == kEndOfSession; }
  // Number of messages carried: 0 for heartbeats and End of Session.
  std::uint16_t messages() const noexcept { return end_of_session() ? 0 : count; }
};

LAB_ALWAYS_INLINE void encode_header(std::byte* out, const Header& h) noexcept {
  std::memcpy(out, h.session.data(), kSessionSize);
  store_be64(out + 10, h.sequence);
  store_be16(out + 18, h.count);
}

LAB_ALWAYS_INLINE void decode_header(const std::byte* in, Header& h) noexcept {
  std::memcpy(h.session.data(), in, kSessionSize);
  h.sequence = load_be64(in + 10);
  h.count = load_be16(in + 18);
}

// A request has the same layout as a header; count is the number of messages wanted.
using Request = Header;

inline void encode_request(std::byte* out, const Request& r) noexcept { encode_header(out, r); }

inline bool decode_request(const std::byte* in, std::size_t len, Request& r) noexcept {
  if (len < kRequestSize) return false;
  decode_header(in, r);
  return true;
}

// A received downstream packet. Blocks point into the caller's buffer.
struct Packet {
  Header header;
  const std::byte* blocks = nullptr;
  std::size_t blocks_len = 0;

  // Calls f(sequence, std::span<const std::byte>) for each message. Returns
  // false if the blocks are truncated, after delivering the complete ones.
  template <typename F>
  bool for_each(F&& f) const {
    std::size_t pos = 0;
    const std::uint16_t n = header.messages();
    for (std::uint16_t i = 0; i < n; ++i) {
      if (LAB_UNLIKELY(blocks_len - pos < kBlockHeaderSize)) return false;
      const std::size_t len = load_be16(blocks + pos);
      pos += kBlockHeaderSize;
      if (LAB_UNLIKELY(blocks_len - pos < len)) return false;
      f(header.sequence + i, std::span<const std::byte>{blocks + pos, len});
      pos += len;
    }
    return true;
  }
};

// Parses the header; blocks are checked lazily by Packet::for_each.
inline bool parse(const std::byte* data, std::size_t len, Packet& p) noexcept {
  if (LAB_UNLIKELY(len < kHeaderSize)) return false;
  decode_header(data, p.header);
  p.blocks = data + kHeaderSize;
  p.blocks_len = len - kHeaderSize;
  return true;
}

// Builds one downstream packet in a fixed inline buffer: no allocation.
template <std::size_t Capacity = kDefaultMaxPayload>
class PacketBuilder {
  static_assert(Capacity >= kHeaderSize + kBlockHeaderSize && Capacity <= kMaxDatagram);

 public:
  // `max_payload` lets one builder type serve several MTUs; it is clamped to Capacity.
  explicit PacketBuilder(std::size_t max_payload = Capacity) noexcept
      : max_{std::clamp(max_payload, kHeaderSize + kBlockHeaderSize, Capacity)} {}

  void begin(const Session& session, std::uint64_t first_sequence) noexcept {
    header_.session = session;
    header_.sequence = first_sequence;
    header_.count = 0;
    size_ = kHeaderSize;
  }

  // Appends one message. Returns false, leaving the packet unchanged, if it
  // does not fit or the packet already holds the maximum count.
  LAB_ALWAYS_INLINE bool add(std::span<const std::byte> msg) noexcept {
    const std::size_t need = kBlockHeaderSize + msg.size();
    if (LAB_UNLIKELY(need > max_ - size_ || header_.count == kMaxMessageCount ||
                     msg.size() > 0xFFFF)) {
      return false;
    }
    store_be16(buf_.data() + size_, static_cast<std::uint16_t>(msg.size()));
    std::memcpy(buf_.data() + size_ + kBlockHeaderSize, msg.data(), msg.size());
    size_ += need;
    ++header_.count;
    return true;
  }

  // Header-only packets. `next_sequence` is the next sequence number the
  // sender will use.
  void heartbeat(const Session& session, std::uint64_t next_sequence) noexcept {
    begin(session, next_sequence);
  }
  void end_of_session(const Session& session, std::uint64_t next_sequence) noexcept {
    begin(session, next_sequence);
    header_.count = kEndOfSession;
  }

  // Finished packet bytes; writes the header.
  std::span<const std::byte> finish() noexcept {
    encode_header(buf_.data(), header_);
    return {buf_.data(), size_};
  }

  const Header& header() const noexcept { return header_; }
  std::uint16_t count() const noexcept { return header_.messages(); }
  bool empty() const noexcept { return header_.messages() == 0; }
  std::size_t size() const noexcept { return size_; }
  std::size_t max_payload() const noexcept { return max_; }
  // Sequence number the next added message will get.
  std::uint64_t next_sequence() const noexcept { return header_.sequence + header_.messages(); }

 private:
  std::array<std::byte, Capacity> buf_{};
  Header header_{};
  std::size_t size_ = kHeaderSize;
  std::size_t max_;
};

}  // namespace lab::mold
