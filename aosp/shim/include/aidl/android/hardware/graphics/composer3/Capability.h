#pragma once
#include <cstdint>
namespace aidl::android::hardware::graphics::composer3 {
enum class Capability : int32_t {
  INVALID = 0,
  SIDEBAND_STREAM = 1,
  SKIP_CLIENT_COLOR_TRANSFORM = 2,
  PRESENT_FENCE_IS_NOT_RELIABLE = 3,
  SKIP_VALIDATE = 4,
  BOOT_DISPLAY_CONFIG = 5,
  HDR_OUTPUT_CONVERSION_CONFIG = 6,
  LAYER_LIFECYCLE_BATCH_COMMAND = 7,
};
}
