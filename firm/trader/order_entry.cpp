#include "order_entry.hpp"

#include <array>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <thread>

#include "lab/core/affinity.hpp"
#include "lab/core/clock.hpp"
#include "lab/core/compiler.hpp"
#include "lab/ouch/codec.hpp"
#include "lab/ouch/messages.hpp"

namespace trader {

namespace ouch = lab::ouch;
namespace soup = lab::soup;
using lab::oms::OrderRequest;
using lab::oms::OrderResponse;

namespace {
constexpr std::size_t kRefMask = (1u << 16) - 1;
}

OrderEntry::OrderEntry(const OrderEntryConfig& cfg, RequestRing& requests, ResponseRing& responses)
    : cfg_{cfg},
      requests_{requests},
      responses_{responses},
      in_{std::make_unique<soup::StreamParser>()},
      sent_tsc_(kRefMask + 1) {}

void OrderEntry::login() {
  const auto to = lab::net::parse_endpoint(cfg_.connect);
  for (int attempt = 0;; ++attempt) {
    try {
      sock_ = lab::net::TcpStream::connect(to);
      break;
    } catch (const std::system_error&) {
      if (attempt == 300) throw;
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
  }
  std::array<std::byte, soup::kLoginRequestSize> buf;
  soup::write_login_request(buf.data(), cfg_.user, cfg_.password, "", 0);  // 0: new messages only
  sock_.send(buf);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
  while (std::chrono::steady_clock::now() < deadline) {
    sock_.flush();
    const auto area = in_->write_area();
    const auto n = sock_.recv(area);
    if (n < 0) throw std::runtime_error("exchange closed the connection during login");
    if (n > 0) in_->commit(static_cast<std::size_t>(n));
    if (auto p = in_->next()) {
      if (p->type == soup::kLoginAccepted) {
        const auto a = soup::parse_login_accepted(p->payload);
        if (!a) throw std::runtime_error("bad Login Accepted");
        next_seq_ = a->next_sequence;
        last_tx_ns_ = lab::mono_ns();
        return;
      }
      if (p->type == soup::kLoginRejected) throw std::runtime_error("login rejected");
    } else if (n == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
  }
  throw std::runtime_error("no answer to login");
}

void OrderEntry::push(const OrderResponse& r) {
  while (LAB_UNLIKELY(!responses_.try_push(r))) {
  }
  ++stats_.responses;
}

void OrderEntry::send(const OrderRequest& r) {
  std::array<std::byte, soup::kHeaderSize + ouch::kMaxMessageSize> pkt;
  std::array<std::byte, ouch::kMaxMessageSize> msg;
  std::size_t n = 0;
  switch (r.type) {
    case OrderRequest::kEnter: {
      ouch::EnterOrder m;
      m.user_ref_num = r.user_ref;
      m.side = r.side;
      m.quantity = r.qty;
      m.symbol = r.symbol;
      m.price = r.price;
      m.time_in_force = r.tif;
      n = ouch::encode(m, msg.data());
      break;
    }
    case OrderRequest::kReplace: {
      ouch::ReplaceOrder m;
      m.orig_user_ref_num = r.orig_ref;
      m.user_ref_num = r.user_ref;
      m.quantity = r.qty;
      m.price = r.price;
      m.time_in_force = r.tif;
      n = ouch::encode(m, msg.data());
      break;
    }
    case OrderRequest::kCancel: {
      ouch::CancelOrder m;
      m.user_ref_num = r.user_ref;
      m.quantity = r.qty;
      n = ouch::encode(m, msg.data());
      break;
    }
    default:
      return;
  }
  const std::size_t len = soup::write_data(pkt.data(), soup::kUnsequencedData, {msg.data(), n});
  sock_.send({pkt.data(), len});
  const std::uint64_t now = lab::rdtsc();
  if (r.tick_tsc != 0) stats_.tick_to_wire.record(now - r.tick_tsc);
  stats_.decide_to_wire.record(now - r.decide_tsc);
  sent_tsc_[r.user_ref & kRefMask] = now;
  last_tx_ns_ = lab::mono_ns();
  ++stats_.requests;
}

void OrderEntry::on_sequenced(std::span<const std::byte> msg, std::uint64_t rx_tsc) {
  ++next_seq_;
  struct H : ouch::HandlerBase {
    using HandlerBase::on;
    OrderResponse r{};
    bool any = false;
    void on(const ouch::OrderAccepted& m) {
      r.type = OrderResponse::kAccepted;
      r.exchange_ns = m.timestamp;
      r.user_ref = m.user_ref_num;
      r.qty = m.quantity;
      r.price = static_cast<std::uint32_t>(m.price);
      r.number = m.order_reference_number;
      any = true;
    }
    void on(const ouch::OrderReplaced& m) {
      r.type = OrderResponse::kReplaced;
      r.exchange_ns = m.timestamp;
      r.user_ref = m.user_ref_num;
      r.orig_ref = m.orig_user_ref_num;
      r.qty = m.quantity;
      r.price = static_cast<std::uint32_t>(m.price);
      r.number = m.order_reference_number;
      any = true;
    }
    void on(const ouch::OrderCanceled& m) {
      r.type = OrderResponse::kCanceled;
      r.exchange_ns = m.timestamp;
      r.user_ref = m.user_ref_num;
      r.qty = m.quantity;
      r.flag = m.reason;
      any = true;
    }
    void on(const ouch::OrderExecuted& m) {
      r.type = OrderResponse::kExecuted;
      r.exchange_ns = m.timestamp;
      r.user_ref = m.user_ref_num;
      r.qty = m.quantity;
      r.price = static_cast<std::uint32_t>(m.price);
      r.number = m.match_number;
      r.flag = m.liquidity_flag;
      any = true;
    }
    void on(const ouch::Rejected& m) {
      r.type = OrderResponse::kRejected;
      r.exchange_ns = m.timestamp;
      r.user_ref = m.user_ref_num;
      r.reason = m.reason;
      any = true;
    }
  } h;
  if (ouch::dispatch_outbound(msg.data(), msg.size(), h) != ouch::DispatchResult::kOk) {
    ++stats_.undecodable;
    return;
  }
  if (!h.any) return;  // system events
  h.r.rx_tsc = rx_tsc;
  std::uint64_t& sent = sent_tsc_[h.r.user_ref & kRefMask];
  if (sent != 0) {
    stats_.round_trip.record(rx_tsc - sent);
    sent = 0;
  }
  push(h.r);
}

void OrderEntry::receive() {
  for (;;) {
    const auto area = in_->write_area();
    const auto n = sock_.recv(area);
    if (n < 0) {
      if (!stats_.session_ended) {
        stats_.connection_lost = true;
        OrderResponse r{};
        r.type = OrderResponse::kSessionEnd;
        push(r);
        stats_.session_ended = true;
      }
      return;
    }
    if (n == 0) return;
    const std::uint64_t rx_tsc = lab::rdtsc();
    in_->commit(static_cast<std::size_t>(n));
    while (auto p = in_->next()) {
      switch (p->type) {
        case soup::kSequencedData:
          on_sequenced(p->payload, rx_tsc);
          break;
        case soup::kServerHeartbeat:
          ++stats_.heartbeats;
          break;
        case soup::kEndOfSession: {
          OrderResponse r{};
          r.type = OrderResponse::kSessionEnd;
          r.rx_tsc = rx_tsc;
          push(r);
          stats_.session_ended = true;
          break;
        }
        default:
          break;
      }
    }
  }
}

void OrderEntry::run(const std::atomic<bool>& stop) {
  lab::set_thread_name("trader-oe");
  if (cfg_.cpu >= 0) (void)lab::pin_this_thread(cfg_.cpu);
  OrderRequest r;
  for (;;) {
    bool idle = true;
    while (requests_.try_pop(r)) {
      if (!stats_.session_ended) send(r);
      idle = false;
    }
    if (!stats_.session_ended) {
      receive();
      sock_.flush();
    }
    if (idle && stop.load(std::memory_order_acquire) && requests_.empty_approx()) break;
    if (!stats_.session_ended && lab::mono_ns() - last_tx_ns_ > 1'000'000'000) {
      std::array<std::byte, soup::kHeaderSize> hb;
      soup::write_empty(hb.data(), soup::kClientHeartbeat);
      sock_.send(hb);
      last_tx_ns_ = lab::mono_ns();
    }
  }
  if (!stats_.session_ended) {
    std::array<std::byte, soup::kHeaderSize> out;
    soup::write_empty(out.data(), soup::kLogoutRequest);
    sock_.send(out);
    for (int i = 0; i < 1000 && sock_.queued() > 0; ++i) sock_.flush();
  }
}

}  // namespace trader
