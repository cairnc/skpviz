// Trace loader: runs the SF FrontEnd pipeline against a .pftrace and
// captures a CapturedFrame per transaction entry. Mirrors the logic in the
// replay_trace CLI — keep the two in sync if you change one.

#include "layer_trace.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

#include <perfetto/trace/trace.pb.h>

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

CapturedLayer captureLayer(const LayerSnapshot &s) {
  CapturedLayer c;
  c.id = static_cast<uint32_t>(s.path.id);
  c.name = s.name;
  // LayerSnapshot has no `z` member — `globalZ` is the final traversal index
  // assigned by LayerSnapshotBuilder::sortSnapshotsByZ, which is what the
  // paint order actually uses.
  c.z = static_cast<int32_t>(s.globalZ);
  c.layerStack = s.outputFilter.layerStack.id;
  c.geomLayerBounds = s.geomLayerBounds;
  c.isVisible = s.isVisible;
  c.isHiddenByPolicy = s.isHiddenByPolicyFromParent;
  c.contentOpaque = s.contentOpaque;
  c.isOpaque = s.isOpaque;
  c.alpha = static_cast<float>(s.alpha);
  c.colorR = static_cast<float>(s.color.r);
  c.colorG = static_cast<float>(s.color.g);
  c.colorB = static_cast<float>(s.color.b);
  c.colorA = static_cast<float>(s.color.a);
  c.hasBuffer = s.externalTexture != nullptr;
  c.bufferFrame = s.frameNumber;
  c.bufferId = s.externalTexture ? s.externalTexture->getId() : 0;
  return c;
}

// Build the tree + rootIds for a captured frame by walking the reachable
// hierarchy. Matches the "main-hierarchy only" filter SF uses for its
// android.surfaceflinger.layers data source (TRACE_EXTRA off).
void populateHierarchy(CapturedFrame &frame,
                       LayerSnapshotBuilder &snapshotBuilder,
                       const LayerLifecycleManager &lifecycle) {
  // Walk snapshots and copy reachable ones into the frame.
  for (const auto &snap : snapshotBuilder.getSnapshots()) {
    if (!snap)
      continue;
    if (snap->reachablilty != LayerSnapshot::Reachablilty::Reachable)
      continue;
    CapturedLayer cl = captureLayer(*snap);
    // Look up parentId from the lifecycle manager (snapshot itself doesn't
    // expose it directly; we take the RequestedLayerState as the source of
    // truth).
    for (const auto &rls : lifecycle.getLayers()) {
      if (rls && rls->id == cl.id) {
        cl.parentId = rls->parentId;
        break;
      }
    }
    frame.layersById.emplace(cl.id, std::move(cl));
  }
  // Connect children under parents; identify roots.
  for (auto &[id, layer] : frame.layersById) {
    if (layer.parentId != 0xffffffff &&
        frame.layersById.count(layer.parentId)) {
      frame.layersById[layer.parentId].childIds.push_back(id);
    } else {
      frame.rootIds.push_back(id);
    }
  }
  // Sort children by z within each parent (ascending — higher z paints on
  // top). Same sort for roots.
  auto byZ = [&frame](uint32_t a, uint32_t b) {
    return frame.layersById[a].z < frame.layersById[b].z;
  };
  for (auto &[_, layer] : frame.layersById)
    std::sort(layer.childIds.begin(), layer.childIds.end(), byZ);
  std::sort(frame.rootIds.begin(), frame.rootIds.end(), byZ);
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
  }

  if (entries.empty()) {
    out->error =
        "no android.surfaceflinger.transactions packets — capture with that "
        "data source enabled on a userdebug build";
    return out;
  }

  DisplayInfos displayInfos;
  int32_t dominantW = 0, dominantH = 0;
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
      if (d.size().w() * d.size().h() > dominantW * dominantH) {
        dominantW = d.size().w();
        dominantH = d.size().h();
      }
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
      addedLayers.emplace_back(std::make_unique<RequestedLayerState>(args));
    }

    std::vector<TransactionState> transactions;
    transactions.reserve(entry.transactions_size());
    for (int j = 0; j < entry.transactions_size(); j++) {
      TransactionState t = parser.fromProto(entry.transactions(j));
      for (auto &rcs : t.states) {
        if (rcs.state.what & layer_state_t::eInputInfoChanged) {
          if (!rcs.state.windowInfoHandle->getInfo()->inputConfig.test(
                  android::gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL)) {
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
      for (const auto &[layerStack, info] : displayInfos) {
        if (info.info.logicalWidth * info.info.logicalHeight >
            dominantW * dominantH) {
          dominantW = info.info.logicalWidth;
          dominantH = info.info.logicalHeight;
        }
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
    populateHierarchy(frame, snapshotBuilder, lifecycleManager);
    out->frames.push_back(std::move(frame));

    lifecycleManager.commitChanges();
  }

  return out;
}

} // namespace layerviewer
