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

  auto trading_action = [&](std::uint16_t l, char state, const char* reason) {
    StockTradingAction h;
    h.stock = symbol_name(l);
    h.trading_state = state;
    h.reserved = ' ';
    h.reason = {' ', ' ', ' ', ' '};
    for (std::size_t i = 0; i < 4 && reason[i] != '\0'; ++i) h.reason[i] = reason[i];
    emit(h, l);
  };
  // With scenarios on, an IPO is listed after the n symbols, halted until
  // its release.
  const bool has_ipo = config_.scenarios && n < 65535;
  const std::uint16_t ipo = static_cast<std::uint16_t>(n + 1);

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
  if (has_ipo) {
    StockDirectory r;
    r.stock = symbol_name(ipo);
    r.market_category = 'Q';
    r.financial_status_indicator = 'N';
    r.round_lot_size = 100;
    r.round_lots_only = 'N';
    r.issue_classification = 'C';
    r.issue_sub_type = {'Z', ' '};
    r.authenticity = 'P';
    r.short_sale_threshold_indicator = 'N';
    r.ipo_flag = 'Y';
    r.luld_reference_price_tier = '1';
    r.etp_flag = 'N';
    r.inverse_indicator = 'N';
    emit(r, ipo);
    trading_action(ipo, 'H', "IPO1");
    IpoQuotingPeriod k;
    k.stock = r.stock;
    k.ipo_quotation_release_time = 11 * 3600;
    k.ipo_quotation_release_qualifier = 'A';
    k.ipo_price = 0;
    emit(k, ipo);
    mid.push_back(mid_dist(rng) * 100);
    refs_by_locate_.emplace_back();
    totals_.emplace_back();
  }
  if (config_.scenarios) {
    MwcbDeclineLevel v;  // an index at 5,000.00: 7%, 13% and 20% below, Price(8)
    v.level1 = 465'000'000'000;
    v.level2 = 435'000'000'000;
    v.level3 = 400'000'000'000;
    emit(v, 0);
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
  // An auction for one symbol: imbalance message, a buy and a sell that cross
  // the book's mid, their non-printable executions, then the cross print.
  auto auction = [&](std::uint16_t l, char type) {
    const std::uint32_t lots = pick_lots(rng);
    const std::uint64_t shares = lots * 100;
    Noii i;
    i.paired_shares = shares;
    i.imbalance_shares = 0;
    i.imbalance_direction = 'N';
    i.stock = symbol_name(l);
    i.far_price = i.near_price = i.current_reference_price = mid[l];
    i.cross_type = type;
    i.price_variation_indicator = 'L';
    emit(i, l);
    std::uint64_t refs[2];
    for (int side = 0; side < 2; ++side) {
      AddOrder a;
      a.order_reference_number = refs[side] = next_ref++;
      a.buy_sell_indicator = side == 0 ? 'B' : 'S';
      a.shares = static_cast<std::uint32_t>(shares);
      a.stock = symbol_name(l);
      a.price = side == 0 ? mid[l] + 100 : mid[l] - 100;  // through the mid: they cross
      emit(a, l);
    }
    const std::uint64_t match = next_match++;
    for (const std::uint64_t ref : refs) {
      OrderExecutedWithPrice c;
      c.order_reference_number = ref;
      c.executed_shares = static_cast<std::uint32_t>(shares);
      c.match_number = match;
      c.printable = 'N';
      c.execution_price = mid[l];
      emit(c, l);
    }
    CrossTrade q;
    q.shares = shares;
    q.stock = symbol_name(l);
    q.cross_price = mid[l];
    q.match_number = next_match++;
    q.cross_type = type;
    totals_[l].cross_shares += q.shares;
    emit(q, l);
  };
  auto cross = [&](char type) {
    for (std::uint32_t l = 1; l <= n; ++l) {
      if (config_.scenarios) {
        auction(static_cast<std::uint16_t>(l), type);
        continue;
      }
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
  // The scenarios that happen once during the day, each at a fixed point.
  std::uint64_t last_execution = 0;
  std::uint16_t last_execution_locate = 0;
  auto scenario = [&](std::uint64_t i) {
    if (!config_.scenarios || config_.book_messages < 10) return;
    const std::uint64_t m = config_.book_messages;
    const auto sym = [&](std::uint32_t k) { return static_cast<std::uint16_t>(1 + k % n); };
    if (i == m * 30 / 100) {  // operational halt on Nasdaq, lifted
      for (const char action : {'H', 'T'}) {
        OperationalHalt h;
        h.stock = symbol_name(sym(3));
        h.market_code = 'Q';
        h.operational_halt_action = action;
        emit(h, sym(3));
      }
    } else if (i == m * 40 / 100 && has_ipo) {  // IPO released, crossed, trading
      trading_action(ipo, 'Q', "IPOQ");
      auction(ipo, 'H');
      trading_action(ipo, 'T', "");
    } else if (i == m * 50 / 100) {  // news halt, quotation period, halt cross
      trading_action(sym(0), 'H', "T1");
      trading_action(sym(0), 'Q', "T3");
      auction(sym(0), 'H');
      trading_action(sym(0), 'T', "");
    } else if (i == m * 55 / 100) {  // LULD pause and reopening
      trading_action(sym(1), 'P', "LUDP");
      LuldAuctionCollar j;
      j.stock = symbol_name(sym(1));
      j.reference_price = mid[sym(1)];
      j.upper_collar_price = mid[sym(1)] + mid[sym(1)] / 20 / 100 * 100;
      j.lower_collar_price = mid[sym(1)] - mid[sym(1)] / 20 / 100 * 100;
      j.collar_extension = 0;
      emit(j, sym(1));
      auction(sym(1), 'H');
      trading_action(sym(1), 'T', "");
    } else if (i == m * 60 / 100) {  // market-wide circuit breaker, level 1
      MwcbStatus w;
      w.breached_level = '1';
      emit(w, 0);
      for (std::uint32_t l = 1; l <= n; ++l)
        trading_action(static_cast<std::uint16_t>(l), 'H', "MWC1");
      for (std::uint32_t l = 1; l <= n; ++l)
        trading_action(static_cast<std::uint16_t>(l), 'Q', "MWCQ");
      for (std::uint32_t l = 1; l <= n; ++l) trading_action(static_cast<std::uint16_t>(l), 'T', "");
    } else if (i == m * 65 / 100) {  // short sale price test
      RegShoRestriction y;
      y.stock = symbol_name(sym(2));
      y.reg_sho_action = '1';
      emit(y, sym(2));
    } else if (i == m * 70 / 100 && last_execution != 0) {  // a clearly erroneous trade
      BrokenTrade b;
      b.match_number = last_execution;
      emit(b, last_execution_locate);
    }
  };

  for (std::uint64_t i = 0; i < config_.book_messages; ++i) {
    ts += step;
    scenario(i);
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
        last_execution = e.match_number;
        last_execution_locate = locate;
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
