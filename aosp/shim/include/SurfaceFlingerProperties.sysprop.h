// SurfaceFlingerProperties.sysprop.h shim. Real one is generated from SF's
// sysprop definitions; we expose matching symbols that all return nullopt /
// defaults.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace android::sysprop::SurfaceFlingerProperties {

inline std::optional<bool> use_color_management() { return std::nullopt; }
inline std::optional<bool> clear_slots_with_set_layer_buffer() {
  return std::nullopt;
}
inline std::optional<int64_t> default_composition_dataspace() {
  return std::nullopt;
}
inline std::optional<int32_t> default_composition_pixel_format() {
  return std::nullopt;
}
inline std::optional<int64_t> wcg_composition_dataspace() {
  return std::nullopt;
}
inline std::optional<int32_t> wcg_composition_pixel_format() {
  return std::nullopt;
}
inline std::optional<double> display_primary_red_x() { return std::nullopt; }
inline std::optional<double> display_primary_red_y() { return std::nullopt; }
inline std::optional<double> display_primary_red_z() { return std::nullopt; }
inline std::optional<double> display_primary_green_x() { return std::nullopt; }
inline std::optional<double> display_primary_green_y() { return std::nullopt; }
inline std::optional<double> display_primary_green_z() { return std::nullopt; }
inline std::optional<double> display_primary_blue_x() { return std::nullopt; }
inline std::optional<double> display_primary_blue_y() { return std::nullopt; }
inline std::optional<double> display_primary_blue_z() { return std::nullopt; }
inline std::optional<double> display_primary_white_x() { return std::nullopt; }
inline std::optional<double> display_primary_white_y() { return std::nullopt; }
inline std::optional<double> display_primary_white_z() { return std::nullopt; }
inline std::optional<bool> support_kernel_idle_timer() { return std::nullopt; }
inline std::optional<bool> use_context_priority() { return std::nullopt; }
inline std::optional<bool> has_HDR_display() { return std::nullopt; }
inline std::optional<bool> has_wide_color_display() { return std::nullopt; }
inline std::optional<int32_t> color_space_agnostic_dataspace() {
  return std::nullopt;
}
inline std::optional<int32_t> max_virtual_display_dimension() {
  return std::nullopt;
}
inline std::optional<bool> running_without_sync_framework() {
  return std::nullopt;
}
inline std::optional<bool> refresh_rate_switching() { return std::nullopt; }
inline std::optional<int32_t> primary_display_orientation() {
  return std::nullopt;
}
inline std::optional<bool> use_smart_90_for_video() { return std::nullopt; }
inline std::optional<bool> enable_protected_contents() { return std::nullopt; }
inline std::optional<bool> present_time_offset_from_vsync_ns() {
  return std::nullopt;
}
inline std::optional<bool> force_hwc_copy_for_virtual_displays() {
  return std::nullopt;
}
inline std::optional<bool> enable_sdr_dimming() { return std::nullopt; }
inline std::optional<bool> set_display_power_timer_ms() { return std::nullopt; }
inline std::optional<int64_t>
update_device_product_info_on_hotplug_reconnect() {
  return std::nullopt;
}
inline std::optional<bool> enable_frame_rate_override() { return std::nullopt; }
inline std::optional<bool> game_default_frame_rate_override() {
  return std::nullopt;
}
inline std::vector<int32_t> supported_hdr_ui_types() { return {}; }
inline std::vector<int32_t> report_unsigned_display_modes() { return {}; }
inline std::optional<bool> ignore_hdr_camera_layers() { return std::nullopt; }

} // namespace android::sysprop::SurfaceFlingerProperties
