// Per-entry replay state. The SurfaceFlinger FrontEnd's LayerSnapshotBuilder
// maintains a stateful current-frame working set, so to let the UI scrub to
// any vsync we deep-copy its output after each commit. The UI reads real
// `LayerSnapshot` fields (no parallel struct, no drift when SF adds a new
// field) and the snapshot's sp<ExternalTexture> → sp<GraphicBuffer> refs
// travel with the copy so the per-buffer GL texture stays alive.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <FrontEnd/LayerSnapshot.h>
#include <ui/LayerStack.h>
#include <ui/Transform.h>

namespace layerviewer {

struct CapturedFrame {
    int64_t vsyncId = 0;
    int64_t tsNs = 0;
    int addedCount = 0;
    int destroyedHandleCount = 0;
    int txnCount = 0;
    bool displaysChanged = false;
    int32_t displayWidth = 0;
    int32_t displayHeight = 0;
    // Dominant display's traced layer stack + rotation. CompositionEngine's
    // Output needs these: `setLayerFilter(layerStack)` scopes which snapshots
    // land on this output, and `setProjection(orientation, ...)` feeds the
    // DisplaySettings.orientation that RenderEngine applies to the output.
    android::ui::LayerStack displayLayerStack{android::ui::INVALID_LAYER_STACK};
    android::ui::Rotation displayRotation = android::ui::ROTATION_0;

    // Reachable snapshots in paint order (globalZ ascending). Matches SF's
    // own android.surfaceflinger.layers emit set — offscreen snapshots are
    // dropped.
    std::vector<android::surfaceflinger::frontend::LayerSnapshot> snapshots;
    // layer id (snapshot.path.id) → index into `snapshots`.
    std::unordered_map<uint32_t, size_t> byLayerId;
    // layer id → parent id (0xffffffff / UNASSIGNED_LAYER_ID for roots).
    // LayerSnapshot doesn't carry parentId directly; it's on
    // RequestedLayerState in the lifecycle manager, so we capture it at
    // replay time alongside the snapshot copy.
    std::unordered_map<uint32_t, uint32_t> parentByLayerId;
    // layer id → child ids in paint order (ascending globalZ).
    std::unordered_map<uint32_t, std::vector<uint32_t>> childrenByLayerId;
    // top-level reachable layers, in paint order.
    std::vector<uint32_t> rootIds;

    // Convenience accessors so the UI doesn't have to rebuild iterators.
    const android::surfaceflinger::frontend::LayerSnapshot *
    snapshot(uint32_t layerId) const {
        auto it = byLayerId.find(layerId);
        return it == byLayerId.end() ? nullptr : &snapshots[it->second];
    }
    const std::vector<uint32_t> &children(uint32_t layerId) const {
        static const std::vector<uint32_t> kEmpty;
        auto it = childrenByLayerId.find(layerId);
        return it == childrenByLayerId.end() ? kEmpty : it->second;
    }
};

// Per-transaction summary for the Transactions window. One per
// TransactionState in the original .pftrace; `frameIndex` points back to
// the CapturedFrame whose entry contained this transaction, so the UI
// can sync the timeline to the frame when a transaction is selected.
struct CapturedLayerChange {
    uint32_t layerId = 0;
    // `what` bitmask from LayerState — which fields were touched. The UI
    // decodes this into flag names (ePositionChanged, eBufferChanged, …).
    uint64_t what = 0;
};

struct CapturedTransaction {
    size_t frameIndex = 0;    // index into ReplayedTrace::frames
    int32_t pid = 0;          // source process
    int32_t uid = 0;          // source app uid
    int32_t inputEventId = 0; // may be 0 if this txn isn't input-driven
    int64_t vsyncId = 0;      // TransactionState.vsync_id
    int64_t postTimeNs = 0;   // when the client posted the txn
    uint64_t transactionId = 0;
    int layerChanges = 0;
    int displayChanges = 0;
    // Per-layer changes in the order the transaction listed them. Same
    // layer id can appear multiple times if the transaction touched it
    // more than once (repeated LayerState entries in one TransactionState).
    std::vector<CapturedLayerChange> layerStateChanges;
    // Deduplicated list of affected layer ids in first-seen order — used
    // by the Affected-Layers mini-table. Each id points into
    // `layerStateChanges` for its per-field bitmask summary.
    std::vector<uint32_t> affectedLayerIds;
    // Merged-into ids (another transaction's id merged into this one).
    std::vector<uint64_t> mergedTransactionIds;
};

struct ReplayedTrace {
    std::string path;
    std::string error;
    std::vector<CapturedFrame> frames;
    std::vector<CapturedTransaction> transactions;
    // transactionRangeByFrame[i] = [first, last) indices into `transactions`
    // for frame i. Lets the Transactions window filter to the current frame
    // and vice versa.
    std::vector<std::pair<size_t, size_t>> transactionRangeByFrame;
    // pid → cmdline / process name, built from ProcessTree packets in the
    // trace (if the capture enabled `linux.process_stats`). Empty for older
    // SF-only captures.
    std::unordered_map<int32_t, std::string> pidNames;
    int packetCount = 0;
    int layerSnapshotPacketCount = 0;
};

// Load + replay synchronously. `error` set and `frames` empty on failure.
std::unique_ptr<ReplayedTrace> LoadAndReplay(const std::string &path);

} // namespace layerviewer
