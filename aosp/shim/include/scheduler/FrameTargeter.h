// FrameTargeter shim.
#pragma once
#include "interface/ICompositor.h"
namespace android::scheduler {
class FrameTargeter {
public:
  TimePoint expectedPresentTime() const { return {}; }
  TimePoint earliestPresentTime() const { return {}; }
};
} // namespace android::scheduler
