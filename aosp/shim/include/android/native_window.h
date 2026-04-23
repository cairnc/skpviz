// ANativeWindow shim — RenderSurface references it but we never hook a real
// one up in layerviewer (the window is SDL-owned). Opaque forward-decl plus
// enough of the API for RenderSurface.cpp to compile.
#pragma once
#include <cstdint>

extern "C" {

struct ANativeWindow {
  int32_t reserved[16]; // real ANW has function pointers + state; we
                        // never dereference.
};
typedef struct ANativeWindow ANativeWindow;

typedef struct ANativeWindow_Buffer {
  int32_t width, height, stride;
  int32_t format;
  void *bits;
  uint32_t reserved[6];
} ANativeWindow_Buffer;

// API stubs; real impls are in libnativewindow.
static inline int ANativeWindow_getWidth(ANativeWindow *) { return 0; }
static inline int ANativeWindow_getHeight(ANativeWindow *) { return 0; }
static inline int ANativeWindow_getFormat(ANativeWindow *) { return 0; }
static inline void ANativeWindow_acquire(ANativeWindow *) {}
static inline void ANativeWindow_release(ANativeWindow *) {}

} // extern "C"
