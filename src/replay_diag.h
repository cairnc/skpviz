// Diagnostic helpers for comparing our FE replay output against SF's own
// layer snapshots (android.surfaceflinger.layers packets).
//
// These are meant to survive stage-1 and keep being useful as we add CE/RE
// stages — if reconstructed layer geometry ever drifts from the device
// snapshot, the first thing we'll want is this diff and hierarchy walk.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

#include <android-base/stringprintf.h>

#include <FrontEnd/LayerCreationArgs.h>
#include <FrontEnd/LayerHierarchy.h>
#include <FrontEnd/LayerLifecycleManager.h>
#include <FrontEnd/LayerSnapshot.h>
#include <FrontEnd/LayerSnapshotBuilder.h>
#include <FrontEnd/RequestedLayerState.h>
#include <perfetto/trace/perfetto_trace.pb.h>

namespace layerviewer::diag {

// Relative-epsilon float compare used by the per-layer diff below. Floats
// stored in the two proto sources (our CapturedFrame vs SF's own
// LayersSnapshotProto) can differ by a ULP after reserialization; 0.01 is
// well below any real geometry delta.
inline bool NearlyEq(float a, float b, float eps = 0.01f) {
    return std::fabs(a - b) <= eps;
}

struct DiffStats {
    int both = 0;
    int diffFields = 0;
    int onlyOurs = 0;
    int onlySf = 0;
};

// Walks up the parent chain via both LayerHierarchyBuilder and
// LayerLifecycleManager and prints each hop. Handy when a layer is missing
// from the hierarchy but present in lifecycle (or vice versa): a divergence
// anywhere along the chain points at the broken link. The hierarchy param is
// non-const because LayerHierarchyBuilder::getDebugString isn't const in
// upstream.
inline void dumpLayerDiagnostics(
    uint32_t id,
    android::surfaceflinger::frontend::LayerHierarchyBuilder &hierarchy,
    const android::surfaceflinger::frontend::LayerLifecycleManager &lifecycle) {
    printf("    hier[self]: %s\n", hierarchy.getDebugString(id, 0).c_str());
    uint32_t walk = id;
    for (int d = 0; d < 8 && walk != UNASSIGNED_LAYER_ID; d++) {
        uint32_t parent = UNASSIGNED_LAYER_ID;
        bool found = false;
        for (const auto &rls : lifecycle.getLayers()) {
            if (rls && rls->id == walk) {
                printf("    chain[%d]: id=%u name=%s parent=%u canBeRoot=%d\n",
                       d, rls->id, rls->name.c_str(), rls->parentId,
                       rls->canBeRoot ? 1 : 0);
                parent = rls->parentId;
                found = true;
                break;
            }
        }
        if (!found) {
            printf("    chain[%d]: id=%u NOT FOUND in lifecycle\n", d, walk);
            break;
        }
        walk = parent;
    }
}

// Build a map of our snapshots keyed by layer id. Only includes Reachable
// snapshots — SF's android.surfaceflinger.layers data source is gated behind
// the main hierarchy too (TRACE_EXTRA is off by default), so comparing the
// reachable set is apples-to-apples.
inline std::unordered_map<
    uint32_t, const android::surfaceflinger::frontend::LayerSnapshot *>
indexOurSnapshots(
    android::surfaceflinger::frontend::LayerSnapshotBuilder &builder) {
    using android::surfaceflinger::frontend::LayerSnapshot;
    std::unordered_map<uint32_t, const LayerSnapshot *> out;
    for (const auto &snap : builder.getSnapshots()) {
        if (!snap)
            continue;
        if (snap->reachablilty != LayerSnapshot::Reachablilty::Reachable)
            continue;
        out[static_cast<uint32_t>(snap->path.id)] = snap.get();
    }
    return out;
}

inline std::unordered_map<uint32_t, const perfetto::protos::LayerProto *>
indexSfLayers(const perfetto::protos::LayersSnapshotProto &snap) {
    std::unordered_map<uint32_t, const perfetto::protos::LayerProto *> out;
    for (int i = 0; i < snap.layers().layers_size(); i++) {
        const auto &l = snap.layers().layers(i);
        out[static_cast<uint32_t>(l.id())] = &l;
    }
    return out;
}

// Compare reconstructed snapshots against SF's own LayersSnapshotProto.
// Prints per-layer field diffs (bounds, layerStack, opaque) and presence
// diffs. Returns aggregate counts. When `diagnoseOnlySf` is true, also prints
// the hierarchy + lifecycle parent chain for each layer present on SF but
// missing from our set — useful for diagnosing orphan bugs.
inline DiffStats diffSnapshotsAgainstSf(
    android::surfaceflinger::frontend::LayerSnapshotBuilder &builder,
    android::surfaceflinger::frontend::LayerHierarchyBuilder &hierarchy,
    const android::surfaceflinger::frontend::LayerLifecycleManager &lifecycle,
    const perfetto::protos::LayersSnapshotProto &sfSnap,
    bool diagnoseOnlySf = true) {
    auto oursById = indexOurSnapshots(builder);
    auto sfById = indexSfLayers(sfSnap);

    DiffStats stats;
    for (const auto &[id, s] : oursById) {
        if (sfById.count(id)) {
            stats.both++;
        } else {
            stats.onlyOurs++;
            printf("  only-ours id=%u name=%s vis=%d isHiddenByPolicy=%d\n", id,
                   s->name.c_str(), s->isVisible ? 1 : 0,
                   s->isHiddenByPolicyFromParent ? 1 : 0);
        }
    }
    for (const auto &[id, lp] : sfById) {
        if (oursById.count(id))
            continue;
        stats.onlySf++;
        printf("  only-sf   id=%u name=%s parent=%d\n", id, lp->name().c_str(),
               lp->parent());
        if (diagnoseOnlySf)
            dumpLayerDiagnostics(id, hierarchy, lifecycle);
    }

    int reported = 0;
    for (const auto &[id, ours] : oursById) {
        auto it = sfById.find(id);
        if (it == sfById.end())
            continue;
        const auto &sf = *it->second;

        std::string mismatches;
        if (ours->name != sf.name())
            mismatches += " name{" + ours->name + " vs " + sf.name() + "}";
        if (ours->outputFilter.layerStack.id != sf.layer_stack())
            mismatches += android::base::StringPrintf(
                " layerStack{%u vs %u}", ours->outputFilter.layerStack.id,
                sf.layer_stack());
        if (sf.has_bounds()) {
            const auto &b = sf.bounds();
            if (!(NearlyEq(ours->geomLayerBounds.left, b.left()) &&
                  NearlyEq(ours->geomLayerBounds.top, b.top()) &&
                  NearlyEq(ours->geomLayerBounds.right, b.right()) &&
                  NearlyEq(ours->geomLayerBounds.bottom, b.bottom()))) {
                mismatches += android::base::StringPrintf(
                    " bounds{(%g,%g,%g,%g) vs (%g,%g,%g,%g)}",
                    ours->geomLayerBounds.left, ours->geomLayerBounds.top,
                    ours->geomLayerBounds.right, ours->geomLayerBounds.bottom,
                    b.left(), b.top(), b.right(), b.bottom());
            }
        }
        if (sf.has_is_opaque() && ours->contentOpaque != sf.is_opaque())
            mismatches += android::base::StringPrintf(
                " opaque{%d vs %d}", ours->contentOpaque ? 1 : 0,
                sf.is_opaque() ? 1 : 0);

        if (!mismatches.empty()) {
            stats.diffFields++;
            if (reported < 20) {
                printf("  diff id=%u name=%s:%s\n", id, ours->name.c_str(),
                       mismatches.c_str());
                reported++;
            }
        }
    }
    if (stats.diffFields > reported)
        printf("  ... and %d more differing layers\n",
               stats.diffFields - reported);
    printf("  totals: both=%d (%d differ) only_ours=%d only_sf=%d\n",
           stats.both, stats.diffFields, stats.onlyOurs, stats.onlySf);
    return stats;
}

} // namespace layerviewer::diag
