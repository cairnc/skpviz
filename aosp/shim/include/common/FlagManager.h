// FlagManager shim. Real one reads aconfig flags; we return false everywhere.
// Any SF code path guarded by a flag stays on its default-off branch.
#pragma once

#include <string>

namespace android {

class FlagManager {
public:
  static const FlagManager &getInstance() {
    static FlagManager inst;
    return inst;
  }
  static FlagManager &getMutableInstance() {
    static FlagManager inst;
    return inst;
  }
  void markBootCompleted() {}
  void dump(std::string &) const {}

  // All server-flag and aconfig-flag readers resolve to false.
  bool test_flag() const { return false; }

// Catch-all for everything else.
#define LAYERVIEWER_FLAG(name)                                                 \
  bool name() const { return false; }
  LAYERVIEWER_FLAG(use_adpf_cpu_hint)
  LAYERVIEWER_FLAG(use_skia_tracing)
  LAYERVIEWER_FLAG(refresh_rate_overlay_on_external_display)
  LAYERVIEWER_FLAG(adpf_gpu_sf)
  LAYERVIEWER_FLAG(adpf_use_fmq_channel)
  LAYERVIEWER_FLAG(adpf_native_session_manager)
  LAYERVIEWER_FLAG(adpf_use_fmq_channel_fixed)
  LAYERVIEWER_FLAG(graphite_renderengine_preview_rollout)
  LAYERVIEWER_FLAG(arr_setframerate_gte_enum)
  LAYERVIEWER_FLAG(adpf_fmq_sf)
  LAYERVIEWER_FLAG(connected_display)
  LAYERVIEWER_FLAG(frame_rate_category_mrr)
  LAYERVIEWER_FLAG(enable_small_area_detection)
  LAYERVIEWER_FLAG(stable_edid_ids)
  LAYERVIEWER_FLAG(misc1)
  LAYERVIEWER_FLAG(vrr_config)
  LAYERVIEWER_FLAG(hdcp_level_hal)
  LAYERVIEWER_FLAG(multithreaded_present)
  LAYERVIEWER_FLAG(add_sf_skipped_frames_to_trace)
  LAYERVIEWER_FLAG(use_known_refresh_rate_for_fps_consistency)
  LAYERVIEWER_FLAG(cache_when_source_crop_layer_only_moved)
  LAYERVIEWER_FLAG(enable_fro_dependent_features)
  LAYERVIEWER_FLAG(display_protected)
  LAYERVIEWER_FLAG(fp16_client_target)
  LAYERVIEWER_FLAG(game_default_frame_rate)
  LAYERVIEWER_FLAG(enable_layer_command_batching)
  LAYERVIEWER_FLAG(vulkan_renderengine)
  LAYERVIEWER_FLAG(renderable_buffer_usage)
  LAYERVIEWER_FLAG(restore_blur_step)
  LAYERVIEWER_FLAG(dont_skip_on_early_ro)
  LAYERVIEWER_FLAG(protected_if_client)
  LAYERVIEWER_FLAG(ce_fence_promise)
  LAYERVIEWER_FLAG(commit_not_composited)
  LAYERVIEWER_FLAG(detached_mirror)
  LAYERVIEWER_FLAG(dim_in_gamma_space_adjusted_for_display_size)
  LAYERVIEWER_FLAG(force_compile_graphite_renderengine)
  LAYERVIEWER_FLAG(graphite_renderengine)
  LAYERVIEWER_FLAG(latch_unsignaled_with_auto_refresh_changed)
  LAYERVIEWER_FLAG(monitor_buffer_fences)
  LAYERVIEWER_FLAG(skip_invisible_windows_in_input)
  LAYERVIEWER_FLAG(sdef_future_graphics_composer_callbacks)
  LAYERVIEWER_FLAG(single_hop_screenshot)
  LAYERVIEWER_FLAG(true_hdr_screenshots)
  LAYERVIEWER_FLAG(view_is_root_for_insets)
  LAYERVIEWER_FLAG(vsync_predictor_recovery)
  LAYERVIEWER_FLAG(local_tonemap_screenshots)
  LAYERVIEWER_FLAG(begone_bright_hlg)
  LAYERVIEWER_FLAG(flush_buffer_slots_to_uncache)
  LAYERVIEWER_FLAG(fp)
#undef LAYERVIEWER_FLAG
};

} // namespace android
