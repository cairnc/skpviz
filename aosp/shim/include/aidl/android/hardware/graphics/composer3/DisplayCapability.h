#pragma once
#include <cstdint>
namespace aidl::android::hardware::graphics::composer3 {
enum class DisplayCapability : int32_t {
  INVALID = 0,
  SKIP_CLIENT_COLOR_TRANSFORM = 1,
  DOZE = 2,
  BRIGHTNESS = 3,
  PROTECTED_CONTENTS = 4,
  AUTO_LOW_LATENCY_MODE = 5,
  SUSPEND = 6,
  DISPLAY_IDLE_TIMER = 7,
  MULTI_THREADED_PRESENT = 8,
  HDR_OUTPUT_CONVERSION = 9,
};
}
