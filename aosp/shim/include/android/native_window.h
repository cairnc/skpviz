// ANativeWindow shim — layerviewer doesn't own a real ANativeWindow since
// our composition target is a caller-owned SkSurface. We provide enough of
// the struct surface that CE's RenderSurface.cpp compiles: function-pointer
// fields for dequeue/queue/cancel, RefBase-style incStrong/decStrong, and a
// companion ANativeWindowBuffer.
#pragma once
#include <cstddef>
#include <cstdint>

extern "C" {

struct ANativeWindowBuffer;
struct ANativeWindow;

typedef struct ANativeWindowBuffer {
  int32_t width = 0;
  int32_t height = 0;
  int32_t stride = 0;
  int32_t format = 0;
  int32_t usage = 0;
  const struct native_handle *handle = nullptr;
} ANativeWindowBuffer_t;

struct ANativeWindow {
  // Function-pointer API CE touches — all nullable, never called in
  // layerviewer since Output doesn't drive a real RenderSurface.
  int (*dequeueBuffer)(struct ANativeWindow *, ANativeWindowBuffer **,
                       int *) = nullptr;
  int (*queueBuffer)(struct ANativeWindow *, ANativeWindowBuffer *,
                     int) = nullptr;
  int (*cancelBuffer)(struct ANativeWindow *, ANativeWindowBuffer *,
                      int) = nullptr;

  // RefBase-compatible counters so sp<ANativeWindow> works via libutils.
  void incStrong(const void *) const {}
  void decStrong(const void *) const {}
};
typedef struct ANativeWindow ANativeWindow;

typedef struct ANativeWindow_Buffer {
  int32_t width, height, stride;
  int32_t format;
  void *bits;
  uint32_t reserved[6];
} ANativeWindow_Buffer;

static inline int ANativeWindow_getWidth(ANativeWindow *) { return 0; }
static inline int ANativeWindow_getHeight(ANativeWindow *) { return 0; }
static inline int ANativeWindow_getFormat(ANativeWindow *) { return 0; }
static inline void ANativeWindow_acquire(ANativeWindow *) {}
static inline void ANativeWindow_release(ANativeWindow *) {}

} // extern "C"
