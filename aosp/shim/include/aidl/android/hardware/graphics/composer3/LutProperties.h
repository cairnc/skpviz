#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace aidl::android::hardware::graphics::composer3 {

struct LutProperties {
  enum class Dimension : int32_t { ONE_D = 1, THREE_D = 3 };
  enum class SamplingKey : int32_t {
    RGB = 0,
    MAX_RGB = 1,
    CIE_Y = 2,
  };
  Dimension dimension = Dimension::ONE_D;
  int32_t size = 0;
  std::vector<SamplingKey> samplingKeys;
};

inline std::string toString(LutProperties::Dimension v) {
  return std::to_string(static_cast<int32_t>(v));
}
inline std::string toString(LutProperties::SamplingKey v) {
  return std::to_string(static_cast<int32_t>(v));
}

} // namespace aidl::android::hardware::graphics::composer3
