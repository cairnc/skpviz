#pragma once
#include <aidl/android/hardware/graphics/common/Hdr.h>
namespace aidl::android::hardware::graphics::common {
struct HdrConversionCapability {
  Hdr sourceType = Hdr::INVALID;
  Hdr outputType = Hdr::INVALID;
  bool addsLatency = false;
};
} // namespace aidl::android::hardware::graphics::common
