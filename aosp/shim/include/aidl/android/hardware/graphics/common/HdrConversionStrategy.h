#pragma once
#include <aidl/android/hardware/graphics/common/Hdr.h>
#include <vector>
namespace aidl::android::hardware::graphics::common {
struct HdrConversionStrategy {
  enum class Tag : int32_t {
    passthrough = 0,
    autoAllowedHdrTypes = 1,
    forceHdrConversion = 2,
  };
  Tag tag = Tag::passthrough;
  bool passthrough = false;
  std::vector<Hdr> autoAllowedHdrTypes;
  Hdr forceHdrConversion = Hdr::INVALID;
};
} // namespace aidl::android::hardware::graphics::common
