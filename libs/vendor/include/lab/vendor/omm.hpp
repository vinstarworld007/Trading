#pragma once

// OMM JSON messages for the Reuters-style vendor (docs/specs/vendor-omm-json.md):
// the Refinitiv Real-Time WebSocket API, subprotocol tr_json2. Message and
// attribute names follow LSEG's open-source Real-Time SDK JSON converter
// (Cpp-C/Eta/Impl/Converter, Include/rtr/jsonSimpleDefs.h); field names and
// IDs follow its RDMFieldDictionary, enumerations its enumtype.def.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lab/vendor/consolidator.hpp"
#include "lab/vendor/json.hpp"

namespace lab::vendor::omm {

inline constexpr std::string_view kSubprotocol = "tr_json2";
inline constexpr std::string_view kPath = "/WebSocket";

enum class Kind : std::uint8_t { kAscii, kPrice, kInteger, kTime, kEnum };

// One RDMFieldDictionary entry the vendor publishes.
struct Field {
  std::string_view name;
  int fid;
  Kind kind;
  std::uint32_t mask;  // Record fields it depends on
};

// TRD_STATUS (FID 6614) values from enumtype.def.
namespace trd_status {
inline constexpr int kNormal = 1;
inline constexpr int kHalted = 2;
inline constexpr int kSuspended = 3;
}  // namespace trd_status

inline constexpr Field kFields[] = {
    {"DSPLY_NAME", 3, Kind::kAscii, 0},
    {"TRDPRC_1", 6, Kind::kPrice, field::kLast},
    {"HIGH_1", 12, Kind::kPrice, field::kHigh},
    {"LOW_1", 13, Kind::kPrice, field::kLow},
    {"OPEN_PRC", 19, Kind::kPrice, field::kOpen},
    {"BID", 22, Kind::kPrice, field::kBid},
    {"ASK", 25, Kind::kPrice, field::kAsk},
    {"BIDSIZE", 30, Kind::kInteger, field::kBidSize},
    {"ASKSIZE", 31, Kind::kInteger, field::kAskSize},
    {"ACVOL_1", 32, Kind::kInteger, field::kVolume},
    {"UPLIMIT", 75, Kind::kPrice, field::kBand},
    {"LOLIMIT", 76, Kind::kPrice, field::kBand},
    {"NUM_MOVES", 77, Kind::kInteger, field::kTrades},
    {"TRDVOL_1", 178, Kind::kInteger, field::kLastSize},
    {"IMB_ACT_TP", 4333, Kind::kEnum, field::kAuction},
    {"IMB_PR_NR", 4335, Kind::kPrice, field::kAuction},
    {"IMB_PR_SH", 4337, Kind::kInteger, field::kAuction},
    {"IMB_SH", 4338, Kind::kInteger, field::kAuction},
    {"IMB_SIDE", 4340, Kind::kEnum, field::kAuction},
    {"HALT_REASN", 6517, Kind::kAscii, field::kStatus},
    {"SH_SAL_RES", 6577, Kind::kEnum, field::kStatus},
    {"TRD_STATUS", 6614, Kind::kEnum, field::kStatus},
    {"QUOTIM_NS", 14265, Kind::kTime, field::kQuote},
    {"SALTIM_NS", 14266, Kind::kTime, field::kLastSize},
};
inline constexpr std::size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
static_assert(kFieldCount <= 32);

inline int field_index(std::string_view name) noexcept {
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    if (kFields[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

// RIC for a Nasdaq-listed symbol: AAPL -> AAPL.O.
inline std::string ric(std::string_view symbol) { return std::string{symbol} + ".O"; }
inline std::string_view symbol_of(std::string_view ric) {
  const auto dot = ric.rfind('.');
  return dot == std::string_view::npos ? ric : ric.substr(0, dot);
}

// IMB_ACT_TP (FID 4333) from the ITCH NOII cross type: 1 Open, 3 Halt,
// 4 Close; 0 Not set when no auction is running.
inline int imb_act_tp_of(char cross_type) noexcept {
  switch (cross_type) {
    case 'O':
      return 1;
    case 'H':
      return 3;
    case 'C':
    case 'A':  // extended trading close
      return 4;
  }
  return 0;
}

// IMB_SIDE (FID 4340) from the ITCH NOII imbalance direction: 1 N, 2 B,
// 3 S, 4 O insufficient orders, 5 P paused; 0 Not Set.
inline int imb_side_of(char direction) noexcept {
  switch (direction) {
    case 'N':
      return 1;
    case 'B':
      return 2;
    case 'S':
      return 3;
    case 'O':
      return 4;
    case 'P':
      return 5;
  }
  return 0;
}

// SH_SAL_RES (FID 6577) from the ITCH Reg SHO action: 0 -> 1 not
// restricted, 1 -> 4 restricted, 2 -> 3 restriction continued.
inline int sh_sal_res_of(char action) noexcept {
  switch (action) {
    case '1':
      return 4;
    case '2':
      return 3;
  }
  return 1;
}

inline int trd_status_of(char s) noexcept {
  switch (s) {
    case 'T':
      return trd_status::kNormal;
    case 'P':
      return trd_status::kSuspended;
    default:
      return trd_status::kHalted;  // H halted, Q quotation only
  }
}

// Appends "Fields":{...} for the fields whose mask intersects `mask`, limited
// to `view` (a bit per kFields entry; all ones for no view).
inline void fields(std::string& out, const Record& r, std::uint32_t mask, std::uint32_t view,
                   bool image) {
  out += "\"Fields\":{";
  bool first = true;
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    const Field& f = kFields[i];
    if (!(view & (1u << i))) continue;
    if (!image && !(f.mask & mask)) continue;
    if (!first) out += ',';
    first = false;
    json::quote(out, f.name);
    out += ':';
    auto price = [&](std::uint32_t p) {
      if (p == 0) {
        out += "null";
      } else {
        json::price(out, p);
      }
    };
    switch (f.fid) {
      case 3:
        json::quote(out, itch::trimmed(r.symbol));
        break;
      case 6:
        price(r.last);
        break;
      case 12:
        price(r.high);
        break;
      case 13:
        price(r.low);
        break;
      case 19:
        price(r.open);
        break;
      case 22:
        price(r.bid);
        break;
      case 25:
        price(r.ask);
        break;
      case 30:
        out += std::to_string(r.bid_size);
        break;
      case 31:
        out += std::to_string(r.ask_size);
        break;
      case 32:
        out += std::to_string(r.volume);
        break;
      case 77:
        out += std::to_string(r.trades);
        break;
      case 178:
        if (r.trades == 0) {
          out += "null";
        } else {
          out += std::to_string(r.last_size);
        }
        break;
      case 75:
        price(r.band_upper);
        break;
      case 76:
        price(r.band_lower);
        break;
      case 4333:
        out += std::to_string(imb_act_tp_of(r.auction_type));
        break;
      case 4335:
        price(r.indicative_price);
        break;
      case 4337:
      case 4338:
        if (r.auction_type == ' ') {
          out += "null";
        } else {
          out += std::to_string(f.fid == 4337 ? r.paired_shares : r.imbalance_shares);
        }
        break;
      case 4340:
        out += std::to_string(imb_side_of(r.imbalance_side));
        break;
      case 6517: {
        std::string_view reason{r.halt_reason.data(), r.halt_reason.size()};
        while (!reason.empty() && reason.back() == ' ') reason.remove_suffix(1);
        json::quote(out, reason);
        break;
      }
      case 6577:
        out += std::to_string(sh_sal_res_of(r.reg_sho));
        break;
      case 6614:
        out += std::to_string(trd_status_of(r.status));
        break;
      case 14265:
        if (r.quote_ns == 0) {
          out += "null";
        } else {
          json::time_ns(out, r.quote_ns);
        }
        break;
      case 14266:
        if (r.trade_ns == 0) {
          out += "null";
        } else {
          json::time_ns(out, r.trade_ns);
        }
        break;
    }
  }
  out += '}';
}

inline void login_refresh(std::string& out, std::int64_t id, std::string_view user) {
  out += "{\"ID\":" + std::to_string(id) +
         ",\"Type\":\"Refresh\",\"Domain\":\"Login\",\"Key\":{\"Name\":";
  json::quote(out, user);
  out +=
      ",\"Elements\":{\"ApplicationId\":\"256\",\"ApplicationName\":\"LAB-RT\","
      "\"SupportBatchRequests\":1,\"SupportViewRequests\":1}},"
      "\"State\":{\"Stream\":\"Open\",\"Data\":\"Ok\",\"Text\":\"Login accepted by host lab.\"},"
      "\"Elements\":{\"MaxMsgSize\":61440,\"PingTimeout\":30}}";
}

inline void refresh(std::string& out, std::int64_t id, std::string_view service, const Record& r,
                    std::uint32_t view, bool streaming, bool conflated) {
  out += "{\"ID\":" + std::to_string(id) + ",\"Type\":\"Refresh\",\"Key\":{\"Service\":";
  json::quote(out, service);
  out += ",\"Name\":";
  json::quote(out, ric(itch::trimmed(r.symbol)));
  out += "},\"State\":{\"Stream\":\"";
  out += streaming ? "Open" : "NonStreaming";
  out +=
      "\",\"Data\":\"Ok\",\"Text\":\"All is well\"},\"Qos\":{\"Timeliness\":\"Realtime\","
      "\"Rate\":\"";
  out += conflated ? "JustInTimeConflated" : "TickByTick";
  out += "\"},\"SeqNumber\":" + std::to_string(r.updates) + ",";
  fields(out, r, field::kAll, view, true);
  out += '}';
}

inline void update(std::string& out, std::int64_t id, const Record& r, std::uint32_t mask,
                   std::uint32_t view) {
  out += "{\"ID\":" + std::to_string(id) + ",\"Type\":\"Update\",\"UpdateType\":\"";
  switch (kind_of(mask)) {
    case vendor::Kind::kTrade:
      out += "Trade";
      break;
    case vendor::Kind::kQuote:
      out += "Quote";
      break;
    case vendor::Kind::kStatus:
      out += "Unspecified";
      break;
  }
  out += "\",\"SeqNumber\":" + std::to_string(r.updates) + ",";
  fields(out, r, mask, view, false);
  out += '}';
}

inline void status(std::string& out, std::int64_t id, std::string_view stream,
                   std::string_view data, std::string_view code, std::string_view text) {
  out += "{\"ID\":" + std::to_string(id) + ",\"Type\":\"Status\",\"State\":{\"Stream\":\"";
  out += stream;
  out += "\",\"Data\":\"";
  out += data;
  out += '"';
  if (!code.empty()) {
    out += ",\"Code\":\"";
    out += code;
    out += '"';
  }
  out += ",\"Text\":";
  json::quote(out, text);
  out += "}}";
}

}  // namespace lab::vendor::omm
