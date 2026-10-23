#include "lab/book/book_builder.hpp"

namespace lab::book {
namespace {

// Order in which Nasdaq sends the day's system events.
constexpr std::string_view kSystemEventOrder = "OSQMEC";

}  // namespace

BookBuilder::BookBuilder(std::size_t expected_live_orders) : instruments_(65536) {
  orders_.reserve(expected_live_orders);
}

std::uint16_t BookBuilder::find_locate(std::string_view symbol) const noexcept {
  for (std::size_t l = 1; l < instruments_.size(); ++l) {
    if (instruments_[l].listed && itch::trimmed(instruments_[l].symbol) == symbol) {
      return static_cast<std::uint16_t>(l);
    }
  }
  return 0;
}

std::vector<std::uint16_t> BookBuilder::listed_locates() const {
  std::vector<std::uint16_t> out;
  for (std::size_t l = 1; l < instruments_.size(); ++l) {
    if (instruments_[l].listed) out.push_back(static_cast<std::uint16_t>(l));
  }
  return out;
}

std::uint64_t BookBuilder::digest() const noexcept {
  std::uint64_t h = 0xCBF29CE484222325ULL;
  auto mix = [&h](std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (i * 8)) & 0xFF;
      h *= 0x100000001B3ULL;
    }
  };
  for (std::size_t l = 1; l < instruments_.size(); ++l) {
    const Instrument& inst = instruments_[l];
    if (!inst.listed) continue;
    mix(l);
    mix(static_cast<unsigned char>(inst.trading_state));
    mix(inst.stats.matched_volume());
    for (const BookSide* side : {&inst.book.bids(), &inst.book.asks()}) {
      mix(side->depth());
      for (std::size_t d = 0; d < side->depth(); ++d) {
        const Level* lv = side->level(d);
        mix(lv->price);
        mix(lv->shares);
        mix(lv->orders);
      }
    }
  }
  mix(orders_.size());
  return h;
}

Instrument* BookBuilder::listed(std::uint16_t locate) noexcept {
  Instrument& inst = instruments_[locate];
  if (LAB_UNLIKELY(!inst.listed)) {
    ++checks_.locate_without_directory;
    return nullptr;
  }
  return &inst;
}

void BookBuilder::on(const itch::SystemEvent& m) {
  see(m.header);
  const auto pos = kSystemEventOrder.find(m.event_code);
  if (pos == std::string_view::npos) return;  // other codes do not affect the sequence
  const std::size_t expected = system_events_.size();
  if (pos != expected) ++checks_.system_event_out_of_order;
  system_events_ += m.event_code;
}

void BookBuilder::on(const itch::StockDirectory& m) {
  see(m.header);
  Instrument& inst = instruments_[m.header.stock_locate];
  inst.listed = true;
  inst.symbol = m.stock;
  inst.round_lot_size = m.round_lot_size;
}

void BookBuilder::on(const itch::StockTradingAction& m) {
  see(m.header);
  if (Instrument* inst = listed(m.header.stock_locate)) {
    if ((m.trading_state == 'H' || m.trading_state == 'P') &&
        inst->trading_state != m.trading_state)
      ++checks_.halts;
    inst->trading_state = m.trading_state;
    inst->halt_reason = m.reason;
  }
}

void BookBuilder::on(const itch::RegShoRestriction& m) {
  see(m.header);
  if (Instrument* inst = listed(m.header.stock_locate)) inst->reg_sho_action = m.reg_sho_action;
}

void BookBuilder::add_order(const itch::Header& h, std::uint64_t ref, char side_char,
                            std::uint32_t shares, const itch::Symbol& stock, std::uint32_t price) {
  see(h);
  Instrument* inst = listed(h.stock_locate);
  if (inst == nullptr) return;
  if (LAB_UNLIKELY(inst->symbol != stock)) ++checks_.locate_symbol_mismatch;
  const Side side = side_char == 'B' ? Side::kBuy : Side::kSell;
  if (LAB_UNLIKELY(!orders_.insert(ref, Order{price, shares, h.stock_locate, side}))) {
    ++checks_.duplicate_order_reference;
    return;
  }
  inst->book.side(side).add(price, shares);
  ++inst->stats.adds;
  check_cross(*inst);
}

void BookBuilder::on(const itch::AddOrder& m) {
  add_order(m.header, m.order_reference_number, m.buy_sell_indicator, m.shares, m.stock, m.price);
}

void BookBuilder::on(const itch::AddOrderMpid& m) {
  add_order(m.header, m.order_reference_number, m.buy_sell_indicator, m.shares, m.stock, m.price);
}

Instrument* BookBuilder::reduce_order(std::uint64_t ref, std::uint32_t shares, bool remove_all,
                                      std::uint32_t* price_out) {
  Order* o = orders_.find(ref);
  if (LAB_UNLIKELY(o == nullptr)) {
    ++checks_.unknown_order_reference;
    return nullptr;
  }
  Instrument& inst = instruments_[o->locate];
  if (price_out != nullptr) *price_out = o->price;
  if (remove_all) shares = o->shares;
  if (LAB_UNLIKELY(shares > o->shares)) {
    ++checks_.over_execution;
    shares = o->shares;  // remove what is there and carry on
  }
  const bool gone = shares == o->shares;
  if (LAB_UNLIKELY(!inst.book.side(o->side).reduce(o->price, shares, gone))) {
    ++checks_.book_inconsistency;
  }
  if (gone) {
    orders_.erase_found(o);
  } else {
    o->shares -= shares;
  }
  check_cross(inst);
  return &inst;
}

void BookBuilder::on(const itch::OrderExecuted& m) {
  see(m.header);
  std::uint32_t price = 0;
  if (Instrument* inst = reduce_order(m.order_reference_number, m.executed_shares, false, &price)) {
    ++inst->stats.executions;
    inst->stats.executed_shares += m.executed_shares;
    traded(*inst, price);  // E executes at the order's own price
  }
}

void BookBuilder::on(const itch::OrderExecutedWithPrice& m) {
  see(m.header);
  if (Instrument* inst =
          reduce_order(m.order_reference_number, m.executed_shares, false, nullptr)) {
    ++inst->stats.executions;
    if (m.printable == 'Y') {
      inst->stats.executed_shares += m.executed_shares;
      traded(*inst, m.execution_price);
    } else {  // part of a cross, printed later by its Q message
      inst->stats.non_printable_shares += m.executed_shares;
      inst->cross_executed += m.executed_shares;
    }
  }
}

void BookBuilder::on(const itch::OrderCancel& m) {
  see(m.header);
  // X pulls the order from the book; Nasdaq sends nothing further for it.
  if (Instrument* inst =
          reduce_order(m.order_reference_number, m.cancelled_shares, true, nullptr)) {
    ++inst->stats.cancels;
  }
}

void BookBuilder::on(const itch::OrderDelete& m) {
  see(m.header);
  if (Instrument* inst = reduce_order(m.order_reference_number, 0, true, nullptr))
    ++inst->stats.deletes;
}

void BookBuilder::on(const itch::OrderReplace& m) {
  see(m.header);
  Order* o = orders_.find(m.original_order_reference_number);
  if (LAB_UNLIKELY(o == nullptr)) {
    ++checks_.unknown_order_reference;
    return;
  }
  const Order original = *o;
  Instrument& inst = instruments_[original.locate];
  if (LAB_UNLIKELY(!inst.book.side(original.side).reduce(original.price, original.shares, true))) {
    ++checks_.book_inconsistency;
  }
  orders_.erase_found(o);
  if (LAB_UNLIKELY(!orders_.insert(m.new_order_reference_number,
                                   Order{m.price, m.shares, original.locate, original.side}))) {
    ++checks_.duplicate_order_reference;
    return;
  }
  inst.book.side(original.side).add(m.price, m.shares);
  ++inst.stats.replaces;
  check_cross(inst);
}

void BookBuilder::on(const itch::Trade& m) {
  see(m.header);
  if (Instrument* inst = listed(m.header.stock_locate)) {
    inst->stats.hidden_trade_shares += m.shares;
    traded(*inst, m.price);
  }
}

void BookBuilder::on(const itch::CrossTrade& m) {
  see(m.header);
  if (Instrument* inst = listed(m.header.stock_locate)) {
    ++checks_.crosses;
    inst->stats.cross_shares += m.shares;
    // Each crossed share executes one buy and one sell order; hidden orders
    // get no C message, so fewer is fine and more is an error.
    if (LAB_UNLIKELY(inst->cross_executed > 2 * m.shares)) ++checks_.cross_overfill;
    inst->cross_executed = 0;
    if (m.shares > 0) {
      inst->stats.last_trade_price = m.cross_price;
      inst->luld_reference = m.cross_price;
    } else if (m.cross_type == 'H') {  // reopened with nothing to cross: the book's midpoint
      const Level* b = inst->book.bids().best();
      const Level* a = inst->book.asks().best();
      if (b && a && b->price < a->price) {
        inst->luld_reference =
            static_cast<std::uint32_t>((std::uint64_t{b->price} + a->price) / 200 * 100);
      }
    }
    if (m.cross_type == 'O') inst->stats.opening_cross_price = m.cross_price;
    if (m.cross_type == 'C') inst->stats.closing_cross_price = m.cross_price;
  }
}

void BookBuilder::on(const itch::BrokenTrade& m) {
  see(m.header);
  if (Instrument* inst = listed(m.header.stock_locate)) ++inst->stats.broken_trades;
}

void BookBuilder::check_cross(Instrument& inst) noexcept {
  // Count transitions into a crossed or locked state, not every message
  // while it lasts.
  const bool crossed = inst.book.crossed();
  const bool locked = inst.book.locked();
  if (crossed && !inst.crossed) ++checks_.crossed_book_events;
  if (locked && !inst.locked) ++checks_.locked_book_events;
  inst.crossed = crossed;
  inst.locked = locked;
}

void BookBuilder::on(const itch::Noii& m) {
  see(m.header);
  ++checks_.noii;
  if (Instrument* inst = listed(m.header.stock_locate)) {
    Imbalance& i = inst->imbalance;
    i.cross_type = m.cross_type;
    i.direction = m.imbalance_direction;
    i.variation = m.price_variation_indicator;
    i.paired = m.paired_shares;
    i.imbalance = m.imbalance_shares;
    i.far_price = m.far_price;
    i.near_price = m.near_price;
    i.reference_price = m.current_reference_price;
    i.timestamp = m.header.timestamp;
    inst->cross_executed = 0;  // an auction is running: count its executions only
  }
}

void BookBuilder::on(const itch::LuldAuctionCollar& m) {
  see(m.header);
  if (Instrument* inst = listed(m.header.stock_locate)) {
    inst->collar =
        Collar{m.reference_price, m.upper_collar_price, m.lower_collar_price, m.collar_extension};
  }
}

void BookBuilder::on(const itch::MwcbDeclineLevel& m) {
  see(m.header);
  mwcb_levels_ = {m.level1, m.level2, m.level3};
}

void BookBuilder::on(const itch::MwcbStatus& m) {
  see(m.header);
  mwcb_breached_ = m.breached_level;
}

void BookBuilder::on(const itch::IpoQuotingPeriod& m) {
  see(m.header);
  if (Instrument* inst = listed(m.header.stock_locate)) {
    inst->ipo_qualifier = m.ipo_quotation_release_qualifier;
    inst->ipo_release_time = m.ipo_quotation_release_time;
    inst->ipo_price = m.ipo_price;
  }
}

void BookBuilder::on(const itch::OperationalHalt& m) {
  see(m.header);
  if (Instrument* inst = listed(m.header.stock_locate)) {
    inst->operational_halt = m.operational_halt_action;
  }
}

}  // namespace lab::book
