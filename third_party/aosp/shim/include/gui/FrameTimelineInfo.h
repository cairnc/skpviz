// FrameTimelineInfo shim — AIDL parcelable. FE only reads the vsync id.
#pragma once
#include <cstdint>
namespace android::gui {
struct FrameTimelineInfo {
  static constexpr int64_t INVALID_VSYNC_ID = -1;
  int64_t vsyncId = INVALID_VSYNC_ID;
  int32_t inputEventId = 0;
  int64_t startTimeNanos = 0;
  bool useForRefreshRateSelection = false;
  int64_t skippedFrameVsyncId = INVALID_VSYNC_ID;
  int64_t skippedFrameStartTimeNanos = 0;
};
} // namespace android::gui
