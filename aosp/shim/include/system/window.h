// system/window.h shim — RenderSurface pulls in native_window definitions.
// Everything collapses to our opaque ANativeWindow stub; layerviewer never
// materializes a real one.
#pragma once
#include <android/native_window.h>
#include <hardware/gralloc.h>
#define NATIVE_WINDOW_API_CPU 2
#define NATIVE_WINDOW_API_EGL 1
#define NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS 7
static inline int native_window_api_connect(ANativeWindow *, int) { return 0; }
static inline int native_window_api_disconnect(ANativeWindow *, int) {
  return 0;
}
static inline int native_window_set_usage(ANativeWindow *, uint64_t) {
  return 0;
}
static inline int native_window_set_buffer_count(ANativeWindow *, size_t) {
  return 0;
}
static inline int native_window_set_buffers_format(ANativeWindow *, int) {
  return 0;
}
static inline int native_window_set_buffers_dimensions(ANativeWindow *, int,
                                                       int) {
  return 0;
}
static inline int native_window_set_buffers_data_space(ANativeWindow *, int) {
  return 0;
}
static inline int native_window_set_scaling_mode(ANativeWindow *, int) {
  return 0;
}
static inline int native_window_get_wide_color_support(ANativeWindow *,
                                                       bool *out) {
  if (out)
    *out = false;
  return 0;
}
static inline int native_window_get_hdr_support(ANativeWindow *, bool *out) {
  if (out)
    *out = false;
  return 0;
}
