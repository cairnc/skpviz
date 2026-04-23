// Minimal shim for Android liblog. All logging collapses to fprintf(stderr).
// Covers the macros used by AOSP code we port; extend as needed.
#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef LOG_TAG
#define LOG_TAG "layerviewer"
#endif

enum {
  ANDROID_LOG_UNKNOWN = 0,
  ANDROID_LOG_DEFAULT,
  ANDROID_LOG_VERBOSE,
  ANDROID_LOG_DEBUG,
  ANDROID_LOG_INFO,
  ANDROID_LOG_WARN,
  ANDROID_LOG_ERROR,
  ANDROID_LOG_FATAL,
  ANDROID_LOG_SILENT,
};

typedef int android_LogPriority;

#ifdef __cplusplus
extern "C" {
#endif

// Minimum log level printed. RefBase.cpp emits a steady stream of ALOGW
// whenever a stack-allocated RefBase subclass is destroyed (common in our
// shim where we fake SurfaceControls / BBinders for the replay pipeline),
// so default to ERROR. Override via LAYERVIEWER_LOG_LEVEL=V/D/I/W/E/F.
static inline int __layerviewer_min_log_level(void) {
  static int cached = -1;
  if (cached >= 0)
    return cached;
  const char *env = getenv("LAYERVIEWER_LOG_LEVEL");
  int lvl = 6; // ERROR
  if (env && env[0]) {
    switch (env[0]) {
    case 'V':
    case 'v':
      lvl = 2;
      break;
    case 'D':
    case 'd':
      lvl = 3;
      break;
    case 'I':
    case 'i':
      lvl = 4;
      break;
    case 'W':
    case 'w':
      lvl = 5;
      break;
    case 'E':
    case 'e':
      lvl = 6;
      break;
    case 'F':
    case 'f':
      lvl = 7;
      break;
    }
  }
  cached = lvl;
  return cached;
}

static inline int __android_log_print(int prio, const char *tag,
                                      const char *fmt, ...) {
  if (prio < __layerviewer_min_log_level())
    return 0;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[%s] ", tag ? tag : "?");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  return 0;
}

static inline int __android_log_write(int prio, const char *tag,
                                      const char *msg) {
  if (prio < __layerviewer_min_log_level())
    return 0;
  fprintf(stderr, "[%s] %s\n", tag ? tag : "?", msg ? msg : "");
  return 0;
}

static inline int __android_log_assert(const char * /*cond*/, const char *tag,
                                       const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[%s][FATAL] ", tag ? tag : "?");
  if (fmt)
    vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  __builtin_trap();
}

#ifdef __cplusplus
}
#endif

#define ALOG(priority, tag, fmt, ...)                                          \
  __android_log_print(priority, tag, fmt, ##__VA_ARGS__)

#define ALOGV(...)                                                             \
  ((void)__android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__))
#define ALOGD(...)                                                             \
  ((void)__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#define ALOGI(...)                                                             \
  ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define ALOGW(...)                                                             \
  ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define ALOGE(...)                                                             \
  ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))
#define ALOGF(...)                                                             \
  ((void)__android_log_print(ANDROID_LOG_FATAL, LOG_TAG, __VA_ARGS__))

#define ALOGV_IF(cond, ...) ((void)((cond) ? ALOGV(__VA_ARGS__) : (void)0))
#define ALOGD_IF(cond, ...) ((void)((cond) ? ALOGD(__VA_ARGS__) : (void)0))
#define ALOGI_IF(cond, ...) ((void)((cond) ? ALOGI(__VA_ARGS__) : (void)0))
#define ALOGW_IF(cond, ...) ((void)((cond) ? ALOGW(__VA_ARGS__) : (void)0))
#define ALOGE_IF(cond, ...) ((void)((cond) ? ALOGE(__VA_ARGS__) : (void)0))

#define IF_ALOGV() if (false)
#define IF_ALOGD() if (false)
#define IF_ALOGI() if (false)

// These macros must accept both `(cond)` and `(cond, fmt, ...)` — the fmt is
// optional in the AOSP originals. We route all variants through a helper that
// tolerates a NULL format.
#ifdef __cplusplus
#include <cstddef>
static inline void __layerviewer_fatal(const char *cond, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[FATAL] %s: ", cond ? cond : "");
  if (fmt)
    vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  __builtin_trap();
}
#endif

#define ALOG_ASSERT(cond, ...)                                                 \
  ((cond) ? (void)0 : __layerviewer_fatal(#cond, "" __VA_ARGS__))
#define LOG_ALWAYS_FATAL(...) __layerviewer_fatal(nullptr, "" __VA_ARGS__)
#define LOG_ALWAYS_FATAL_IF(cond, ...)                                         \
  ((cond) ? __layerviewer_fatal(#cond, "" __VA_ARGS__) : (void)0)
#define LOG_FATAL(...) LOG_ALWAYS_FATAL(__VA_ARGS__)
#define LOG_FATAL_IF(cond, ...) LOG_ALWAYS_FATAL_IF(cond, __VA_ARGS__)

#define android_errorWriteLog(tag, subtag) ((void)0)
#define android_errorWriteWithInfoLog(tag, subtag, uid, data, len) ((void)0)
