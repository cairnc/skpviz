// HAL loader shim. PixelFormat.h expects HAL_PIXEL_FORMAT_* enums to be
// visible through this header, which on-device come via hardware/hardware.h
// → hardware/gralloc.h → system/graphics.h. Shortcut directly to graphics.h.
#pragma once
#include <system/graphics.h>
