// aconfig flag header shim. Real one is generated from *.aconfig files and
// defines a namespace of constexpr flag accessors. For layerviewer, every
// flag is off by default.
#pragma once

namespace com::android::graphics::libgui::flags {
#define LV_LIBGUI_FLAG(name)                                                   \
  inline constexpr bool name() { return false; }
LV_LIBGUI_FLAG(apply_picture_profiles)
LV_LIBGUI_FLAG(edge_extension_shader)
LV_LIBGUI_FLAG(trace_frame_rate_override)
LV_LIBGUI_FLAG(frametimestamps_previousrelease)
LV_LIBGUI_FLAG(bufferqueue_delete_fence_on_free)
LV_LIBGUI_FLAG(wb_camera3_and_processors)
LV_LIBGUI_FLAG(wb_consumer_base_owns_bq)
LV_LIBGUI_FLAG(wb_libcameraservice)
LV_LIBGUI_FLAG(wb_platform_api_improvements)
LV_LIBGUI_FLAG(wb_ring_buffer)
LV_LIBGUI_FLAG(wb_stream_splitter)
#undef LV_LIBGUI_FLAG
} // namespace com::android::graphics::libgui::flags

// Aconfig also generates C-style macros of the form NAMESPACE_FLAG() that
// expand to the namespaced function call. Mirror just the ones CE uses.
#define com_android_graphics_libgui_flags_apply_picture_profiles()             \
  ::com::android::graphics::libgui::flags::apply_picture_profiles()
#define com_android_graphics_libgui_flags_edge_extension_shader()              \
  ::com::android::graphics::libgui::flags::edge_extension_shader()
