#pragma once
#include <aidl/android/hardware/graphics/composer3/Luts.h>
#include <cstdint>
#include <vector>
namespace aidl::android::hardware::graphics::composer3 {
struct DisplayLuts {
  struct LayerLut {
    int64_t layer = 0;
    Luts luts;
  };
  int64_t display = 0;
  std::vector<LayerLut> layerLuts;
};
} // namespace aidl::android::hardware::graphics::composer3
