// HWComposer shim. Layerviewer has no HWC backing — every layer is composed
// via RenderEngine (CLIENT composition). Method signatures match AOSP CE's
// call sites so CE compiles unchanged; bodies return neutral "no changes"
// responses that drive CE down its CLIENT path.

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

#include <future>
#include <log/log.h>
#include <math/mat4.h>
#include <ui/DisplayIdentification.h>
#include <ui/FenceTime.h>
#include <ui/GraphicTypes.h>
#include <ui/PictureProfileHandle.h>
#include <utils/StrongPointer.h>
#include <utils/Timers.h>

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace android {

// Minimal Fps stand-in.
struct Fps {
  float value = 60.f;
};

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
    using LutOffsetAndProperties = std::vector<std::pair<
        int32_t, aidl::android::hardware::graphics::composer3::LutProperties>>;
    using LayerLuts = std::unordered_map<HWC2::Layer *, LutOffsetAndProperties>;
    ChangedTypes changedTypes;
    DisplayRequests displayRequests{};
    LayerRequests layerRequests;
    ClientTargetProperty clientTargetProperty{};
    LayerLuts layerLuts;

    // Real AOSP compares two DeviceRequestedChanges for equality to
    // decide whether HWC's choice changed since last frame; our stub
    // always produces nullopt so this is only touched if CE gets there.
    bool operator==(const DeviceRequestedChanges &) const { return true; }
  };

  // Map returned by getLutFileDescriptorMapper — fd per HWC layer.
  using LutFileDescriptorMap =
      std::unordered_map<HWC2::Layer *, ndk::ScopedFileDescriptor>;

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
      const std::optional<TimePoint> & /*earliestPresentTime*/,
      nsecs_t /*expectedPresentTime*/, Fps /*frameInterval*/,
      std::optional<DeviceRequestedChanges> *outChanges) {
    if (outChanges)
      outChanges->reset();
    return OK;
  }

  sp<Fence> getLayerReleaseFence(HalDisplayId, HWC2::Layer *);
  LutFileDescriptorMap &getLutFileDescriptorMapper();
  const aidl::android::hardware::graphics::composer3::OverlayProperties &
  getOverlaySupport() const;
  sp<Fence> getPresentFence(HalDisplayId) const;
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
                                      const std::optional<TimePoint> &) {
    return OK;
  }
  void reset() {}
  status_t setActiveColorMode(HalDisplayId, ui::ColorMode, ui::RenderIntent) {
    return OK;
  }
  status_t setColorTransform(HalDisplayId, const mat4 &) { return OK; }
  // CE calls `.get()` on the return — return a std::future<status_t>.
  std::future<status_t>
  setDisplayBrightness(HalDisplayId, float, float,
                       Hwc2::Composer::DisplayBrightnessOptions) {
    std::promise<status_t> p;
    p.set_value(OK);
    return p.get_future();
  }
  status_t setDisplayPictureProfileHandle(HalDisplayId,
                                          const PictureProfileHandle &) {
    return OK;
  }
};

} // namespace android
