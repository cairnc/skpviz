#pragma once
#include <cstdarg>
#define PR_SET_NAME 15
static inline int prctl(int, ...) { return 0; }
