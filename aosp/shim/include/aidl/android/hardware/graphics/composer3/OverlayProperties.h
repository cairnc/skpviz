#pragma once
#include <cstdint>
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
  int32_t lutProperties = 0;
};
} // namespace aidl::android::hardware::graphics::composer3
