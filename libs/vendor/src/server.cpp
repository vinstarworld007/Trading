#include "lab/vendor/server.hpp"

#include <chrono>
#include <ctime>
#include <thread>

#include "lab/core/clock.hpp"

namespace lab::vendor {

std::uint64_t ns_since_midnight() noexcept {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_r(&t, &local);
  const auto since_epoch =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  return (static_cast<std::uint64_t>(local.tm_hour) * 3600 +
          static_cast<std::uint64_t>(local.tm_min) * 60 +
          static_cast<std::uint64_t>(local.tm_sec)) *
             1'000'000'000ULL +
         static_cast<std::uint64_t>(since_epoch % 1'000'000'000);
}

// ---------------------------------------------------------------- Link

void Link::send(std::span<const std::byte> bytes) {
  if (!stream_.send(bytes)) closing_ = true;
  server_->stats_.bytes_out += bytes.size();
}

std::uint32_t Link::subscribe(std::uint16_t locate, std::uint64_t handle,
                              std::uint64_t interval_ns) {
  Server& s = *server_;
  std::uint32_t id;
  if (!s.free_subs_.empty()) {
    id = s.free_subs_.back();
    s.free_subs_.pop_back();
  } else {
    id = static_cast<std::uint32_t>(s.subs_.size());
    s.subs_.emplace_back();
  }
  Subscription& sub = s.subs_[id];
  sub = Subscription{locate, handle, interval_ns, 0, s.now_ns_, id_, true};
  s.by_locate_[locate].push_back(id);
  if (interval_ns != 0) s.conflating_.push_back(id);
  subs_.push_back(id);
  ++s.stats_.subscriptions;
  return id;
}

void Link::unsubscribe(std::uint32_t id) {
  Server& s = *server_;
  if (id >= s.subs_.size() || !s.subs_[id].active || s.subs_[id].link != id_) return;
  Subscription& sub = s.subs_[id];
  auto drop = [id](std::vector<std::uint32_t>& v) {
    for (auto& x : v) {
      if (x == id) {
        x = v.back();
        v.pop_back();
        return;
      }
    }
  };
  drop(s.by_locate_[sub.locate]);
  if (sub.interval_ns != 0) drop(s.conflating_);
  drop(subs_);
  sub.active = false;
  s.free_subs_.push_back(id);
}

const Subscription& Link::subscription(std::uint32_t sub) const { return server_->subs_[sub]; }
const Consolidator& Link::data() const { return server_->data_; }
std::uint64_t Link::now_ns() const { return server_->now_ns_; }

void Link::published(const Record& r) {
  if (r.exch_ns != 0 && server_->now_ns_ > r.exch_ns) {
    server_->stats_.exch_to_publish.record(server_->now_ns_ - r.exch_ns);
  }
}

// ---------------------------------------------------------------- Server

Server::Server(const ServerConfig& cfg, Factory factory)
    : cfg_{cfg},
      factory_{std::move(factory)},
      listener_{net::TcpListener::listen(net::parse_endpoint(cfg.listen))},
      rx_{cfg.feed},
      by_locate_(65536) {
  subs_.reserve(1u << 16);
}

Server::~Server() = default;

std::string Server::address() const { return listener_.local().str(); }

Server::Conn* Server::conn(std::uint32_t id) noexcept {
  for (auto& c : conns_) {
    if (c->link.id_ == id) return c.get();
  }
  return nullptr;
}

void Server::deliver(std::uint32_t sub_id, std::uint32_t mask) {
  Subscription& s = subs_[sub_id];
  Conn* c = conn(s.link);
  if (c == nullptr || c->link.closing_) return;
  s.last_ns = now_ns_;
  ++stats_.updates;
  c->session->on_update(sub_id, s, data_.record(s.locate), mask, c->link);
}

void Server::on_change(const Record& r, std::uint32_t mask) {
  ++stats_.changes;
  for (const std::uint32_t id : by_locate_[r.locate]) {
    Subscription& s = subs_[id];
    if (s.interval_ns == 0) {
      deliver(id, mask);
    } else {
      if (s.pending != 0) ++stats_.conflated;
      s.pending |= mask;
    }
  }
}

void Server::flush_conflated(bool all) {
  for (const std::uint32_t id : conflating_) {
    Subscription& s = subs_[id];
    if (s.pending != 0 && (all || now_ns_ - s.last_ns >= s.interval_ns)) {
      const std::uint32_t mask = s.pending;
      s.pending = 0;
      deliver(id, mask);
    }
  }
}

void Server::accept_new() {
  while (auto s = listener_.accept()) {
    auto c = std::make_unique<Conn>();
    c->link.server_ = this;
    c->link.id_ = next_link_++;
    c->link.stream_ = std::move(*s);
    c->session = factory_();
    c->opened_ns = now_ns_;
    conns_.push_back(std::move(c));
    ++stats_.connections;
  }
}

void Server::serve_links() {
  std::byte buf[16384];
  for (std::size_t i = 0; i < conns_.size();) {
    Conn& c = *conns_[i];
    for (int reads = 0; reads < 8 && !c.link.closing_; ++reads) {
      const ssize_t n = c.link.stream_.recv(buf);
      if (n < 0) {
        c.link.closing_ = true;
        break;
      }
      if (n == 0) break;
      c.session->on_bytes({buf, static_cast<std::size_t>(n)}, c.link);
    }
    c.session->on_timer(now_ns_, c.link);
    const bool sent = c.link.stream_.flush();
    if ((c.link.closing_ && c.link.stream_.queued() == 0) || !sent) {
      while (!c.link.subs_.empty()) c.link.unsubscribe(c.link.subs_.back());
      conns_.erase(conns_.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }
    ++i;
  }
}

void Server::run(const std::atomic<bool>& stop) {
  rx_.start();
  std::uint64_t last_timer = 0;
  std::uint64_t ended_at = 0;
  auto changed = [this](const Record& r, std::uint32_t mask) { on_change(r, mask); };
  while (!stop.load(std::memory_order_relaxed)) {
    now_ns_ = ns_since_midnight();
    int packets = 0;
    while (packets < 64 &&
           rx_.poll([&](std::uint64_t, std::span<const std::byte> msg, std::uint64_t) {
             ++stats_.messages;
             data_.apply(msg, changed);
           })) {
      ++packets;
    }
    if (now_ns_ - last_timer >= 1'000'000 || packets == 0) {
      last_timer = now_ns_;
      accept_new();
      flush_conflated(false);
      serve_links();
    }
    if (packets == 0 && cfg_.feed.idle_sleep_us != 0) {
      std::this_thread::sleep_for(std::chrono::microseconds{cfg_.feed.idle_sleep_us});
    }
    if (!feed_ended_ && rx_.finished()) {
      feed_ended_ = true;
      ended_at = now_ns_;
      flush_conflated(true);
      for (auto& c : conns_) c->session->on_end(c->link);
    }
    if (feed_ended_ && (conns_.empty() || now_ns_ - ended_at > cfg_.linger_s * 1e9)) break;
    if (packets == 0) std::this_thread::sleep_for(std::chrono::microseconds{50});
  }
  for (auto& c : conns_) c->link.stream_.flush();
  rx_.stop();
}

bool Server::report(std::FILE* out) const {
  const auto& ck = data_.books().checks();
  std::fprintf(out,
               "%s: %llu ITCH messages, %llu record changes, %llu updates sent, "
               "%llu merged by conflation\n",
               cfg_.name.c_str(), static_cast<unsigned long long>(stats_.messages),
               static_cast<unsigned long long>(stats_.changes),
               static_cast<unsigned long long>(stats_.updates),
               static_cast<unsigned long long>(stats_.conflated));
  std::fprintf(out, "Clients      %llu connections, %llu subscriptions, %.1f MB sent\n",
               static_cast<unsigned long long>(stats_.connections),
               static_cast<unsigned long long>(stats_.subscriptions),
               static_cast<double>(stats_.bytes_out) / 1e6);
  stats_.exch_to_publish.print(out, "Exchange to vendor out");
  std::fprintf(out, "Books        %llu integrity errors\n",
               static_cast<unsigned long long>(ck.error_count()));
  rx_.print_report(out);
  return ck.error_count() == 0;
}

}  // namespace lab::vendor
