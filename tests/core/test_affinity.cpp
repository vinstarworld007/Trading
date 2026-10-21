#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>

#include <string>
#include <thread>

#include "lab/core/affinity.hpp"

namespace {

TEST(Affinity, ReportsAtLeastOneCpu) { EXPECT_GE(lab::available_cpus(), 1); }

TEST(Affinity, PinsThreadToAllowedCpu) {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  ASSERT_EQ(sched_getaffinity(0, sizeof allowed, &allowed), 0);
  int target = -1;
  for (int c = 0; c < CPU_SETSIZE; ++c) {
    if (CPU_ISSET(c, &allowed)) target = c;  // last allowed CPU
  }
  ASSERT_GE(target, 0);

  std::error_code ec;
  int ran_on = -2;
  std::thread t([&] {
    ec = lab::pin_this_thread(target);
    ran_on = lab::current_cpu();
  });
  t.join();
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_EQ(ran_on, target);
}

TEST(Affinity, RejectsInvalidCpu) {
  EXPECT_TRUE(lab::pin_this_thread(-1));
  EXPECT_TRUE(lab::pin_this_thread(CPU_SETSIZE));
}

TEST(Affinity, NamesThread) {
  std::string name;
  std::thread t([&] {
    lab::set_thread_name("a-very-long-thread-name");
    char buf[16] = {};
    pthread_getname_np(pthread_self(), buf, sizeof buf);
    name = buf;
  });
  t.join();
  EXPECT_EQ(name, "a-very-long-thr");
}

}  // namespace
