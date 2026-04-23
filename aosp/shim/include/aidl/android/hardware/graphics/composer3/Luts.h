#pragma once
#include <aidl/android/hardware/graphics/composer3/LutProperties.h>
#include <android-base/unique_fd.h>
#include <cstdint>
#include <vector>
namespace aidl::android::hardware::graphics::composer3 {
struct Luts {
  ::android::base::unique_fd pfd;
  std::vector<int32_t> offsets;
  std::vector<LutProperties> lutProperties;
};
} // namespace aidl::android::hardware::graphics::composer3
