// ATRACE shim — all tracing calls are no-ops.
#pragma once
#include <stdint.h>

#define ATRACE_TAG 0
#define ATRACE_TAG_GRAPHICS 0
#define ATRACE_TAG_VIEW 0

#define ATRACE_ENABLED() 0
#define ATRACE_CATEGORY_IS_ENABLED(cat) 0

static inline void atrace_begin(uint64_t /*tag*/, const char * /*name*/)
{
}
static inline void atrace_end(uint64_t /*tag*/)
{
}
static inline void atrace_async_begin(uint64_t, const char *, int32_t)
{
}
static inline void atrace_async_end(uint64_t, const char *, int32_t)
{
}
static inline void atrace_async_for_track_begin(uint64_t, const char *,
                                                const char *, int32_t)
{
}
static inline void atrace_async_for_track_end(uint64_t, const char *, int32_t)
{
}
static inline void atrace_instant(uint64_t, const char *)
{
}
static inline void atrace_instant_for_track(uint64_t, const char *,
                                            const char *)
{
}
static inline void atrace_int(uint64_t, const char *, int32_t)
{
}
static inline void atrace_int64(uint64_t, const char *, int64_t)
{
}
static inline void atrace_set_debuggable(bool)
{
}
static inline void atrace_set_tracing_enabled(bool)
{
}
static inline void atrace_setup()
{
}
static inline void atrace_update_tags()
{
}
static inline uint64_t atrace_get_enabled_tags()
{
    return 0;
}

#define ATRACE_BEGIN(name) ((void)0)
#define ATRACE_END() ((void)0)
#define ATRACE_CALL() ((void)0)
#define ATRACE_NAME(name) ((void)0)
#define ATRACE_INT(name, v) ((void)0)
#define ATRACE_INT64(name, v) ((void)0)
#define ATRACE_ASYNC_BEGIN(name, cookie) ((void)0)
#define ATRACE_ASYNC_END(name, cookie) ((void)0)
#define ATRACE_FORMAT(fmt, ...) ((void)0)
#define ATRACE_FORMAT_BEGIN(fmt, ...) ((void)0)
#define ATRACE_FORMAT_INSTANT(fmt, ...) ((void)0)
#define ATRACE_INSTANT(name) ((void)0)
#define ATRACE_INSTANT_FOR_TRACK(track, name) ((void)0)
