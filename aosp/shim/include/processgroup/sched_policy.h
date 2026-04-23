// sched_policy shim — Linux scheduling APIs don't exist on macOS. Map the
// calls AOSP sources use to no-ops / equivalent behaviour there.
#pragma once
#include <pthread.h>
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

#if defined(__APPLE__)
// macOS variants: pthread_setname_np takes only the name (applies to current
// thread); there is no gettid / sched_setscheduler. Provide shims that accept
// the Linux call shape but behave as no-ops.
#include <stddef.h>

static inline int lv_pthread_setname_np(pthread_t /*handle*/,
                                        const char *name) {
  return pthread_setname_np(name);
}
#define pthread_setname_np lv_pthread_setname_np

static inline int gettid() { return 0; }

// sched_setscheduler prototype without the Linux-only SCHED_* definitions.
#ifndef SCHED_FIFO
#define SCHED_FIFO 1
#endif
struct sched_param;
static inline int sched_setscheduler(int /*tid*/, int /*policy*/,
                                     const struct sched_param * /*param*/) {
  return 0;
}
#endif
