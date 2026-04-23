#pragma once
#include <aidl/android/hardware/graphics/composer3/LutProperties.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace aidl::android::hardware::graphics::composer3 {
struct OverlayProperties {
  struct SupportedBufferCombinations {
    std::vector<int32_t> pixelFormats;
    std::vector<int32_t> standards;
    std::vector<int32_t> transfers;
    std::vector<int32_t> ranges;
  };
  std::vector<SupportedBufferCombinations> combinations;
  bool supportMixedColorSpaces = false;
  // Real AIDL exposes this as an optional list of optional entries.
  std::optional<std::vector<std::optional<LutProperties>>> lutProperties;
};
} // namespace aidl::android::hardware::graphics::composer3
