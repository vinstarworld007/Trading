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
  if (Instrument* inst = listed(m.header.stock_locate)) inst->trading_state = m.trading_state;
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
    inst->stats.last_trade_price = price;  // E executes at the order's own price
  }
}

void BookBuilder::on(const itch::OrderExecutedWithPrice& m) {
  see(m.header);
  if (Instrument* inst =
          reduce_order(m.order_reference_number, m.executed_shares, false, nullptr)) {
    ++inst->stats.executions;
    if (m.printable == 'Y') {
      inst->stats.executed_shares += m.executed_shares;
      inst->stats.last_trade_price = m.execution_price;
    } else {
      inst->stats.non_printable_shares += m.executed_shares;
    }
  }
}

void BookBuilder::on(const itch::OrderCancel& m) {
  see(m.header);
  if (Instrument* inst =
          reduce_order(m.order_reference_number, m.cancelled_shares, false, nullptr)) {
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
    inst->stats.last_trade_price = m.price;
  }
}

void BookBuilder::on(const itch::CrossTrade& m) {
  see(m.header);
  if (Instrument* inst = listed(m.header.stock_locate)) {
    inst->stats.cross_shares += m.shares;
    if (m.shares > 0) inst->stats.last_trade_price = m.cross_price;
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

}  // namespace lab::book
