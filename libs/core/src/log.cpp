#include "lab/core/log.hpp"

#include <cinttypes>
#include <cstring>
#include <ctime>
#include <vector>

#include "lab/core/affinity.hpp"

namespace lab {
namespace {

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO ";
    case LogLevel::kWarn:
      return "WARN ";
    case LogLevel::kError:
      return "ERROR";
  }
  return "?    ";
}

void append_arg(std::string& out, const LogArg& a) {
  char buf[32];
  switch (a.kind) {
    case LogArg::Kind::kI64:
      std::snprintf(buf, sizeof buf, "%" PRId64, a.i);
      out += buf;
      break;
    case LogArg::Kind::kU64:
      std::snprintf(buf, sizeof buf, "%" PRIu64, a.u);
      out += buf;
      break;
    case LogArg::Kind::kF64:
      std::snprintf(buf, sizeof buf, "%g", a.f);
      out += buf;
      break;
    case LogArg::Kind::kStr:
      out += a.s != nullptr ? a.s : "(null)";
      break;
  }
}

}  // namespace

AsyncLogger::AsyncLogger(std::FILE* out) : AsyncLogger(out, TscClock::calibrate()) {}

AsyncLogger::AsyncLogger(std::FILE* out, TscClock clock)
    : out_{out}, clock_{clock}, start_tsc_{rdtsc()}, start_wall_ns_{wall_ns()} {}

AsyncLogger::~AsyncLogger() { stop(); }

LogChannel& AsyncLogger::channel(std::string name) {
  std::lock_guard lock{channels_mu_};
  channels_.push_back(std::make_unique<LogChannel>(std::move(name)));
  return *channels_.back();
}

void AsyncLogger::start(int cpu) {
  if (running_.exchange(true)) return;
  worker_ = std::thread([this, cpu] {
    set_thread_name("lab-logger");
    if (cpu >= 0) pin_this_thread(cpu);
    while (running_.load(std::memory_order_acquire)) {
      if (drain_once() == 0) std::this_thread::sleep_for(std::chrono::microseconds{200});
    }
    drain_once();
  });
}

void AsyncLogger::stop() {
  if (running_.exchange(false)) {
    if (worker_.joinable()) worker_.join();
  }
  flush();
}

std::size_t AsyncLogger::flush() {
  const std::size_t n = drain_once();
  std::fflush(out_);
  return n;
}

std::size_t AsyncLogger::drain_once() {
  std::lock_guard drain_lock{drain_mu_};
  std::vector<LogChannel*> snapshot;
  {
    std::lock_guard lock{channels_mu_};
    snapshot.reserve(channels_.size());
    for (auto& ch : channels_) snapshot.push_back(ch.get());
  }
  std::size_t n = 0;
  for (LogChannel* ch : snapshot) {
    while (const LogRecord* r = ch->ring_.front()) {
      write_record(*ch, *r);
      ch->ring_.pop();
      ++n;
    }
  }
  return n;
}

void AsyncLogger::write_record(const LogChannel& ch, const LogRecord& r) {
  // Records from different channels are written in drain order, not strictly
  // by timestamp; each line carries its own timestamp for sorting.
  const std::uint64_t ns =
      start_wall_ns_ +
      (r.tsc > start_tsc_ ? static_cast<std::uint64_t>(clock_.to_ns(r.tsc - start_tsc_)) : 0);
  const std::time_t secs = static_cast<std::time_t>(ns / 1'000'000'000ULL);
  std::tm tm{};
  gmtime_r(&secs, &tm);

  std::string msg;
  msg.reserve(128);
  int next = 0;
  for (const char* p = r.fmt; *p != '\0'; ++p) {
    if (p[0] == '{' && p[1] == '}' && next < r.nargs) {
      append_arg(msg, r.args[next++]);
      ++p;
    } else {
      msg += *p;
    }
  }

  std::fprintf(out_, "%02d:%02d:%02d.%09llu %s [%s] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
               static_cast<unsigned long long>(ns % 1'000'000'000ULL), level_name(r.level),
               ch.name().c_str(), msg.c_str());
}

}  // namespace lab
