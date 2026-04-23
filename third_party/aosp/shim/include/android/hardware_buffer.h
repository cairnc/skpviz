// AHardwareBuffer shim. Real AHB is the opaque gralloc-backed buffer type;
// we forward-declare it so pointer-typed signatures survive but we never
// actually materialize one.
#pragma once
#include <stdint.h>

// Usage bit used by SkiaBackendTexture paths; mirror the NDK constant.
#ifndef AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT
#define AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT (1ULL << 14)
#endif
#ifndef AHARDWAREBUFFER_USAGE_FRONT_BUFFER
#define AHARDWAREBUFFER_USAGE_FRONT_BUFFER (1ULL << 32)
#endif

extern "C" {
typedef struct AHardwareBuffer AHardwareBuffer;

typedef struct AHardwareBuffer_Desc {
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint32_t format;
  uint64_t usage;
  uint32_t stride;
  uint32_t rfu0;
  uint64_t rfu1;
} AHardwareBuffer_Desc;

static inline int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *,
                                           AHardwareBuffer **out) {
  if (out)
    *out = nullptr;
  return -1;
}
static inline void AHardwareBuffer_acquire(AHardwareBuffer *) {}
static inline void AHardwareBuffer_release(AHardwareBuffer *) {}
static inline void AHardwareBuffer_describe(const AHardwareBuffer *,
                                            AHardwareBuffer_Desc *) {}
static inline uint64_t AHardwareBuffer_getId(const AHardwareBuffer *) {
  return 0;
}
}
