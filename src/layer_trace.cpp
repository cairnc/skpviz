// Trace loader: runs the SF FrontEnd pipeline against a .pftrace and
// captures a CapturedFrame per transaction entry. Mirrors the logic in the
// replay_trace CLI — keep the two in sync if you change one.

#include "layer_trace.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_set>

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

namespace layerviewer {
namespace {

using android::BBinder;
using android::layer_state_t;
using android::TransactionState;
using android::surfaceflinger::LayerCreationArgs;
using android::surfaceflinger::TransactionProtoParser;
using android::surfaceflinger::frontend::DisplayInfos;
using android::surfaceflinger::frontend::LayerHierarchyBuilder;
using android::surfaceflinger::frontend::LayerLifecycleManager;
using android::surfaceflinger::frontend::LayerSnapshot;
using android::surfaceflinger::frontend::LayerSnapshotBuilder;
using android::surfaceflinger::frontend::RequestedLayerState;

bool readFile(const std::string &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Deep-copy the reachable snapshots out of `snapshotBuilder` into `frame`,
// then stitch together parent/child ids using the lifecycle manager's
// RequestedLayerState (the snapshot itself doesn't carry parentId).
void captureFrame(CapturedFrame &frame, LayerSnapshotBuilder &snapshotBuilder,
                  const LayerLifecycleManager &lifecycle) {
    // Parent lookup by layer id. Built once; reused for every snapshot.
    std::unordered_map<uint32_t, uint32_t> parentOf;
    parentOf.reserve(lifecycle.getLayers().size());
    for (const auto &rls : lifecycle.getLayers()) {
        if (rls)
            parentOf[rls->id] = rls->parentId;
    }

    // Keep only reachable snapshots — matches SF's own android.surfaceflinger
    // .layers emit (offscreen hierarchy is gated behind TRACE_EXTRA).
    frame.snapshots.reserve(snapshotBuilder.getSnapshots().size());
    for (const auto &snap : snapshotBuilder.getSnapshots()) {
        if (!snap)
            continue;
        if (snap->reachablilty != LayerSnapshot::Reachablilty::Reachable)
            continue;
        frame.snapshots.push_back(*snap); // deep copy; sp<> refs travel with it
    }
    // Paint order (ascending globalZ) — makes rootIds/childIds deterministic.
    std::sort(frame.snapshots.begin(), frame.snapshots.end(),
              [](const LayerSnapshot &a, const LayerSnapshot &b) {
                  return a.globalZ < b.globalZ;
              });

    // Index + tree construction in paint order.
    frame.byLayerId.reserve(frame.snapshots.size());
    for (size_t i = 0; i < frame.snapshots.size(); i++) {
        const uint32_t id = static_cast<uint32_t>(frame.snapshots[i].path.id);
        frame.byLayerId[id] = i;
        auto it = parentOf.find(id);
        uint32_t parent =
            (it != parentOf.end()) ? it->second : UNASSIGNED_LAYER_ID;
        frame.parentByLayerId[id] = parent;
    }
    for (size_t i = 0; i < frame.snapshots.size(); i++) {
        const uint32_t id = static_cast<uint32_t>(frame.snapshots[i].path.id);
        uint32_t parent = frame.parentByLayerId[id];
        if (parent != UNASSIGNED_LAYER_ID && frame.byLayerId.count(parent)) {
            frame.childrenByLayerId[parent].push_back(id);
        } else {
            frame.rootIds.push_back(id);
        }
    }
}

} // namespace

std::unique_ptr<ReplayedTrace> LoadAndReplay(const std::string &path) {
    auto out = std::make_unique<ReplayedTrace>();
    out->path = path;

    std::string blob;
    if (!readFile(path, blob)) {
        std::ostringstream msg;
        msg << "open failed: " << std::strerror(errno);
        out->error = msg.str();
        return out;
    }

    auto trace = std::make_unique<perfetto::protos::Trace>();
    if (!trace->ParseFromString(blob)) {
        out->error = "not a valid perfetto Trace";
        return out;
    }
    out->packetCount = trace->packet_size();

    // Collect transaction entries and also seed displayInfos from the first
    // LayersSnapshotProto.displays — see replay_trace.cpp for the rationale.
    std::vector<const perfetto::protos::TransactionTraceEntry *> entries;
    entries.reserve(trace->packet_size());
    for (int i = 0; i < trace->packet_size(); i++) {
        const auto &pkt = trace->packet(i);
        if (pkt.has_surfaceflinger_transactions())
            entries.push_back(&pkt.surfaceflinger_transactions());
        if (pkt.has_surfaceflinger_layers_snapshot())
            out->layerSnapshotPacketCount++;
        // ProcessTree packets (from linux.process_stats with
        // scan_all_processes_on_start: true) give us a pid → cmdline mapping
        // so transactions can be annotated with a real process name. Later
        // packets for the same pid overwrite, so we keep the most recent.
        if (pkt.has_process_tree()) {
            const auto &pt = pkt.process_tree();
            for (int j = 0; j < pt.processes_size(); j++) {
                const auto &p = pt.processes(j);
                if (!p.has_pid() || p.cmdline_size() == 0)
                    continue;
                // cmdline is null-delimited on disk but the proto splits it
                // into repeated strings; the first token is the binary/package
                // name.
                out->pidNames[p.pid()] = p.cmdline(0);
            }
        }
    }

    if (entries.empty()) {
        out->error = "no android.surfaceflinger.transactions packets — capture "
                     "with that "
                     "data source enabled on a userdebug build";
        return out;
    }

    DisplayInfos displayInfos;
    bool sawDisplayInfo = false;
    for (int i = 0; i < trace->packet_size() && !sawDisplayInfo; i++) {
        const auto &pkt = trace->packet(i);
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

    TransactionProtoParser parser(
        std::make_unique<TransactionProtoParser::FlingerDataMapper>());
    LayerLifecycleManager lifecycleManager;
    LayerHierarchyBuilder hierarchyBuilder;
    LayerSnapshotBuilder snapshotBuilder;
    android::ShadowSettings globalShadowSettings{.ambientColor = {1, 1, 1, 1}};

    out->frames.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); i++) {
        const auto &entry = *entries[i];
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
        for (int j = 0; j < entry.destroyed_layer_handles_size(); j++)
            destroyedHandles.push_back({entry.destroyed_layer_handles(j), ""});

        bool displayChanged =
            entry.displays_changed() || forceDisplayChangedThisEntry;
        if (entry.displays_changed()) {
            parser.fromProto(entry.displays(), displayInfos);
        }
        // Pick the current largest display from live displayInfos — this is
        // what the transaction trace says the display is *right now*. Tracking
        // a historical max here would pin the output buffer to whichever size
        // came first and stretch later frames when the device rotates or
        // resizes.
        int32_t dominantW = 0, dominantH = 0;
        for (const auto &[layerStack, info] : displayInfos) {
            if (info.info.logicalWidth * info.info.logicalHeight >
                dominantW * dominantH) {
                dominantW = info.info.logicalWidth;
                dominantH = info.info.logicalHeight;
            }
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

        CapturedFrame frame;
        frame.vsyncId = entry.vsync_id();
        frame.tsNs = entry.elapsed_realtime_nanos();
        frame.addedCount = entry.added_layers_size();
        frame.destroyedHandleCount = entry.destroyed_layer_handles_size();
        frame.txnCount = entry.transactions_size();
        frame.displaysChanged = entry.displays_changed();
        frame.displayWidth = dominantW;
        frame.displayHeight = dominantH;
        // Pick the LayerStack + rotation off whichever display in the current
        // displayInfos best matches the dominant logical size — that's the one
        // Output will render to. Also convert RotationFlags → ui::Rotation for
        // setProjection's parameter type (first is a bitmask of ROT_{0,90,180,
        // 270}|FLIP_{H,V}; we only care about the rotation component).
        for (const auto &[layerStack, info] : displayInfos) {
            if (info.info.logicalWidth == dominantW &&
                info.info.logicalHeight == dominantH) {
                frame.displayLayerStack = layerStack;
                switch (info.rotationFlags &
                        (android::ui::Transform::ROT_90 |
                         android::ui::Transform::ROT_180)) {
                case android::ui::Transform::ROT_90:
                    frame.displayRotation = android::ui::ROTATION_90;
                    break;
                case android::ui::Transform::ROT_180:
                    frame.displayRotation = android::ui::ROTATION_180;
                    break;
                case android::ui::Transform::ROT_90 |
                    android::ui::Transform::ROT_180:
                    frame.displayRotation = android::ui::ROTATION_270;
                    break;
                default:
                    frame.displayRotation = android::ui::ROTATION_0;
                    break;
                }
                break;
            }
        }
        captureFrame(frame, snapshotBuilder, lifecycleManager);
        out->frames.push_back(std::move(frame));

        // Snapshot per-transaction summary for the Transactions window. Read
        // directly from the proto (not from the parsed TransactionState) so we
        // capture fields the proto parser discards (post_time, transaction_id,
        // merged ids) without re-plumbing the parser.
        const size_t frameIndex = out->frames.size() - 1;
        const size_t txnBegin = out->transactions.size();
        for (int j = 0; j < entry.transactions_size(); j++) {
            const auto &tp = entry.transactions(j);
            CapturedTransaction ct;
            ct.frameIndex = frameIndex;
            ct.pid = tp.pid();
            ct.uid = tp.uid();
            ct.inputEventId = tp.input_event_id();
            ct.vsyncId = tp.vsync_id();
            ct.postTimeNs = tp.post_time();
            ct.transactionId = tp.transaction_id();
            // The TransactionState proto's `pid` field is optional and
            // some producers leave it unset, dumping all those txns
            // into a phantom "pid 0" track with apps mixed together.
            // libgui builds transactionId as (pid << 32) | counter,
            // so the high half is a reliable fallback.
            if (ct.pid == 0) {
                int32_t derivedPid =
                    static_cast<int32_t>(ct.transactionId >> 32);
                if (derivedPid > 0)
                    ct.pid = derivedPid;
            }
            ct.layerChanges = tp.layer_changes_size();
            ct.displayChanges = tp.display_changes_size();
            ct.affectedLayerIds.reserve(tp.layer_changes_size());
            ct.layerStateChanges.reserve(tp.layer_changes_size());
            std::unordered_set<uint32_t> seen;
            for (int k = 0; k < tp.layer_changes_size(); k++) {
                const auto &lc = tp.layer_changes(k);
                CapturedLayerChange clc;
                clc.layerId = lc.layer_id();
                clc.what = lc.what();
                ct.layerStateChanges.push_back(clc);
                if (seen.insert(lc.layer_id()).second)
                    ct.affectedLayerIds.push_back(lc.layer_id());
            }
            ct.mergedTransactionIds.reserve(tp.merged_transaction_ids_size());
            for (int k = 0; k < tp.merged_transaction_ids_size(); k++)
                ct.mergedTransactionIds.push_back(tp.merged_transaction_ids(k));
            out->transactions.push_back(std::move(ct));
        }
        out->transactionRangeByFrame.emplace_back(txnBegin,
                                                  out->transactions.size());

        lifecycleManager.commitChanges();
    }

    return out;
}

} // namespace layerviewer
