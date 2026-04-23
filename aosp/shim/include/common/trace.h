// SF-internal trace wrapper. Layerviewer code often reaches for CC_LIKELY /
// CC_UNLIKELY and FlagManager alongside tracing; include them here so the
// transitive surface matches real AOSP without editing CE sources.
#pragma once
#include <common/FlagManager.h>
#include <cutils/compiler.h>
#include <cutils/trace.h>

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
