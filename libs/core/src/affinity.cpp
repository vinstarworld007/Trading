#include "lab/core/affinity.hpp"

#include <pthread.h>
#include <sched.h>

#include <cerrno>
#include <string>

namespace lab {

std::error_code pin_this_thread(int cpu) noexcept {
  if (cpu < 0 || cpu >= CPU_SETSIZE) return std::make_error_code(std::errc::invalid_argument);
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  const int rc = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
  return rc == 0 ? std::error_code{} : std::error_code{rc, std::system_category()};
}

int current_cpu() noexcept { return sched_getcpu(); }

int available_cpus() noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set) != 0) return 1;
  return CPU_COUNT(&set);
}

void set_thread_name(std::string_view name) noexcept {
  char buf[16] = {};
  name.copy(buf, sizeof buf - 1);
  pthread_setname_np(pthread_self(), buf);
}

std::error_code set_realtime_priority(int priority) noexcept {
  sched_param param{};
  param.sched_priority = priority;
  const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
  return rc == 0 ? std::error_code{} : std::error_code{rc, std::system_category()};
}

}  // namespace lab
