// HWComposer shim. Layerviewer has no HWC backing — every layer is composed
// via RenderEngine (CLIENT composition). Every method fatal-stubs (for ones
// that shouldn't be called on our CLIENT-only path) or returns a neutral
// "no changes" response. Enough structure to satisfy CompositionEngine's
// compile-time references.

#pragma once

#include "HWC2.h"
#include "Hal.h"

#include <aidl/android/hardware/graphics/common/DisplayDecorationSupport.h>
#include <aidl/android/hardware/graphics/common/Hdr.h>
#include <aidl/android/hardware/graphics/common/HdrConversionCapability.h>
#include <aidl/android/hardware/graphics/common/HdrConversionStrategy.h>
#include <aidl/android/hardware/graphics/composer3/Capability.h>
#include <aidl/android/hardware/graphics/composer3/ClientTargetPropertyWithBrightness.h>
#include <aidl/android/hardware/graphics/composer3/Composition.h>
#include <aidl/android/hardware/graphics/composer3/DisplayCapability.h>
#include <aidl/android/hardware/graphics/composer3/DisplayLuts.h>
#include <aidl/android/hardware/graphics/composer3/LutProperties.h>
#include <aidl/android/hardware/graphics/composer3/OutputType.h>
#include <aidl/android/hardware/graphics/composer3/OverlayProperties.h>

#include <log/log.h>
#include <math/mat4.h>
#include <ui/DisplayIdentification.h>
#include <ui/FenceTime.h>
#include <ui/GraphicTypes.h>
#include <ui/PictureProfileHandle.h>
#include <utils/StrongPointer.h>
#include <utils/Timers.h>

// Minimal Fps shim — CE passes this to getDeviceCompositionChanges but the
// stub never uses it. A one-field struct keeps signatures correct.
namespace android {
struct Fps {
  float value = 60.f;
};
} // namespace android

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace android {

class Fence;
class GraphicBuffer;

namespace hal = hardware::graphics::composer::hal;

class HWComposer {
public:
  struct DeviceRequestedChanges {
    using ChangedTypes = std::unordered_map<
        HWC2::Layer *,
        aidl::android::hardware::graphics::composer3::Composition>;
    using ClientTargetProperty = aidl::android::hardware::graphics::composer3::
        ClientTargetPropertyWithBrightness;
    using DisplayRequests = hal::DisplayRequest;
    using LayerRequests = std::unordered_map<HWC2::Layer *, hal::LayerRequest>;
    using LayerLuts = std::unordered_map<
        HWC2::Layer *,
        aidl::android::hardware::graphics::composer3::LutProperties>;
    ChangedTypes changedTypes;
    DisplayRequests displayRequests{};
    LayerRequests layerRequests;
    ClientTargetProperty clientTargetProperty{};
    LayerLuts layerLuts;
  };

  // === Methods CE actually calls (stubbed) ===
  void clearReleaseFences(HalDisplayId) {}
  std::shared_ptr<HWC2::Layer> createLayer(HalDisplayId) { return nullptr; }
  status_t disconnectDisplay(HalDisplayId) { return OK; }
  status_t executeCommands(HalDisplayId) { return OK; }

  // validateDisplay / getDeviceCompositionChanges: always return "no
  // changes" which causes CE to take the CLIENT composition path for every
  // layer. This is the core of the layerviewer HWC-stub strategy.
  status_t getDeviceCompositionChanges(
      HalDisplayId, bool /*frameUsesClientComposition*/,
      std::chrono::steady_clock::time_point /*earliestPresentTime*/,
      const sp<Fence> & /*previousPresentFence*/,
      nsecs_t /*expectedPresentTime*/, Fps /*frameInterval*/,
      std::optional<DeviceRequestedChanges> *outChanges) {
    if (outChanges)
      outChanges->reset();
    return OK;
  }

  sp<Fence> getLayerReleaseFence(HalDisplayId, HWC2::Layer *);
  const std::unordered_map<
      HWC2::Layer *,
      aidl::android::hardware::graphics::composer3::LutProperties> &
  getLutFileDescriptorMapper() const;
  std::optional<aidl::android::hardware::graphics::composer3::OverlayProperties>
  getOverlaySupport() const {
    return std::nullopt;
  }
  sp<Fence> getPresentFence(HalDisplayId);
  bool getValidateSkipped(HalDisplayId) const { return true; }
  bool hasCapability(
      aidl::android::hardware::graphics::composer3::Capability) const {
    return false;
  }
  bool hasDisplayCapability(
      HalDisplayId,
      aidl::android::hardware::graphics::composer3::DisplayCapability) const {
    return false;
  }
  status_t presentAndGetReleaseFences(HalDisplayId,
                                      std::chrono::steady_clock::time_point,
                                      const sp<Fence> &) {
    return OK;
  }
  void reset() {}
  status_t setActiveColorMode(
      HalDisplayId, ui::ColorMode,
      aidl::android::hardware::graphics::composer3::RenderIntent) {
    return OK;
  }
  status_t setColorTransform(HalDisplayId, const mat4 &) { return OK; }
  status_t setDisplayBrightness(HalDisplayId, float, float, void *) {
    return OK;
  }
  status_t setDisplayPictureProfileHandle(HalDisplayId,
                                          const PictureProfileHandle &) {
    return OK;
  }
};

} // namespace android
