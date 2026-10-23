// One order's life through the lab, built with the lab's own codecs, book
// builder, strategy, risk, OMS and matching engine. Prints JSON: every wire
// message as bytes plus decoded fields, the state at each stage, and p50/p99
// cost of each in-process step on this machine.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lab/book/book_builder.hpp"
#include "lab/core/clock.hpp"
#include "lab/core/endian.hpp"
#include "lab/fix/codec.hpp"
#include "lab/itch/codec.hpp"
#include "lab/match/engine.hpp"
#include "lab/mold/mold.hpp"
#include "lab/oms/oms.hpp"
#include "lab/ouch/codec.hpp"
#include "lab/ouch/drop_copy.hpp"
#include "lab/risk/risk.hpp"
#include "lab/soup/soup.hpp"
#include "lab/strategy/market_maker.hpp"
#include "messages.hpp"  // exchange/simex

using namespace lab;

static std::string esc(std::string_view s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if (c == '\x01')
      o += "|";
    else if (static_cast<unsigned char>(c) < 32)
      o += ' ';
    else
      o += c;
  }
  return o;
}
static std::string hex(const std::byte* p, std::size_t n) {
  std::string s;
  char b[4];
  for (std::size_t i = 0; i < n; ++i) {
    std::snprintf(b, sizeof b, "%02x", static_cast<unsigned>(p[i]));
    s += b;
  }
  return s;
}
static std::string px(std::uint64_t p) {
  char b[32];
  std::snprintf(b, sizeof b, "%llu.%04llu", static_cast<unsigned long long>(p / 10000),
                static_cast<unsigned long long>(p % 10000));
  return b;
}

struct Field {
  std::size_t off, len;
  std::string name, value;
};

// Records every field a visit() walks, with a name from `names` in order.
template <class M>
std::vector<Field> fields_of(const M& m, std::vector<std::string> names, bool itch_header,
                             std::vector<std::string> price_fields = {}) {
  std::vector<Field> out;
  out.push_back({0, 1, "Message Type", std::string(1, M::kType)});
  if constexpr (requires { m.header; }) {
    if (itch_header) {
      out.push_back({1, 2, "Stock Locate", std::to_string(m.header.stock_locate)});
      out.push_back({3, 2, "Tracking Number", std::to_string(m.header.tracking_number)});
      out.push_back({5, 6, "Timestamp", std::to_string(m.header.timestamp) + " ns"});
    }
  }
  std::size_t i = 0;
  M::visit(m, [&](std::size_t off, const auto& f) {
    using T = std::remove_cvref_t<decltype(f)>;
    Field x{off, 0, i < names.size() ? names[i] : "?", ""};
    ++i;
    if constexpr (std::is_same_v<T, char>) {
      x.len = 1;
      x.value = std::string(1, f);
    } else if constexpr (std::is_same_v<T, std::uint16_t>) {
      x.len = 2;
      x.value = std::to_string(f);
    } else if constexpr (std::is_same_v<T, std::uint32_t>) {
      x.len = 4;
      x.value = std::to_string(f);
    } else if constexpr (std::is_same_v<T, std::uint64_t>) {
      x.len = 8;
      x.value = std::to_string(f);
    } else {
      x.len = f.size();
      x.value = "\"" + std::string(f.data(), f.size()) + "\"";
    }
    if (std::find(price_fields.begin(), price_fields.end(), x.name) != price_fields.end())
      x.value += "  ($" + px(std::stoull(x.value)) + ")";
    out.push_back(x);
  });
  return out;
}

struct Msg {
  std::string name, proto, link, hexs, text{};  // text: FIX tag=value
  std::vector<Field> fields{};
  std::string note{};
};

static std::string json(const Msg& m) {
  std::string s = "{\"name\":\"" + esc(m.name) + "\",\"proto\":\"" + esc(m.proto) +
                  "\",\"link\":\"" + esc(m.link) + "\",\"hex\":\"" + m.hexs + "\",\"size\":" +
                  std::to_string(m.hexs.size() / 2 ? m.hexs.size() / 2 : m.text.size()) +
                  ",\"text\":\"" + esc(m.text) + "\",\"note\":\"" + esc(m.note) + "\",\"fields\":[";
  for (std::size_t i = 0; i < m.fields.size(); ++i) {
    const auto& f = m.fields[i];
    s += (i ? "," : "") + std::string("[") + std::to_string(f.off) + "," + std::to_string(f.len) +
         ",\"" + esc(f.name) + "\",\"" + esc(f.value) + "\"]";
  }
  return s + "]}";
}

// ---------------------------------------------------------------- timing
static TscClock g_clock = TscClock::from_ratio(1);
struct Cost {
  double p50, p99;
};
template <class F>
Cost measure(F&& f, int n = 200000) {
  std::vector<std::uint64_t> t(n), base(n);
  for (int i = 0; i < n; ++i) {
    const auto a = rdtscp();
    asm volatile("" ::: "memory");
    base[i] = rdtscp() - a;
  }
  for (int i = 0; i < n; ++i) {
    const auto a = rdtscp();
    f(i);
    asm volatile("" ::: "memory");
    t[i] = rdtscp() - a;
  }
  std::sort(base.begin(), base.end());
  std::sort(t.begin(), t.end());
  const double ov = g_clock.to_ns(base[n / 2]);
  return {std::max(0.0, g_clock.to_ns(t[n / 2]) - ov),
          std::max(0.0, g_clock.to_ns(t[n * 99 / 100]) - ov)};
}
static std::string cost(const char* k, Cost c) {
  char b[128];
  std::snprintf(b, sizeof b, "\"%s\":[%.1f,%.1f]", k, c.p50, c.p99);
  return b;
}

// ---------------------------------------------------------------- engine listener
struct Ev {
  std::string what;
  match::OrderView a{}, b{};
  std::uint32_t q = 0, p = 0;
  std::uint64_t m = 0;
};
struct Listener {
  std::vector<Ev>* log;
  void on_accepted(const match::OrderView& o) {
    if (log) log->push_back({"accepted", o});
  }
  void on_rejected(match::OrderKey, match::Reject) {
    if (log) log->push_back({"rejected"});
  }
  void on_execution(const match::OrderView& r, const match::OrderView& a, std::uint32_t q,
                    std::uint32_t p, std::uint64_t m) {
    if (log) log->push_back({"execution", r, a, q, p, m});
  }
  void on_rested(const match::OrderView& o) {
    if (log) log->push_back({"rested", o});
  }
  void on_canceled(const match::OrderView& o, std::uint32_t, match::CancelReason) {
    if (log) log->push_back({"canceled", o});
  }
  void on_replaced(const match::OrderView&, const match::OrderView&) {}
};

int main() {
  g_clock = TscClock::calibrate();
  constexpr std::uint16_t kLocate = 14;
  constexpr std::uint16_t kFirm = 3, kFixb = 5;  // simex owner ids
  const auto aapl = itch::make_symbol("AAPL");
  std::uint64_t t_ns = 34'200'000'123'456ULL;  // 09:30:00.000123456

  std::vector<std::string> stages;
  auto stage = [&](std::string id, std::string where, std::string title, std::string body,
                   std::vector<Msg> msgs, std::string state, std::string timing) {
    std::string s = "{\"id\":\"" + id + "\",\"where\":\"" + esc(where) + "\",\"title\":\"" +
                    esc(title) + "\",\"body\":\"" + esc(body) +
                    "\",\"state\":" + (state.empty() ? "null" : state) +
                    ",\"timing\":" + (timing.empty() ? "null" : timing) + ",\"msgs\":[";
    for (std::size_t i = 0; i < msgs.size(); ++i) s += (i ? "," : "") + json(msgs[i]);
    stages.push_back(s + "]}");
  };

  // ---------------- simex: historical liquidity enters the engine, ITCH out over MoldUDP64
  match::EngineConfig ec;
  ec.max_orders = 1u << 16;
  ec.max_instruments = 64;
  match::Engine eng{ec};
  eng.add_instrument(kLocate);
  std::vector<Ev> evs;
  Listener L{&evs};
  struct Rest {
    char side;
    std::uint32_t qty, price;
  };
  const Rest hist[] = {
      {'B', 300, 1'895'000}, {'B', 500, 1'894'900}, {'S', 200, 1'895'300}, {'S', 400, 1'895'400}};
  std::vector<itch::AddOrder> adds;
  std::uint64_t hist_ref = 1000;
  for (const auto& h : hist) {
    match::NewOrder o{match::make_key(0, ++hist_ref),
                      kLocate,
                      h.side == 'B' ? match::Side::kBuy : match::Side::kSell,
                      match::Tif::kDay,
                      h.qty,
                      h.price};
    evs.clear();
    eng.add(o, L);
    itch::AddOrder a;
    a.header = {kLocate, 0, t_ns};
    t_ns += 1'837;
    a.order_reference_number = evs.back().a.orn;
    a.buy_sell_indicator = h.side;
    a.shares = h.qty;
    a.stock = aapl;
    a.price = h.price;
    adds.push_back(a);
  }
  // MoldUDP64 packet with the four Add Orders.
  std::byte pkt[512];
  mold::Header mh{mold::make_session("LAB0000001"), 4'812'331, 4};
  mold::encode_header(pkt, mh);
  std::size_t pos = mold::kHeaderSize;
  for (const auto& a : adds) {
    store_be16(pkt + pos, itch::AddOrder::kSize);
    itch::encode(a, pkt + pos + 2);
    pos += 2 + itch::AddOrder::kSize;
  }
  const std::vector<std::string> add_names = {"Order Reference Number", "Buy/Sell Indicator",
                                              "Shares", "Stock", "Price"};
  {
    Msg pk{"MoldUDP64 packet", "MoldUDP64", "UDP multicast 239.1.1.1:30001", hex(pkt, pos)};
    pk.fields = {{0, 10, "Session", "\"LAB0000001\""},
                 {10, 8, "Sequence Number", "4812331"},
                 {18, 2, "Message Count", "4"}};
    std::size_t o = mold::kHeaderSize;
    for (int i = 0; i < 4; ++i) {
      pk.fields.push_back({o, 2, "Message Length [" + std::to_string(i + 1) + "]", "36"});
      pk.fields.push_back({o + 2, 36, "ITCH Add Order [" + std::to_string(i + 1) + "]",
                           std::string(1, adds[i].buy_sell_indicator) + " " +
                               std::to_string(adds[i].shares) + " @ " + px(adds[i].price)});
      o += 38;
    }
    pk.note = "Header 20 bytes, then 4 blocks of 2 + 36. 172 bytes on the wire plus UDP/IP.";
    Msg first{"ITCH Add Order (first block)", "ITCH 5.0", "inside the MoldUDP64 block",
              hex(pkt + mold::kHeaderSize + 2, 36)};
    first.fields = fields_of(adds[0], add_names, true, {"Price"});
    std::string st =
        "{\"kind\":\"engine\",\"bids\":[[\"189.5000\",300,1],[\"189.4900\",500,1]],"
        "\"asks\":[[\"189.5300\",200,1],[\"189.5400\",400,1]]}";
    const Cost c_enc = measure([&](int) { itch::encode(adds[0], pkt + 22); });
    stage(
        "feed-out", "simex · market data thread", "The exchange publishes its book",
        "A replayed Nasdaq day is fed into simex as historical liquidity (owner 0). Each order the "
        "matching engine accepts goes out as an ITCH 5.0 Add Order. The market data thread packs "
        "up to "
        "1400 bytes of messages into one MoldUDP64 packet and multicasts it.",
        {pk, first}, st, "{" + cost("ITCH encode", c_enc) + "}");
  }

  // ---------------- itch-fh / trader md thread: decode and build the book
  book::BookBuilder bb{1u << 16};
  {
    itch::StockDirectory sd;
    sd.header = {kLocate, 0, 1};
    sd.stock = aapl;
    sd.round_lot_size = 100;
    std::byte b[64];
    itch::encode(sd, b);
    bb.apply(b, itch::StockDirectory::kSize);
  }
  mold::Packet parsed;
  mold::parse(pkt, pos, parsed);
  std::string steps = "[";
  int k = 0;
  parsed.for_each([&](std::uint64_t seq, std::span<const std::byte> m) {
    bb.apply(m);
    const auto& bk = bb.book(kLocate);
    auto lv = [](const book::Level* l) {
      return l ? "[\"" + px(l->price) + "\"," + std::to_string(l->shares) + "]"
               : std::string("null");
    };
    steps += (k++ ? "," : "") + std::string("{\"seq\":") + std::to_string(seq) +
             ",\"bid\":" + lv(bk.bids().best()) + ",\"ask\":" + lv(bk.asks().best()) + "}";
  });
  steps += "]";
  const auto& bk = bb.book(kLocate);
  auto side_json = [&](const book::BookSide& s) {
    std::string o = "[";
    for (std::size_t d = 0; d < s.depth(); ++d) {
      const auto* l = s.level(d);
      o += (d ? "," : "") + std::string("[\"") + px(l->price) + "\"," + std::to_string(l->shares) +
           "," + std::to_string(l->orders) + "]";
    }
    return o + "]";
  };
  oms::TopOfBook tob{};
  tob.rx_tsc = rdtsc();
  tob.feed_ns = adds.back().header.timestamp;
  for (int d = 0; d < 2; ++d) {
    if (!bk.bids().level(d) || !bk.asks().level(d)) break;
    tob.bid_px[d] = bk.bids().level(d)->price;
    tob.bid_qty[d] = static_cast<std::uint32_t>(bk.bids().level(d)->shares);
    tob.ask_px[d] = bk.asks().level(d)->price;
    tob.ask_qty[d] = static_cast<std::uint32_t>(bk.asks().level(d)->shares);
  }
  tob.instrument = 0;
  tob.locate = kLocate;
  {
    std::uint64_t ref = 1u << 30;
    std::byte addb[36], delb[19];
    itch::AddOrder a = adds[0];
    itch::OrderDelete del;
    del.header = {kLocate, 0, t_ns};
    const Cost c_apply = measure(
        [&](int) {
          a.order_reference_number = ++ref;
          itch::encode(a, addb);
          bb.apply(addb, 36);
          del.order_reference_number = ref;
          itch::encode(del, delb);
          bb.apply(delb, 19);
        },
        100000);
    std::byte one[64];
    mold::Packet pp;
    const Cost c_parse = measure([&](int) {
      mold::parse(pkt, pos, pp);
      std::size_t n = 0;
      pp.for_each([&](std::uint64_t, std::span<const std::byte> m) { n += m.size(); });
      asm volatile("" ::"r"(n));
    });
    (void)one;
    Msg tm{"TopOfBook", "lab struct", "SPSC ring, md thread to trading thread", ""};
    tm.hexs = hex(reinterpret_cast<const std::byte*>(&tob), sizeof tob);
    tm.fields = {{0, 8, "rx_tsc", "rdtsc when the packet arrived"},
                 {8, 8, "feed_ns", std::to_string(tob.feed_ns)},
                 {16, 8, "bid_px[2]", px(tob.bid_px[0]) + ", " + px(tob.bid_px[1])},
                 {24, 8, "bid_qty[2]",
                  std::to_string(tob.bid_qty[0]) + ", " + std::to_string(tob.bid_qty[1])},
                 {32, 8, "ask_px[2]", px(tob.ask_px[0]) + ", " + px(tob.ask_px[1])},
                 {40, 8, "ask_qty[2]",
                  std::to_string(tob.ask_qty[0]) + ", " + std::to_string(tob.ask_qty[1])},
                 {48, 2, "instrument", "0"},
                 {50, 2, "locate", std::to_string(kLocate)},
                 {52, 12, "reserved", "pads to one 64-byte cache line"}};
    tm.note = "Host byte order (little-endian x86); never leaves the process.";
    const auto& ck = bb.checks();
    std::string st =
        "{\"kind\":\"book\",\"bids\":" + side_json(bk.bids()) +
        ",\"asks\":" + side_json(bk.asks()) + ",\"steps\":" + steps +
        ",\"integrity\":" + std::to_string(ck.book_inconsistency + ck.unknown_order_reference) +
        "}";
    stage("book", "trader · rx and market data threads", "The feed handler rebuilds the book",
          "The rx thread reads the datagram and checks the sequence number (4,812,331 is the next "
          "one "
          "expected, so no gap). The market data thread decodes each Add Order, applies it to the "
          "order-level store and the price-level book, then sends the two best levels per side to "
          "the "
          "trading thread as a 64-byte TopOfBook.",
          {tm}, st,
          "{" + cost("MoldUDP64 parse, 4 msgs", c_parse) + "," +
              cost("ITCH decode + book apply (add and delete)", c_apply) + "}");
  }

  // ---------------- strategy
  strategy::MakerParams mp;
  strategy::MarketMaker mm{mp};
  const auto mkt = strategy::others(tob, 0, 0, 0, 0);
  const auto q = mm.quote(mkt, 0);
  {
    const Cost c_q = measure([&](int) {
      auto r = mm.quote(strategy::others(tob, 0, 0, 0, 0), 0);
      asm volatile("" ::"r"(r.bid_px));
    });
    char b[640];
    std::snprintf(
        b, sizeof b,
        "{\"kind\":\"strategy\",\"bid\":\"%s\",\"ask\":\"%s\",\"spread_ticks\":%u,\"improve\":%u,"
        "\"position\":0,\"lean\":0,\"quote_bid\":\"%s\",\"quote_ask\":\"%s\",\"qty\":%u,\"mid\":\"%"
        "s\"}",
        px(mkt.bid).c_str(), px(mkt.ask).c_str(), (mkt.ask - mkt.bid) / mp.tick,
        mp.improve_spread_ticks, px(q.bid_px).c_str(), px(q.ask_px).c_str(), q.bid_qty,
        px(mkt.mid()).c_str());
    stage("strategy", "trader · trading thread", "The market maker decides",
          "The others' best bid and offer are 3 ticks apart, which meets improve_spread_ticks, so "
          "the maker "
          "steps one tick inside on both sides. Position is flat, so there is no lean. It wants to "
          "buy 100 "
          "at 189.51 and sell 100 at 189.52. We follow the bid.",
          {}, b, "{" + cost("MarketMaker::quote", c_q) + "}");
  }

  // ---------------- risk
  oms::Oms book_oms{1, 1u << 10};
  risk::Limits lim;
  risk::Risk rk{lim, g_clock.ticks_per_ns() * 1e9};
  const std::uint32_t ref_px = mkt.mid();
  const auto verdict = rk.check_new(book_oms, 0, 'B', q.bid_qty, q.bid_px, ref_px, rdtsc());
  {
    risk::Limits fast = lim;
    fast.max_orders_per_second = 1e12;
    fast.burst = 1e12;
    risk::Risk rk2{fast, g_clock.ticks_per_ns() * 1e9};
    const Cost c_r = measure([&](int) {
      auto v = rk2.check_new(book_oms, 0, 'B', q.bid_qty, q.bid_px, ref_px, rdtsc());
      asm volatile("" ::"r"(v));
    });
    const std::uint64_t band = static_cast<std::uint64_t>(ref_px) * lim.collar_bps / 10'000;
    char b[1600];
    std::snprintf(
        b, sizeof b,
        "{\"kind\":\"risk\",\"verdict\":\"%s\",\"checks\":["
        "[\"Kill switch\",\"off\",\"must be off\",true],"
        "[\"Order quantity\",\"%u\",\"1 to %u shares\",true],"
        "[\"Order notional\",\"%u x %s = $%s\",\"at most $%s\",true],"
        "[\"Price collar\",\"%s\",\"mid %s +/- %u bps = %s to %s\",true],"
        "[\"Worst-case position\",\"0 + 0 open buys + %u = %u\",\"at most %lld either way\",true],"
        "[\"Open orders\",\"%zu\",\"below %u\",true],"
        "[\"Order rate\",\"%.0f tokens left\",\"bucket of %.0f at %.0f/s\",true]]}",
        risk::verdict_name(verdict), q.bid_qty, lim.max_order_qty, q.bid_qty, px(q.bid_px).c_str(),
        px(std::uint64_t{q.bid_qty} * q.bid_px).c_str(), px(lim.max_order_notional).c_str(),
        px(q.bid_px).c_str(), px(ref_px).c_str(), lim.collar_bps, px(ref_px - band).c_str(),
        px(ref_px + band).c_str(), q.bid_qty, q.bid_qty, static_cast<long long>(lim.max_position),
        book_oms.working(), lim.max_open_orders, lim.burst - 1, lim.burst,
        lim.max_orders_per_second);
    stage(
        "risk", "trader · trading thread", "Pre-trade risk checks the order",
        "Inline on the trading thread: seven compares against the OMS, no locks and no calls out. "
        "Every check uses integer Price(4) arithmetic except the token bucket.",
        {}, b, "{" + cost("Risk::check_new", c_r) + "}");
  }

  // ---------------- OMS + order request + OUCH out
  const std::uint32_t uref = book_oms.enter(0, 'B', q.bid_qty, q.bid_px);
  oms::OrderRequest req{};
  req.tick_tsc = tob.rx_tsc;
  req.decide_tsc = rdtsc();
  req.user_ref = uref;
  req.qty = q.bid_qty;
  req.price = q.bid_px;
  req.symbol = aapl;
  req.type = oms::OrderRequest::kEnter;
  req.side = 'B';
  req.tif = '0';
  ouch::EnterOrder eo;
  eo.user_ref_num = uref;
  eo.side = 'B';
  eo.quantity = q.bid_qty;
  eo.symbol = aapl;
  eo.price = q.bid_px;
  eo.time_in_force = '0';
  std::byte ob[128];
  ob[3] = std::byte{};
  ouch::encode(eo, ob + soup::kHeaderSize);
  const std::size_t olen = soup::write_header(ob, soup::kUnsequencedData, ouch::EnterOrder::kSize) +
                           ouch::EnterOrder::kSize;
  const std::vector<std::string> eo_names = {
      "UserRefNum", "Side",     "Quantity",          "Symbol",     "Price",   "Time In Force",
      "Display",    "Capacity", "Intermarket Sweep", "Cross Type", "ClOrdID", "Appendage Length"};
  {
    Msg rq{"OrderRequest", "lab struct", "SPSC ring, trading thread to order entry thread",
           hex(reinterpret_cast<const std::byte*>(&req), sizeof req)};
    rq.fields = {{0, 8, "tick_tsc", "rdtsc of the packet that caused it"},
                 {8, 8, "decide_tsc", "rdtsc at the decision"},
                 {16, 4, "user_ref", std::to_string(uref)},
                 {20, 4, "orig_ref", "0"},
                 {24, 4, "qty", std::to_string(req.qty)},
                 {28, 4, "price", std::to_string(req.price) + "  ($" + px(req.price) + ")"},
                 {32, 8, "symbol", "\"AAPL    \""},
                 {40, 1, "type", "O (enter)"},
                 {41, 1, "side", "B"},
                 {42, 1, "tif", "0 (day)"},
                 {43, 21, "reserved", ""}};
    rq.note =
        "Host byte order. The OMS has already marked the order pending-new and counted its 100 "
        "shares as open buy exposure.";
    Msg sp{"SoupBinTCP Unsequenced Data + OUCH Enter Order", "OUCH 5.0",
           "TCP to simex :15000, account FIRM01", hex(ob, olen)};
    sp.fields = {{0, 2, "Soup Packet Length", std::to_string(1 + ouch::EnterOrder::kSize)},
                 {2, 1, "Soup Packet Type", "U"}};
    for (auto f : fields_of(eo, eo_names, false, {"Price"})) {
      f.off += 3;
      sp.fields.push_back(f);
    }
    sp.note =
        "3-byte SoupBinTCP header, then the 47-byte OUCH message. Big-endian integers, "
        "space-padded alphas.";
    const Cost c_e = measure([&](int) {
      ouch::encode(eo, ob + 3);
      soup::write_header(ob, soup::kUnsequencedData, ouch::EnterOrder::kSize);
    });
    char st[256];
    std::snprintf(
        st, sizeof st,
        "{\"kind\":\"oms\",\"ref\":%u,\"state\":\"pending new\",\"open_buy\":%u,\"position\":0}",
        uref, q.bid_qty);
    stage("ouch-out", "trader · order entry thread", "The order leaves the firm",
          "The trading thread pushes a 64-byte OrderRequest. The order entry thread, the only one "
          "that "
          "makes syscalls, encodes OUCH Enter Order, wraps it in SoupBinTCP and writes it to the "
          "socket. "
          "Tick-to-wire is measured here, from the packet's rx_tsc to this send.",
          {rq, sp}, st, "{" + cost("OUCH Enter Order + Soup header encode", c_e) + "}");
  }

  // ---------------- simex gateway + sequencer + matching: accept and rest
  std::uint64_t seq = 9'120'044;
  evs.clear();
  match::NewOrder fo{match::make_key(kFirm, uref),
                     kLocate,
                     match::Side::kBuy,
                     match::Tif::kDay,
                     q.bid_qty,
                     q.bid_px};
  eng.add(fo, L);
  const auto firm_view = evs.front().a;
  const std::uint64_t accept_ns = t_ns += 41'210;
  ouch::OrderAccepted oa;
  oa.timestamp = accept_ns;
  oa.user_ref_num = uref;
  oa.side = 'B';
  oa.quantity = q.bid_qty;
  oa.symbol = aapl;
  oa.price = q.bid_px;
  oa.order_reference_number = firm_view.orn;
  std::byte ab[128];
  ouch::encode(oa, ab + 3);
  const std::size_t alen =
      soup::write_header(ab, soup::kSequencedData, ouch::OrderAccepted::kSize) +
      ouch::OrderAccepted::kSize;
  itch::AddOrder fadd;
  fadd.header = {kLocate, 0, accept_ns};
  fadd.order_reference_number = firm_view.orn;
  fadd.buy_sell_indicator = 'B';
  fadd.shares = q.bid_qty;
  fadd.stock = aapl;
  fadd.price = q.bid_px;
  std::byte fab[36];
  itch::encode(fadd, fab);
  {
    simex::Command cmd{};
    cmd.seq = seq;
    cmd.ts_ns = accept_ns;
    cmd.ref = uref;
    cmd.rx_tsc = rdtsc();
    cmd.qty = 100;
    cmd.price = q.bid_px;
    cmd.symbol = aapl;
    cmd.owner = kFirm;
    cmd.type = simex::Command::kEnter;
    cmd.side = 'B';
    cmd.tif = '0';
    Msg cm{"Command", "lab struct", "SPSC ring, gateway thread to matching thread",
           hex(reinterpret_cast<const std::byte*>(&cmd), sizeof cmd)};
    cm.fields = {{0, 8, "seq", std::to_string(seq) + " (set by the matcher)"},
                 {8, 8, "ts_ns", std::to_string(accept_ns)},
                 {16, 8, "ref", std::to_string(uref)},
                 {24, 8, "orig_ref", "0"},
                 {32, 8, "rx_tsc", "rdtsc at the gateway"},
                 {40, 4, "qty", "100"},
                 {44, 4, "price", std::to_string(q.bid_px)},
                 {48, 8, "symbol", "\"AAPL    \""},
                 {56, 2, "owner", std::to_string(kFirm) + " (FIRM01)"},
                 {58, 2, "locate", "0 (look up)"},
                 {60, 1, "type", "kEnter"},
                 {61, 1, "side", "B"},
                 {62, 1, "tif", "0"},
                 {63, 1, "reserved", ""}};
    cm.note =
        "simex's own 64-byte command; the matcher stamps seq and the journal records it, so every "
        "run replays exactly.";
    const std::vector<std::string> oa_names = {"Timestamp",
                                               "UserRefNum",
                                               "Side",
                                               "Quantity",
                                               "Symbol",
                                               "Price",
                                               "Time In Force",
                                               "Display",
                                               "Order Reference Number",
                                               "Capacity",
                                               "Intermarket Sweep",
                                               "Cross Type",
                                               "Order State",
                                               "ClOrdID",
                                               "Appendage Length"};
    Msg ac{"SoupBinTCP Sequenced Data + OUCH Order Accepted", "OUCH 5.0", "TCP to FIRM01",
           hex(ab, alen)};
    ac.fields = {{0, 2, "Soup Packet Length", "65"}, {2, 1, "Soup Packet Type", "S"}};
    for (auto f : fields_of(oa, oa_names, false, {"Price"})) {
      f.off += 3;
      ac.fields.push_back(f);
    }
    ac.note =
        "Sequenced: both sides count Soup S packets, so a reconnect replays from the next number.";
    Msg ad{"ITCH Add Order (the firm's bid, public)", "ITCH 5.0",
           "MoldUDP64 multicast, seq 4,812,335", hex(fab, 36)};
    ad.fields = fields_of(fadd, add_names, true, {"Price"});
    ad.note = "Anonymous: the feed shows the order reference number, never the account.";
    match::Engine e2{ec};
    e2.add_instrument(kLocate);
    Listener nl{nullptr};
    const Cost c_m = measure(
        [&](int i) {
          match::NewOrder o{match::make_key(kFirm, 1'000'000 + i),
                            kLocate,
                            match::Side::kBuy,
                            match::Tif::kDay,
                            100,
                            1'895'100};
          e2.add(o, nl);
          e2.cancel(o.key, 0, nl);
        },
        100000);
    std::string st =
        "{\"kind\":\"engine\",\"bids\":[[\"189.5100\",100,1,\"FIRM01\"],[\"189.5000\",300,1],["
        "\"189.4900\",500,1]],"
        "\"asks\":[[\"189.5300\",200,1],[\"189.5400\",400,1]],\"orn\":" +
        std::to_string(firm_view.orn) + "}";
    stage("accept", "simex · gateway and matching threads", "simex accepts the order and it rests",
          "The gateway decodes the Enter Order and pushes a 64-byte Command. The matching thread "
          "gives it "
          "sequence " +
              std::to_string(seq) +
              ", finds nothing to cross at 189.51 (best offer 189.53), and rests "
              "it at the front of a new best bid. Two things go out: Order Accepted to the firm, "
              "and an Add Order "
              "on the public feed.",
          {cm, ac, ad}, st, "{" + cost("Engine add + cancel (no cross)", c_m) + "}");
  }

  // ---------------- FIX client sells into the bid via fixgw
  char fb[512];
  fix::Writer w{fb, sizeof fb};
  w.begin("FIX.4.4", fix::msg_type::kNewOrderSingle);
  w.add(fix::tag::kSenderCompID, "FIXB");
  w.add(fix::tag::kTargetCompID, "SIMEX");
  w.add_uint(fix::tag::kMsgSeqNum, 217);
  w.add(fix::tag::kSendingTime, "20261022-13:30:00.044");
  w.add(fix::tag::kClOrdID, "FIXB-000217");
  w.add(fix::tag::kSymbol, "AAPL");
  w.add(fix::tag::kSide, '2');
  w.add(fix::tag::kTransactTime, "20261022-13:30:00.044");
  w.add_uint(fix::tag::kOrderQty, 100);
  w.add(fix::tag::kOrdType, '2');
  w.add_price(fix::tag::kPrice, 1'895'100);
  w.add(fix::tag::kTimeInForce, '0');
  const std::string nos{w.finish()};
  fix::Message fm;
  fm.parse(nos);
  const std::uint32_t fix_ref = 400'001;
  ouch::EnterOrder feo;
  feo.user_ref_num = fix_ref;
  feo.side = 'S';
  feo.quantity = 100;
  feo.symbol = aapl;
  feo.price = 1'895'100;
  feo.cl_ord_id = ouch::make_alpha<14>("FIXB-000217");
  std::byte fob[64];
  ouch::encode(feo, fob + 3);
  const std::size_t folen =
      soup::write_header(fob, soup::kUnsequencedData, ouch::EnterOrder::kSize) +
      ouch::EnterOrder::kSize;
  {
    auto tagfields = [&](const std::string& s) {
      std::vector<Field> v;
      std::size_t p = 0;
      while (p < s.size()) {
        const auto eq = s.find('=', p), soh = s.find('\x01', p);
        v.push_back({p, soh - p + 1, s.substr(p, eq - p), s.substr(eq + 1, soh - eq - 1)});
        p = soh + 1;
      }
      return v;
    };
    Msg nm{"FIX NewOrderSingle (35=D)", "FIX 4.4", "TCP to fixgw :16000, session FIXB", "", nos};
    nm.fields = tagfields(nos);
    nm.note =
        "tag=value, SOH-delimited (shown as |). 9 is the byte count after it up to 10=; 10 is the "
        "byte sum mod 256.";
    Msg fo2{"SoupBinTCP + OUCH Enter Order (from fixgw)", "OUCH 5.0", "TCP to simex, account OUCHB",
            hex(fob, folen)};
    fo2.fields = {{0, 2, "Soup Packet Length", "48"}, {2, 1, "Soup Packet Type", "U"}};
    for (auto f : fields_of(feo, eo_names, false, {"Price"})) {
      f.off += 3;
      fo2.fields.push_back(f);
    }
    fo2.note =
        "54=2 becomes Side S, 44=189.51 becomes 1895100, 11 is copied into ClOrdID. UserRefNum "
        "continues from the base the Account Query gave fixgw.";
    const Cost c_fp = measure([&](int) {
      fix::Message mm2;
      mm2.parse(nos);
      asm volatile("" ::"r"(&mm2));
    });
    stage("fix-in", "fix-client → fixgw", "A FIX firm sells into the bid",
          "Another firm trades over FIX 4.4. fixgw checks the session sequence (217), validates "
          "the tags, "
          "indexes the ClOrdID and maps the order onto an OUCH Enter Order on its own SoupBinTCP "
          "session "
          "to simex.",
          {nm, fo2}, "", "{" + cost("FIX parse NewOrderSingle", c_fp) + "}");
  }

  // ---------------- match
  evs.clear();
  match::NewOrder so{match::make_key(kFixb, fix_ref),
                     kLocate,
                     match::Side::kSell,
                     match::Tif::kDay,
                     100,
                     1'895'100};
  eng.add(so, L);
  Ev ex;
  for (auto& e : evs)
    if (e.what == "execution") ex = e;
  const std::uint64_t exec_ns = t_ns += 63'448;
  ouch::OrderExecuted oe;
  oe.timestamp = exec_ns;
  oe.user_ref_num = uref;
  oe.quantity = ex.q;
  oe.price = ex.p;
  oe.liquidity_flag = 'A';
  oe.match_number = ex.m;
  std::byte eb[64];
  ouch::encode(oe, eb + 3);
  const std::size_t elen =
      soup::write_header(eb, soup::kSequencedData, ouch::OrderExecuted::kSize) +
      ouch::OrderExecuted::kSize;
  itch::OrderExecuted ie;
  ie.header = {kLocate, 0, exec_ns};
  ie.order_reference_number = ex.a.orn;
  ie.executed_shares = ex.q;
  ie.match_number = ex.m;
  std::byte ieb[31];
  itch::encode(ie, ieb);
  dropcopy::ExecutionReport dc;
  dc.timestamp = exec_ns;
  dc.account = ouch::make_alpha<6>("FIRM01");
  dc.user_ref_num = uref;
  dc.side = 'B';
  dc.quantity = ex.q;
  dc.symbol = aapl;
  dc.price = ex.p;
  dc.match_number = ex.m;
  dc.liquidity_flag = 'A';
  dc.order_reference_number = ex.a.orn;
  std::byte db[80];
  ouch::encode(dc, db + 3);
  const std::size_t dlen =
      soup::write_header(db, soup::kSequencedData, dropcopy::ExecutionReport::kSize) +
      dropcopy::ExecutionReport::kSize;
  {
    const std::vector<std::string> oe_names = {"Timestamp",       "UserRefNum",     "Quantity",
                                               "Price",           "Liquidity Flag", "Match Number",
                                               "Appendage Length"};
    Msg m1{"SoupBinTCP + OUCH Order Executed (to the firm)", "OUCH 5.0", "TCP to FIRM01",
           hex(eb, elen)};
    m1.fields = {{0, 2, "Soup Packet Length", "37"}, {2, 1, "Soup Packet Type", "S"}};
    for (auto f : fields_of(oe, oe_names, false, {"Price"})) {
      f.off += 3;
      m1.fields.push_back(f);
    }
    m1.note =
        "Liquidity A: the firm's order was resting, so it added liquidity. fixgw gets the same "
        "message with its own UserRefNum and flag R.";
    Msg m2{"ITCH Order Executed (public)", "ITCH 5.0", "MoldUDP64 multicast, seq 4,812,336",
           hex(ieb, 31)};
    m2.fields = fields_of(ie, {"Order Reference Number", "Executed Shares", "Match Number"}, true);
    m2.note = "Every feed handler takes 100 shares off order " + std::to_string(ex.a.orn) +
              ". The match number is the same one the two firms see.";
    Msg m3{"SoupBinTCP + Drop Copy Execution Report", "lab drop copy",
           "TCP to DROP01, the back office", hex(db, dlen)};
    m3.fields = {{0, 2, "Soup Packet Length", "58"}, {2, 1, "Soup Packet Type", "S"}};
    for (auto f : fields_of(dc,
                            {"Timestamp", "Account", "UserRefNum", "Side", "Quantity", "Symbol",
                             "Price", "Match Number", "Liquidity Flag", "Order Reference Number"},
                            false, {"Price"})) {
      f.off += 3;
      m3.fields.push_back(f);
    }
    m3.note =
        "Independent of the trading session, so the back office can reconcile what the trader "
        "believes.";
    match::Engine e3{ec};
    e3.add_instrument(kLocate);
    Listener nl{nullptr};
    std::uint64_t r = 0;
    // Untimed: rest a bid. Timed: a sell that crosses it fully.
    std::vector<std::uint64_t> raw;
    const Cost c_x = [&] {
      const int n = 100000;
      std::vector<std::uint64_t> t(n);
      for (int i = 0; i < n; ++i) {
        e3.add({match::make_key(kFirm, ++r), kLocate, match::Side::kBuy, match::Tif::kDay, 100,
                1'895'100},
               nl);
        const match::NewOrder s{match::make_key(kFixb, ++r),
                                kLocate,
                                match::Side::kSell,
                                match::Tif::kDay,
                                100,
                                1'895'100};
        const auto a = rdtscp();
        e3.add(s, nl);
        t[i] = rdtscp() - a;
      }
      std::sort(t.begin(), t.end());
      return Cost{g_clock.to_ns(t[n / 2]), g_clock.to_ns(t[n * 99 / 100])};
    }();
    const Cost c_dc = measure([&](int) { ouch::encode(dc, db + 3); });
    char st[512];
    std::snprintf(st, sizeof st,
                  "{\"kind\":\"engine\",\"bids\":[[\"189.5000\",300,1],[\"189.4900\",500,1]],"
                  "\"asks\":[[\"189.5300\",200,1],[\"189.5400\",400,1]],\"match\":%llu,\"filled\":"
                  "\"100 @ %s\"}",
                  static_cast<unsigned long long>(ex.m), px(ex.p).c_str());
    stage("match", "simex · matching thread", "The engine matches",
          "The sell at 189.51 crosses the firm's resting bid. Price-time priority: the resting "
          "order sets "
          "the price, 189.51. The firm's order is fully filled and leaves the book. One execution "
          "fans out "
          "into four messages: two OUCH Order Executed, one ITCH Order Executed and one drop copy "
          "report.",
          {m1, m2, m3}, st,
          "{" + cost("Engine add that crosses a resting order", c_x) + "," +
              cost("Drop copy encode", c_dc) + "}");
  }

  // ---------------- back to FIX + OMS
  w.begin("FIX.4.4", fix::msg_type::kExecutionReport);
  w.add(fix::tag::kSenderCompID, "SIMEX");
  w.add(fix::tag::kTargetCompID, "FIXB");
  w.add_uint(fix::tag::kMsgSeqNum, 431);
  w.add(fix::tag::kSendingTime, "20261022-13:30:00.044");
  w.add_uint(fix::tag::kOrderID, ex.b.orn);
  w.add(fix::tag::kClOrdID, "FIXB-000217");
  w.add_uint(fix::tag::kExecID, 862);
  w.add(fix::tag::kExecType, 'F');
  w.add(fix::tag::kOrdStatus, '2');
  w.add(fix::tag::kSymbol, "AAPL");
  w.add(fix::tag::kSide, '2');
  w.add_uint(fix::tag::kOrderQty, 100);
  w.add_price(fix::tag::kPrice, 1'895'100);
  w.add(fix::tag::kOrdType, '2');
  w.add(fix::tag::kTimeInForce, '0');
  w.add_uint(fix::tag::kLastQty, 100);
  w.add_price(fix::tag::kLastPx, ex.p);
  w.add_uint(fix::tag::kLeavesQty, 0);
  w.add_uint(fix::tag::kCumQty, 100);
  w.add_price(fix::tag::kAvgPx, ex.p);
  w.add(fix::tag::kTransactTime, "20261022-13:30:00.044");
  const std::string er{w.finish()};
  oms::OrderResponse resp{};
  resp.type = oms::OrderResponse::kAccepted;
  resp.user_ref = uref;
  resp.qty = 100;
  resp.price = q.bid_px;
  resp.number = firm_view.orn;
  book_oms.apply(resp);
  resp = {};
  resp.type = oms::OrderResponse::kExecuted;
  resp.user_ref = uref;
  resp.qty = ex.q;
  resp.price = ex.p;
  resp.number = ex.m;
  resp.flag = 'A';
  resp.exchange_ns = exec_ns;
  book_oms.apply(resp);
  const auto& pos0 = book_oms.position(0);
  {
    std::vector<Field> v;
    std::size_t p = 0;
    while (p < er.size()) {
      const auto eq = er.find('=', p), soh = er.find('\x01', p);
      v.push_back({p, soh - p + 1, er.substr(p, eq - p), er.substr(eq + 1, soh - eq - 1)});
      p = soh + 1;
    }
    Msg x{"FIX ExecutionReport (35=8, 150=F)", "FIX 4.4", "TCP to FIXB", "", er};
    x.fields = v;
    x.note =
        "fixgw turns OUCH Order Executed into ExecType F (trade), OrdStatus 2 (filled), with "
        "LastQty/LastPx and running CumQty/AvgPx. It is stored for resend in case FIXB reconnects.";
    Msg r{"OrderResponse", "lab struct", "SPSC ring, order entry thread to trading thread",
          hex(reinterpret_cast<const std::byte*>(&resp), sizeof resp)};
    r.fields = {{0, 8, "rx_tsc", "rdtsc when the OUCH packet arrived"},
                {8, 8, "exchange_ns", std::to_string(exec_ns)},
                {16, 8, "number", std::to_string(ex.m) + " (match number)"},
                {24, 4, "user_ref", std::to_string(uref)},
                {28, 4, "orig_ref", "0"},
                {32, 4, "qty", "100"},
                {36, 4, "price", std::to_string(ex.p)},
                {40, 2, "reason", "0"},
                {42, 1, "type", "E (executed)"},
                {43, 1, "flag", "A"},
                {44, 20, "reserved", ""}};
    std::string fx;
    {
      char tmp[64];
      fix::Writer w2{tmp, 0};
      (void)w2;
    }
    const Cost c_fe = measure([&](int) {
      char bb2[512];
      fix::Writer w3{bb2, sizeof bb2};
      w3.begin("FIX.4.4", '8');
      w3.add(fix::tag::kSenderCompID, "SIMEX");
      w3.add(fix::tag::kTargetCompID, "FIXB");
      w3.add_uint(fix::tag::kMsgSeqNum, 431);
      w3.add_uint(fix::tag::kOrderID, ex.b.orn);
      w3.add(fix::tag::kClOrdID, "FIXB-000217");
      w3.add_uint(fix::tag::kExecID, 862);
      w3.add(fix::tag::kExecType, 'F');
      w3.add(fix::tag::kOrdStatus, '2');
      w3.add(fix::tag::kSymbol, "AAPL");
      w3.add_uint(fix::tag::kLastQty, 100);
      w3.add_price(fix::tag::kLastPx, ex.p);
      w3.add_uint(fix::tag::kLeavesQty, 0);
      w3.add_uint(fix::tag::kCumQty, 100);
      w3.add_price(fix::tag::kAvgPx, ex.p);
      auto s = w3.finish();
      asm volatile("" ::"r"(s.data()));
    });
    oms::OrderResponse rr = resp;
    const Cost c_o = measure([&](int i) {
      rr.user_ref = 9'999'999 + i;  // unknown ref: the lookup and checks without changing state
      auto a = book_oms.apply(rr);
      asm volatile("" ::"r"(a.order));
    });
    const std::int64_t mark = 1'895'150;
    char st[600];
    std::snprintf(
        st, sizeof st,
        "{\"kind\":\"position\",\"shares\":%lld,\"cash\":\"-%s\",\"bought\":%llu,\"fills\":%llu,"
        "\"open_buy\":%llu,\"mark\":\"%s\",\"pnl\":\"%s\",\"working\":%zu,\"match\":%llu}",
        static_cast<long long>(pos0.shares), px(static_cast<std::uint64_t>(-pos0.cash)).c_str(),
        static_cast<unsigned long long>(pos0.bought), static_cast<unsigned long long>(pos0.fills),
        static_cast<unsigned long long>(pos0.open_buy), px(mark).c_str(),
        px(static_cast<std::uint64_t>(pos0.pnl(mark))).c_str(), book_oms.working(),
        static_cast<unsigned long long>(ex.m));
    stage(
        "fills", "fixgw → FIX firm · trader OMS · post-trade", "Both firms book the fill",
        "The FIX firm gets an ExecutionReport. The firm's order entry thread decodes Order "
        "Executed into "
        "a 64-byte OrderResponse; the OMS turns pending exposure into a position. The quote is now "
        "lopsided (long 100), so the next quote leans down a tick. After the run, "
        "research/report.py "
        "joins trader fills and drop copy on match number: they must agree exactly.",
        {x, r}, st,
        "{" + cost("FIX ExecutionReport encode", c_fe) + "," + cost("OMS apply lookup", c_o) + "}");
  }

  std::printf("{\"ghz\":%.3f,\"stages\":[", g_clock.ghz());
  for (std::size_t i = 0; i < stages.size(); ++i)
    std::printf("%s%s", i ? "," : "", stages[i].c_str());
  std::printf("]}\n");
}
