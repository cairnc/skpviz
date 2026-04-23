// DisplayHardware/Hal.h shim. Real file pulls in the full IComposer HIDL
// interface; CE only needs a handful of type aliases under the `hal::`
// namespace (Transform, Error, BlendMode, PixelFormat, DisplayRequest,
// LayerRequest), all of which are enums.
#pragma once

#include <aidl/android/hardware/graphics/common/BlendMode.h>
#include <aidl/android/hardware/graphics/composer3/Composition.h>
#include <android/hardware/graphics/common/1.0/types.h>
#include <android/hardware/graphics/common/1.2/types.h>
#include <cstdint>
#include <vector>

#define ERROR_HAS_CHANGES 5

namespace android {
namespace hardware::graphics::composer::hal {

using Transform = ::android::hardware::graphics::common::V1_0::Transform;
using Dataspace = ::android::hardware::graphics::common::V1_2::Dataspace;
using PixelFormat = ::android::hardware::graphics::common::V1_2::PixelFormat;
using ColorMode = ::android::hardware::graphics::common::V1_2::ColorMode;
using RenderIntent = ::android::hardware::graphics::common::V1_1::RenderIntent;
using BlendMode = ::aidl::android::hardware::graphics::common::BlendMode;

enum class Error : int32_t {
  NONE = 0,
  BAD_CONFIG = 1,
  BAD_DISPLAY = 2,
  BAD_LAYER = 3,
  BAD_PARAMETER = 4,
  NO_RESOURCES = 5,
  NOT_VALIDATED = 6,
  UNSUPPORTED = 8,
  SEAMLESS_NOT_ALLOWED = 9,
  SEAMLESS_NOT_POSSIBLE = 10,
  CONFIG_FAILED = 11,
  PICTURE_PROFILE_MAX_EXCEEDED = 12,
};

enum class DisplayRequest : int32_t {
  NONE = 0,
  FLIP_CLIENT_TARGET = 1,
  WRITE_CLIENT_TARGET_TO_OUTPUT = 2,
};

enum class LayerRequest : int32_t {
  NONE = 0,
  CLEAR_CLIENT_TARGET = 1,
};

enum class Connection : int32_t {
  INVALID = 0,
  CONNECTED = 1,
  DISCONNECTED = 2
};
enum class PowerMode : int32_t { OFF = 0, DOZE = 1, ON = 2 };
enum class Vsync : int32_t { INVALID = 0, ENABLE = 1, DISABLE = 2 };

using HWDisplayId = uint64_t;
using HWLayerId = uint64_t;
using HWConfigId = uint32_t;

// HIDL generated these as free-function stringifiers; replicate so CE
// source code can call to_string(Error) / toString(Transform) unchanged.
inline std::string to_string(Error e) {
  return "Error(" + std::to_string(static_cast<int32_t>(e)) + ")";
}
inline std::string toString(Error e) { return to_string(e); }
inline std::string toString(LayerRequest r) {
  return "LayerRequest(" + std::to_string(static_cast<int32_t>(r)) + ")";
}
inline std::string toString(DisplayRequest r) {
  return "DisplayRequest(" + std::to_string(static_cast<int32_t>(r)) + ")";
}

struct DisplayIdentification_t {
  uint8_t port = 0;
  std::vector<uint8_t> data;
};

} // namespace hardware::graphics::composer::hal
} // namespace android
