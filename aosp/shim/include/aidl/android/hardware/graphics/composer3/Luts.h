#pragma once
#include <aidl/android/hardware/graphics/composer3/LutProperties.h>
#include <android/binder_auto_utils.h> // ScopedFileDescriptor — own header
#include <cstdint>
#include <vector>

namespace aidl::android::hardware::graphics::composer3 {
struct Luts {
  ::android::ndk::ScopedFileDescriptor pfd;
  std::vector<int32_t> offsets;
  std::vector<LutProperties> lutProperties;
};
} // namespace aidl::android::hardware::graphics::composer3
