// FenceTime shim: AOSP tracks when fences signaled for latency metrics.
// All our fences are pre-signaled, so FenceTime is a no-op wrapper.
#pragma once

#include <scheduler/interface/ICompositor.h> // TimePoint — CE headers use it
#include <ui/Fence.h>
#include <utils/RefBase.h>

namespace android {

class FenceTime : public RefBase {
public:
  static const sp<FenceTime> NO_FENCE;
  FenceTime() = default;
  explicit FenceTime(const sp<Fence> &) {}
  nsecs_t getSignalTime() const { return 0; }
  nsecs_t getCachedSignalTime() const { return 0; }
};

} // namespace android
