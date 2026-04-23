// IPCThreadState shim — FE reads calling pid/uid; in offline replay both
// come from the trace, not from the current thread. Return 0 (AID_ROOT).
#pragma once
#include <sstream> // FE transitively expects std::stringstream through this
#include <sys/types.h>

namespace android {
class IPCThreadState {
public:
  static IPCThreadState *self() {
    static IPCThreadState s;
    return &s;
  }
  pid_t getCallingPid() const { return 0; }
  uid_t getCallingUid() const { return 0; }
};
} // namespace android
