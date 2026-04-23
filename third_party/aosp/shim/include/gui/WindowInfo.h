// WindowInfo shim — FE references InputConfig flags + WindowInfoHandle with
// getInfo/editInfo. Stripped from the real 400-line header.
#pragma once

#include <binder/Binder.h>
#include <cstdint>
#include <gui/PidUid.h>
#include <memory>
#include <ui/LogicalDisplayId.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <ui/Size.h>
#include <ui/Transform.h>
#include <utils/RefBase.h>

namespace android::gui {

enum class TouchOcclusionMode : int32_t {
  BLOCK_UNTRUSTED = 0,
  USE_OPAQUE_BOUNDS = 1,
  ALLOW = 2,
};

struct InputApplicationInfo {
  sp<IBinder> token;
  std::string name;
  int32_t dispatchingTimeoutMillis = 0;
};

class InputApplicationHandle : public RefBase {};

struct WindowInfo {
  enum class Type : int32_t {
    UNKNOWN = 0,
    APPLICATION = 1,
    BASE_APPLICATION = 2,
    INPUT_METHOD = 11,
    NAVIGATION_BAR = 19,
    STATUS_BAR = 21,
    TOAST = 31,
    FIRST_APPLICATION_WINDOW = 1,
    LAST_APPLICATION_WINDOW = 99,
    FIRST_SYSTEM_WINDOW = 2000,
    ftl_first = FIRST_SYSTEM_WINDOW,
    ftl_last = FIRST_SYSTEM_WINDOW + 15,
  };
  enum class InputConfig : uint32_t {
    NO_INPUT_CHANNEL = 1 << 0,
    NOT_VISIBLE = 1 << 1,
    NOT_FOCUSABLE = 1 << 2,
    NOT_TOUCHABLE = 1 << 3,
    PREVENT_SPLITTING = 1 << 4,
    DUPLICATE_TOUCH_TO_WALLPAPER = 1 << 5,
    IS_WALLPAPER = 1 << 6,
    PAUSE_DISPATCHING = 1 << 7,
    TRUSTED_OVERLAY = 1 << 8,
    WATCH_OUTSIDE_TOUCH = 1 << 9,
    SLIPPERY = 1 << 10,
    DISABLE_USER_ACTIVITY = 1 << 11,
    SPY = 1 << 12,
    INTERCEPTS_STYLUS = 1 << 13,
    CLONE = 1 << 14,
    GLOBAL_STYLUS_BLOCKS_TOUCH = 1 << 15,
    SENSITIVE_FOR_PRIVACY = 1 << 16,
    DROP_INPUT = 1 << 17,
    DROP_INPUT_IF_OBSCURED = 1 << 18,
  };
  struct InputConfigMask {
    uint32_t value = 0;
    InputConfigMask() = default;
    InputConfigMask(InputConfig v) : value(static_cast<uint32_t>(v)) {}
    bool test(InputConfig v) const {
      return (value & static_cast<uint32_t>(v)) != 0;
    }
    void set(InputConfig v, bool on = true) {
      if (on)
        value |= static_cast<uint32_t>(v);
      else
        value &= ~static_cast<uint32_t>(v);
    }
    void clear(InputConfig v) { set(v, false); }
    InputConfigMask &operator|=(InputConfig v) {
      set(v);
      return *this;
    }
    InputConfigMask &operator&=(InputConfigMask o) {
      value &= o.value;
      return *this;
    }
    InputConfigMask operator|(InputConfigMask o) const {
      InputConfigMask r = *this;
      r.value |= o.value;
      return r;
    }
    bool any() const { return value != 0; }
    std::string string() const { return std::to_string(value); }
  };
  sp<IBinder> token;
  std::string name;
  int32_t id = -1;
  ui::LogicalDisplayId displayId = ui::LogicalDisplayId::INVALID;
  Pid ownerPid = Pid::INVALID;
  Uid ownerUid = Uid::INVALID;
  TouchOcclusionMode touchOcclusionMode = TouchOcclusionMode::BLOCK_UNTRUSTED;
  float alpha = 1.f;
  InputConfigMask inputConfig;
  int32_t layoutParamsFlags = 0;
  int32_t layoutParamsType = 0;
  int32_t surfaceInset = 0;
  Rect frame;
  ui::Size contentSize;
  Region touchableRegion;
  ui::Transform transform;
  sp<InputApplicationHandle> applicationInfo;
  std::string packageName;
  int64_t frameRateSelectionPriority = 0;
  bool focusable = true;
  bool hasWallpaper = false;
  bool paused = false;
  bool visible = true;
  bool trustedOverlay = false;
  bool replaceTouchableRegionWithCrop = false;
  bool canOccludePresentation = false;
  wp<IBinder> touchableRegionCropHandle;

  // AOSP real API — replay path only mutates mask bits.
  void setInputConfig(InputConfigMask flags, bool value) {
    if (value)
      inputConfig.value |= flags.value;
    else
      inputConfig.value &= ~flags.value;
  }
  void setInputConfig(InputConfig flag, bool value) {
    setInputConfig(InputConfigMask(flag), value);
  }
};

class WindowInfoHandle : public RefBase {
public:
  WindowInfoHandle() = default;
  explicit WindowInfoHandle(const WindowInfo &info) : mInfo(info) {}
  const WindowInfo *getInfo() const { return &mInfo; }
  WindowInfo *editInfo() { return &mInfo; }
  void updateFrom(const sp<WindowInfoHandle> &other) {
    if (other)
      mInfo = other->mInfo;
  }

private:
  WindowInfo mInfo;
};

} // namespace android::gui
