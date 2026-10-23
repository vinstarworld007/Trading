#pragma once

// Asynchronous logger for hot threads.
//
// A hot thread never formats or writes. It copies a pointer to a string
// literal plus up to six numeric or string-literal arguments into its own
// LogChannel (an SPSC ring) and returns. A background thread drains every
// channel, formats "{}" placeholders and writes the lines. If a channel is
// full the record is dropped and counted rather than blocking the caller.
//
//   lab::AsyncLogger logger{stderr};
//   auto& ch = logger.channel("itch-fh");
//   logger.start();
//   ch.info("gap on session {} from seq {} to {}", "A", 1001, 1042);
//
// String arguments must be literals or otherwise outlive the logger: only the
// pointer is copied.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>

#include "lab/core/clock.hpp"
#include "lab/core/compiler.hpp"
#include "lab/core/spsc_ring.hpp"

namespace lab {

enum class LogLevel : std::uint8_t { kDebug, kInfo, kWarn, kError };

struct LogArg {
  enum class Kind : std::uint8_t { kI64, kU64, kF64, kStr };
  Kind kind;
  union {
    std::int64_t i;
    std::uint64_t u;
    double f;
    const char* s;
  };
};

struct LogRecord {
  static constexpr int kMaxArgs = 6;
  std::uint64_t tsc;
  const char* fmt;
  LogLevel level;
  std::uint8_t nargs;
  LogArg args[kMaxArgs];
};

namespace detail {

template <typename A>
LAB_ALWAYS_INLINE LogArg to_log_arg(A a) noexcept {
  using D = std::decay_t<A>;
  LogArg r{};
  if constexpr (std::is_same_v<D, bool>) {
    r.kind = LogArg::Kind::kU64;
    r.u = a ? 1 : 0;
  } else if constexpr (std::is_integral_v<D> && std::is_signed_v<D>) {
    r.kind = LogArg::Kind::kI64;
    r.i = static_cast<std::int64_t>(a);
  } else if constexpr (std::is_integral_v<D> || std::is_enum_v<D>) {
    r.kind = LogArg::Kind::kU64;
    r.u = static_cast<std::uint64_t>(a);
  } else if constexpr (std::is_floating_point_v<D>) {
    r.kind = LogArg::Kind::kF64;
    r.f = static_cast<double>(a);
  } else if constexpr (std::is_same_v<D, const char*> || std::is_same_v<D, char*>) {
    r.kind = LogArg::Kind::kStr;
    r.s = a;
  } else {
    static_assert(sizeof(D) == 0, "log arguments must be numbers, enums or string literals");
  }
  return r;
}

}  // namespace detail

class LogChannel {
 public:
  static constexpr std::size_t kCapacity = 8192;  // about 1 MB per channel

  explicit LogChannel(std::string name) : name_{std::move(name)} {}

  template <typename... A>
  LAB_ALWAYS_INLINE bool log(LogLevel level, const char* fmt, A... args) noexcept {
    static_assert(sizeof...(A) <= LogRecord::kMaxArgs, "too many log arguments");
    LogRecord* r = ring_.try_claim();
    if (LAB_UNLIKELY(r == nullptr)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    r->tsc = rdtsc();
    r->fmt = fmt;
    r->level = level;
    r->nargs = static_cast<std::uint8_t>(sizeof...(A));
    [[maybe_unused]] int i = 0;
    ((r->args[i++] = detail::to_log_arg(args)), ...);
    ring_.commit();
    return true;
  }

  template <typename... A>
  bool debug(const char* fmt, A... a) noexcept {
    return log(LogLevel::kDebug, fmt, a...);
  }
  template <typename... A>
  bool info(const char* fmt, A... a) noexcept {
    return log(LogLevel::kInfo, fmt, a...);
  }
  template <typename... A>
  bool warn(const char* fmt, A... a) noexcept {
    return log(LogLevel::kWarn, fmt, a...);
  }
  template <typename... A>
  bool error(const char* fmt, A... a) noexcept {
    return log(LogLevel::kError, fmt, a...);
  }

  const std::string& name() const noexcept { return name_; }
  std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

 private:
  friend class AsyncLogger;
  std::string name_;
  std::atomic<std::uint64_t> dropped_{0};
  SpscRing<LogRecord, kCapacity> ring_;
};

class AsyncLogger {
 public:
  // `out` must stay open for the logger's lifetime. `clock` converts record
  // timestamps; pass a calibrated clock to avoid a 50 ms calibration here.
  explicit AsyncLogger(std::FILE* out);
  AsyncLogger(std::FILE* out, TscClock clock);
  ~AsyncLogger();

  AsyncLogger(const AsyncLogger&) = delete;
  AsyncLogger& operator=(const AsyncLogger&) = delete;

  // Creates a channel for one producer thread. Safe to call before or after
  // start(); the reference stays valid for the logger's lifetime.
  LogChannel& channel(std::string name);

  // Starts the background writer, optionally pinned to `cpu` (-1 = unpinned).
  void start(int cpu = -1);

  // Drains everything already logged, then stops the writer. Idempotent.
  void stop();

  // Drains and writes all pending records on the calling thread. Useful in
  // tests and single-threaded tools that never call start().
  std::size_t flush();

 private:
  std::size_t drain_once();
  void write_record(const LogChannel& ch, const LogRecord& r);

  std::FILE* out_;
  TscClock clock_;
  std::uint64_t start_tsc_;
  std::uint64_t start_wall_ns_;

  std::mutex channels_mu_;
  std::deque<std::unique_ptr<LogChannel>> channels_;

  std::mutex drain_mu_;
  std::atomic<bool> running_{false};
  std::thread worker_;
};

}  // namespace lab
