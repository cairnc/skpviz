// sched_policy shim — HwcAsyncWorker pins thread priority; we no-op.
#pragma once
#include <sys/types.h>
enum SchedPolicy {
  SP_BACKGROUND = 0,
  SP_FOREGROUND = 1,
  SP_SYSTEM = 2,
  SP_TOP_APP = 4
};
static inline int set_sched_policy(int /*tid*/, int /*policy*/) { return 0; }
static inline int get_sched_policy(int /*tid*/, SchedPolicy * /*out*/) {
  return 0;
}
