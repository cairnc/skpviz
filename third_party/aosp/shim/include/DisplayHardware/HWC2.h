// HWC2 shim. Real HWC2 has Display/Layer/Client classes driving HAL. For
// layerviewer all we need is an opaque Layer type — any identity-comparable
// object works; we use an empty class.
#pragma once
#include "Hal.h"
#include <aidl/android/hardware/graphics/composer3/Color.h>
#include <aidl/android/hardware/graphics/composer3/Luts.h>
#include <android/binder_auto_utils.h>
#include <cstdint>
#include <cutils/native_handle.h>
#include <gui/HdrMetadata.h>
#include <math/mat4.h>
#include <string>
#include <ui/FloatRect.h>
#include <ui/GraphicTypes.h>
#include <ui/PictureProfileHandle.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <utils/RefBase.h>
#include <vector>

namespace android {

namespace Hwc2 {
// Composer is a fully-defined no-op so nested-name lookups (Hwc2::Composer::X)
// work where needed; we never actually call through to a real HAL composer.
class Composer {
public:
  struct DisplayBrightnessOptions {
    bool applyImmediately = false;
  };
  struct LayerGenericMetadataKey {
    std::string name;
    bool mandatory = false;
  };
  virtual ~Composer() = default;
};
class IComposerClient {
public:
  using LayerRequest =
      ::android::hardware::graphics::composer::hal::LayerRequest;
  using Transform = ::android::hardware::graphics::composer::hal::Transform;
  using BlendMode = ::aidl::android::hardware::graphics::common::BlendMode;
  using DisplayRequest =
      ::android::hardware::graphics::composer::hal::DisplayRequest;
};
using Transform = ::android::hardware::graphics::composer::hal::Transform;
} // namespace Hwc2

// ndk::ScopedAStatus / ScopedFileDescriptor live in
// <android/binder_auto_utils.h> — separated so Luts.h can include them without
// a cycle through HWC2.h.

class GraphicBuffer;
class Fence;

namespace HWC2 {

class Layer {
public:
  virtual ~Layer() = default;
  uint64_t getId() const { return reinterpret_cast<uintptr_t>(this); }

  // Methods OutputLayer.cpp calls — all stubs returning OK / no-ops since
  // we never talk to real HWC.
  using Error = ::android::hardware::graphics::composer::hal::Error;
  Error setCursorPosition(int, int) { return Error::NONE; }
  Error setBuffer(uint32_t, const sp<GraphicBuffer> &, const sp<Fence> &) {
    return Error::NONE;
  }
  Error setSurfaceDamage(const Region &) { return Error::NONE; }
  Error setBlendMode(::aidl::android::hardware::graphics::common::BlendMode) {
    return Error::NONE;
  }
  Error
  setColor(const ::aidl::android::hardware::graphics::composer3::Color &) {
    return Error::NONE;
  }
  Error setCompositionType(
      ::aidl::android::hardware::graphics::composer3::Composition) {
    return Error::NONE;
  }
  Error setDataspace(ui::Dataspace) { return Error::NONE; }
  Error setPerFrameMetadata(int32_t, const HdrMetadata &) {
    return Error::NONE;
  }
  Error setDisplayFrame(const Rect &) { return Error::NONE; }
  Error setPlaneAlpha(float) { return Error::NONE; }
  Error setSidebandStream(const native_handle_t *) { return Error::NONE; }
  Error setSourceCrop(const FloatRect &) { return Error::NONE; }
  Error setTransform(::android::hardware::graphics::composer::hal::Transform) {
    return Error::NONE;
  }
  Error setVisibleRegion(const Region &) { return Error::NONE; }
  Error setZOrder(uint32_t) { return Error::NONE; }
  Error setColorTransform(const mat4 &) { return Error::NONE; }
  Error setBlockingRegion(const Region &) { return Error::NONE; }
  Error setBrightness(float) { return Error::NONE; }
  Error setBufferInt(uint32_t, const sp<GraphicBuffer> &, const sp<Fence> &) {
    return Error::NONE;
  }
  Error setLuts(const ::aidl::android::hardware::graphics::composer3::Luts &) {
    return Error::NONE;
  }
  Error setPictureProfileHandle(const PictureProfileHandle &) {
    return Error::NONE;
  }
  Error setLayerGenericMetadata(const std::string & /*name*/,
                                bool /*mandatory*/,
                                const std::vector<uint8_t> & /*value*/) {
    return Error::NONE;
  }
  Error setBufferSlotsToClear(const std::vector<uint32_t> & /*slots*/,
                              uint32_t /*activeBufferSlot*/) {
    return Error::NONE;
  }
};

using DisplayRequest =
    ::android::hardware::graphics::composer::hal::DisplayRequest;

} // namespace HWC2
} // namespace android
