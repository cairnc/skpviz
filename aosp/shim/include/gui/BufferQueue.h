// BufferQueue shim — real AOSP gui/BufferQueue.h pulls in utils/Timers,
// ui/Fence, and enough of the HWC2 / Hwc2 namespaces that CE headers can use
// Hwc2::Transform and nsecs_t without adding their own includes. Replicate
// that transitive surface so we don't have to modify CE sources.
#pragma once

#include <DisplayHardware/HWC2.h>
#include <cstdint>
#include <ui/Fence.h>
#include <utils/Timers.h>

namespace android {
class BufferQueue {
public:
  enum : uint32_t { NUM_BUFFER_SLOTS = 64 };
};
} // namespace android
