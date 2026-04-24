// layerviewer — interactive UI on top of the FE replay pipeline.
//
// Usage:
//     layerviewer [trace.pftrace]
// or drop a .pftrace onto the window. The load path runs the SurfaceFlinger
// FrontEnd (LayerLifecycleManager → LayerHierarchyBuilder →
// LayerSnapshotBuilder) against every transaction entry in the trace and
// captures a CapturedFrame summary per entry (see layer_trace.{h,cpp}).

#include <SDL3/SDL.h>

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include "imgui_impl_opengl3_loader.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <android-base/stringprintf.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h" // DockBuilder

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"

#include <ui/GraphicBuffer.h>

#include "layer_trace.h"

// CompositionEngine + SkiaRenderEngine pipeline. The Preview composes each
// frame's LayerSnapshots into a GraphicBuffer the same way SurfaceFlinger
// would: every reachable snapshot is wrapped in a resident LayerFE, Output
// prepares + generates client composition requests, SkiaRenderEngine draws
// into an ExternalTexture backed by our GraphicBuffer, and ImGui samples
// the resulting GL texture.
#include "LayerFE.h"
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/CompositionRefreshArgs.h>
#include <compositionengine/Display.h>
#include <compositionengine/DisplayColorProfileCreationArgs.h>
#include <compositionengine/DisplayCreationArgs.h>
#include <compositionengine/LayerFE.h>
#include <compositionengine/OutputColorSetting.h>
#include <compositionengine/impl/CompositionEngine.h>
#include <compositionengine/impl/Display.h>
#include <compositionengine/impl/OutputCompositionState.h>
#include <renderengine/DesktopGLRenderEngineFactory.h>
#include <renderengine/DisplaySettings.h>
#include <renderengine/LayerSettings.h>
#include <renderengine/RenderEngine.h>
#include <renderengine/impl/ExternalTexture.h>
#include <ui/DisplayId.h>
#include <ui/GraphicTypes.h>
#include <ui/Size.h>

#include <ui/GraphicBuffer.h>

namespace {

// ---------------------------------------------------------------------------
// Composition pipeline — SurfaceFlinger-style.
//
// We hold a RenderEngine + CompositionEngine + Display long-lived on the GL
// thread. Each Preview frame:
//   1. Look up / create a LayerFE per reachable LayerSnapshot, swap in the
//      current frame's snapshot (matches upstream SF::updateLayerSnapshots).
//   2. Build a CompositionRefreshArgs with those LayerFEs and ask Display to
//      prepare + generate client-composition LayerSettings.
//   3. RenderEngine::drawLayers composes into a GraphicBuffer-backed
//      ExternalTexture we own. The GraphicBuffer's GL texture is what ImGui
//      samples for the Preview window.
// ---------------------------------------------------------------------------

// The public `compositionengine::Output` interface marks updateCompositionState
// / writeCompositionState / generateClientCompositionRequests as protected
// because upstream SF reaches them only through Output::present() after it has
// set up a RenderSurface. We don't have a RenderSurface — we own our own
// ExternalTexture output buffer — so we bypass present() and call the compose
// steps directly. The subclass exists purely to re-export those three methods;
// behaviour is unchanged.
class LayerViewerDisplay : public android::compositionengine::impl::Display {
public:
  using android::compositionengine::impl::Output::
      generateClientCompositionRequests;
  using android::compositionengine::impl::Output::updateCompositionState;
  using android::compositionengine::impl::Output::writeCompositionState;
};

struct LayerViewerCompositor {
  std::unique_ptr<android::renderengine::RenderEngine> re;
  std::unique_ptr<android::compositionengine::CompositionEngine> ce;
  std::shared_ptr<LayerViewerDisplay> display;
  int displayW = 0, displayH = 0;

  android::sp<android::GraphicBuffer> outputBuffer;
  std::shared_ptr<android::renderengine::impl::ExternalTexture> outputTexture;
  int outputW = 0, outputH = 0;

  // Resident LayerFEs keyed by snapshot path.id. SF reuses LayerFE objects
  // across frames for the same layer, just swapping mSnapshot each vsync. We
  // mirror that: snapshots are scrubable so stale entries linger, but that's
  // cheap and avoids per-frame sp<>::make churn.
  std::unordered_map<uint64_t, android::sp<android::LayerFE>> layerFEs;

  // Explicit teardown because member-wise move-assignment would destroy the
  // RenderEngine before outputTexture unmapped itself against it — the
  // compiler-generated order is declaration order, but ~ExternalTexture's
  // unmapExternalTextureBuffer call on mRenderEngine needs RE alive.
  void destroy() {
    layerFEs.clear();
    outputTexture.reset();
    outputBuffer.clear();
    display.reset();
    ce.reset();
    re.reset();
  }

  void init(sk_sp<const GrGLInterface> glInterface) {
    using android::renderengine::RenderEngine;
    using android::renderengine::RenderEngineCreationArgs;
    auto args = RenderEngineCreationArgs::Builder()
                    .setPixelFormat(1 /*RGBA_8888*/)
                    .setImageCacheSize(0)
                    .setEnableProtectedContext(false)
                    .setPrecacheToneMapperShaderOnly(false)
                    .setBlurAlgorithm(RenderEngine::BlurAlgorithm::NONE)
                    .setContextPriority(RenderEngine::ContextPriority::MEDIUM)
                    .setThreaded(RenderEngine::Threaded::NO)
                    .setGraphicsApi(RenderEngine::GraphicsApi::GL)
                    .setSkiaBackend(RenderEngine::SkiaBackend::GANESH)
                    .build();
    re = android::renderengine::createDesktopGLRenderEngine(
        args, std::move(glInterface));

    ce = android::compositionengine::impl::createCompositionEngine();
    ce->setRenderEngine(re.get());
  }

  void ensureDisplay(int w, int h, const layerviewer::CapturedFrame &frame) {
    using namespace android::compositionengine;
    if (display && displayW == w && displayH == h)
      return;
    auto displayArgs = DisplayCreationArgsBuilder()
                           .setId(android::GpuVirtualDisplayId(0))
                           .setPixels(android::ui::Size(w, h))
                           .setName("layerviewer-preview")
                           .build();
    // Use the templated factory so the returned shared_ptr is typed as our
    // subclass (which re-exposes the protected compose entry points). The
    // stock impl::createDisplay would return shared_ptr<impl::Display> and
    // strip the visibility change.
    display =
        impl::createDisplayTemplated<LayerViewerDisplay>(*ce, displayArgs);
    // CE derefs getDisplayColorProfile() without null-checking during
    // OutputLayer::updateCompositionState, so create one even though we
    // never honour any color management here.
    display->createDisplayColorProfile(
        DisplayColorProfileCreationArgsBuilder()
            .setHasWideColorGamut(false)
            .setHdrCapabilities(android::HdrCapabilities{})
            .setSupportedPerFrameMetadata(0)
            .setHwcColorModes({})
            .Build());
    display->setCompositionEnabled(true);
    // Upstream SF wires a RenderSurface via setSurface(...), whose
    // setDisplaySize() side-effects the framebufferSpace + displaySpace
    // bounds. We don't wire a RenderSurface (no BufferQueue, no swapchain —
    // we own a single ExternalTexture output), so poke the bounds directly.
    // Output::setProjection LOG_FATAL_IFs on INVALID_RECT otherwise.
    display->editState().framebufferSpace.setBounds({w, h});
    display->editState().displaySpace.setBounds({w, h});
    display->setLayerFilter(android::ui::LayerFilter{
        frame.displayLayerStack, /*toInternalDisplay=*/false});
    android::Rect rect(0, 0, w, h);
    display->setProjection(frame.displayRotation, rect, rect);
    displayW = w;
    displayH = h;
  }

  void ensureOutput(int w, int h) {
    if (w == outputW && h == outputH && outputBuffer)
      return;
    outputW = w;
    outputH = h;
    outputBuffer = android::sp<android::GraphicBuffer>::make(
        static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 /*RGBA_8888*/, 1,
        android::GraphicBuffer::USAGE_HW_RENDER |
            android::GraphicBuffer::USAGE_HW_TEXTURE,
        "layerviewer-output");
    outputTexture =
        std::make_shared<android::renderengine::impl::ExternalTexture>(
            outputBuffer, *re,
            android::renderengine::impl::ExternalTexture::Usage::WRITEABLE);
  }

  // Compose one frame into outputBuffer. Returns the GL texture name of the
  // composed buffer (0 if no display info is available).
  unsigned int composeFrame(const layerviewer::CapturedFrame &frame) {
    using namespace android::compositionengine;
    using namespace android::renderengine;
    using android::ui::Dataspace;

    const int rw = frame.displayWidth;
    const int rh = frame.displayHeight;
    if (rw <= 0 || rh <= 0)
      return 0;

    ensureOutput(rw, rh);
    ensureDisplay(rw, rh, frame);
    // Per-frame state that may drift even at a fixed size (layer stack + the
    // orientation we projection into): refresh on every compose so scrubbing
    // across a trace where these change doesn't wedge the output.
    display->setLayerFilter(android::ui::LayerFilter{
        frame.displayLayerStack, /*toInternalDisplay=*/false});
    android::Rect rect(0, 0, rw, rh);
    display->setProjection(frame.displayRotation, rect, rect);

    Layers layersVec;
    layersVec.reserve(frame.snapshots.size());
    for (const auto &snap : frame.snapshots) {
      const uint64_t id = static_cast<uint64_t>(snap.path.id);
      auto &fe = layerFEs[id];
      if (!fe)
        fe = android::sp<android::LayerFE>::make(snap.name);
      fe->mSnapshot =
          std::make_unique<android::surfaceflinger::frontend::LayerSnapshot>(
              snap);
      layersVec.push_back(fe);
    }

    CompositionRefreshArgs refreshArgs;
    refreshArgs.outputs = {display};
    refreshArgs.layers = std::move(layersVec);
    refreshArgs.updatingGeometryThisFrame = true;
    refreshArgs.updatingOutputGeometryThisFrame = true;
    refreshArgs.outputColorSetting = OutputColorSetting::kUnmanaged;
    refreshArgs.forceOutputColorMode = android::ui::ColorMode::NATIVE;

    LayerFESet latchedLayers;
    display->prepare(refreshArgs, latchedLayers);
    display->updateCompositionState(refreshArgs);
    display->writeCompositionState(refreshArgs);

    std::vector<LayerFE *> outRefs;
    auto layerSettings = display->generateClientCompositionRequests(
        /*supportsProtectedContent=*/false, Dataspace::SRGB, outRefs);

    DisplaySettings displaySettings;
    displaySettings.physicalDisplay = rect;
    displaySettings.clip = rect;
    displaySettings.orientation = android::ui::Transform::ROT_0;
    displaySettings.outputDataspace = Dataspace::SRGB;
    displaySettings.maxLuminance = 500.f;
    displaySettings.targetLuminanceNits = 500.f;

    // drawLayers takes the base renderengine::LayerSettings type; LayerFE::
    // LayerSettings inherits from it with extra buffer-tracking fields. Slice
    // via copy — upstream SF does the same (Output::composeSurfaces pushes
    // through clientCompositionLayers as-is because the slicing happens
    // inside RenderEngine's caching layer).
    std::vector<LayerSettings> baseLayers;
    baseLayers.reserve(layerSettings.size());
    for (const auto &ls : layerSettings)
      baseLayers.push_back(ls);
    // ImGui leaves glViewport at the window's drawable size (smaller than a
    // phone-sized output buffer on a typical macOS window). Ganesh caches GL
    // state and assumes no one else touched it, so binding the output FBO
    // does *not* re-issue glViewport — the stale small viewport clips
    // composition to just the top rows of the 1080×2400 output. Explicitly
    // set viewport to the full output dimensions before handing off to RE.
    glViewport(0, 0, outputW, outputH);
    re->drawLayers(displaySettings, baseLayers, outputTexture,
                   android::base::unique_fd{});

    return outputBuffer ? outputBuffer->getGLTextureIfAny() : 0;
  }
};

constexpr uint32_t kUnassignedLayerId = 0xffffffffu;
constexpr float kPi = 3.14159265f;

struct View3D {
  float yawDeg = 25.f;        // rotation around vertical screen axis
  float pitchDeg = 10.f;      // rotation around horizontal screen axis
  float depthSpacing = 200.f; // per-layer Z step in device pixels
  float opacity = 0.8f;       // quad fill alpha (stroke stays opaque)
};

struct PreviewView {
  float zoom = 1.f; // 1.0 = fit-to-window
  float panX = 0.f; // extra offset in screen pixels (added after zoom)
  float panY = 0.f;
};

struct AppState {
  std::unique_ptr<layerviewer::ReplayedTrace> trace;
  int frameIndex = 0;
  uint32_t selectedLayerId = kUnassignedLayerId;
  // Set to true when selection comes from anywhere other than the tree (3D
  // wireframe click, inspector action, etc.) — the tree consumes it on the
  // next draw to auto-scroll to the selected node.
  bool scrollTreeToSelection = false;
  bool showInvisible = false;
  bool treeOnlyVisible =
      false;                   // tree filter: hide non-visible (keep ancestors)
  bool treeShortNames = true;  // winscope-style name shortening in tree
  bool resetLayoutOnce = true; // first-run default layout
  bool requestResetLayout = false; // View → Reset Layout
  // Transactions window state.
  int selectedTransactionIdx = -1; // index into ReplayedTrace::transactions
  bool autoSyncTimeline = true;    // auto-move frameIndex on txn selection
  bool scrollTxnTableToSelection = false;
  View3D wireframe3D;
  PreviewView preview;
};

struct FitTransform {
  float scale = 1.f;
  float tx = 0.f, ty = 0.f;
};

FitTransform FitRectToCanvas(float rw, float rh, float cw, float ch,
                             float pad) {
  FitTransform t;
  float availW = std::max(1.f, cw - 2 * pad);
  float availH = std::max(1.f, ch - 2 * pad);
  if (rw <= 0 || rh <= 0)
    return t;
  t.scale = std::min(availW / rw, availH / rh);
  t.tx = pad + (availW - rw * t.scale) * 0.5f;
  t.ty = pad + (availH - rh * t.scale) * 0.5f;
  return t;
}

// ---------------------------------------------------------------------------
// 3D math helpers (used by the exploded wireframe view)
// ---------------------------------------------------------------------------

struct Rotation3D {
  float cosYaw, sinYaw, cosPitch, sinPitch;
  float cx, cy, cz; // world-space pivot
};

Rotation3D MakeRotation(float yawDeg, float pitchDeg, float cx, float cy,
                        float cz) {
  return {std::cos(yawDeg * kPi / 180.f),
          std::sin(yawDeg * kPi / 180.f),
          std::cos(pitchDeg * kPi / 180.f),
          std::sin(pitchDeg * kPi / 180.f),
          cx,
          cy,
          cz};
}

// Applies yaw around Y then pitch around X around the pivot. Returns the
// post-rotation (X, Y, Z); Z is along screen-normal, used for depth sort.
void Rotate(const Rotation3D &R, float x, float y, float z, float &ox,
            float &oy, float &oz) {
  float X = (x - R.cx) * R.cosYaw - (z - R.cz) * R.sinYaw;
  float Y = (y - R.cy);
  float Z = (x - R.cx) * R.sinYaw + (z - R.cz) * R.cosYaw;
  float Y2 = Y * R.cosPitch - Z * R.sinPitch;
  float Z2 = Y * R.sinPitch + Z * R.cosPitch;
  ox = X;
  oy = Y2;
  oz = Z2;
}

ImVec2 Project3D(const Rotation3D &R, float x, float y, float z, float screenCx,
                 float screenCy, float scale) {
  float rx, ry, rz;
  Rotate(R, x, y, z, rx, ry, rz);
  (void)rz;
  return ImVec2(screenCx + rx * scale, screenCy + ry * scale);
}

// Point-in-convex-quad via cross-product signs. Works for any winding order.
bool PointInQuad(ImVec2 m, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d) {
  auto cross = [](ImVec2 p, ImVec2 q, ImVec2 r) {
    return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
  };
  float s1 = cross(a, b, m), s2 = cross(b, c, m);
  float s3 = cross(c, d, m), s4 = cross(d, a, m);
  return (s1 >= 0 && s2 >= 0 && s3 >= 0 && s4 >= 0) ||
         (s1 <= 0 && s2 <= 0 && s3 <= 0 && s4 <= 0);
}

// ---------------------------------------------------------------------------
// Color helpers (used by the Preview's fake graphic-buffer content)
// ---------------------------------------------------------------------------

void HsvToRgb(float h, float s, float v, float &r, float &g, float &b) {
  h = h - std::floor(h);
  float i = std::floor(h * 6.f);
  float f = h * 6.f - i;
  float p = v * (1.f - s);
  float q = v * (1.f - s * f);
  float u = v * (1.f - s * (1.f - f));
  switch (static_cast<int>(i) % 6) {
  case 0:
    r = v;
    g = u;
    b = p;
    return;
  case 1:
    r = q;
    g = v;
    b = p;
    return;
  case 2:
    r = p;
    g = v;
    b = u;
    return;
  case 3:
    r = p;
    g = q;
    b = v;
    return;
  case 4:
    r = u;
    g = p;
    b = v;
    return;
  case 5:
    r = v;
    g = p;
    b = q;
    return;
  }
}

// Deterministic light+dark shade pair per bufferId. Golden-ratio hue step
// so adjacent ids never land on similar hues.
void HashColorsForBuffer(uint64_t bufferId, SkColor4f &light, SkColor4f &dark) {
  float hue = (bufferId ? static_cast<float>(bufferId) : 0.5f) * 0.61803398875f;
  float r, g, b;
  HsvToRgb(hue, 0.40f, 0.95f, r, g, b);
  light = SkColor4f{r, g, b, 1.f};
  HsvToRgb(hue, 0.65f, 0.60f, r, g, b);
  dark = SkColor4f{r, g, b, 1.f};
}

// Largest SkFont size such that `text` fits within (maxW, maxH) bounds.
// Binary search on the font size — Skia can measureText but only at a
// specific size, so we iterate.
float FitFontSize(const char *text, float maxW, float maxH, float minSize,
                  float maxSize) {
  if (maxW <= 0 || maxH <= 0)
    return minSize;
  size_t len = std::strlen(text);
  if (len == 0)
    return minSize;
  float lo = minSize, hi = maxSize;
  for (int i = 0; i < 12; i++) {
    float mid = 0.5f * (lo + hi);
    SkFont probe(nullptr, mid);
    SkRect bounds;
    probe.measureText(text, len, SkTextEncoding::kUTF8, &bounds);
    if (bounds.width() <= maxW && bounds.height() <= maxH) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

// ---------------------------------------------------------------------------
// Layer tree
// ---------------------------------------------------------------------------

// Winscope-style name shortening for display in the tree. Handles the two
// common wrappers SF layer names get wrapped in:
//
//   - `Surface(name=<X>)/@0xhex ...`  → unwrap to <X>
//   - `<package>/<fqcn>#id`           → take part after the last '/'
//
// Anything else (`Task=12#164`, `TaskFragment{…}#115`, `[Gesture Monitor] …`)
// is returned unchanged. `=` is NOT treated as a separator in general — it
// appears inside metadata like `mode=fullscreen`, `Task=12`.
std::string shortLayerName(const std::string &full) {
  // Unwrap Surface(name=X)/@0xhex …
  constexpr const char kSurfacePrefix[] = "Surface(name=";
  constexpr size_t kSurfacePrefixLen = sizeof(kSurfacePrefix) - 1;
  if (full.compare(0, kSurfacePrefixLen, kSurfacePrefix) == 0) {
    size_t close = full.find(')', kSurfacePrefixLen);
    if (close != std::string::npos)
      return full.substr(kSurfacePrefixLen, close - kSurfacePrefixLen);
  }
  // Activity/package path: `com.x.y/com.x.y.Foo#N` → `com.x.y.Foo#N`.
  size_t slash = full.find_last_of('/');
  if (slash != std::string::npos)
    return full.substr(slash + 1);
  return full;
}

// Recursively draw one node, and push visible rows into `flat` in the order
// they appear — used for up/down arrow key navigation. `keep` (optional,
// for the "only visible" filter) limits recursion to the ids in the set.
void DrawLayerTreeNode(const layerviewer::CapturedFrame &frame, uint32_t id,
                       AppState &app, std::vector<uint32_t> &flat,
                       const std::unordered_set<uint32_t> *keep) {
  if (keep && !keep->count(id))
    return;
  const auto *snap = frame.snapshot(id);
  if (!snap)
    return;
  const auto &children = frame.children(id);
  // With the "only visible" filter, a parent may be kept only because some
  // descendant is visible. Treat it as a leaf if *it* has no kept children.
  bool hasKeptChild = false;
  if (keep) {
    for (uint32_t c : children)
      if (keep->count(c)) {
        hasKeptChild = true;
        break;
      }
  } else {
    hasKeptChild = !children.empty();
  }

  ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
      ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen;
  if (!hasKeptChild)
    flags |= ImGuiTreeNodeFlags_Leaf;
  const bool selected = app.selectedLayerId == id;
  if (selected)
    flags |= ImGuiTreeNodeFlags_Selected;

  ImGui::PushID(static_cast<int>(id));
  // Selection came from outside the tree (3D wireframe click, arrow-key
  // nav etc.): force this branch open so the row is reachable, scroll to
  // it, consume the flag.
  if (selected && app.scrollTreeToSelection)
    ImGui::SetNextItemOpen(true);
  const std::string label =
      app.treeShortNames ? shortLayerName(snap->name) : snap->name;
  bool open = ImGui::TreeNodeEx("##n", flags, "#%u %s", id, label.c_str());
  if (selected && app.scrollTreeToSelection) {
    ImGui::SetScrollHereY(0.5f);
    app.scrollTreeToSelection = false;
  }
  flat.push_back(id);
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    app.selectedLayerId = id;
  if (open) {
    for (uint32_t childId : children)
      DrawLayerTreeNode(frame, childId, app, flat, keep);
    ImGui::TreePop();
  }
  ImGui::PopID();
}

// Build the set of ids to keep when "only visible" is on: every visible
// layer plus all of its ancestors (so the tree shape is preserved).
std::unordered_set<uint32_t>
BuildVisibleKeepSet(const layerviewer::CapturedFrame &frame) {
  std::unordered_set<uint32_t> keep;
  keep.reserve(frame.snapshots.size());
  for (const auto &snap : frame.snapshots) {
    if (!snap.isVisible)
      continue;
    uint32_t id = static_cast<uint32_t>(snap.path.id);
    while (id != kUnassignedLayerId && !keep.count(id)) {
      keep.insert(id);
      auto it = frame.parentByLayerId.find(id);
      if (it == frame.parentByLayerId.end())
        break;
      id = it->second;
    }
  }
  return keep;
}

void DrawLayerTreePane(const layerviewer::CapturedFrame &frame, AppState &app) {
  ImGui::Checkbox("only visible", &app.treeOnlyVisible);
  ImGui::SameLine();
  ImGui::Checkbox("short names", &app.treeShortNames);
  if (frame.snapshots.empty()) {
    ImGui::TextUnformatted("(no layers in this frame)");
    return;
  }
  std::unordered_set<uint32_t> keepStorage;
  const std::unordered_set<uint32_t> *keep = nullptr;
  if (app.treeOnlyVisible) {
    keepStorage = BuildVisibleKeepSet(frame);
    keep = &keepStorage;
  }
  std::vector<uint32_t> visible;
  visible.reserve(frame.snapshots.size());
  for (uint32_t rootId : frame.rootIds)
    DrawLayerTreeNode(frame, rootId, app, visible, keep);

  // Up/Down arrow selection when the Layers window is focused. Walks the
  // same flat list that was just rendered, so collapsed branches are
  // skipped correctly.
  if (!visible.empty() &&
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
    bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
    if (up || down) {
      int idx = -1;
      for (size_t i = 0; i < visible.size(); i++) {
        if (visible[i] == app.selectedLayerId) {
          idx = static_cast<int>(i);
          break;
        }
      }
      if (idx < 0) {
        idx = down ? 0 : static_cast<int>(visible.size() - 1);
      } else {
        idx = std::clamp(idx + (down ? 1 : -1), 0,
                         static_cast<int>(visible.size() - 1));
      }
      app.selectedLayerId = visible[idx];
      app.scrollTreeToSelection = true;
    }
  }
}

// ---------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------

void DrawInspector(const layerviewer::CapturedFrame &frame,
                   const AppState &app) {
  if (app.selectedLayerId == kUnassignedLayerId) {
    ImGui::TextUnformatted("Click a layer in the tree to inspect it.");
    return;
  }
  const auto *snap = frame.snapshot(app.selectedLayerId);
  if (!snap) {
    ImGui::Text("Layer #%u not in this frame.", app.selectedLayerId);
    return;
  }

  auto parentIt = frame.parentByLayerId.find(app.selectedLayerId);
  uint32_t parentId =
      parentIt != frame.parentByLayerId.end() ? parentIt->second : UINT32_MAX;

  // Helpers: a collapsible section containing a two-column key/value table.
  auto section =
      [](const char *label, bool defaultOpen,
         std::initializer_list<std::pair<const char *, std::string>> rows) {
        ImGuiTreeNodeFlags flags =
            defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0;
        if (!ImGui::CollapsingHeader(label, flags))
          return;
        if (!ImGui::BeginTable(label, 2,
                               ImGuiTableFlags_SizingStretchProp |
                                   ImGuiTableFlags_RowBg))
          return;
        for (const auto &r : rows) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(r.first);
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(r.second.c_str());
        }
        ImGui::EndTable();
      };
  auto yn = [](bool b) -> std::string { return b ? "yes" : "no"; };
  auto rectStr = [](const android::Rect &r) -> std::string {
    return android::base::StringPrintf("(%d, %d) -> (%d, %d)  [%dx%d]", r.left,
                                       r.top, r.right, r.bottom, r.width(),
                                       r.height());
  };
  auto floatRectStr = [](const android::FloatRect &r) -> std::string {
    return android::base::StringPrintf(
        "(%.1f, %.1f) -> (%.1f, %.1f)  [%.1fx%.1f]", r.left, r.top, r.right,
        r.bottom, r.right - r.left, r.bottom - r.top);
  };
  auto xformStr = [](const android::ui::Transform &t) -> std::string {
    return android::base::StringPrintf("dsdx=%.3f dtdx=%.3f tx=%.1f\n"
                                       "dtdy=%.3f dsdy=%.3f ty=%.1f",
                                       t.dsdx(), t.dtdx(), t.tx(), t.dtdy(),
                                       t.dsdy(), t.ty());
  };
  auto reachStr = [](decltype(snap->reachablilty) r) -> std::string {
    using R = decltype(snap->reachablilty);
    switch (r) {
    case R::Reachable:
      return "reachable";
    case R::Unreachable:
      return "unreachable";
    case R::ReachableByRelativeParent:
      return "via relative parent";
    }
    return "?";
  };

  ImGui::TextWrapped("%s", snap->name.c_str());
  ImGui::TextDisabled("#%u  globalZ=%zu  sequence=%d  unique=%u",
                      app.selectedLayerId, snap->globalZ, snap->sequence,
                      snap->uniqueSequence);

  section("Identity", true,
          {
              {"id", std::to_string(app.selectedLayerId)},
              {"parent", parentId == UINT32_MAX ? std::string("-")
                                                : std::to_string(parentId)},
              {"sequence", std::to_string(snap->sequence)},
              {"uniqueSequence", std::to_string(snap->uniqueSequence)},
              {"globalZ", std::to_string(snap->globalZ)},
              {"layerStack", std::to_string(snap->outputFilter.layerStack.id)},
              {"uid", std::to_string(snap->uid.val())},
              {"pid", std::to_string(snap->pid.val())},
              {"debugName", snap->debugName},
          });

  section(
      "Visibility", true,
      {
          {"isVisible", yn(snap->isVisible)},
          {"reachablilty", reachStr(snap->reachablilty)},
          {"hiddenByPolicyFromParent", yn(snap->isHiddenByPolicyFromParent)},
          {"hiddenByPolicyFromRelativeParent",
           yn(snap->isHiddenByPolicyFromRelativeParent)},
          {"contentDirty", yn(snap->contentDirty)},
          {"hasReadyFrame", yn(snap->hasReadyFrame)},
          {"isOpaque", yn(snap->isOpaque)},
          {"contentOpaque", yn(snap->contentOpaque)},
          {"layerOpaqueFlagSet", yn(snap->layerOpaqueFlagSet)},
          {"isSecure", yn(snap->isSecure)},
          {"forceClientComposition", yn(snap->forceClientComposition)},
          {"isSmallDirty", yn(snap->isSmallDirty)},
          {"reason", snap->getIsVisibleReason()},
      });

  section("Geometry", true,
          {
              {"transformedBounds", floatRectStr(snap->transformedBounds)},
              {"geomLayerBounds", floatRectStr(snap->geomLayerBounds)},
              {"geomLayerCrop", floatRectStr(snap->geomLayerCrop)},
              {"geomCrop", floatRectStr(snap->geomCrop)},
              {"geomContentCrop", rectStr(snap->geomContentCrop)},
              {"bufferSize", rectStr(snap->bufferSize)},
              {"croppedBufferSize", floatRectStr(snap->croppedBufferSize)},
              {"geomBufferSize", rectStr(snap->geomBufferSize)},
              {"cursorFrame", rectStr(snap->cursorFrame)},
              {"geomLayerTransform", xformStr(snap->geomLayerTransform)},
              {"localTransform", xformStr(snap->localTransform)},
              {"parentTransform", xformStr(snap->parentTransform)},
              {"geomBufferTransform",
               android::base::StringPrintf("0x%x", snap->geomBufferTransform)},
              {"bufferTransform",
               android::base::StringPrintf("0x%x", snap->geomBufferTransform)},
              {"invalidTransform", yn(snap->invalidTransform)},
              {"geomUsesSourceCrop", yn(snap->geomUsesSourceCrop)},
              {"geomBufferUsesDisplayInverseTransform",
               yn(snap->geomBufferUsesDisplayInverseTransform)},
          });

  section(
      "Color / Blending", true,
      {
          {"color",
           android::base::StringPrintf("r=%.2f g=%.2f b=%.2f a=%.2f",
                                       static_cast<float>(snap->color.r),
                                       static_cast<float>(snap->color.g),
                                       static_cast<float>(snap->color.b),
                                       static_cast<float>(snap->color.a))},
          {"alpha", android::base::StringPrintf(
                        "%.2f", static_cast<float>(snap->alpha))},
          {"dataspace", android::base::StringPrintf(
                            "0x%x", static_cast<uint32_t>(snap->dataspace))},
          {"dimmingEnabled", yn(snap->dimmingEnabled)},
          {"colorTransformIsIdentity", yn(snap->colorTransformIsIdentity)},
          {"premultipliedAlpha", yn(snap->premultipliedAlpha)},
          {"cornerRadius", android::base::StringPrintf(
                               "x=%.2f y=%.2f", snap->roundedCorner.radius.x,
                               snap->roundedCorner.radius.y)},
          {"cornerCrop", floatRectStr(snap->roundedCorner.cropRect)},
          {"backgroundBlurRadius", std::to_string(snap->backgroundBlurRadius)},
          {"blurRegions", std::to_string(snap->blurRegions.size())},
      });

  if (snap->externalTexture) {
    section("Buffer", true,
            {
                {"id", std::to_string(snap->externalTexture->getId())},
                {"size", android::base::StringPrintf(
                             "%ux%u", snap->externalTexture->getWidth(),
                             snap->externalTexture->getHeight())},
                {"pixelFormat",
                 std::to_string(snap->externalTexture->getPixelFormat())},
                {"usage",
                 android::base::StringPrintf(
                     "0x%llx",
                     (unsigned long long)snap->externalTexture->getUsage())},
                {"frameNumber", std::to_string(snap->frameNumber)},
                {"hasProtectedContent", yn(snap->hasProtectedContent)},
            });
  }

  section(
      "Input", false,
      {
          {"hasInputInfo", yn(snap->hasInputInfo())},
          {"canReceiveInput", yn(snap->canReceiveInput())},
          {"touchableRegion bounds",
           rectStr(snap->inputInfo.touchableRegion.getBounds())},
          {"frame", rectStr(snap->inputInfo.frame)},
          {"globalScaleFactor", android::base::StringPrintf(
                                    "%.3f", snap->inputInfo.globalScaleFactor)},
          {"surfaceInset", std::to_string(snap->inputInfo.surfaceInset)},
          {"token", snap->inputInfo.token ? "yes" : "no"},
          {"dropInputMode",
           std::to_string(static_cast<int>(snap->dropInputMode))},
          {"trustedOverlay",
           std::to_string(static_cast<int>(snap->trustedOverlay))},
      });

  // getDebugString is SF's own structured dump — keep it around for
  // anything the section rows don't cover (damage region, shadows,
  // metadata keys, etc.).
  if (ImGui::CollapsingHeader("Debug string")) {
    std::string s = snap->getDebugString();
    ImGui::TextWrapped("%s", s.c_str());
  }
}

// ---------------------------------------------------------------------------
// Transactions window (big table) + Transaction Inspector (detail table)
// ---------------------------------------------------------------------------

// Decode a LayerState.what bitmask into a " | "-separated list of the
// symbolic flag names. Values taken from
// perfetto/trace/android/surfaceflinger_transactions.proto (LayerState::
// ChangesLsb + ChangesMsb). Unknown bits are rendered as hex so weird
// traces still round-trip readably.
std::string DecodeLayerStateWhat(uint64_t what) {
  struct Flag {
    uint64_t bit;
    const char *name;
  };
  static const Flag kFlags[] = {
      {0x00000001, "ePositionChanged"},
      {0x00000002, "eLayerChanged"},
      {0x00000008, "eAlphaChanged"},
      {0x00000010, "eMatrixChanged"},
      {0x00000020, "eTransparentRegionChanged"},
      {0x00000040, "eFlagsChanged"},
      {0x00000080, "eLayerStackChanged"},
      {0x00000400, "eReleaseBufferListenerChanged"},
      {0x00000800, "eShadowRadiusChanged"},
      {0x00002000, "eBufferCropChanged"},
      {0x00004000, "eRelativeLayerChanged"},
      {0x00008000, "eReparent"},
      {0x00010000, "eColorChanged"},
      {0x00040000, "eBufferTransformChanged"},
      {0x00080000, "eTransformToDisplayInverseChanged"},
      {0x00100000, "eCropChanged"},
      {0x00200000, "eBufferChanged"},
      {0x00400000, "eAcquireFenceChanged"},
      {0x00800000, "eDataspaceChanged"},
      {0x01000000, "eHdrMetadataChanged"},
      {0x02000000, "eSurfaceDamageRegionChanged"},
      {0x04000000, "eApiChanged"},
      {0x08000000, "eSidebandStreamChanged"},
      {0x10000000, "eColorTransformChanged"},
      {0x20000000, "eHasListenerCallbacksChanged"},
      {0x0000000100000000ull, "eInputInfoChanged"},
      {0x0000000200000000ull, "eCornerRadiusChanged"},
      {0x0000000400000000ull, "eFrameChanged"},
      {0x0000000800000000ull, "eBackgroundColorChanged"},
      {0x0000001000000000ull, "eMetadataChanged"},
      {0x0000002000000000ull, "eColorSpaceAgnosticChanged"},
      {0x0000004000000000ull, "eFrameRateSelectionPriority"},
      {0x0000008000000000ull, "eFrameRateChanged"},
      {0x0000010000000000ull, "eBackgroundBlurRadiusChanged"},
      {0x0000020000000000ull, "eProducerDisconnect"},
      {0x0000040000000000ull, "eFixedTransformHintChanged"},
      {0x0000080000000000ull, "eFrameNumberChanged"},
      {0x0000100000000000ull, "eBlurRegionsChanged"},
      {0x0000200000000000ull, "eAutoRefreshChanged"},
      {0x0000400000000000ull, "eStretchChanged"},
      {0x0000800000000000ull, "eTrustedOverlayChanged"},
      {0x0001000000000000ull, "eDropInputModeChanged"},
  };
  std::string out;
  uint64_t unknown = what;
  for (const auto &f : kFlags) {
    if (what & f.bit) {
      if (!out.empty())
        out += " | ";
      out += f.name;
      unknown &= ~f.bit;
    }
  }
  if (unknown) {
    if (!out.empty())
      out += " | ";
    out += android::base::StringPrintf("0x%llx", (unsigned long long)unknown);
  }
  if (out.empty())
    out = "(none)";
  return out;
}

// Make a docked window's tab the active one in its dock node without
// moving keyboard focus. Used when a transaction is selected: we want
// the Transaction Inspector to be visible, but leave arrow-key focus
// on the Transactions table where the user is navigating.
void RaiseDockedTab(const char *windowName) {
  ImGuiWindow *w = ImGui::FindWindowByName(windowName);
  if (!w || !w->DockNode || !w->DockNode->TabBar)
    return;
  w->DockNode->TabBar->NextSelectedTabId = w->TabId;
}

void DrawTransactions(AppState &app) {
  if (!app.trace || app.trace->transactions.empty()) {
    ImGui::TextUnformatted("(no transactions in trace)");
    return;
  }
  const auto &txns = app.trace->transactions;
  const int n = static_cast<int>(txns.size());

  const bool focused =
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

  // Toolbar: sync controls.
  ImGui::Checkbox("auto sync timeline", &app.autoSyncTimeline);
  ImGui::SameLine();
  const bool canManualSync = !app.autoSyncTimeline &&
                             app.selectedTransactionIdx >= 0 &&
                             app.selectedTransactionIdx < n;
  ImGui::BeginDisabled(!canManualSync);
  if (ImGui::Button("sync timeline to selected")) {
    app.frameIndex =
        static_cast<int>(txns[app.selectedTransactionIdx].frameIndex);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("%d transactions / %zu frames", n,
                      app.trace->frames.size());

  // Four essential debugging columns — anything else is a right-click away
  // in the Transaction Inspector.
  ImGuiTableFlags tflags = ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_SizingFixedFit;
  // Table id bumped (_v3) so any stale per-column layout in imgui.ini
  // from previous column sets is discarded.
  if (ImGui::BeginTable("txns_v3", 4, tflags)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 56.f);
    ImGui::TableSetupColumn("frame", ImGuiTableColumnFlags_WidthFixed, 64.f);
    ImGui::TableSetupColumn("process", ImGuiTableColumnFlags_WidthStretch,
                            0.30f);
    ImGui::TableSetupColumn("layers", ImGuiTableColumnFlags_WidthStretch,
                            0.70f);
    ImGui::TableHeadersRow();

    // No ImGuiListClipper: rows have variable height (the layers column is
    // multi-line) and the clipper assumes uniform row heights, which made
    // scrolling jumpy. Letting ImGui render every row is fine for the
    // transaction counts we see — a few thousand at most.
    for (int i = 0; i < n; i++) {
      const auto &t = txns[i];
      ImGui::TableNextRow();
      bool sel = (app.selectedTransactionIdx == i);
      ImGui::TableSetColumnIndex(0);
      ImGui::PushID(i);
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%d", i);
      if (ImGui::Selectable(buf, sel, ImGuiSelectableFlags_SpanAllColumns)) {
        app.selectedTransactionIdx = i;
        if (app.autoSyncTimeline)
          app.frameIndex = static_cast<int>(t.frameIndex);
        // Raise the Transaction Inspector tab without stealing focus.
        RaiseDockedTab("Transaction Inspector");
      }
      if (sel && app.scrollTxnTableToSelection) {
        ImGui::SetScrollHereY(0.5f);
        app.scrollTxnTableToSelection = false;
      }
      ImGui::PopID();
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%zu", t.frameIndex);
      ImGui::TableSetColumnIndex(2);
      // "process" falls back to "pid N" when the trace has no ProcessTree.
      {
        auto it = app.trace->pidNames.find(t.pid);
        if (it != app.trace->pidNames.end())
          ImGui::TextUnformatted(it->second.c_str());
        else
          ImGui::Text("pid %d", t.pid);
      }
      ImGui::TableSetColumnIndex(3);
      // "layers" column: comma-separated short names from the frame's
      // snapshot map (falls back to `#<id>` for layers not present in
      // the frame, e.g. destroyed-same-entry). Shows the count as a
      // prefix so busy txns still stand out at a glance.
      {
        const auto *frame = t.frameIndex < app.trace->frames.size()
                                ? &app.trace->frames[t.frameIndex]
                                : nullptr;
        std::string row;
        row.reserve(64);
        for (size_t k = 0; k < t.affectedLayerIds.size(); k++) {
          if (k > 0)
            row += '\n';
          uint32_t lid = t.affectedLayerIds[k];
          const auto *snap = frame ? frame->snapshot(lid) : nullptr;
          row += snap ? shortLayerName(snap->name)
                      : android::base::StringPrintf("#%u", lid);
        }
        if (row.empty())
          row = "(none)";
        ImGui::TextUnformatted(row.c_str());
      }
    }
    ImGui::EndTable();
  }

  // Arrow-key navigation when the Transactions window is focused.
  if (focused && n > 0) {
    bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
    bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
    if (up || down) {
      int idx = std::clamp(app.selectedTransactionIdx, 0, n - 1);
      if (app.selectedTransactionIdx < 0)
        idx = down ? 0 : n - 1;
      else
        idx = std::clamp(idx + (down ? 1 : -1), 0, n - 1);
      app.selectedTransactionIdx = idx;
      app.scrollTxnTableToSelection = true;
      if (app.autoSyncTimeline)
        app.frameIndex = static_cast<int>(txns[idx].frameIndex);
      RaiseDockedTab("Transaction Inspector");
    }
  }
}

void DrawTransactionInspector(AppState &app) {
  if (!app.trace || app.trace->transactions.empty()) {
    ImGui::TextDisabled("(no transactions in trace)");
    return;
  }
  const auto &txns = app.trace->transactions;
  const int n = static_cast<int>(txns.size());
  if (app.selectedTransactionIdx < 0 || app.selectedTransactionIdx >= n) {
    ImGui::TextDisabled("Select a transaction in the Transactions tab.");
    return;
  }
  const auto &t = txns[app.selectedTransactionIdx];
  const auto *frame = t.frameIndex < app.trace->frames.size()
                          ? &app.trace->frames[t.frameIndex]
                          : nullptr;

  auto section =
      [](const char *label, bool defaultOpen,
         std::initializer_list<std::pair<const char *, std::string>> rows) {
        ImGuiTreeNodeFlags flags =
            defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0;
        if (!ImGui::CollapsingHeader(label, flags))
          return;
        if (!ImGui::BeginTable(label, 2,
                               ImGuiTableFlags_SizingStretchProp |
                                   ImGuiTableFlags_RowBg))
          return;
        for (const auto &r : rows) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(r.first);
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(r.second.c_str());
        }
        ImGui::EndTable();
      };

  ImGui::TextWrapped("Transaction #%d  (id=%llu)", app.selectedTransactionIdx,
                     (unsigned long long)t.transactionId);
  ImGui::TextDisabled("frame %zu%s%s", t.frameIndex, frame ? "  vsync=" : "",
                      frame ? std::to_string(frame->vsyncId).c_str() : "");

  // Layer changes first — most actionable info on a transaction (which
  // layers and which fields it touched). One row per LayerState in the
  // TransactionState (not deduped), so repeated touches on the same
  // layer show their own `what` summary. Table fits its content — no
  // fixed height, no inner scrollbar.
  if (ImGui::CollapsingHeader("Layer changes",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    if (t.layerStateChanges.empty()) {
      ImGui::TextDisabled("  (none)");
    } else if (ImGui::BeginTable("layerchanges", 3,
                                 ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, 48.f);
      ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch,
                              0.35f);
      ImGui::TableSetupColumn("changes", ImGuiTableColumnFlags_WidthStretch,
                              0.65f);
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < t.layerStateChanges.size(); i++) {
        const auto &lc = t.layerStateChanges[i];
        const auto *snap = frame ? frame->snapshot(lc.layerId) : nullptr;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(std::to_string(lc.layerId).c_str(),
                              app.selectedLayerId == lc.layerId,
                              ImGuiSelectableFlags_SpanAllColumns)) {
          app.selectedLayerId = lc.layerId;
          app.scrollTreeToSelection = true;
        }
        ImGui::PopID();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(snap ? snap->name.c_str() : "(not in frame)");
        ImGui::TableSetColumnIndex(2);
        ImGui::TextWrapped("%s", DecodeLayerStateWhat(lc.what).c_str());
      }
      ImGui::EndTable();
    }
  }

  section("Identity", true,
          {
              {"index", std::to_string(app.selectedTransactionIdx)},
              {"transactionId",
               android::base::StringPrintf(
                   "%llu (0x%llx)", (unsigned long long)t.transactionId,
                   (unsigned long long)t.transactionId)},
              {"frameIndex", std::to_string(t.frameIndex)},
              {"frame vsyncId",
               frame ? std::to_string(frame->vsyncId) : std::string("-")},
              {"frame elapsed-realtime",
               frame ? android::base::StringPrintf("%.9f s", frame->tsNs / 1e9)
                     : std::string("-")},
          });

  std::string processName;
  {
    auto it = app.trace->pidNames.find(t.pid);
    if (it != app.trace->pidNames.end())
      processName = it->second;
    else
      processName = "(unknown — no ProcessTree in trace)";
  }
  section("Source", true,
          {
              {"pid", std::to_string(t.pid)},
              {"process", processName},
              {"uid", std::to_string(t.uid)},
              {"inputEventId", t.inputEventId ? std::to_string(t.inputEventId)
                                              : std::string("0 (none)")},
          });

  section("Timing", true,
          {
              {"postTime (ns)", std::to_string(t.postTimeNs)},
              {"postTime (s)",
               android::base::StringPrintf("%.9f", t.postTimeNs / 1e9)},
              {"vsyncId (in txn)", std::to_string(t.vsyncId)},
              {"vsyncId (frame)",
               frame ? std::to_string(frame->vsyncId) : std::string("-")},
              {"matches frame vsync",
               frame ? (frame->vsyncId == t.vsyncId ? "yes" : "no")
                     : std::string("-")},
          });

  section("Contents", true,
          {
              {"layer changes", std::to_string(t.layerChanges)},
              {"display changes", std::to_string(t.displayChanges)},
              {"affected layers (unique)",
               std::to_string(t.affectedLayerIds.size())},
              {"merged transactions",
               std::to_string(t.mergedTransactionIds.size())},
          });

  if (!t.mergedTransactionIds.empty() &&
      ImGui::CollapsingHeader("Merged transaction ids")) {
    if (ImGui::BeginTable("merged", 1,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 120.f))) {
      ImGui::TableSetupColumn("id");
      ImGui::TableHeadersRow();
      for (uint64_t id : t.mergedTransactionIds) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%llu (0x%llx)", (unsigned long long)id,
                    (unsigned long long)id);
      }
      ImGui::EndTable();
    }
  }
}

// ---------------------------------------------------------------------------
// Timeline (Tracy-style scrubber)
// ---------------------------------------------------------------------------

void DrawTimeline(AppState &app) {
  if (!app.trace || app.trace->frames.empty()) {
    ImGui::TextUnformatted("(no trace loaded)");
    return;
  }
  const auto &frames = app.trace->frames;
  const int n = static_cast<int>(frames.size());

  // Fixed-width controls first so their X positions don't shift with the
  // varying width of the frame/vsync/ts label. Variable-width text lives
  // at the end of the row where growth only moves nothing after it.
  if (ImGui::Button("prev frame"))
    app.frameIndex = std::max(0, app.frameIndex - 1);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Previous frame (left arrow)");
  ImGui::SameLine();
  if (ImGui::Button("next frame"))
    app.frameIndex = std::min(n - 1, app.frameIndex + 1);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Next frame (right arrow)");
  ImGui::SameLine();
  ImGui::Text("Frame %d / %d   vsync=%lld   ts=%.3fs", app.frameIndex + 1, n,
              (long long)frames[app.frameIndex].vsyncId,
              frames[app.frameIndex].tsNs / 1e9);

  // The strip: a tall InvisibleButton we draw over.
  ImVec2 avail = ImGui::GetContentRegionAvail();
  const float stripH = std::max(60.f, avail.y);
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 size(avail.x, stripH);
  if (size.x < 1 || size.y < 1)
    return;

  ImGui::InvisibleButton("##timeline", size);
  bool hovered = ImGui::IsItemHovered();
  bool active = ImGui::IsItemActive();

  ImDrawList *dl = ImGui::GetWindowDrawList();
  // Background.
  dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                    IM_COL32(20, 20, 28, 255));

  const float perFrame = size.x / std::max(1, n);
  const int64_t t0 = frames.front().tsNs;
  const int64_t t1 = frames.back().tsNs;
  const float totalMs = (t1 - t0) / 1e6f;

  // Gridlines every ~100ms.
  {
    ImU32 gridCol = IM_COL32(60, 60, 80, 180);
    int step = (totalMs > 2000) ? 500 : (totalMs > 500) ? 100 : 10;
    for (float ms = step; ms < totalMs; ms += step) {
      float frac = ms / std::max(1.f, totalMs);
      float x = origin.x + frac * size.x;
      dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y), gridCol);
      std::string label = android::base::StringPrintf("%.0fms", ms);
      dl->AddText(ImVec2(x + 2, origin.y + 2), IM_COL32(160, 160, 180, 255),
                  label.c_str());
    }
  }

  // Bars: one thin column per frame; shade = transaction count, tick marks
  // on top for add/destroy/display-change events.
  for (int i = 0; i < n; i++) {
    const auto &f = frames[i];
    float x0 = origin.x + i * perFrame;
    float x1 = x0 + std::max(1.f, perFrame);

    // Base tint tracks transaction count (busier frames are brighter).
    int busy = std::min(255, 40 + f.txnCount * 20);
    ImU32 col = IM_COL32(40, busy, 80, 255);
    dl->AddRectFilled(ImVec2(x0, origin.y + size.y * 0.4f),
                      ImVec2(x1, origin.y + size.y), col);

    // Top band: events.
    float evY = origin.y + 6;
    if (f.addedCount > 0) {
      dl->AddRectFilled(ImVec2(x0, evY), ImVec2(x1, evY + 8),
                        IM_COL32(80, 200, 120, 255));
    }
    if (f.destroyedHandleCount > 0) {
      dl->AddRectFilled(ImVec2(x0, evY + 10), ImVec2(x1, evY + 18),
                        IM_COL32(220, 100, 80, 255));
    }
    if (f.displaysChanged) {
      dl->AddRectFilled(ImVec2(x0, evY + 20), ImVec2(x1, evY + 28),
                        IM_COL32(240, 200, 60, 255));
    }
  }

  // Current-frame marker.
  {
    float x = origin.x + (app.frameIndex + 0.5f) * perFrame;
    dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y),
                IM_COL32(255, 200, 60, 255), 2.f);
  }

  // Hover + click.
  if (hovered) {
    ImVec2 m = ImGui::GetMousePos();
    int hoverIdx = std::clamp(
        static_cast<int>((m.x - origin.x) / std::max(1.f, perFrame)), 0, n - 1);
    const auto &hf = frames[hoverIdx];
    ImGui::BeginTooltip();
    ImGui::Text("entry %d", hoverIdx);
    ImGui::Text("vsync %lld", (long long)hf.vsyncId);
    ImGui::Text("ts    %.3fs", hf.tsNs / 1e9);
    ImGui::Separator();
    ImGui::Text("txns      %d", hf.txnCount);
    ImGui::Text("+layers   %d", hf.addedCount);
    ImGui::Text("-handles  %d", hf.destroyedHandleCount);
    ImGui::Text("displays  %s", hf.displaysChanged ? "changed" : "-");
    ImGui::Text("layers    %zu reachable", hf.snapshots.size());
    ImGui::EndTooltip();

    if (active)
      app.frameIndex = hoverIdx;
  }

  // Keyboard shortcuts when timeline window is focused.
  if (ImGui::IsWindowFocused()) {
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
      app.frameIndex = std::max(0, app.frameIndex - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
      app.frameIndex = std::min(n - 1, app.frameIndex + 1);
  }
}

// ---------------------------------------------------------------------------
// Preview: offscreen Skia surface → GL texture → ImGui::Image
// ---------------------------------------------------------------------------

// Owns a GL texture + FBO and wraps it as an SkSurface. Recreated on resize;
// the texture id is what we hand to ImGui::Image. This is also where the real
// RenderEngine output will land in stage 2 — the rest of the preview pipeline
// doesn't change when CE/RE start writing into this surface.
struct PreviewTarget {
  GLuint texId = 0;
  GLuint fbo = 0;
  int w = 0, h = 0;
  sk_sp<SkSurface> surface;

  void destroy() {
    surface.reset();
    if (fbo) {
      glDeleteFramebuffers(1, &fbo);
      fbo = 0;
    }
    if (texId) {
      glDeleteTextures(1, &texId);
      texId = 0;
    }
    w = h = 0;
  }

  bool ensure(GrDirectContext *ctx, int newW, int newH) {
    if (newW <= 0 || newH <= 0)
      return false;
    if (newW == w && newH == h && surface)
      return true;
    destroy();
    w = newW;
    h = newH;

    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           texId, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    GrGLFramebufferInfo fi;
    fi.fFBOID = fbo;
    fi.fFormat = GL_RGBA8;
    GrBackendRenderTarget target =
        GrBackendRenderTargets::MakeGL(w, h, 0, 8, fi);
    surface = SkSurfaces::WrapBackendRenderTarget(
        ctx, target, kTopLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
        SkColorSpace::MakeSRGB(), nullptr);
    return surface != nullptr;
  }
};

// Checkerboard background inside a rect — signals "this is device space,
// nothing painted here yet" so empty/transparent layers stand out.
void DrawCheckerboard(SkCanvas *canvas, const SkRect &rect, float cellPx) {
  SkPaint a, b;
  a.setColor(SkColorSetARGB(255, 44, 44, 52));
  b.setColor(SkColorSetARGB(255, 56, 56, 64));
  canvas->save();
  canvas->clipRect(rect);
  int cols = static_cast<int>(std::ceil(rect.width() / cellPx)) + 1;
  int rows = static_cast<int>(std::ceil(rect.height() / cellPx)) + 1;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      SkRect cell = SkRect::MakeXYWH(rect.left() + c * cellPx,
                                     rect.top() + r * cellPx, cellPx, cellPx);
      canvas->drawRect(cell, ((r + c) & 1) ? a : b);
    }
  }
  canvas->restore();
}

// Winscope-style "exploded" 3D wireframe. Each layer's rect is placed at a
// fixed depth proportional to its paint-order rank; click-and-drag rotates
// yaw/pitch. Drawn entirely with ImGui's DrawList — no Skia, no FBO.
void DrawWireframe(const layerviewer::CapturedFrame &frame, AppState &app) {
  const float kDepthSpacing = app.wireframe3D.depthSpacing;

  ImGui::TextDisabled("click and drag to rotate");
  ImGui::SameLine();
  ImGui::Checkbox("show all layers", &app.showInvisible);
  ImGui::SameLine();
  ImGui::PushItemWidth(160);
  ImGui::SliderFloat("spacing", &app.wireframe3D.depthSpacing, 0.f, 800.f,
                     "%.0fpx");
  ImGui::SameLine();
  ImGui::SliderFloat("opacity", &app.wireframe3D.opacity, 0.f, 1.f, "%.2f");
  ImGui::PopItemWidth();

  ImVec2 avail = ImGui::GetContentRegionAvail();
  if (avail.x < 4 || avail.y < 4)
    return;
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##wire3d", avail, ImGuiButtonFlags_MouseButtonLeft);
  bool hovered = ImGui::IsItemHovered();
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    ImVec2 delta = ImGui::GetIO().MouseDelta;
    app.wireframe3D.yawDeg += delta.x * 0.4f;
    app.wireframe3D.pitchDeg += delta.y * 0.4f;
    // Clamp yaw strictly inside (−90°, +90°) so we never rotate past the
    // edge of the stack — looking from behind makes no sense for an
    // exploded layer view and just flips the sort direction.
    app.wireframe3D.yawDeg = std::clamp(app.wireframe3D.yawDeg, -85.f, 85.f);
    app.wireframe3D.pitchDeg =
        std::clamp(app.wireframe3D.pitchDeg, -85.f, 85.f);
  }

  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
                    IM_COL32(22, 22, 28, 255));

  float cw = frame.displayWidth;
  float ch = frame.displayHeight;
  if (cw <= 0 || ch <= 0) {
    dl->AddText(ImVec2(origin.x + 12, origin.y + 12),
                IM_COL32(180, 180, 200, 220), "(no display info)");
    return;
  }

  // Filter drawable snapshots — same rule as the Preview. `frame.snapshots`
  // is already sorted by globalZ ascending (paint order), so no extra sort
  // needed after filtering.
  using Snap = android::surfaceflinger::frontend::LayerSnapshot;
  std::vector<const Snap *> layers;
  layers.reserve(frame.snapshots.size());
  for (const auto &snap : frame.snapshots) {
    if (!app.showInvisible && !snap.isVisible)
      continue;
    // transformedBounds = geomLayerTransform * geomLayerBounds — the actual
    // on-screen rect. geomLayerBounds alone is the buffer-sized rect in the
    // layer's local space, so using it would ignore per-frame translate/
    // scale/rotate from the transaction (e.g. a 1080×63 taskbar buffer that
    // lives at y=2337 would be drawn at y=0).
    const auto &b = snap.transformedBounds;
    if (b.right <= b.left || b.bottom <= b.top)
      continue;
    if (b.left < 0 || b.top < 0 || b.right > cw || b.bottom > ch)
      continue;
    layers.push_back(&snap);
  }

  // Rotate around the *center* of the layer stack so the view pivots in
  // place when you drag. Fixed scale: fit the unrotated device rect into
  // the window so rotation never zooms — rotated corners may extend past
  // the viewport, that's fine.
  const float czd = 0.5f *
                    std::max<int>(0, static_cast<int>(layers.size() - 1)) *
                    kDepthSpacing;
  const Rotation3D R =
      MakeRotation(app.wireframe3D.yawDeg, app.wireframe3D.pitchDeg, cw * 0.5f,
                   ch * 0.5f, czd);
  const float pad = 80.f;
  float scale = std::min((avail.x - 2 * pad) / cw, (avail.y - 2 * pad) / ch);
  scale = std::max(0.01f, scale);
  const float screenCx = origin.x + avail.x * 0.5f;
  const float screenCy = origin.y + avail.y * 0.5f;

  // Device-rect (at z=0) as a reference plane.
  {
    ImVec2 p00 = Project3D(R, 0.f, 0.f, 0.f, screenCx, screenCy, scale);
    ImVec2 p10 = Project3D(R, cw, 0.f, 0.f, screenCx, screenCy, scale);
    ImVec2 p11 = Project3D(R, cw, ch, 0.f, screenCx, screenCy, scale);
    ImVec2 p01 = Project3D(R, 0.f, ch, 0.f, screenCx, screenCy, scale);
    dl->AddQuadFilled(p00, p10, p11, p01, IM_COL32(40, 40, 48, 140));
    dl->AddQuad(p00, p10, p11, p01, IM_COL32(170, 170, 210, 220), 1.5f);
  }

  // Pre-project every layer and also compute its center's post-rotation
  // depth so both painter's-algorithm ordering and hit-testing match the
  // actual viewer-space order.
  //
  // Convention: world z grows with paint-order rank (z = 0 is bottom of
  // stack, z = N*spacing is top), and our pitch/yaw rotation leaves
  // `viewZ` meaningful as "distance along screen normal toward viewer" —
  // so *larger viewZ = closer to viewer*, regardless of pitch sign.
  struct Proj {
    const Snap *l;
    int rank; // index in z-ascending `layers`: 0 = bottom of stack.
    ImVec2 c[4];
    float viewZ; // larger = closer to viewer (see View3D comment).
  };
  std::vector<Proj> projs;
  projs.reserve(layers.size());
  for (size_t k = 0; k < layers.size(); k++) {
    const auto &b = layers[k]->transformedBounds;
    // Foreground (high paint rank) at z=0; background at z=max. Camera
    // looks from +z toward the display plane, so foreground sits closer.
    // Paint-order rank 0 is the background → gets the largest z.
    float z = static_cast<float>(layers.size() - 1 - k) * kDepthSpacing;
    Proj p;
    p.l = layers[k];
    p.rank = static_cast<int>(k);
    p.c[0] = Project3D(R, b.left, b.top, z, screenCx, screenCy, scale);
    p.c[1] = Project3D(R, b.right, b.top, z, screenCx, screenCy, scale);
    p.c[2] = Project3D(R, b.right, b.bottom, z, screenCx, screenCy, scale);
    p.c[3] = Project3D(R, b.left, b.bottom, z, screenCx, screenCy, scale);
    float rx, ry;
    Rotate(R, 0.5f * (b.left + b.right), 0.5f * (b.top + b.bottom), z, rx, ry,
           p.viewZ);
    (void)rx;
    (void)ry;
    projs.push_back(p);
  }
  // Sort by RANK, not by post-rotation viewZ of the center. Center-based
  // sort wobbled when a layer's (x, y) contribution to viewZ swamped the
  // rank delta for adjacent layers. Yaw is clamped to ±85° so stack
  // direction is stable. Rank 0 = background (z = max, farthest from
  // camera), rank N-1 = foreground (z = 0, closest). Painter's: draw far
  // first, near last — ascending rank. Hit-testing iterates projs in
  // reverse (rbegin→rend) so the frontmost quad wins.
  std::sort(projs.begin(), projs.end(),
            [](const Proj &a, const Proj &b) { return a.rank < b.rank; });

  // Hit-test first, in near→far order (projs is far→near, so iterate back).
  uint32_t hoverId = kUnassignedLayerId;
  ImVec2 mouse = ImGui::GetMousePos();
  if (hovered) {
    for (auto it = projs.rbegin(); it != projs.rend(); ++it) {
      if (PointInQuad(mouse, it->c[0], it->c[1], it->c[2], it->c[3])) {
        hoverId = static_cast<uint32_t>(it->l->path.id);
        break;
      }
    }
  }

  // Two-tone palette — the wireframe's job is to show bounds and stacking,
  // not identity. Mint vs warm coral is a complementary high-contrast pair
  // that both stay legible on the dark background. Per-layer identity
  // (checkerboard + buffer-id watermark) lives in the Skia preview
  // instead, where it'll be replaced by real RenderEngine output.
  const uint8_t fillA = static_cast<uint8_t>(
      std::clamp(app.wireframe3D.opacity, 0.f, 1.f) * 255.f + 0.5f);
  const ImU32 fillVisible = IM_COL32(0x4e, 0xc9, 0xb0, fillA);
  const ImU32 strokeVisible = IM_COL32(0x2e, 0x8b, 0x77, 255); // deeper mint
  const ImU32 fillHidden = IM_COL32(0xd6, 0x7a, 0x4a, fillA);
  const ImU32 strokeHidden = IM_COL32(0x9a, 0x4e, 0x2a, 220); // deeper coral

  // Draw far → near.
  for (const auto &p : projs) {
    uint32_t lid = static_cast<uint32_t>(p.l->path.id);
    bool selected = app.selectedLayerId == lid;
    ImU32 fill = p.l->isVisible ? fillVisible : fillHidden;
    dl->AddQuadFilled(p.c[0], p.c[1], p.c[2], p.c[3], fill);
    ImU32 stroke = selected ? IM_COL32(255, 215, 50, 255)
                            : (p.l->isVisible ? strokeVisible : strokeHidden);
    dl->AddQuad(p.c[0], p.c[1], p.c[2], p.c[3], stroke, selected ? 2.5f : 1.f);
  }

  if (hovered && hoverId != kUnassignedLayerId) {
    if (const Snap *snap = frame.snapshot(hoverId)) {
      ImGui::BeginTooltip();
      ImGui::Text("#%u %s", hoverId, snap->name.c_str());
      const auto &b = snap->transformedBounds;
      ImGui::Text("bounds: (%.1f, %.1f) → (%.1f, %.1f)", b.left, b.top, b.right,
                  b.bottom);
      ImGui::Text("globalZ=%zu  alpha=%.2f", snap->globalZ,
                  static_cast<float>(snap->alpha));
      ImGui::EndTooltip();
    }
    if (ImGui::IsItemClicked()) {
      app.selectedLayerId = hoverId;
      app.scrollTreeToSelection = true;
    }
  }
}

// Placeholder preview. The real composition path — LayerSnapshots → resident
// `LayerFE`s → `compositionengine::Output::generateClientCompositionRequests`
// → `SkiaDesktopGLRenderEngine::drawLayers` into a GraphicBuffer-backed
// ExternalTexture — replaces this function in the next commit. For now we
// just draw the display viewport so the window isn't blank while the rest
// of the plumbing is coming online.
void DrawPreviewCanvas(SkCanvas *canvas, int fbW, int fbH,
                       const layerviewer::CapturedFrame &frame,
                       const AppState &app) {
  (void)app;
  SkPaint bg;
  bg.setColor(SkColorSetARGB(255, 24, 24, 28));
  canvas->drawRect(SkRect::MakeIWH(fbW, fbH), bg);

  float cw = frame.displayWidth;
  float ch = frame.displayHeight;
  if (cw <= 0 || ch <= 0)
    return;

  FitTransform t = FitRectToCanvas(cw, ch, (float)fbW, (float)fbH, 32.f);
  SkRect displayRect = SkRect::MakeXYWH(t.tx, t.ty, cw * t.scale, ch * t.scale);
  DrawCheckerboard(canvas, displayRect, 16.f);

  SkPaint border;
  border.setStyle(SkPaint::kStroke_Style);
  border.setAntiAlias(true);
  border.setStrokeWidth(1.5f);
  border.setColor(SkColorSetARGB(200, 160, 160, 200));
  canvas->drawRect(displayRect, border);

  SkFont font(nullptr, 18.f);
  font.setEdging(SkFont::Edging::kAntiAlias);
  SkPaint text;
  text.setColor4f({0.80f, 0.82f, 0.90f, 1.f}, nullptr);
  canvas->drawString("composition pipeline landing in next commit",
                     displayRect.left() + 12.f, displayRect.top() + 28.f, font,
                     text);
  canvas->drawString(
      frame.snapshots.size() == 0
          ? "(no reachable layers in this frame)"
          : (std::to_string(frame.snapshots.size()) + " reachable snapshots")
                .c_str(),
      displayRect.left() + 12.f, displayRect.top() + 52.f, font, text);
}

// ---------------------------------------------------------------------------
// File open
// ---------------------------------------------------------------------------

struct OpenDialogContext {
  AppState *app = nullptr;
};

void OnFileDialogResult(void *userdata, const char *const *files,
                        int /*filter*/) {
  auto *ctx = static_cast<OpenDialogContext *>(userdata);
  if (files && files[0]) {
    ctx->app->trace = layerviewer::LoadAndReplay(files[0]);
    ctx->app->frameIndex = 0;
    ctx->app->selectedLayerId = kUnassignedLayerId;
  }
  delete ctx;
}

void OpenFileDialog(SDL_Window *window, AppState &app) {
  auto *ctx = new OpenDialogContext{&app};
  static const SDL_DialogFileFilter filters[] = {
      {"Perfetto trace (*.pftrace)", "pftrace"},
      {"All files", "*"},
  };
  SDL_ShowOpenFileDialog(OnFileDialogResult, ctx, window, filters,
                         SDL_arraysize(filters), nullptr, false);
}

// ---------------------------------------------------------------------------
// Dockspace layout
// ---------------------------------------------------------------------------

void BuildDefaultLayout(ImGuiID dockspaceId) {
  ImGui::DockBuilderRemoveNode(dockspaceId);
  ImGui::DockBuilderAddNode(dockspaceId,
                            ImGuiDockNodeFlags_DockSpace |
                                ImGuiDockNodeFlags_PassthruCentralNode);
  ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

  ImGuiID main = dockspaceId;
  ImGuiID bottom =
      ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.22f, nullptr, &main);
  ImGuiID left =
      ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.24f, nullptr, &main);
  ImGuiID right =
      ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.28f, nullptr, &main);

  ImGui::DockBuilderDockWindow("Timeline", bottom);
  ImGui::DockBuilderDockWindow("Layers", left);
  ImGui::DockBuilderDockWindow("Snapshot Inspector", right);
  ImGui::DockBuilderDockWindow("Transaction Inspector",
                               right); // tabbed with Snapshot Inspector
  ImGui::DockBuilderDockWindow("Trace Info", right);
  ImGui::DockBuilderDockWindow("Preview", main);
  ImGui::DockBuilderDockWindow("Wireframe", main); // tabbed with Preview
  ImGui::DockBuilderDockWindow("Transactions",
                               main); // tabbed with Preview/Wireframe
  ImGui::DockBuilderFinish(dockspaceId);
}

} // namespace

int main(int argc, char **argv) {
  AppState app;
  std::string initialTrace;
  if (argc >= 2)
    initialTrace = argv[1];

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "layerviewer",
                             SDL_GetError(), nullptr);
    return 1;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                      SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);

  SDL_Window *window = SDL_CreateWindow(
      "layerviewer", 1600, 1000,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "layerviewer",
                             SDL_GetError(), nullptr);
    SDL_Quit();
    return 1;
  }

  SDL_GLContext glCtx = SDL_GL_CreateContext(window);
  if (!glCtx) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "layerviewer",
                             SDL_GetError(), window);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_GL_MakeCurrent(window, glCtx);
  SDL_GL_SetSwapInterval(1);

  sk_sp<const GrGLInterface> glInterface = GrGLMakeNativeInterface();

  // Stand up RE + CE + Display singletons. From this point on every layer
  // buffer's GL texture (both content + compositor output) is allocated on
  // this GL context, so the compositor must be built before any trace load.
  LayerViewerCompositor compositor;
  compositor.init(glInterface);
  if (!compositor.re) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "layerviewer",
                             "SkiaDesktopGLRenderEngine::create failed",
                             window);
    return 1;
  }

  // Install a content populator so every GraphicBuffer's GL texture has a
  // recognisable pattern (checkerboard + buffer id watermark) — otherwise
  // the composed output is just a field of zeros. Must be installed before
  // any GraphicBuffer is constructed, i.e. before LoadAndReplay.
  android::GraphicBuffer::setContentPopulator([](uint32_t w, uint32_t h,
                                                 uint64_t bufferId,
                                                 unsigned int glTexId) {
    if (w == 0 || h == 0 || glTexId == 0)
      return;
    // Rasterize at the buffer's full w×h — RE/Skia wraps the GL texture
    // with the GraphicBuffer's reported dimensions, so shrinking the
    // texture storage here would desync them and stretch/skew the UVs.

    SkImageInfo info =
        SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface)
      return;
    SkCanvas *c = surface->getCanvas();

    SkColor4f light, dark;
    HashColorsForBuffer(bufferId, light, dark);
    SkPaint p;
    p.setStyle(SkPaint::kFill_Style);
    p.setColor4f(light, nullptr);
    c->drawRect(SkRect::MakeWH(w, h), p);

    p.setColor4f(dark, nullptr);
    const float cell = std::max(16.f, std::min<float>(w, h) / 8.f);
    for (float y = 0; y < h; y += cell) {
      for (float x = 0; x < w; x += cell) {
        if ((static_cast<int>(x / cell) + static_cast<int>(y / cell)) & 1)
          continue;
        c->drawRect(SkRect::MakeXYWH(x, y, cell, cell), p);
      }
    }

    std::string label = std::to_string(bufferId);
    float sz = FitFontSize(label.c_str(), w * 0.85f, h * 0.6f, 12.f, 480.f);
    SkFont font(nullptr, sz);
    font.setEdging(SkFont::Edging::kAntiAlias);
    SkPaint textPaint;
    textPaint.setAntiAlias(true);
    textPaint.setColor4f({0.08f, 0.08f, 0.12f, 0.95f}, nullptr);
    SkRect tb;
    font.measureText(label.c_str(), label.size(), SkTextEncoding::kUTF8, &tb);
    c->drawString(label.c_str(), w * 0.5f - tb.centerX(),
                  h * 0.5f - tb.centerY(), font, textPaint);

    SkPixmap pm;
    if (!surface->peekPixels(&pm))
      return;

    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, glTexId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pm.addr());
    glBindTexture(GL_TEXTURE_2D, prev);
  });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // Keyboard nav disabled: we drive our own arrow-key handlers for the
  // layer tree and the timeline. ImGui's built-in nav was moving a hidden
  // "focused" cursor on up/down without changing our selection, which just
  // looked like nothing was happening.
  ImGui::StyleColorsDark();

  // ImGui 1.91+ ships an embedded scalable font (ProggyForever) in addition
  // to the ProggyClean bitmap one. AddFontDefaultVector renders crisply at
  // any size / DPI with no file loading or binary embedding on our side.
  io.Fonts->AddFontDefaultVector();

  {
    ImGuiStyle &s = ImGui::GetStyle();
    // Selection highlight (ImGuiCol_Header) — default is a translucent
    // blue that loses contrast against dark-theme text. Brighten it and
    // lock text at near-white so rows clearly read as selected.
    s.Colors[ImGuiCol_Header] = ImVec4(0.22f, 0.50f, 0.90f, 0.70f);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.58f, 0.96f, 0.80f);
    s.Colors[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.54f, 0.94f, 0.92f);
    s.Colors[ImGuiCol_Text] = ImVec4(0.94f, 0.96f, 1.0f, 1.0f);
    s.Colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.63f, 1.0f);
  }

  ImGui_ImplSDL3_InitForOpenGL(window, glCtx);
  ImGui_ImplOpenGL3_Init("#version 150");

  if (!initialTrace.empty()) {
    app.trace = layerviewer::LoadAndReplay(initialTrace);
    if (!app.trace->error.empty())
      std::fprintf(stderr, "trace load failed: %s\n", app.trace->error.c_str());
  }

  bool quit = false;
  while (!quit) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      ImGui_ImplSDL3_ProcessEvent(&ev);
      if (ev.type == SDL_EVENT_QUIT)
        quit = true;
      else if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
               ev.window.windowID == SDL_GetWindowID(window))
        quit = true;
      else if (ev.type == SDL_EVENT_DROP_FILE) {
        app.trace = layerviewer::LoadAndReplay(ev.drop.data);
        app.frameIndex = 0;
        app.selectedLayerId = kUnassignedLayerId;
      }
    }

    int fbW, fbH;
    SDL_GetWindowSizeInPixels(window, &fbW, &fbH);

    if (app.trace && !app.trace->frames.empty()) {
      app.frameIndex =
          std::clamp(app.frameIndex, 0, (int)app.trace->frames.size() - 1);
    }

    // Clear the window's default framebuffer — ImGui paints everything on
    // top, the Preview window separately owns an offscreen Skia surface.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fbW, fbH);
    glClearColor(0.09f, 0.09f, 0.11f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Dockspace + menu bar.
    ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(
        0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    if (app.resetLayoutOnce || app.requestResetLayout) {
      BuildDefaultLayout(dockspaceId);
      app.resetLayoutOnce = false;
      app.requestResetLayout = false;
    }

    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Cmd+O"))
          OpenFileDialog(window, app);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Cmd+Q"))
          quit = true;
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Reset Layout"))
          app.requestResetLayout = true;
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }

    // Cmd+O shortcut.
    if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_O, false))
      OpenFileDialog(window, app);

    // Snapshot Inspector before Trace Info and Transaction Inspector so it
    // wins the default-active-tab for the "right" dock node (tab order
    // follows Begin-call order within the same docked node, not
    // DockBuilderDockWindow order).
    if (ImGui::Begin("Snapshot Inspector")) {
      if (app.trace && !app.trace->frames.empty())
        DrawInspector(app.trace->frames[app.frameIndex], app);
    }
    ImGui::End();

    if (ImGui::Begin("Transaction Inspector"))
      DrawTransactionInspector(app);
    ImGui::End();

    if (ImGui::Begin("Trace Info")) {
      if (!app.trace) {
        ImGui::TextUnformatted(
            "File → Open (⌘O) or drop a .pftrace onto the window.");
      } else if (!app.trace->error.empty()) {
        ImGui::Text("Load error: %s", app.trace->error.c_str());
      } else {
        ImGui::Text("File:         %s", app.trace->path.c_str());
        ImGui::Text("Packets:      %d", app.trace->packetCount);
        ImGui::Text("SF snapshots: %d", app.trace->layerSnapshotPacketCount);
        ImGui::Text("Replay frames: %zu", app.trace->frames.size());
        if (!app.trace->frames.empty()) {
          const auto &f = app.trace->frames[app.frameIndex];
          int withBuffer = 0;
          for (const auto &snap : f.snapshots)
            if (snap.externalTexture)
              withBuffer++;
          ImGui::Separator();
          ImGui::Text("Current vsync: %lld", (long long)f.vsyncId);
          ImGui::Text("Display:       %dx%d", f.displayWidth, f.displayHeight);
          ImGui::Text("Reachable:     %zu layers (%d with buffer)",
                      f.snapshots.size(), withBuffer);
        }
      }
    }
    ImGui::End();

    if (ImGui::Begin("Layers")) {
      if (app.trace && !app.trace->frames.empty())
        DrawLayerTreePane(app.trace->frames[app.frameIndex], app);
    }
    ImGui::End();

    if (ImGui::Begin("Timeline"))
      DrawTimeline(app);
    ImGui::End();

    if (ImGui::Begin("Transactions"))
      DrawTransactions(app);
    ImGui::End();

    if (ImGui::Begin("Preview")) {
      // Toolbar — keep interactions discoverable without cluttering.
      ImGui::TextDisabled(
          "drag/two-finger scroll = pan   ⌘scroll/pinch = zoom");
      ImGui::SameLine();
      if (ImGui::SmallButton("reset view"))
        app.preview = PreviewView{};
      ImGui::SameLine();
      ImGui::Text("%.2fx", app.preview.zoom);

      ImVec2 avail = ImGui::GetContentRegionAvail();
      if (avail.x > 0 && avail.y > 0 && app.trace &&
          !app.trace->frames.empty()) {
        // Compose this frame through CE + SkiaRenderEngine. Returns the GL
        // texture name of the composed output buffer (0 on failure).
        unsigned int composedTex =
            compositor.composeFrame(app.trace->frames[app.frameIndex]);

        // RE's own Ganesh context touched a bunch of GL state — restore the
        // default framebuffer + viewport for ImGui, same as we used to do
        // after Skia FBO rendering.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fbW, fbH);

        ImVec2 winMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##previewnav", avail,
                               ImGuiButtonFlags_MouseButtonLeft |
                                   ImGuiButtonFlags_MouseButtonMiddle |
                                   ImGuiButtonFlags_MouseButtonRight);
        ImVec2 winMax(winMin.x + avail.x, winMin.y + avail.y);
        float winCx = 0.5f * (winMin.x + winMax.x);
        float winCy = 0.5f * (winMin.y + winMax.y);

        // Aspect-preserving fit of the composed display rect into the window.
        float rw =
            static_cast<float>(app.trace->frames[app.frameIndex].displayWidth);
        float rh =
            static_cast<float>(app.trace->frames[app.frameIndex].displayHeight);
        float baseW = avail.x, baseH = avail.y;
        if (rw > 0 && rh > 0) {
          float ar = rw / rh;
          float winAr = avail.x / avail.y;
          if (winAr > ar) {
            baseH = avail.y;
            baseW = baseH * ar;
          } else {
            baseW = avail.x;
            baseH = baseW / ar;
          }
        }
        float imgW = baseW * app.preview.zoom;
        float imgH = baseH * app.preview.zoom;
        ImVec2 imgMin(winCx + app.preview.panX - imgW * 0.5f,
                      winCy + app.preview.panY - imgH * 0.5f);
        ImVec2 imgMax(imgMin.x + imgW, imgMin.y + imgH);

        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(winMin, winMax, true);
        dl->AddRectFilled(winMin, winMax, IM_COL32(18, 18, 22, 255));
        if (composedTex) {
          dl->AddImage(static_cast<ImTextureID>(composedTex), imgMin, imgMax);
        } else {
          dl->AddText(ImVec2(winMin.x + 12, winMin.y + 12),
                      IM_COL32(200, 200, 220, 220),
                      "(no display info in this frame)");
        }
        dl->AddRect(imgMin, imgMax, IM_COL32(160, 160, 200, 220), 0.f, 0, 1.f);
        dl->PopClipRect();

        // Input handling — unchanged from the Skia-FBO era. Plain wheel pans
        // (matches macOS two-finger scroll); ⌘/Ctrl+wheel zooms around the
        // cursor; dragging pans. See earlier commit for the pan-anchor math.
        if (ImGui::IsItemHovered()) {
          float wheelY = io.MouseWheel;
          float wheelX = io.MouseWheelH;
          bool zoomMod = io.KeyCtrl || io.KeySuper;
          if (wheelY != 0.f && zoomMod) {
            float oldZoom = app.preview.zoom;
            float newZoom =
                std::clamp(oldZoom * std::pow(1.15f, wheelY), 0.1f, 30.f);
            float ratio = newZoom / oldZoom;
            ImVec2 m = ImGui::GetMousePos();
            app.preview.panX =
                (m.x - winCx) * (1.f - ratio) + app.preview.panX * ratio;
            app.preview.panY =
                (m.y - winCy) * (1.f - ratio) + app.preview.panY * ratio;
            app.preview.zoom = newZoom;
          } else if (wheelX != 0.f || wheelY != 0.f) {
            app.preview.panX += wheelX * 30.f;
            app.preview.panY += wheelY * 30.f;
          }
        }
        if (ImGui::IsItemActive() &&
            (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
             ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
             ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
          ImVec2 delta = io.MouseDelta;
          app.preview.panX += delta.x;
          app.preview.panY += delta.y;
        }
      } else if (!app.trace) {
        ImGui::TextUnformatted("(no trace loaded)");
      }
    }
    ImGui::End();

    if (ImGui::Begin("Wireframe")) {
      if (app.trace && !app.trace->frames.empty())
        DrawWireframe(app.trace->frames[app.frameIndex], app);
      else
        ImGui::TextUnformatted("(no trace loaded)");
    }
    ImGui::End();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  // Tear down the compositor before the GL context goes away — the output
  // GraphicBuffer + every layer's populated buffer owns GL textures, and
  // SkiaRenderEngine owns an internal GrDirectContext, all needing a live
  // current context. Ordered teardown: see LayerViewerCompositor::destroy().
  compositor.destroy();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(glCtx);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
