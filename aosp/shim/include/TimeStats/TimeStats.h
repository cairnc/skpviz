// TimeStats shim — SF-internal latency telemetry. No-op class.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <ui/FenceTime.h>
#include <utils/Timers.h>
#include <vector>

namespace android {
class Fence;

namespace gui {
enum class GameMode : int32_t {};
} // namespace gui

class TimeStats {
public:
  virtual ~TimeStats() = default;
  virtual void onBootFinished() {}
  virtual void parseArgs(bool, const std::vector<std::string> &,
                         std::string &) {}
  virtual bool isEnabled() { return false; }
  virtual std::string miniDump() { return {}; }
  virtual void incrementTotalFrames() {}
  virtual void incrementMissedFrames() {}
  virtual void pushCompositionStrategyState(int32_t, int32_t, bool) {}
  virtual void incrementRefreshRateSwitches() {}
  virtual void recordDisplayEventConnectionCount(int32_t) {}
  virtual void recordFrameDuration(nsecs_t, nsecs_t) {}
  virtual void recordRenderEngineDuration(nsecs_t, nsecs_t) {}
  virtual void recordRenderEngineDuration(nsecs_t,
                                          const std::shared_ptr<FenceTime> &) {}
  virtual void setPostTime(int32_t, uint64_t, const std::string &, uid_t,
                           nsecs_t, gui::GameMode) {}
  virtual void setLatchTime(int32_t, uint64_t, nsecs_t) {}
  virtual void setDesiredTime(int32_t, uint64_t, nsecs_t) {}
  virtual void setAcquireTime(int32_t, uint64_t, nsecs_t) {}
  virtual void setAcquireFence(int32_t, uint64_t,
                               const std::shared_ptr<FenceTime> &) {}
  virtual void setPresentTime(int32_t, uint64_t, nsecs_t, int32_t, int32_t,
                              int32_t) {}
  virtual void setPresentFence(int32_t, uint64_t,
                               const std::shared_ptr<FenceTime> &, int32_t,
                               int32_t, int32_t) {}
  virtual void incrementJankyFrames(int32_t) {}
  virtual void onDestroy(int32_t) {}
  virtual void removeTimeRecord(int32_t, uint64_t) {}
  virtual void setPowerMode(int32_t) {}
  virtual void recordRefreshRate(uint32_t, nsecs_t) {}
  virtual void setPresentFenceGlobal(const std::shared_ptr<FenceTime> &) {}
};

} // namespace android
