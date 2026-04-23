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

// __predict_true / __predict_false — bionic macros. SF code uses them with
// LOG_ALWAYS_FATAL guards; shim to the same __builtin_expect pattern.
#ifndef __predict_true
#define __predict_true(exp) __builtin_expect(!!(exp), 1)
#endif
#ifndef __predict_false
#define __predict_false(exp) __builtin_expect(!!(exp), 0)
#endif
