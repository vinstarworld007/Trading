#pragma once

// ICF: the lab's look-alike of a consolidated vendor feed in the style of the
// ICE Consolidated Feed (docs/specs/vendor-icf.md). A lab protocol: ICE's own
// wire format (CTF) is licensed, so only the model is copied: text commands
// (SUBSCRIBE,SYMBOL_TICKER:AAPL), a source id per venue, and market data as
// numbered tokens. Carried over SoupBinTCP: commands in Unsequenced Data,
// responses and market data in Sequenced Data.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lab/core/endian.hpp"
#include "lab/vendor/consolidator.hpp"

namespace lab::vendor::icf {

inline constexpr char kMarketData = 'M';
inline constexpr char kResponse = 'R';

// Event types of a market data message.
inline constexpr char kImage = 'I';
inline constexpr char kQuote = 'Q';
inline constexpr char kTrade = 'T';
inline constexpr char kStatus = 'S';

// Token value types.
inline constexpr char kPrice = 'P';    // signed, 4 implied decimals
inline constexpr char kInteger = 'I';  // signed
inline constexpr char kTime = 'T';     // ns since midnight
inline constexpr char kChar = 'C';     // ASCII in the low byte
inline constexpr char kAlpha = 'A';    // up to 8 ASCII bytes, left-aligned, space padded

namespace token {
inline constexpr std::uint16_t kBidPrice = 1;
inline constexpr std::uint16_t kBidSize = 2;
inline constexpr std::uint16_t kAskPrice = 3;
inline constexpr std::uint16_t kAskSize = 4;
inline constexpr std::uint16_t kQuoteTime = 5;
inline constexpr std::uint16_t kTradePrice = 10;
inline constexpr std::uint16_t kTradeSize = 11;
inline constexpr std::uint16_t kTradeTime = 12;
inline constexpr std::uint16_t kVolume = 20;
inline constexpr std::uint16_t kOpen = 21;
inline constexpr std::uint16_t kHigh = 22;
inline constexpr std::uint16_t kLow = 23;
inline constexpr std::uint16_t kTrades = 24;
inline constexpr std::uint16_t kTradingStatus = 30;
inline constexpr std::uint16_t kHaltReason = 31;
inline constexpr std::uint16_t kRegSho = 32;
inline constexpr std::uint16_t kAuctionType = 40;
inline constexpr std::uint16_t kIndicativePrice = 41;
inline constexpr std::uint16_t kPairedShares = 42;
inline constexpr std::uint16_t kImbalanceShares = 43;
inline constexpr std::uint16_t kImbalanceSide = 44;
inline constexpr std::uint16_t kLuldLower = 50;
inline constexpr std::uint16_t kLuldUpper = 51;
}  // namespace token

inline const char* token_name(std::uint16_t t) noexcept {
  switch (t) {
    case token::kBidPrice:
      return "QUOTE.BID.PRICE";
    case token::kBidSize:
      return "QUOTE.BID.SIZE";
    case token::kAskPrice:
      return "QUOTE.ASK.PRICE";
    case token::kAskSize:
      return "QUOTE.ASK.SIZE";
    case token::kQuoteTime:
      return "QUOTE.DATETIME";
    case token::kTradePrice:
      return "TRADE.PRICE";
    case token::kTradeSize:
      return "TRADE.SIZE";
    case token::kTradeTime:
      return "TRADE.DATETIME";
    case token::kVolume:
      return "DAY.VOLUME";
    case token::kOpen:
      return "DAY.OPEN";
    case token::kHigh:
      return "DAY.HIGH";
    case token::kLow:
      return "DAY.LOW";
    case token::kTrades:
      return "DAY.TRADES";
    case token::kTradingStatus:
      return "STATUS.TRADING";
    case token::kHaltReason:
      return "STATUS.HALT_REASON";
    case token::kRegSho:
      return "STATUS.SHORT_SALE_RESTRICTION";
    case token::kAuctionType:
      return "AUCTION.TYPE";
    case token::kIndicativePrice:
      return "AUCTION.INDICATIVE.PRICE";
    case token::kPairedShares:
      return "AUCTION.PAIRED.SIZE";
    case token::kImbalanceShares:
      return "AUCTION.IMBALANCE.SIZE";
    case token::kImbalanceSide:
      return "AUCTION.IMBALANCE.SIDE";
    case token::kLuldLower:
      return "LULD.LOWER.PRICE";
    case token::kLuldUpper:
      return "LULD.UPPER.PRICE";
  }
  return "?";
}

// Market data message header (docs/specs/vendor-icf.md).
inline constexpr std::size_t kHeaderSize = 37;
inline constexpr std::size_t kTokenSize = 11;  // token 2, type 1, value 8
inline constexpr std::size_t kMaxTokens = 24;
inline constexpr std::size_t kMaxMessage = kHeaderSize + kMaxTokens * kTokenSize;

struct Token {
  std::uint16_t id;
  char type;
  std::int64_t value;
};

struct MarketData {
  char event = kImage;
  std::uint16_t source = 0;
  std::uint32_t request_id = 0;
  std::array<char, 8> symbol{};
  std::uint64_t exch_ns = 0;
  std::uint64_t vendor_ns = 0;
  std::uint32_t record_seq = 0;
  std::uint8_t count = 0;
  std::array<Token, kMaxTokens> tokens{};
};

// An alpha token's value: up to 8 ASCII bytes, first byte most significant.
template <std::size_t N>
inline std::int64_t alpha(const std::array<char, N>& a) noexcept {
  static_assert(N <= 8);
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | static_cast<unsigned char>(i < N ? a[i] : ' ');
  return static_cast<std::int64_t>(v);
}
inline std::array<char, 8> alpha_chars(std::int64_t value) noexcept {
  std::array<char, 8> a{};
  const auto v = static_cast<std::uint64_t>(value);
  for (std::size_t i = 0; i < 8; ++i) a[i] = static_cast<char>(v >> (56 - 8 * i));
  return a;
}

// Encodes the tokens in `mask` from `r`. Returns the message length.
inline std::size_t encode(const MarketData& m, std::byte* p) noexcept {
  p[0] = static_cast<std::byte>(kMarketData);
  p[1] = static_cast<std::byte>(m.event);
  store_be16(p + 2, m.source);
  store_be32(p + 4, m.request_id);
  std::memcpy(p + 8, m.symbol.data(), 8);
  store_be64(p + 16, m.exch_ns);
  store_be64(p + 24, m.vendor_ns);
  store_be32(p + 32, m.record_seq);
  p[36] = static_cast<std::byte>(m.count);
  std::byte* t = p + kHeaderSize;
  for (std::uint8_t i = 0; i < m.count; ++i, t += kTokenSize) {
    store_be16(t, m.tokens[i].id);
    t[2] = static_cast<std::byte>(m.tokens[i].type);
    store_be64(t + 3, static_cast<std::uint64_t>(m.tokens[i].value));
  }
  return kHeaderSize + m.count * kTokenSize;
}

inline std::optional<MarketData> decode(std::span<const std::byte> b) noexcept {
  if (b.size() < kHeaderSize || static_cast<char>(b[0]) != kMarketData) return std::nullopt;
  MarketData m;
  const std::byte* p = b.data();
  m.event = static_cast<char>(p[1]);
  m.source = load_be16(p + 2);
  m.request_id = load_be32(p + 4);
  std::memcpy(m.symbol.data(), p + 8, 8);
  m.exch_ns = load_be64(p + 16);
  m.vendor_ns = load_be64(p + 24);
  m.record_seq = load_be32(p + 32);
  m.count = static_cast<std::uint8_t>(p[36]);
  if (m.count > kMaxTokens || b.size() < kHeaderSize + m.count * kTokenSize) return std::nullopt;
  const std::byte* t = p + kHeaderSize;
  for (std::uint8_t i = 0; i < m.count; ++i, t += kTokenSize) {
    m.tokens[i] = {load_be16(t), static_cast<char>(t[2]),
                   static_cast<std::int64_t>(load_be64(t + 3))};
  }
  return m;
}

// The tokens a change mask maps to, in token order.
inline void fill(MarketData& m, const Record& r, std::uint32_t mask) noexcept {
  m.count = 0;
  auto add = [&m](std::uint16_t id, char type, std::int64_t v) {
    m.tokens[m.count++] = {id, type, v};
  };
  if (mask & field::kBid) add(token::kBidPrice, kPrice, r.bid);
  if (mask & field::kBidSize) add(token::kBidSize, kInteger, static_cast<std::int64_t>(r.bid_size));
  if (mask & field::kAsk) add(token::kAskPrice, kPrice, r.ask);
  if (mask & field::kAskSize) add(token::kAskSize, kInteger, static_cast<std::int64_t>(r.ask_size));
  if (mask & field::kQuote) add(token::kQuoteTime, kTime, static_cast<std::int64_t>(r.quote_ns));
  if (mask & field::kLast) add(token::kTradePrice, kPrice, r.last);
  if (mask & field::kLastSize) {
    add(token::kTradeSize, kInteger, static_cast<std::int64_t>(r.last_size));
    add(token::kTradeTime, kTime, static_cast<std::int64_t>(r.trade_ns));
  }
  if (mask & field::kVolume) add(token::kVolume, kInteger, static_cast<std::int64_t>(r.volume));
  if (mask & field::kOpen) add(token::kOpen, kPrice, r.open);
  if (mask & field::kHigh) add(token::kHigh, kPrice, r.high);
  if (mask & field::kLow) add(token::kLow, kPrice, r.low);
  if (mask & field::kTrades) add(token::kTrades, kInteger, static_cast<std::int64_t>(r.trades));
  if (mask & field::kStatus) {
    add(token::kTradingStatus, kChar, r.status);
    add(token::kHaltReason, kAlpha, alpha(r.halt_reason));
    add(token::kRegSho, kChar, r.reg_sho);
  }
  if (mask & field::kAuction) {
    add(token::kAuctionType, kChar, r.auction_type);
    add(token::kIndicativePrice, kPrice, r.indicative_price);
    add(token::kPairedShares, kInteger, static_cast<std::int64_t>(r.paired_shares));
    add(token::kImbalanceShares, kInteger, static_cast<std::int64_t>(r.imbalance_shares));
    add(token::kImbalanceSide, kChar, r.imbalance_side);
  }
  if (mask & field::kBand) {
    add(token::kLuldLower, kPrice, r.band_lower);
    add(token::kLuldUpper, kPrice, r.band_upper);
  }
}

// Command response.
inline std::size_t encode_response(std::byte* p, std::uint32_t request_id, bool ok,
                                   std::string_view text) noexcept {
  const std::size_t n = std::min<std::size_t>(text.size(), 512);
  p[0] = static_cast<std::byte>(kResponse);
  store_be32(p + 1, request_id);
  p[5] = static_cast<std::byte>(ok ? 'A' : 'E');
  store_be16(p + 6, static_cast<std::uint16_t>(n));
  std::memcpy(p + 8, text.data(), n);
  return 8 + n;
}

struct Response {
  std::uint32_t request_id;
  bool ok;
  std::string_view text;
};

inline std::optional<Response> decode_response(std::span<const std::byte> b) noexcept {
  if (b.size() < 8 || static_cast<char>(b[0]) != kResponse) return std::nullopt;
  const std::size_t n = load_be16(b.data() + 6);
  if (b.size() < 8 + n) return std::nullopt;
  return Response{load_be32(b.data() + 1),
                  static_cast<char>(b[5]) == 'A',
                  {reinterpret_cast<const char*>(b.data() + 8), n}};
}

// One command: VERB,PARM:VAL,PARM:VAL.
struct Command {
  std::string verb;
  std::vector<std::pair<std::string, std::string>> params;

  std::string_view get(std::string_view name) const noexcept {
    for (const auto& [k, v] : params) {
      if (k == name) return v;
    }
    return {};
  }
};

// Splits "SUBSCRIBE,SYMBOL_TICKER:AAPL;SUBSCRIBE,SYMBOL_TICKER:MSFT" into
// commands. Whitespace around separators is ignored.
inline std::vector<Command> parse_commands(std::string_view text) {
  auto trim = [](std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r'))
      s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r'))
      s.remove_suffix(1);
    return s;
  };
  std::vector<Command> out;
  while (!text.empty()) {
    const auto semi = text.find(';');
    std::string_view one = trim(text.substr(0, semi));
    text = semi == std::string_view::npos ? std::string_view{} : text.substr(semi + 1);
    if (one.empty()) continue;
    Command c;
    bool first = true;
    while (!one.empty()) {
      // A regex value in braces may contain commas.
      std::size_t comma = std::string_view::npos;
      int depth = 0;
      for (std::size_t i = 0; i < one.size(); ++i) {
        if (one[i] == '{') ++depth;
        if (one[i] == '}') --depth;
        if (one[i] == ',' && depth == 0) {
          comma = i;
          break;
        }
      }
      const std::string_view part = trim(one.substr(0, comma));
      one = comma == std::string_view::npos ? std::string_view{} : one.substr(comma + 1);
      if (first) {
        c.verb = part;
        first = false;
        continue;
      }
      const auto colon = part.find(':');
      if (colon == std::string_view::npos) {
        c.params.emplace_back(std::string{part}, "");
      } else {
        std::string_view v = part.substr(colon + 1);
        if (v.size() >= 2 && v.front() == '{' && v.back() == '}') v = v.substr(1, v.size() - 2);
        c.params.emplace_back(std::string{part.substr(0, colon)}, std::string{v});
      }
    }
    out.push_back(std::move(c));
  }
  return out;
}

}  // namespace lab::vendor::icf
