// LayerMetadata shim — flat_map<int, bytes> + well-known keys. FE uses
// .mMap iteration and .clear() only; the Parcelable surface is a no-op.
#pragma once

#include <binder/Parcelable.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace android::gui {

enum {
  METADATA_OWNER_UID = 1,
  METADATA_WINDOW_TYPE = 2,
  METADATA_TASK_ID = 3,
  METADATA_MOUSE_CURSOR = 4,
  METADATA_ACCESSIBILITY_ID = 5,
  METADATA_OWNER_PID = 6,
  METADATA_DEQUEUE_TIME = 7,
  METADATA_GAME_MODE = 8,
  METADATA_CALLING_UID = 9,
};

struct LayerMetadata : public Parcelable {
  LayerMetadata() = default;
  LayerMetadata(const LayerMetadata &) = default;
  LayerMetadata &operator=(const LayerMetadata &) = default;

  std::unordered_map<uint32_t, std::vector<uint8_t>> mMap;

  bool has(uint32_t key) const { return mMap.count(key) != 0; }
  int32_t getInt32(uint32_t key, int32_t fallback) const {
    auto it = mMap.find(key);
    if (it == mMap.end() || it->second.size() < sizeof(int32_t))
      return fallback;
    int32_t v;
    memcpy(&v, it->second.data(), sizeof(v));
    return v;
  }
  void setInt32(uint32_t key, int32_t value) {
    std::vector<uint8_t> buf(sizeof(value));
    memcpy(buf.data(), &value, sizeof(value));
    mMap[key] = std::move(buf);
  }
  void merge(const LayerMetadata &other, bool /*eraseEmpty*/ = false) {
    for (const auto &[k, v] : other.mMap)
      mMap[k] = v;
  }
  bool empty() const { return mMap.empty(); }
  void clear() { mMap.clear(); }
};

// Mirrors GameManager.java — FE port reads `gui::GameMode` through this
// header in real AOSP, so keep the symbol in the same spot.
enum class GameMode : int32_t {
  Unsupported = 0,
  Standard = 1,
  Performance = 2,
  Battery = 3,
  Custom = 4,
  ftl_last = Custom,
};

} // namespace android::gui
