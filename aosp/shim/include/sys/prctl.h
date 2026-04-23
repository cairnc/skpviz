// prctl shim — Linux-only; on macOS HwcAsyncWorker uses PR_SET_NAME, we no-op.
#pragma once
#include <cstdarg>
#define PR_SET_NAME 15
static inline int prctl(int /*option*/, ...) { return 0; }
