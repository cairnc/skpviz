// GraphicBuffer — minimal, GL-backed, layerviewer-specific.
//
// On Android, GraphicBuffer is literally AHardwareBuffer with compat shims on
// top. AHB abstracts gralloc-allocated, EGLImage-imported GPU buffers that
// RenderEngine imports as GL textures via GrAHardwareBufferUtils::
// MakeGLBackendTexture().
//
// For layerviewer we skip the gralloc/EGLImage dance and back each
// GraphicBuffer directly with a GL 2D texture (created lazily the first time
// RE asks for a backend texture). toAHardwareBuffer() casts `this` to the
// opaque AHB pointer; our shim GrAHardwareBufferUtils casts it back to reach
// mTextureId. Imports and exports are reinterpret_casts.

#pragma once

#include <cstdint>
#include <string>

#include <hardware/gralloc.h> // GRALLOC_USAGE_* — matches upstream include chain
#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <utils/RefBase.h>

// AHB is an opaque forward-declared struct throughout the Android headers we
// port; we reinterpret_cast between AHardwareBuffer* and GraphicBuffer* inside
// our shim.
extern "C" {
typedef struct AHardwareBuffer AHardwareBuffer;
}

namespace android {

class GraphicBuffer : public RefBase {
public:
  enum : uint64_t {
    USAGE_SW_READ_OFTEN = 0x03ULL,
    USAGE_SW_WRITE_OFTEN = 0x30ULL,
    USAGE_SOFTWARE_MASK = 0xFFULL,
    USAGE_PROTECTED = 1ULL << 14,
    USAGE_HW_TEXTURE = 1ULL << 8,
    USAGE_HW_RENDER = 1ULL << 9,
    USAGE_HW_COMPOSER = 1ULL << 11,
    USAGE_HW_VIDEO_ENCODER = 1ULL << 16,
    USAGE_HW_MASK = 0xFFFF00ULL,
    USAGE_CURSOR = 1ULL << 15,
  };

  GraphicBuffer() = default;
  GraphicBuffer(uint32_t w, uint32_t h, PixelFormat format, uint32_t layerCount,
                uint64_t usage, std::string requestorName = {})
      : mWidth(w), mHeight(h), mFormat(format), mLayerCount(layerCount),
        mUsage(usage), mRequestor(std::move(requestorName)) {
    mId = sNextId++;
  }
  ~GraphicBuffer() override; // defined in GraphicBuffer.cpp; frees GL tex

  uint32_t getWidth() const { return mWidth; }
  uint32_t getHeight() const { return mHeight; }
  uint32_t getStride() const { return mWidth; }
  PixelFormat getPixelFormat() const { return mFormat; }
  uint64_t getUsage() const { return mUsage; }
  uint32_t getLayerCount() const { return mLayerCount; }
  uint64_t getId() const { return mId; }
  uint64_t getUniqueId() const { return mId; }
  status_t initCheck() const { return OK; }
  const std::string &getRequestorName() const { return mRequestor; }
  Rect getBounds() const {
    return Rect(0, 0, static_cast<int32_t>(mWidth),
                static_cast<int32_t>(mHeight));
  }

  // Gralloc handle accessors; stubbed out (no real gralloc).
  const struct native_handle *handle = nullptr;

  // ANativeWindowBuffer facade — real AOSP has GraphicBuffer inherit from
  // ANativeObjectBase<ANativeWindowBuffer,...>. Our stub returns nullptr
  // since RenderSurface.cpp paths aren't actually exercised.
  struct ANativeWindowBuffer *getNativeBuffer() const { return nullptr; }
  static GraphicBuffer *from(struct ANativeWindowBuffer *) { return nullptr; }

  // GL texture name. Allocated on first call; subsequent calls return the
  // same id. The current GL context must be current when this is called.
  unsigned int getOrCreateGLTexture();
  unsigned int getGLTextureIfAny() const { return mTextureId; }

  // GraphicBuffer IS AHardwareBuffer on Android (AHB is the public
  // type alias for the same gralloc handle). Our GraphicBuffer isn't
  // layout-compatible with the ANativeWindowBuffer+AHB base struct, but
  // since nothing in our port dereferences the AHB* — it's only ever
  // passed back into GrAHardwareBufferUtils (which we own) — an opaque
  // round-trip via reinterpret_cast is enough.
  AHardwareBuffer *toAHardwareBuffer() {
    return reinterpret_cast<AHardwareBuffer *>(this);
  }
  const AHardwareBuffer *toAHardwareBuffer() const {
    return reinterpret_cast<const AHardwareBuffer *>(this);
  }
  static GraphicBuffer *fromAHardwareBuffer(AHardwareBuffer *ahb) {
    return reinterpret_cast<GraphicBuffer *>(ahb);
  }
  static const GraphicBuffer *fromAHardwareBuffer(const AHardwareBuffer *ahb) {
    return reinterpret_cast<const GraphicBuffer *>(ahb);
  }

private:
  static inline uint64_t sNextId = 1;
  uint32_t mWidth = 0;
  uint32_t mHeight = 0;
  PixelFormat mFormat = 0;
  uint32_t mLayerCount = 1;
  uint64_t mUsage = 0;
  uint64_t mId = 0;
  std::string mRequestor;
  unsigned int mTextureId = 0; // GLuint
};

} // namespace android
