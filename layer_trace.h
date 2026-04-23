// Replayed-trace data model. Loading a .pftrace runs the full SurfaceFlinger
// FrontEnd pipeline (LayerLifecycleManager → LayerHierarchyBuilder →
// LayerSnapshotBuilder) to reconstruct per-entry snapshots — see
// replay_trace.cpp for the reference CLI that proved byte-for-byte
// equivalence with SF's runtime snapshots. The UI just renders the captured
// frame summaries; it never touches live FE state.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <ui/Rect.h>

namespace layerviewer {

struct CapturedLayer {
  uint32_t id = 0;
  uint32_t parentId = 0;
  std::string name;
  int32_t z = 0;
  uint32_t layerStack = 0;
  android::FloatRect geomLayerBounds;
  bool isVisible = false;
  bool isHiddenByPolicy = false;
  bool contentOpaque = false;
  bool isOpaque = false;
  float alpha = 1.f;
  float colorR = 0.f, colorG = 0.f, colorB = 0.f, colorA = 1.f;
  bool hasBuffer = false;
  uint64_t bufferFrame = 0;
  // Unique id of the externalTexture (SurfaceFlinger's GraphicBuffer id).
  // Used as the "big number" watermark in the wireframe so overlapping
  // layers with identical-looking bounds can still be told apart.
  uint64_t bufferId = 0;

  // Child ids in traversal order — only populated for reachable layers.
  std::vector<uint32_t> childIds;
};

struct CapturedFrame {
  int64_t vsyncId = 0;
  int64_t tsNs = 0;
  int addedCount = 0;
  int destroyedHandleCount = 0;
  int txnCount = 0;
  bool displaysChanged = false;

  // Reachable layers only (matches SF's android.surfaceflinger.layers emit).
  std::unordered_map<uint32_t, CapturedLayer> layersById;
  std::vector<uint32_t> rootIds;

  // The dominant display at this entry — used by the preview canvas.
  int32_t displayWidth = 0;
  int32_t displayHeight = 0;
};

struct ReplayedTrace {
  std::string path;
  std::string error;
  std::vector<CapturedFrame> frames;
  int packetCount = 0;
  int layerSnapshotPacketCount = 0;
};

// Load + replay synchronously. Returns a populated ReplayedTrace; on failure,
// `error` is set and `frames` is empty.
std::unique_ptr<ReplayedTrace> LoadAndReplay(const std::string &path);

} // namespace layerviewer
