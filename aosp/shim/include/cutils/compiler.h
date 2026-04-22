#pragma once

#ifndef CC_LIKELY
#define CC_LIKELY(exp) __builtin_expect(!!(exp), true)
#endif
#ifndef CC_UNLIKELY
#define CC_UNLIKELY(exp) __builtin_expect(!!(exp), false)
#endif

#ifndef ANDROID_API
#define ANDROID_API
#endif
