// BufferQueue shim — HwcBufferCache uses a single constant from here.
#pragma once
#include <cstdint>

namespace android {

class BufferQueue {
public:
  enum : uint32_t { NUM_BUFFER_SLOTS = 64 };
};

} // namespace android
