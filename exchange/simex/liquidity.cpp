#include "liquidity.hpp"

#include <algorithm>

#include "lab/core/clock.hpp"
#include "lab/core/endian.hpp"
#include "lab/itch/codec.hpp"
#include "lab/itch/file_reader.hpp"

namespace simex {

namespace itch = lab::itch;

namespace {

struct Converter : itch::HandlerBase {
  CommandRing& out;
  const std::atomic<bool>& stop;
  std::uint64_t commands = 0;

  Converter(CommandRing& r, const std::atomic<bool>& s) : out{r}, stop{s} {}
  using HandlerBase::on;

  void push(Command c, std::uint16_t locate) {
    c.owner = kHistOwner;
    c.locate = locate;
    c.rx_tsc = lab::rdtsc();
    while (!out.try_push(c)) {
      if (stop.load(std::memory_order_relaxed)) return;
    }
    ++commands;
  }

  void on(const itch::StockDirectory& m) {
    Command c{};
    c.type = Command::kDirectory;
    c.symbol = m.stock;
    push(c, m.header.stock_locate);
  }
  void add(const itch::Header& h, std::uint64_t ref, char side, std::uint32_t shares,
           const itch::Symbol& stock, std::uint32_t price) {
    Command c{};
    c.type = Command::kEnter;
    c.ref = ref;
    c.side = side;
    c.tif = '0';
    c.qty = shares;
    c.price = price;
    c.symbol = stock;
    push(c, h.stock_locate);
  }
  void on(const itch::AddOrder& m) {
    add(m.header, m.order_reference_number, m.buy_sell_indicator, m.shares, m.stock, m.price);
  }
  void on(const itch::AddOrderMpid& m) {
    add(m.header, m.order_reference_number, m.buy_sell_indicator, m.shares, m.stock, m.price);
  }
  void reduce(const itch::Header& h, std::uint64_t ref, std::uint32_t shares) {
    Command c{};
    c.type = Command::kReduce;
    c.ref = ref;
    c.qty = shares;
    push(c, h.stock_locate);
  }
  void on(const itch::OrderExecuted& m) {
    reduce(m.header, m.order_reference_number, m.executed_shares);
  }
  void on(const itch::OrderExecutedWithPrice& m) {
    reduce(m.header, m.order_reference_number, m.executed_shares);
  }
  void on(const itch::OrderCancel& m) {
    reduce(m.header, m.order_reference_number, m.cancelled_shares);
  }
  void on(const itch::OrderDelete& m) {
    Command c{};
    c.type = Command::kCancel;
    c.ref = m.order_reference_number;
    c.qty = 0;
    push(c, m.header.stock_locate);
  }
  void on(const itch::OrderReplace& m) {
    Command c{};
    c.type = Command::kReplaceOpen;
    c.orig_ref = m.original_order_reference_number;
    c.ref = m.new_order_reference_number;
    c.qty = m.shares;
    c.price = m.price;
    c.tif = '0';
    push(c, m.header.stock_locate);
  }
};

}  // namespace

LiquidityStats replay_liquidity(const LiquidityConfig& cfg, CommandRing& out,
                                const std::atomic<bool>& stop) {
  LiquidityStats st;
  itch::FileReader reader{cfg.path};
  if (!reader.ok()) {
    st.ok = false;
    st.error = reader.error();
    return st;
  }
  Converter conv{out, stop};
  const std::uint64_t start = lab::mono_ns();
  const double ns_per_msg = cfg.rate > 0 ? 1e9 / cfg.rate : 0;
  std::uint64_t first_ts = 0;
  while (!stop.load(std::memory_order_relaxed)) {
    const auto msg = reader.next();
    if (!msg) break;
    std::uint64_t due = start;
    if (cfg.speed > 0 && msg->size() >= 11) {
      const std::uint64_t ts = lab::load_be48(msg->data() + 5);
      if (st.messages == 0) first_ts = ts;
      due +=
          static_cast<std::uint64_t>(static_cast<double>(ts - std::min(ts, first_ts)) / cfg.speed);
    } else if (ns_per_msg > 0) {
      due += static_cast<std::uint64_t>(static_cast<double>(st.messages) * ns_per_msg);
    }
    while (lab::mono_ns() < due) {
      if (stop.load(std::memory_order_relaxed)) break;
    }
    const std::uint64_t before = conv.commands;
    itch::dispatch(msg->data(), msg->size(), conv);
    if (conv.commands == before) ++st.ignored;
    ++st.messages;
    if (cfg.limit != 0 && st.messages >= cfg.limit) break;
  }
  if (!reader.ok()) {
    st.ok = false;
    st.error = reader.error();
  }
  st.commands = conv.commands;
  return st;
}

}  // namespace simex
