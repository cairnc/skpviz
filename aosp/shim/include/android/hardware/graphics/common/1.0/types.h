// HIDL graphics/common@1.0 types shim. Values mirror the real HAL enums.
#pragma once
#include <cstdint>
#include <string>
namespace android::hardware::graphics::common::V1_0 {

enum class PixelFormat : int32_t {
  UNSPECIFIED = 0,
  RGBA_8888 = 0x1,
  RGBX_8888 = 0x2,
  RGB_888 = 0x3,
  RGB_565 = 0x4,
  BGRA_8888 = 0x5,
  YV12 = 0x32315659,
  Y8 = 0x20203859,
  Y16 = 0x20363159,
  RAW16 = 0x20,
  RAW10 = 0x25,
  RAW12 = 0x26,
  RAW_OPAQUE = 0x24,
  IMPLEMENTATION_DEFINED = 0x22,
  YCBCR_420_888 = 0x23,
  YCBCR_422_SP = 0x10,
  YCRCB_420_SP = 0x11,
  YCBCR_422_I = 0x14,
  JPEG = 0x100,
  DEPTH_16 = 0x30,
  DEPTH_24 = 0x31,
  DEPTH_24_STENCIL_8 = 0x32,
  DEPTH_32F = 0x33,
  DEPTH_32F_STENCIL_8 = 0x34,
  STENCIL_8 = 0x35,
  YCBCR_P010 = 0x36,
  HSV_888 = 0x37,
  BLOB = 0x21,
  RGBA_FP16 = 0x16,
  RGBA_1010102 = 0x2b,
};

enum class BufferUsage : uint64_t {
  CPU_READ_NEVER = 0,
  CPU_READ_RARELY = 2,
  CPU_READ_OFTEN = 3,
  CPU_WRITE_NEVER = 0,
  CPU_WRITE_RARELY = 2 << 4,
  CPU_WRITE_OFTEN = 3 << 4,
  GPU_TEXTURE = 1ULL << 8,
  GPU_RENDER_TARGET = 1ULL << 9,
  COMPOSER_OVERLAY = 1ULL << 11,
  COMPOSER_CLIENT_TARGET = 1ULL << 12,
  PROTECTED = 1ULL << 14,
  COMPOSER_CURSOR = 1ULL << 15,
  VIDEO_ENCODER = 1ULL << 16,
  CAMERA_OUTPUT = 1ULL << 17,
  CAMERA_INPUT = 1ULL << 18,
  RENDERSCRIPT = 1ULL << 20,
  VIDEO_DECODER = 1ULL << 22,
  SENSOR_DIRECT_DATA = 1ULL << 23,
  GPU_DATA_BUFFER = 1ULL << 24,
  GPU_CUBE_MAP = 1ULL << 25,
  GPU_MIPMAP_COMPLETE = 1ULL << 26,
  HW_IMAGE_ENCODER = 1ULL << 27,
  VENDOR_MASK = 0xFULL << 28,
  VENDOR_MASK_HI = 0xFFFFULL << 48,
};

enum class ColorTransform : int32_t {
  IDENTITY = 0,
  ARBITRARY_MATRIX = 1,
  VALUE_INVERSE = 2,
  GRAYSCALE = 3,
  CORRECT_PROTANOPIA = 4,
  CORRECT_DEUTERANOPIA = 5,
  CORRECT_TRITANOPIA = 6,
};

enum class Dataspace : int32_t {
  UNKNOWN = 0,
  ARBITRARY = 1,
  STANDARD_UNSPECIFIED = 0,
  V0_SRGB = 142671872,
  V0_SCRGB = 411107328,
  V0_SCRGB_LINEAR = 406913024,
  V0_BT709 = 281083904,
  V0_BT601_625 = 281149440,
  V0_BT601_525 = 281280512,
  V0_JFIF = 146931712,
  SRGB_LINEAR = 512,
  SRGB = 142671872,
  BT709 = 281083904,
  BT601_625 = 281149440,
  BT601_525 = 281280512,
  DISPLAY_P3 = 143261696,
  DISPLAY_P3_LINEAR = 139067392,
  BT2020 = 147193856,
  BT2020_PQ = 163971072,
  BT2020_LINEAR = 138018816,
  DCI_P3 = 155844608,
  DCI_P3_LINEAR = 139067392,
  JFIF = 146931712,
  DEPTH = 4096,
  SENSOR = 4097,
};

enum class ColorMode : int32_t {
  NATIVE = 0,
  STANDARD_BT601_625 = 1,
  STANDARD_BT601_625_UNADJUSTED = 2,
  STANDARD_BT601_525 = 3,
  STANDARD_BT601_525_UNADJUSTED = 4,
  STANDARD_BT709 = 5,
  DCI_P3 = 6,
  SRGB = 7,
  ADOBE_RGB = 8,
  DISPLAY_P3 = 9,
};

enum class Transform : int32_t {
  FLIP_H = 0x01,
  FLIP_V = 0x02,
  ROT_90 = 0x04,
  ROT_180 = 0x03,
  ROT_270 = 0x07,
};

// HIDL-generated toString + operators for the Transform enum class, used by
// OutputLayer printing and HWC bit-ops.
inline std::string toString(Transform t) {
  return "Transform(" + std::to_string(static_cast<int32_t>(t)) + ")";
}
inline constexpr Transform operator|(Transform a, Transform b) {
  return static_cast<Transform>(static_cast<int32_t>(a) |
                                static_cast<int32_t>(b));
}
inline constexpr int32_t operator|(Transform a, int32_t b) {
  return static_cast<int32_t>(a) | b;
}
inline constexpr int32_t operator|(int32_t a, Transform b) {
  return a | static_cast<int32_t>(b);
}
inline constexpr Transform operator&(Transform a, Transform b) {
  return static_cast<Transform>(static_cast<int32_t>(a) &
                                static_cast<int32_t>(b));
}
inline constexpr int32_t operator&(Transform a, int32_t b) {
  return static_cast<int32_t>(a) & b;
}

} // namespace android::hardware::graphics::common::V1_0
