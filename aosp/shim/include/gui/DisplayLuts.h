// DisplayLuts shim. We don't do binder IPC, so drop the Parcelable-ness and
// keep just the data LayerSettings touches.
#pragma once

#include <android-base/unique_fd.h>
#include <cstdint>
#include <vector>

namespace android::gui {

struct DisplayLuts {
  struct Entry {
    Entry() = default;
    Entry(int32_t d, int32_t s, int32_t k)
        : dimension(d), size(s), samplingKey(k) {}
    int32_t dimension = 0;
    int32_t size = 0;
    int32_t samplingKey = 0;
  };

  DisplayLuts() = default;
  // Matches real AOSP constructor: (fd, offsets, dimensions, sizes, keys)
  // — the per-entry vectors get fused into lutProperties.
  DisplayLuts(base::unique_fd lutfd, std::vector<int32_t> lutoffsets,
              std::vector<int32_t> lutdimensions, std::vector<int32_t> lutsizes,
              std::vector<int32_t> lutsamplingKeys)
      : fd(std::move(lutfd)), offsets(std::move(lutoffsets)) {
    lutProperties.reserve(offsets.size());
    for (size_t i = 0; i < offsets.size(); ++i) {
      lutProperties.emplace_back(lutdimensions[i], lutsizes[i],
                                 lutsamplingKeys[i]);
    }
  }

  base::unique_fd fd;
  std::vector<int32_t> offsets;
  std::vector<Entry> lutProperties;

  const base::unique_fd &getLutFileDescriptor() const { return fd; }
  const std::vector<int32_t> &getOffsets() const { return offsets; }
  const std::vector<Entry> &getLutProperties() const { return lutProperties; }
};

} // namespace android::gui
