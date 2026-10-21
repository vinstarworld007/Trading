#include "lab/itch/synthetic.hpp"

#include <array>
#include <cstdio>
#include <random>

#include "lab/itch/codec.hpp"

namespace lab::itch {

SyntheticDay::SyntheticDay(SyntheticConfig config) : config_{config} {}

void SyntheticDay::generate(const std::function<void(std::span<const std::byte>)>& sink) {
  std::mt19937_64 rng{config_.seed};
  const std::uint32_t n = config_.symbols;
  live_.clear();
  refs_by_locate_.assign(n + 1, {});
  totals_.assign(n + 1, {});
  written_ = 0;

  std::array<std::byte, kMaxMessageSize> buf{};
  std::uint64_t ts = 4ULL * 3600 * 1'000'000'000;  // 04:00:00
  std::uint16_t tracking = 0;
  std::uint64_t next_ref = 1;
  std::uint64_t next_match = 1;

  auto emit = [&](auto msg, std::uint16_t locate) {
    msg.header.stock_locate = locate;
    msg.header.tracking_number = ++tracking;
    msg.header.timestamp = ts;
    encode(msg, buf.data());
    sink(std::span<const std::byte>{buf.data(), decltype(msg)::kSize});
    ++written_;
  };
  auto system_event = [&](char code) {
    SystemEvent e;
    e.event_code = code;
    emit(e, 0);
  };
  auto symbol_name = [](std::uint32_t locate) {
    char name[9];
    std::snprintf(name, sizeof name, "SYM%04u", locate);
    return make_symbol(name);
  };

  // Mid prices between $10 and $500, on a $0.01 grid (Price(4) units of 100).
  std::vector<std::uint32_t> mid(n + 1);
  std::uniform_int_distribution<std::uint32_t> mid_dist{1'000, 50'000};
  for (std::uint32_t l = 1; l <= n; ++l) mid[l] = mid_dist(rng) * 100;

  system_event('O');
  for (std::uint32_t l = 1; l <= n; ++l) {
    StockDirectory r;
    r.stock = symbol_name(l);
    r.market_category = 'Q';
    r.financial_status_indicator = 'N';
    r.round_lot_size = 100;
    r.round_lots_only = 'N';
    r.issue_classification = 'C';
    r.issue_sub_type = {'Z', ' '};
    r.authenticity = 'P';
    r.short_sale_threshold_indicator = 'N';
    r.ipo_flag = 'N';
    r.luld_reference_price_tier = '1';
    r.etp_flag = 'N';
    r.inverse_indicator = 'N';
    emit(r, static_cast<std::uint16_t>(l));
    StockTradingAction h;
    h.stock = r.stock;
    h.trading_state = 'T';
    emit(h, static_cast<std::uint16_t>(l));
  }
  system_event('S');

  std::uniform_int_distribution<std::uint32_t> pick_symbol{1, n};
  std::uniform_int_distribution<int> pick_action{0, 99};
  std::uniform_int_distribution<std::uint32_t> pick_ticks{1, 20};
  std::uniform_int_distribution<std::uint32_t> pick_lots{1, 10};
  const std::uint64_t day_ns = 16ULL * 3600 * 1'000'000'000;  // 04:00 to 20:00
  const std::uint64_t step = config_.book_messages ? day_ns / config_.book_messages : 1;
  const std::uint64_t market_open = config_.book_messages * 35 / 100;
  const std::uint64_t market_close = config_.book_messages * 75 / 100;

  auto remove_ref = [&](std::uint16_t locate, std::size_t index) {
    auto& refs = refs_by_locate_[locate];
    live_.erase(refs[index]);
    refs[index] = refs.back();
    refs.pop_back();
  };
  auto cross = [&](char type) {
    for (std::uint32_t l = 1; l <= n; ++l) {
      CrossTrade q;
      q.shares = pick_lots(rng) * 1000;
      q.stock = symbol_name(l);
      q.cross_price = mid[l];
      q.match_number = next_match++;
      q.cross_type = type;
      totals_[l].cross_shares += q.shares;
      emit(q, static_cast<std::uint16_t>(l));
    }
  };

  for (std::uint64_t i = 0; i < config_.book_messages; ++i) {
    ts += step;
    if (i == market_open) {
      system_event('Q');
      cross('O');
    }
    if (i == market_close) {
      cross('C');
      system_event('M');
    }

    const auto locate = static_cast<std::uint16_t>(pick_symbol(rng));
    auto& refs = refs_by_locate_[locate];
    int action = pick_action(rng);
    if (refs.empty()) action = 0;  // nothing to act on yet: add

    if (action < 45) {  // add
      const bool buy = rng() & 1;
      const std::uint32_t offset = pick_ticks(rng) * 100;
      const LiveOrder o{locate, buy ? 'B' : 'S', buy ? mid[locate] - offset : mid[locate] + offset,
                        pick_lots(rng) * 100};
      const std::uint64_t ref = next_ref++;
      if (action < 40) {
        AddOrder a;
        a.order_reference_number = ref;
        a.buy_sell_indicator = o.side;
        a.shares = o.shares;
        a.stock = symbol_name(locate);
        a.price = o.price;
        emit(a, locate);
      } else {
        AddOrderMpid f;
        f.order_reference_number = ref;
        f.buy_sell_indicator = o.side;
        f.shares = o.shares;
        f.stock = symbol_name(locate);
        f.price = o.price;
        f.attribution = {'L', 'A', 'B', 'X'};
        emit(f, locate);
      }
      live_.emplace(ref, o);
      refs.push_back(ref);
      continue;
    }

    std::uniform_int_distribution<std::size_t> pick_order{0, refs.size() - 1};
    const std::size_t index = pick_order(rng);
    const std::uint64_t ref = refs[index];
    LiveOrder& o = live_.at(ref);

    if (action < 70) {  // delete
      OrderDelete d;
      d.order_reference_number = ref;
      emit(d, locate);
      remove_ref(locate, index);
    } else if (action < 78) {  // partial or full cancel
      OrderCancel x;
      x.order_reference_number = ref;
      x.cancelled_shares = o.shares > 100 ? o.shares / 2 : o.shares;
      emit(x, locate);
      o.shares -= x.cancelled_shares;
      if (o.shares == 0) remove_ref(locate, index);
    } else if (action < 90) {  // execution
      const std::uint32_t shares = o.shares > 100 ? 100 : o.shares;
      if (action < 87) {
        OrderExecuted e;
        e.order_reference_number = ref;
        e.executed_shares = shares;
        e.match_number = next_match++;
        emit(e, locate);
        totals_[locate].executed_shares += shares;
      } else {
        OrderExecutedWithPrice c;
        c.order_reference_number = ref;
        c.executed_shares = shares;
        c.match_number = next_match++;
        c.printable = action < 89 ? 'Y' : 'N';
        c.execution_price = o.price;
        emit(c, locate);
        if (c.printable == 'Y') totals_[locate].executed_shares += shares;
      }
      o.shares -= shares;
      if (o.shares == 0) remove_ref(locate, index);
    } else if (action < 98) {  // replace: new ref, same side, new price and size
      const std::uint32_t offset = pick_ticks(rng) * 100;
      OrderReplace u;
      u.original_order_reference_number = ref;
      u.new_order_reference_number = next_ref++;
      u.shares = pick_lots(rng) * 100;
      u.price = o.side == 'B' ? mid[locate] - offset : mid[locate] + offset;
      emit(u, locate);
      const LiveOrder replaced{locate, o.side, u.price, u.shares};
      remove_ref(locate, index);
      live_.emplace(u.new_order_reference_number, replaced);
      refs_by_locate_[locate].push_back(u.new_order_reference_number);
    } else {  // execution against a hidden order: no book change
      Trade p;
      p.order_reference_number = 0;
      p.buy_sell_indicator = 'B';
      p.shares = 100;
      p.stock = symbol_name(locate);
      p.price = mid[locate];
      p.match_number = next_match++;
      emit(p, locate);
      totals_[locate].hidden_trade_shares += p.shares;
    }
  }

  if (config_.book_messages <= market_close) {
    cross('C');
    system_event('M');
  }
  system_event('E');
  system_event('C');
}

}  // namespace lab::itch
