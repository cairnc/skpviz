#pragma once
#include <cstdint>
namespace aidl::android::hardware::graphics::common {
struct DisplayDecorationSupport {
  int32_t format = 0;
  int32_t alphaInterpretation = 0;
};
} // namespace aidl::android::hardware::graphics::common
