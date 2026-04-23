// SF-internal trace wrapper. Layerviewer code often reaches for CC_LIKELY /
// CC_UNLIKELY, FlagManager, gralloc usage flags, Skia's framework trace util,
// and macOS-compat scheduling shims alongside tracing; pull them all in here
// so the transitive surface matches real AOSP without editing SF/RE sources.
#pragma once
#include <SkAndroidFrameworkTraceUtil.h>
#include <common/FlagManager.h>
#include <cutils/compiler.h>
#include <cutils/trace.h>
#include <log/log.h>                   // LOG_ALWAYS_FATAL etc.
#include <processgroup/sched_policy.h> // macOS pthread_setname_np + sched stubs
#include <unistd.h>                    // usleep

#define SFTRACE_ENABLED() 0

#define SFTRACE_CALL() ((void)0)
#define SFTRACE_NAME(name) ((void)0)
#define SFTRACE_INT(name, value) ((void)0)
#define SFTRACE_INT64(name, value) ((void)0)
#define SFTRACE_FORMAT(fmt, ...) ((void)0)
#define SFTRACE_FORMAT_INSTANT(fmt, ...) ((void)0)
#define SFTRACE_INSTANT(name) ((void)0)
#define SFTRACE_ASYNC_BEGIN(name, cookie) ((void)0)
#define SFTRACE_ASYNC_END(name, cookie) ((void)0)
#define SFTRACE_ASYNC_FOR_TRACK_BEGIN(track, name, cookie) ((void)0)
#define SFTRACE_ASYNC_FOR_TRACK_END(track, cookie) ((void)0)
#define SFTRACE_BEGIN(name) ((void)0)
#define SFTRACE_END() ((void)0)
