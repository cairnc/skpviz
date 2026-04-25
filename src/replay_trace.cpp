// replay_trace — CLI smoke-test for the FrontEnd replay.
//
// Reads a perfetto .pftrace that has both android.surfaceflinger.transactions
// and android.surfaceflinger.layers, drives the SF FrontEnd (LayerLifecycle-
// Manager → LayerHierarchyBuilder → LayerSnapshotBuilder) the same way the
// upstream layertracegenerator tool does, and dumps a text summary of the
// reconstructed snapshots so we can eyeball whether the FE is recreating
// state correctly.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <perfetto/trace/perfetto_trace.pb.h>

#include <Client.h>
#include <FrontEnd/LayerCreationArgs.h>
#include <FrontEnd/LayerHierarchy.h>
#include <FrontEnd/LayerLifecycleManager.h>
#include <FrontEnd/LayerSnapshot.h>
#include <FrontEnd/LayerSnapshotBuilder.h>
#include <FrontEnd/RequestedLayerState.h>
#include <Tracing/TransactionProtoParser.h>
#include <TransactionState.h>
#include <gui/LayerState.h>
#include <gui/WindowInfo.h>

#include "replay_diag.h"

using android::BBinder;
using android::layer_state_t;
using android::TransactionState;
using android::surfaceflinger::LayerCreationArgs;
using android::surfaceflinger::TransactionProtoParser;
using android::surfaceflinger::frontend::DisplayInfos;
using android::surfaceflinger::frontend::LayerHierarchyBuilder;
using android::surfaceflinger::frontend::LayerLifecycleManager;
using android::surfaceflinger::frontend::LayerSnapshotBuilder;
using android::surfaceflinger::frontend::RequestedLayerState;

namespace {

bool readFile(const std::string &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fprintf(stderr, "failed to open %s\n", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <trace.pftrace> [--dump-every N] [--dump-last]\n",
                argv[0]);
        return 1;
    }
    const std::string tracePath = argv[1];
    int dumpEvery = 0;
    bool dumpLast = true;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dump-every" && i + 1 < argc)
            dumpEvery = std::atoi(argv[++i]);
        else if (a == "--no-dump-last")
            dumpLast = false;
    }

    std::string blob;
    if (!readFile(tracePath, blob))
        return 1;

    perfetto::protos::Trace trace;
    if (!trace.ParseFromString(blob)) {
        fprintf(stderr, "failed to parse perfetto trace (got %zu bytes)\n",
                blob.size());
        return 1;
    }

    std::vector<const perfetto::protos::TransactionTraceEntry *> entries;
    entries.reserve(trace.packet_size());
    // Also index SF's own snapshots by vsync_id so we can diff against them.
    std::unordered_map<int64_t, const perfetto::protos::LayersSnapshotProto *>
        sfSnapshotsByVsync;
    for (int i = 0; i < trace.packet_size(); i++) {
        const auto &pkt = trace.packet(i);
        if (pkt.has_surfaceflinger_transactions()) {
            entries.push_back(&pkt.surfaceflinger_transactions());
        }
        if (pkt.has_surfaceflinger_layers_snapshot()) {
            const auto &snap = pkt.surfaceflinger_layers_snapshot();
            if (snap.has_vsync_id())
                sfSnapshotsByVsync[snap.vsync_id()] = &snap;
        }
    }

    printf("parsed %d packets, %zu transaction entries\n", trace.packet_size(),
           entries.size());

    // How many entries carry display info? If none, geomLayerBounds fall back
    // to the ±50000 no-display constant and will mismatch SF.
    int displayChangeEntries = 0;
    for (const auto *e : entries)
        if (e->displays_changed())
            displayChangeEntries++;
    printf("  entries with displays_changed=true: %d (displays in first such = "
           "%d)\n",
           displayChangeEntries,
           entries.front()->displays_changed()
               ? entries.front()->displays_size()
               : 0);
    if (entries.empty()) {
        fprintf(stderr,
                "no android.surfaceflinger.transactions packets — recapture "
                "with that data source enabled on a userdebug build\n");
        return 1;
    }

    TransactionProtoParser parser(
        std::make_unique<TransactionProtoParser::FlingerDataMapper>());
    LayerLifecycleManager lifecycleManager;
    LayerHierarchyBuilder hierarchyBuilder;
    LayerSnapshotBuilder snapshotBuilder;
    DisplayInfos displayInfos;

    // SF's perfetto transaction trace doesn't always emit a starting-state
    // entry with displays_changed=true (it depends on session mode). When it
    // doesn't, the FE falls back to a ±50000 "max display bounds" constant. We
    // can seed displayInfos from the first LayersSnapshotProto in the same
    // trace — LayersSnapshotProto.displays has the same geometry SF saw at
    // runtime.
    bool sawDisplayInfo = false;
    for (int i = 0; i < trace.packet_size() && !sawDisplayInfo; i++) {
        const auto &pkt = trace.packet(i);
        if (!pkt.has_surfaceflinger_layers_snapshot())
            continue;
        const auto &snap = pkt.surfaceflinger_layers_snapshot();
        for (int j = 0; j < snap.displays_size(); j++) {
            const auto &d = snap.displays(j);
            if (!d.has_size())
                continue;
            android::surfaceflinger::frontend::DisplayInfo info;
            info.info.displayId =
                android::ui::LogicalDisplayId{static_cast<int32_t>(d.id())};
            info.info.logicalWidth = d.size().w();
            info.info.logicalHeight = d.size().h();
            displayInfos.emplace_or_replace(
                android::ui::LayerStack::fromValue(d.layer_stack()), info);
            sawDisplayInfo = true;
        }
    }
    printf("  seeded %zu display(s) from LayersSnapshotProto\n",
           displayInfos.size());
    android::ShadowSettings globalShadowSettings{.ambientColor = {1, 1, 1, 1}};

    for (size_t i = 0; i < entries.size(); i++) {
        const auto &entry = *entries[i];
        // Force displayChanges=true on every entry when we have seeded info.
        // SF's rootSnapshot.geomLayerBounds is only recomputed when
        // displayChanges is true, otherwise it defaults back to ±50000 via
        // getRootSnapshot(). SF gets away with this because, at runtime, its
        // frontend caches each layer's bounds and new layers are rare. During
        // replay we see many additions in later entries, so we need a fresh
        // root every entry for them to inherit proper display-scale bounds.
        const bool forceDisplayChangedThisEntry = sawDisplayInfo;

        std::vector<std::unique_ptr<RequestedLayerState>> addedLayers;
        addedLayers.reserve(entry.added_layers_size());
        for (int j = 0; j < entry.added_layers_size(); j++) {
            LayerCreationArgs args;
            parser.fromProto(entry.added_layers(j), args);
            addedLayers.emplace_back(
                std::make_unique<RequestedLayerState>(args));
        }

        std::vector<TransactionState> transactions;
        transactions.reserve(entry.transactions_size());
        for (int j = 0; j < entry.transactions_size(); j++) {
            TransactionState t = parser.fromProto(entry.transactions(j));
            for (auto &rcs : t.states) {
                if (rcs.state.what & layer_state_t::eInputInfoChanged) {
                    if (!rcs.state.windowInfoHandle->getInfo()
                             ->inputConfig.test(
                                 android::gui::WindowInfo::InputConfig::
                                     NO_INPUT_CHANNEL)) {
                        rcs.state.windowInfoHandle->editInfo()->token =
                            android::sp<BBinder>::make();
                    }
                }
            }
            transactions.emplace_back(std::move(t));
        }

        std::vector<std::pair<uint32_t, std::string>> destroyedHandles;
        destroyedHandles.reserve(entry.destroyed_layer_handles_size());
        for (int j = 0; j < entry.destroyed_layer_handles_size(); j++) {
            destroyedHandles.push_back({entry.destroyed_layer_handles(j), ""});
        }

        bool displayChanged =
            entry.displays_changed() || forceDisplayChangedThisEntry;
        if (entry.displays_changed()) {
            parser.fromProto(entry.displays(), displayInfos);
        }

        lifecycleManager.addLayers(std::move(addedLayers));
        lifecycleManager.applyTransactions(transactions,
                                           /*ignoreUnknownHandles=*/true);
        lifecycleManager.onHandlesDestroyed(destroyedHandles,
                                            /*ignoreUnknownHandles=*/true);

        hierarchyBuilder.update(lifecycleManager);

        LayerSnapshotBuilder::Args args{
            .root = hierarchyBuilder.getHierarchy(),
            .layerLifecycleManager = lifecycleManager,
            .displays = displayInfos,
            .displayChanges = displayChanged,
            .globalShadowSettings = globalShadowSettings,
            .supportsBlur = true,
            .forceFullDamage = false,
            .supportedLayerGenericMetadata = {},
            .genericLayerMetadataKeyMap = {},
        };
        snapshotBuilder.update(args);
        lifecycleManager.commitChanges();

        bool last = (i == entries.size() - 1);
        bool dumpNow =
            (dumpEvery > 0 && (i % dumpEvery) == 0) || (last && dumpLast);
        // At each entry where SF also emitted a layer snapshot, count-match
        // against our reconstruction. Catches drift partway through the replay,
        // not just at end.
        auto sfIt = sfSnapshotsByVsync.find(entry.vsync_id());
        if (sfIt != sfSnapshotsByVsync.end()) {
            auto oursById =
                layerviewer::diag::indexOurSnapshots(snapshotBuilder);
            auto sfById = layerviewer::diag::indexSfLayers(*sfIt->second);
            int onlyOurs = 0, onlySf = 0;
            for (const auto &[id, _] : oursById)
                if (!sfById.count(id))
                    onlyOurs++;
            for (const auto &[id, _] : sfById)
                if (!oursById.count(id))
                    onlySf++;
            if (dumpNow || onlyOurs || onlySf) {
                printf("vsync=%" PRId64
                       " entry=%zu ours=%zu sf=%zu diff=+%d/-%d\n",
                       entry.vsync_id(), i, oursById.size(), sfById.size(),
                       onlyOurs, onlySf);
            }
        } else if (dumpNow) {
            printf("vsync=%" PRId64
                   " entry=%zu layers=%zu (no sf snapshot at this vsync)\n",
                   entry.vsync_id(), i, lifecycleManager.getLayers().size());
        }
    }

    // Per-layer diff against the last SF snapshot whose vsync <= the last
    // entry's vsync. Layer snapshot packets don't always line up exactly with
    // transaction entries (snapshots are emitted on commit, transactions on
    // apply), so pick the latest SF snapshot at-or-before the final entry.
    {
        int64_t lastVsync = entries.back()->vsync_id();
        const perfetto::protos::LayersSnapshotProto *sfLast = nullptr;
        int64_t sfLastVsync = -1;
        for (const auto &kv : sfSnapshotsByVsync) {
            if (kv.first <= lastVsync && kv.first > sfLastVsync) {
                sfLastVsync = kv.first;
                sfLast = kv.second;
            }
        }
        if (sfLast) {
            printf("\n=== per-layer diff (our final vsync=%" PRId64
                   " vs SF vsync=%" PRId64 ") ===\n",
                   lastVsync, sfLastVsync);
            layerviewer::diag::diffSnapshotsAgainstSf(
                snapshotBuilder, hierarchyBuilder, lifecycleManager, *sfLast);
        }
    }

    if (dumpLast) {
        printf("\n=== final reconstructed snapshots (sample) ===\n");
        int shown = 0;
        for (const auto &snap : snapshotBuilder.getSnapshots()) {
            if (!snap)
                continue;
            if (!snap->isVisible)
                continue;
            printf("  %s\n", snap->getDebugString().c_str());
            if (++shown >= 15) {
                printf("  ... (more)\n");
                break;
            }
        }
    }

    return 0;
}
