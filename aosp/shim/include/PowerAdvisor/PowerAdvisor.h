// PowerAdvisor shim — SF uses this to hint the CPU/GPU scheduler. No-op.
#pragma once
#include <cstdint>
#include <memory>
#include <scheduler/interface/ICompositor.h>
#include <ui/DisplayId.h>
#include <ui/FenceTime.h>
#include <vector>

namespace android::adpf {

class PowerAdvisor {
public:
  virtual ~PowerAdvisor() = default;
  virtual void setHwcValidateTiming(DisplayId, TimePoint, TimePoint) {}
  virtual void setHwcPresentTiming(DisplayId, TimePoint, TimePoint) {}
  virtual void setHwcPresentDelayedTime(DisplayId, TimePoint) {}
  virtual void setSkippedValidate(DisplayId, bool) {}
  virtual void setValidateTiming(DisplayId, TimePoint, TimePoint) {}
  virtual void setSfPresentTiming(TimePoint, TimePoint) {}
  virtual void setCommitStart(TimePoint) {}
  virtual void setCompositeEnd(TimePoint) {}
  virtual void setExpectedPresentTime(TimePoint) {}
  virtual void setTargetWorkDuration(int64_t) {}
  virtual void setGpuStartTime(DisplayId, TimePoint) {}
  virtual void setGpuFenceTime(DisplayId, std::unique_ptr<FenceTime> &&) {}
  virtual void setHintSessionThreadIds(const std::vector<int32_t> &) {}
  virtual void setRequiresRenderEngine(DisplayId, bool) {}
  virtual void reportActualWorkDuration() {}
  virtual void enablePowerHintSession(bool) {}
  virtual bool startPowerHintSession(std::vector<int32_t>) { return false; }
  virtual bool usePowerHintSession() { return false; }
  virtual bool supportsPowerHintSession() { return false; }
  virtual bool supportsGpuReporting() { return false; }
};

} // namespace android::adpf
