#pragma once
#include <cstdint>
#include <string>
namespace aidl::android::hardware::graphics::composer3 {
enum class Composition : int32_t {
  INVALID = 0,
  CLIENT = 1,
  DEVICE = 2,
  SOLID_COLOR = 3,
  CURSOR = 4,
  SIDEBAND = 5,
  DISPLAY_DECORATION = 6,
  REFRESH_RATE_INDICATOR = 7,
};
inline std::string toString(Composition v) {
  return "Composition(" + std::to_string(static_cast<int32_t>(v)) + ")";
}
} // namespace aidl::android::hardware::graphics::composer3
