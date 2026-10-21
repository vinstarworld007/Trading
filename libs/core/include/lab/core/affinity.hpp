#pragma once

// Thread placement. Each hot thread in the lab is pinned to its own core and
// busy-polls; see docs/core-plan.md for the default layout.

#include <string_view>
#include <system_error>

namespace lab {

// Pins the calling thread to one CPU.
std::error_code pin_this_thread(int cpu) noexcept;

// CPU the calling thread is running on right now, or -1 if unknown.
int current_cpu() noexcept;

// Number of CPUs this process may run on.
int available_cpus() noexcept;

// Names the calling thread (visible in top -H, perf, gdb). Truncated to 15 chars.
void set_thread_name(std::string_view name) noexcept;

// Switches the calling thread to SCHED_FIFO at `priority`. Needs CAP_SYS_NICE
// or root; returns the error otherwise. Use with care: a spinning FIFO thread
// on a shared core can starve the OS.
std::error_code set_realtime_priority(int priority) noexcept;

}  // namespace lab
