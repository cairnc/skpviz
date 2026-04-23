#pragma once
#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/composer3/DimmingStage.h>
#include <cstdint>
namespace aidl::android::hardware::graphics::composer3 {
struct ClientTargetProperty {
  int32_t pixelFormat = 0;
  ::aidl::android::hardware::graphics::common::Dataspace dataspace =
      ::aidl::android::hardware::graphics::common::Dataspace::UNKNOWN;
};
struct ClientTargetPropertyWithBrightness {
  int64_t display = 0;
  ClientTargetProperty clientTargetProperty;
  float brightness = 1.f;
  DimmingStage dimmingStage = DimmingStage::NONE;
};
} // namespace aidl::android::hardware::graphics::composer3
