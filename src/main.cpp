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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
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
        auto args =
            RenderEngineCreationArgs::Builder()
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
            static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 /*RGBA_8888*/,
            1,
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
        // Per-frame state that may drift even at a fixed size (layer stack +
        // the orientation we projection into): refresh on every compose so
        // scrubbing across a trace where these change doesn't wedge the output.
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
            fe->mSnapshot = std::make_unique<
                android::surfaceflinger::frontend::LayerSnapshot>(snap);
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
        // LayerSettings inherits from it with extra buffer-tracking fields.
        // Slice via copy — upstream SF does the same (Output::composeSurfaces
        // pushes through clientCompositionLayers as-is because the slicing
        // happens inside RenderEngine's caching layer).
        std::vector<LayerSettings> baseLayers;
        baseLayers.reserve(layerSettings.size());
        for (const auto &ls : layerSettings)
            baseLayers.push_back(ls);
        // ImGui leaves glViewport at the window's drawable size (smaller than a
        // phone-sized output buffer on a typical macOS window). Ganesh caches
        // GL state and assumes no one else touched it, so binding the output
        // FBO does *not* re-issue glViewport — the stale small viewport clips
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
        false; // tree filter: hide non-visible (keep ancestors)
    bool treeShortNames = true;      // winscope-style name shortening in tree
    bool resetLayoutOnce = true;     // first-run default layout
    bool requestResetLayout = false; // View → Reset Layout
    // Transactions window state.
    int selectedTransactionIdx = -1; // index into ReplayedTrace::transactions
    bool autoSyncTimeline = true;    // auto-move frameIndex on txn selection
    bool scrollTxnTableToSelection = false;
    // What the user last picked manually — drives the yellow vs. cyan
    // distinction on timeline slices. Yellow is reserved for the actual
    // click target; the cross-linked side gets cyan.
    enum class ManualSelect { Frame, Txn };
    ManualSelect manualSelect = ManualSelect::Frame;
    // Pinned tracks render before unpinned ones in the timeline. Stores
    // pids; -1 represents the synthetic Frames track.
    std::set<int> pinnedTrackPids;
    // Click-and-drag measurement on the ruler. -1 = no active or sticky
    // measurement. While the user holds the mouse on the ruler, both
    // values update; on release, the markers stay until the user clicks
    // a slice or starts a new measurement.
    int64_t measureStartNs = -1;
    int64_t measureEndNs = -1;
    // True between the mouse-down on the ruler and the matching mouse-up.
    // Tracked explicitly (rather than via IsItemActive) so the drag
    // continues even when the cursor leaves the ruler band.
    bool measuring = false;
    // Timeline hover — captured each frame while the user is over the strip,
    // read one frame later to render an info line above the strip. Using
    // last-frame's value avoids a two-pass layout.
    int timelineHoverIdx = -1;
    // Timeline pan+zoom. zoom=1 fits all entries into the strip width;
    // scrollX is a pixel offset into the virtual (zoomed) strip.
    float timelineZoom = 1.f;
    float timelineScrollX = 0.f;
    // Last frame's timeline cell width — used to rescale `timelineScrollX`
    // when the cell resizes (e.g. the Timeline window grows a vertical
    // scrollbar) so the time-under-cursor doesn't shift.
    float lastTimelineCellW = 0.f;
    // WASD animation — spring-with-acceleration, ported from perfetto's
    // wasd_navigation_handler.ts. `target` grows while the key is held;
    // `current` chases it each frame via SNAP * (target - current).
    // Releasing the key lets the chase finish so motion eases out
    // instead of stopping abruptly.
    int panDir = 0; // -1 / 0 / +1 while A / (none) / D held
    float panMsElapsed = 0.f;
    float panTarget = 0.f;
    float panCurrent = 0.f;
    int zoomDir = 0;
    float zoomMsElapsed = 0.f;
    float zoomTargetRatio = 0.f;
    float zoomCurrentRatio = 0.f;
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

// Cross-product z of (q - p) x (r - p). Positive / negative tells which side
// of the segment pq the point r is on.
float Cross2(ImVec2 p, ImVec2 q, ImVec2 r) {
    return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
}

// Point-in-convex-quad via cross-product signs. Works for any winding order.
bool PointInQuad(ImVec2 m, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d) {
    float s1 = Cross2(a, b, m), s2 = Cross2(b, c, m);
    float s3 = Cross2(c, d, m), s4 = Cross2(d, a, m);
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
    float hue =
        (bufferId ? static_cast<float>(bufferId) : 0.5f) * 0.61803398875f;
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
    // Default ImGui indent is 21px/level — a 10-deep AOSP layer hierarchy
    // easily eats 200+ pixels of horizontal space before the label even
    // starts. Shrink to 12px for the tree only.
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 12.f);
    for (uint32_t rootId : frame.rootIds)
        DrawLayerTreeNode(frame, rootId, app, visible, keep);
    ImGui::PopStyleVar();

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

// Bracketed key/value section idiom used by both inspectors. Use:
//
//     if (BeginKvSection("Identity")) {
//       KvInt("pid", pid);
//       KvStr("name", name);
//       KvBool("visible", visible);
//       ...
//       EndKvSection();
//     }
//
// Each Kv* row is only formatted when the section is actually open, so
// long sections can stay lazy without the caller threading a bool.
bool BeginKvSection(const char *label, bool defaultOpen = true) {
    ImGuiTreeNodeFlags flags = defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0;
    if (!ImGui::CollapsingHeader(label, flags))
        return false;
    ImGuiTableFlags tflags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg;
    return ImGui::BeginTable(label, 2, tflags);
}
void EndKvSection() {
    ImGui::EndTable();
}

// Lowest-level row writer — the typed helpers below funnel through this.
void KvStr(const char *key, const char *value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(key);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(value);
}
void KvStr(const char *key, const std::string &value) {
    KvStr(key, value.c_str());
}

void KvBool(const char *key, bool v) {
    KvStr(key, v ? "yes" : "no");
}
void KvInt(const char *key, long long v) {
    KvStr(key, std::to_string(v).c_str());
}
void KvHex(const char *key, unsigned long long v) {
    KvStr(key, android::base::StringPrintf("0x%llx", v));
}

// printf-style convenience for one-off formats. Separate from KvStr so
// we don't accidentally pass a std::string as a format string.
template <class... Args>
void KvFmt(const char *key, const char *fmt, Args &&...args) {
    KvStr(key, android::base::StringPrintf(fmt, std::forward<Args>(args)...));
}

void KvRect(const char *key, const android::Rect &r) {
    KvFmt(key, "(%d, %d) -> (%d, %d)  [%dx%d]", r.left, r.top, r.right,
          r.bottom, r.width(), r.height());
}
void KvFloatRect(const char *key, const android::FloatRect &r) {
    KvFmt(key, "(%.1f, %.1f) -> (%.1f, %.1f)  [%.1fx%.1f]", r.left, r.top,
          r.right, r.bottom, r.right - r.left, r.bottom - r.top);
}
void KvTransform(const char *key, const android::ui::Transform &t) {
    KvFmt(key, "dsdx=%.3f dtdx=%.3f tx=%.1f\ndtdy=%.3f dsdy=%.3f ty=%.1f",
          t.dsdx(), t.dtdx(), t.tx(), t.dtdy(), t.dsdy(), t.ty());
}

using Reachability =
    android::surfaceflinger::frontend::LayerSnapshot::Reachablilty;

std::string ReachStr(Reachability r) {
    switch (r) {
    case Reachability::Reachable:
        return "reachable";
    case Reachability::Unreachable:
        return "unreachable";
    case Reachability::ReachableByRelativeParent:
        return "via relative parent";
    }
    return "?";
}

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

    ImGui::TextWrapped("%s", snap->name.c_str());
    ImGui::TextDisabled("#%u  globalZ=%zu  sequence=%d  unique=%u",
                        app.selectedLayerId, snap->globalZ, snap->sequence,
                        snap->uniqueSequence);

    if (BeginKvSection("Identity")) {
        KvInt("id", app.selectedLayerId);
        if (parentId == UINT32_MAX)
            KvStr("parent", "-");
        else
            KvInt("parent", parentId);
        KvInt("sequence", snap->sequence);
        KvInt("uniqueSequence", snap->uniqueSequence);
        KvInt("globalZ", snap->globalZ);
        KvInt("layerStack", snap->outputFilter.layerStack.id);
        KvInt("uid", snap->uid.val());
        KvInt("pid", snap->pid.val());
        KvStr("debugName", snap->debugName);
        EndKvSection();
    }

    if (BeginKvSection("Visibility")) {
        KvBool("isVisible", snap->isVisible);
        KvStr("reachablilty", ReachStr(snap->reachablilty));
        KvBool("hiddenByPolicyFromParent", snap->isHiddenByPolicyFromParent);
        KvBool("hiddenByPolicyFromRelativeParent",
               snap->isHiddenByPolicyFromRelativeParent);
        KvBool("contentDirty", snap->contentDirty);
        KvBool("hasReadyFrame", snap->hasReadyFrame);
        KvBool("isOpaque", snap->isOpaque);
        KvBool("contentOpaque", snap->contentOpaque);
        KvBool("layerOpaqueFlagSet", snap->layerOpaqueFlagSet);
        KvBool("isSecure", snap->isSecure);
        KvBool("forceClientComposition", snap->forceClientComposition);
        KvBool("isSmallDirty", snap->isSmallDirty);
        KvStr("reason", snap->getIsVisibleReason());
        EndKvSection();
    }

    if (BeginKvSection("Geometry")) {
        KvFloatRect("transformedBounds", snap->transformedBounds);
        KvFloatRect("geomLayerBounds", snap->geomLayerBounds);
        KvFloatRect("geomLayerCrop", snap->geomLayerCrop);
        KvFloatRect("geomCrop", snap->geomCrop);
        KvRect("geomContentCrop", snap->geomContentCrop);
        KvRect("bufferSize", snap->bufferSize);
        KvFloatRect("croppedBufferSize", snap->croppedBufferSize);
        KvRect("geomBufferSize", snap->geomBufferSize);
        KvRect("cursorFrame", snap->cursorFrame);
        KvTransform("geomLayerTransform", snap->geomLayerTransform);
        KvTransform("localTransform", snap->localTransform);
        KvTransform("parentTransform", snap->parentTransform);
        KvHex("geomBufferTransform", snap->geomBufferTransform);
        KvHex("bufferTransform", snap->geomBufferTransform);
        KvBool("invalidTransform", snap->invalidTransform);
        KvBool("geomUsesSourceCrop", snap->geomUsesSourceCrop);
        KvBool("geomBufferUsesDisplayInverseTransform",
               snap->geomBufferUsesDisplayInverseTransform);
        EndKvSection();
    }

    if (BeginKvSection("Color / Blending")) {
        KvFmt("color", "r=%.2f g=%.2f b=%.2f a=%.2f",
              static_cast<float>(snap->color.r),
              static_cast<float>(snap->color.g),
              static_cast<float>(snap->color.b),
              static_cast<float>(snap->color.a));
        KvFmt("alpha", "%.2f", static_cast<float>(snap->alpha));
        KvHex("dataspace", static_cast<uint32_t>(snap->dataspace));
        KvBool("dimmingEnabled", snap->dimmingEnabled);
        KvBool("colorTransformIsIdentity", snap->colorTransformIsIdentity);
        KvBool("premultipliedAlpha", snap->premultipliedAlpha);
        KvFmt("cornerRadius", "x=%.2f y=%.2f", snap->roundedCorner.radius.x,
              snap->roundedCorner.radius.y);
        KvFloatRect("cornerCrop", snap->roundedCorner.cropRect);
        KvInt("backgroundBlurRadius", snap->backgroundBlurRadius);
        KvInt("blurRegions", snap->blurRegions.size());
        EndKvSection();
    }

    if (snap->externalTexture && BeginKvSection("Buffer")) {
        KvInt("id", snap->externalTexture->getId());
        KvFmt("size", "%ux%u", snap->externalTexture->getWidth(),
              snap->externalTexture->getHeight());
        KvInt("pixelFormat", snap->externalTexture->getPixelFormat());
        KvHex("usage", snap->externalTexture->getUsage());
        KvInt("frameNumber", snap->frameNumber);
        KvBool("hasProtectedContent", snap->hasProtectedContent);
        EndKvSection();
    }

    if (BeginKvSection("Input", /*defaultOpen=*/false)) {
        KvBool("hasInputInfo", snap->hasInputInfo());
        KvBool("canReceiveInput", snap->canReceiveInput());
        KvRect("touchableRegion bounds",
               snap->inputInfo.touchableRegion.getBounds());
        KvRect("frame", snap->inputInfo.frame);
        KvFmt("globalScaleFactor", "%.3f", snap->inputInfo.globalScaleFactor);
        KvInt("surfaceInset", snap->inputInfo.surfaceInset);
        KvBool("token", static_cast<bool>(snap->inputInfo.token));
        KvInt("dropInputMode", static_cast<int>(snap->dropInputMode));
        KvInt("trustedOverlay", static_cast<int>(snap->trustedOverlay));
        EndKvSection();
    }

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
        out +=
            android::base::StringPrintf("0x%llx", (unsigned long long)unknown);
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
    ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingFixedFit;
    // Table id bumped (_v3) so any stale per-column layout in imgui.ini
    // from previous column sets is discarded.
    if (ImGui::BeginTable("txns_v3", 4, tflags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 56.f);
        ImGui::TableSetupColumn("frame", ImGuiTableColumnFlags_WidthFixed,
                                64.f);
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
            std::string idLabel = std::to_string(i);
            if (ImGui::Selectable(idLabel.c_str(), sel,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                app.selectedTransactionIdx = i;
                app.manualSelect = AppState::ManualSelect::Txn;
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
            // "process" falls back to "pid N" when the trace has no
            // ProcessTree.
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
            app.manualSelect = AppState::ManualSelect::Txn;
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
            ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed,
                                    48.f);
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch,
                                    0.35f);
            ImGui::TableSetupColumn("changes",
                                    ImGuiTableColumnFlags_WidthStretch, 0.65f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < t.layerStateChanges.size(); i++) {
                const auto &lc = t.layerStateChanges[i];
                const auto *snap =
                    frame ? frame->snapshot(lc.layerId) : nullptr;
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
                ImGui::TextUnformatted(snap ? snap->name.c_str()
                                            : "(not in frame)");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", DecodeLayerStateWhat(lc.what).c_str());
            }
            ImGui::EndTable();
        }
    }

    if (BeginKvSection("Identity")) {
        KvInt("index", app.selectedTransactionIdx);
        KvFmt("transactionId", "%llu (0x%llx)",
              (unsigned long long)t.transactionId,
              (unsigned long long)t.transactionId);
        KvInt("frameIndex", t.frameIndex);
        if (frame)
            KvInt("frame vsyncId", frame->vsyncId);
        else
            KvStr("frame vsyncId", "-");
        if (frame)
            KvFmt("frame elapsed-realtime", "%.9f s", frame->tsNs / 1e9);
        else
            KvStr("frame elapsed-realtime", "-");
        EndKvSection();
    }

    if (BeginKvSection("Source")) {
        KvInt("pid", t.pid);
        std::unordered_map<int32_t, std::string>::const_iterator it =
            app.trace->pidNames.find(t.pid);
        if (it != app.trace->pidNames.end())
            KvStr("process", it->second);
        else
            KvStr("process", "(unknown — no ProcessTree in trace)");
        KvInt("uid", t.uid);
        if (t.inputEventId)
            KvInt("inputEventId", t.inputEventId);
        else
            KvStr("inputEventId", "0 (none)");
        EndKvSection();
    }

    if (BeginKvSection("Timing")) {
        KvInt("postTime (ns)", t.postTimeNs);
        KvFmt("postTime (s)", "%.9f", t.postTimeNs / 1e9);
        KvInt("vsyncId (in txn)", t.vsyncId);
        if (frame)
            KvInt("vsyncId (frame)", frame->vsyncId);
        else
            KvStr("vsyncId (frame)", "-");
        if (frame)
            KvBool("matches frame vsync", frame->vsyncId == t.vsyncId);
        else
            KvStr("matches frame vsync", "-");
        EndKvSection();
    }

    if (BeginKvSection("Contents")) {
        KvInt("layer changes", t.layerChanges);
        KvInt("display changes", t.displayChanges);
        KvInt("affected layers (unique)", t.affectedLayerIds.size());
        KvInt("merged transactions", t.mergedTransactionIds.size());
        EndKvSection();
    }

    if (!t.mergedTransactionIds.empty() &&
        ImGui::CollapsingHeader("Merged transaction ids")) {
        if (ImGui::BeginTable("merged", 1,
                              ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY,
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
// Timeline — Perfetto-style tracks + slices.
// ---------------------------------------------------------------------------
// Layout:
//   [time ruler     ]    — tick marks every pow-of-10 ns at current zoom
//   [Frames         ]    — one slice per CapturedFrame
//   [<pid/process>  ]    — one slice per transaction, one track per pid
//   [<pid/process>  ]
//   ...
// Controls (copied from Perfetto):
//   Plain wheel scroll         → pan
//   Shift / Ctrl / Cmd + wheel → zoom, cursor-anchored
//   W / S                      → zoom in / out at cursor
//   A / D                      → pan left / right
//   Click slice                → select (selects that transaction or frame)
// ---------------------------------------------------------------------------

// Front-truncate a string so its rendered width fits in maxPx. The tail
// (the informative part of an Android package/activity name) stays
// visible; a leading "..." signals truncation. Default ImGui font doesn't
// ship the unicode ellipsis, so we use three ASCII dots.
std::string TruncateFrontToWidth(const std::string &s, float maxPx) {
    if (s.empty())
        return s;
    if (ImGui::CalcTextSize(s.c_str()).x <= maxPx)
        return s;
    const char *ell = "...";
    float ellW = ImGui::CalcTextSize(ell).x;
    if (ellW >= maxPx)
        return ell;
    int n = static_cast<int>(s.size());
    for (int drop = 1; drop < n; drop++) {
        std::string out;
        out.reserve(3 + (n - drop));
        out.append(ell);
        out.append(s, drop, n - drop);
        if (ImGui::CalcTextSize(out.c_str()).x <= maxPx)
            return out;
    }
    return ell;
}

// Pretty-format a nanosecond duration for ruler labels, picking the biggest
// unit where the integer part is >= 1 AND using only as many decimals as the
// tick step itself has (so "1s", "100ms", "250ms" — never "1.00s", etc.).
std::string FormatNs(double ns, int64_t stepNs = 1) {
    struct Unit {
        double scale;
        const char *suffix;
    };
    const Unit units[] = {{1e9, "s"}, {1e6, "ms"}, {1e3, "us"}, {1.0, "ns"}};
    const Unit *u = &units[3];
    for (const auto &cand : units) {
        if (std::abs(ns) >= cand.scale) {
            u = &cand;
            break;
        }
    }
    double v = ns / u->scale;
    double stepInUnit = double(stepNs) / u->scale;
    // Match label precision to the step so "1s"/"250ms"/"12.5ms" drop
    // trailing zeros based on what the step actually carries.
    if (stepInUnit >= 1.0)
        return android::base::StringPrintf("%.0f%s", v, u->suffix);
    if (stepInUnit >= 0.1)
        return android::base::StringPrintf("%.1f%s", v, u->suffix);
    return android::base::StringPrintf("%.2f%s", v, u->suffix);
}

// Format `ns` in a fixed unit (chosen by FormatNsInUnit's caller from the
// ruler's major step) so every label on the same ruler reads in one unit —
// never a mixed "0s 200ms 400ms" sequence.
std::string FormatNsInUnit(double ns, double scale, const char *suffix,
                           double stepInUnit) {
    double v = ns / scale;
    if (stepInUnit >= 1.0)
        return android::base::StringPrintf("%.0f%s", v, suffix);
    if (stepInUnit >= 0.1)
        return android::base::StringPrintf("%.1f%s", v, suffix);
    return android::base::StringPrintf("%.2f%s", v, suffix);
}

// Ported from Perfetto's gridline_helper.ts. Each pattern occupies one
// "patternSize" duration and is split into pattern.length subdivisions:
// '|' = MAJOR (drawn full-height + label), ':' = MEDIUM, '.' = MINOR.
// Picking patternSize from this table (smallest >= minPatternSize) gives
// nice round labels at every zoom level. We stop at days — the trace
// timebase fits in seconds, but the table is robust.
struct RulerPattern {
    int64_t size;
    const char *pattern;
};
static const RulerPattern kRulerPatterns[] = {
    {1LL, "|"},
    {2LL, "|:"},
    {5LL, "|...."},
    {10LL, "|....:...."},
    {20LL, "|.:."},
    {50LL, "|...."},
    {100LL, "|....:...."},
    {200LL, "|.:."},
    {500LL, "|...."},
    {1'000LL, "|....:...."},
    {2'000LL, "|.:."},
    {5'000LL, "|...."},
    {10'000LL, "|....:...."},
    {20'000LL, "|.:."},
    {50'000LL, "|...."},
    {100'000LL, "|....:...."},
    {200'000LL, "|.:."},
    {500'000LL, "|...."},
    {1'000'000LL, "|....:...."},
    {2'000'000LL, "|.:."},
    {5'000'000LL, "|...."},
    {10'000'000LL, "|....:...."},
    {20'000'000LL, "|.:."},
    {50'000'000LL, "|...."},
    {100'000'000LL, "|....:...."},
    {200'000'000LL, "|.:."},
    {500'000'000LL, "|...."},
    {1'000'000'000LL, "|....:...."},
    {2'000'000'000LL, "|.:."},
    {5'000'000'000LL, "|...."},
    {10'000'000'000LL, "|....:...."},
    {30'000'000'000LL, "|.:.:."},
    {60'000'000'000LL, "|....."},
};

// Pick a label color that stays readable on any slice fill — black on
// bright fills, near-white on dark ones. Uses sRGB-luma weights so a
// saturated green slice (which the eye reads as bright) flips to dark
// text correctly.
ImU32 PickReadableTextColor(ImU32 fill) {
    int r = (fill >> IM_COL32_R_SHIFT) & 0xff;
    int g = (fill >> IM_COL32_G_SHIFT) & 0xff;
    int b = (fill >> IM_COL32_B_SHIFT) & 0xff;
    float luma = (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.f;
    return luma > 0.55f ? IM_COL32(15, 15, 25, 255)
                        : IM_COL32(240, 240, 250, 255);
}

// Consistent per-pid color so each process's track stays recognisable.
ImU32 PidTrackColor(int pid) {
    uint32_t h = uint32_t(pid) * 2654435761u;
    uint8_t r = 70 + ((h >> 0) & 0xff) / 2;
    uint8_t g = 70 + ((h >> 8) & 0xff) / 2;
    uint8_t b = 80 + ((h >> 16) & 0xff) / 2;
    return IM_COL32(r, g, b, 255);
}

// Per-row state passed to the slice-drawing helper.
struct TimelineRow {
    ImDrawList *dl;
    ImVec2 cellTL;
    float cellW;
    float rowH;
    float scrollX;
    float virtW;
    int64_t t0Ns;
    int64_t totalNs;
    bool rowHovered;
    ImVec2 mouse;
};

// Slice visual width in pixels — fixed 1ms, with a 1px floor so deeply
// zoomed-out slices are still drawable. Time-based: every slice
// represents the same 1ms duration regardless of zoom or label.
inline float SliceWidthPx(float virtW, int64_t totalNs) {
    float oneMsPx = static_cast<float>(1.0e6 * virtW / double(totalNs));
    return std::max(1.f, oneMsPx);
}

// Slice duration in ns — also fixed at 1ms. Used by the depth-stacking
// pre-walk so maxDepth depends only on the underlying timestamps, not
// on zoom — track row heights stay static across zoom.
constexpr int64_t kSliceDurationNs = 1'000'000;

// Draw one slice on a timeline row. Returns true if the mouse is over it
// (caller uses this to flag the nearest transaction/frame as hovered).
// `isCurrent` = the slice's own selection (yellow); `isLinked` = a
// cross-reference highlight from the *other* selection (cyan, e.g. txns
// that fed the selected frame, or the frame a selected txn fed into).
// `depth` = which sub-row of the (possibly stacked) track to draw in;
// `r.rowH` is the sub-row height.
bool DrawTimelineSlice(const TimelineRow &r, int64_t ns, ImU32 col,
                       const std::string &name, bool isCurrent,
                       bool isLinked = false, int depth = 0) {
    float x =
        r.cellTL.x - r.scrollX +
        static_cast<float>(double(ns - r.t0Ns) / double(r.totalNs) * r.virtW);
    double visibleNs = double(r.totalNs) * double(r.cellW) / double(r.virtW);
    // Text appears below the 500ms-visible threshold; the label gets
    // clipped to the slice rect so narrow slices just show the leading
    // characters rather than scaling the slice up to fit.
    bool textOn = !name.empty() && visibleNs <= 500.0e6;
    float w = SliceWidthPx(r.virtW, r.totalNs);
    float sx0 = x - w * 0.5f;
    float sx1 = x + w * 0.5f;
    if (sx1 < r.cellTL.x || sx0 > r.cellTL.x + r.cellW)
        return false;
    float yTop = r.cellTL.y + depth * r.rowH;
    float yBot = yTop + r.rowH;
    // Margin scales with sub-row height so very thin rows (zoomed out)
    // still get a visible fill instead of collapsing into the gap.
    float margin = std::min(2.f, r.rowH * 0.15f);
    r.dl->AddRectFilled(ImVec2(sx0, yTop + margin), ImVec2(sx1, yBot - margin),
                        col);
    // Highlight: single bright stroke just outside the fill on the
    // dark track background. `isCurrent` wins over `isLinked` so the
    // user's actual pick is the most prominent border.
    if (isCurrent || isLinked) {
        ImU32 stroke = isCurrent ? IM_COL32(255, 200, 60, 255)
                                 : IM_COL32(120, 220, 255, 255);
        r.dl->AddRect(ImVec2(sx0 - 1.5f, yTop + 0.5f),
                      ImVec2(sx1 + 1.5f, yBot - 0.5f), stroke, 0.f, 0, 2.f);
    }
    if (textOn) {
        // Clip the label to the slice rect so text from zoomed-in slices
        // can't bleed onto neighbours. ImDrawList intersects this with the
        // already-pushed row clip, so we still get the cell boundary too.
        r.dl->PushClipRect(ImVec2(sx0 + 1, yTop + margin),
                           ImVec2(sx1 - 1, yBot - margin), true);
        r.dl->AddText(ImVec2(sx0 + 3, yTop + 3), PickReadableTextColor(col),
                      name.c_str());
        r.dl->PopClipRect();
    }
    return r.rowHovered && r.mouse.x >= sx0 && r.mouse.x <= sx1 &&
           r.mouse.y >= yTop && r.mouse.y <= yBot;
}

// Cursor-anchored zoom: keep the virtual-x under `anchorPx` fixed so the
// content under the mouse doesn't slide while zooming.
void ZoomTimeline(float newZoom, float anchorPx, float &zoom, float &scrollX) {
    float old = zoom;
    if (newZoom == old)
        return;
    float virtAtAnchor = anchorPx + scrollX;
    zoom = newZoom;
    scrollX = virtAtAnchor * (newZoom / old) - anchorPx;
}

void DrawTimeline(AppState &app) {
    if (!app.trace || app.trace->frames.empty()) {
        ImGui::TextUnformatted("(no trace loaded)");
        return;
    }
    const auto &frames = app.trace->frames;
    const auto &txns = app.trace->transactions;
    const int n = static_cast<int>(frames.size());

    // --- Toolbar -----------------------------------------------------------
    if (ImGui::Button("prev frame")) {
        app.frameIndex = std::max(0, app.frameIndex - 1);
        app.manualSelect = AppState::ManualSelect::Frame;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Previous frame (left arrow)");
    ImGui::SameLine();
    if (ImGui::Button("next frame")) {
        app.frameIndex = std::min(n - 1, app.frameIndex + 1);
        app.manualSelect = AppState::ManualSelect::Frame;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Next frame (right arrow)");
    ImGui::SameLine();
    ImGui::Text("Frame %d / %d   vsync=%lld   ts=%.3fs", app.frameIndex + 1, n,
                (long long)frames[app.frameIndex].vsyncId,
                frames[app.frameIndex].tsNs / 1e9);
    ImGui::SameLine();
    if (ImGui::SmallButton("reset view")) {
        app.timelineZoom = 1.f;
        app.timelineScrollX = 0.f;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reset zoom (1x) and scroll to 0");

    // Hover detail line (one-frame-latency — cursor under the strip next
    // frame uses this row for its summary).
    if (app.timelineHoverIdx >= 0 && app.timelineHoverIdx < n) {
        const auto &hf = frames[app.timelineHoverIdx];
        ImGui::Text("frame %d  vsync=%lld  ts=%.3fs  txns=%d  +layers=%d  "
                    "-handles=%d  displays=%s  reachable=%zu",
                    app.timelineHoverIdx, (long long)hf.vsyncId, hf.tsNs / 1e9,
                    hf.txnCount, hf.addedCount, hf.destroyedHandleCount,
                    hf.displaysChanged ? "changed" : "-", hf.snapshots.size());
    } else {
        ImGui::TextDisabled(
            "scroll = pan · shift/ctrl + scroll = zoom · W/S zoom · A/D pan");
    }

    // --- Track list --------------------------------------------------------
    struct Track {
        int pid;          // -1 for the Frames track
        std::string name; // "Frames" or the process cmdline (if resolved)
    };
    std::vector<Track> tracks;
    tracks.push_back({-1, "Frames"});
    {
        std::set<int32_t> pids;
        for (const auto &t : txns)
            pids.insert(t.pid);
        for (int32_t pid : pids) {
            auto it = app.trace->pidNames.find(pid);
            tracks.push_back({pid, it != app.trace->pidNames.end()
                                       ? it->second
                                       : std::string()});
        }
    }
    // Reorder so pinned tracks come first, preserving relative order
    // within each group (stable partition). The Frames track (pid -1)
    // is implicitly always-pinned so it stays at the top.
    std::stable_partition(tracks.begin(), tracks.end(), [&](const Track &t) {
        return t.pid == -1 || app.pinnedTrackPids.count(t.pid) > 0;
    });
    const int numTracks = static_cast<int>(tracks.size());
    if (numTracks == 0)
        return;

    const float kRowH = 22.f;
    const float kRulerH = 22.f;
    const int64_t t0Ns = frames.front().tsNs;
    const int64_t t1Ns = frames.back().tsNs;
    const int64_t totalNs = std::max<int64_t>(1, t1Ns - t0Ns);

    // Hover state collected from all timeline cells this frame.
    int hoverFrameIdx = -1;
    int hoverTxnIdx = -1;
    bool anyTimelineHovered = false;
    bool anyTimelineClicked = false; // left-click fell on a timeline cell
    const ImVec2 mouse = ImGui::GetMousePos();

    // Cache the timeline cell geometry so the pan/zoom math below (outside
    // the table) uses the same reference as the slice drawing inside the
    // cells. Captured in the ruler cell (first row, col 2).
    float timelineCellW = 0.f;
    float timelineCellLeft = 0.f;

    ImGuiTableFlags tflags = ImGuiTableFlags_Resizable |
                             ImGuiTableFlags_BordersInnerV |
                             ImGuiTableFlags_NoPadInnerX;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 0));
    if (!ImGui::BeginTable("timeline_split", 4, tflags)) {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::TableSetupColumn("pin", ImGuiTableColumnFlags_WidthFixed, 24.f);
    ImGui::TableSetupColumn("pid", ImGuiTableColumnFlags_WidthFixed, 48.f);
    ImGui::TableSetupColumn("track", ImGuiTableColumnFlags_WidthFixed, 180.f);
    ImGui::TableSetupColumn("timeline", ImGuiTableColumnFlags_WidthStretch);

    // --- Ruler row ---------------------------------------------------------
    ImGui::TableNextRow(0, kRulerH);
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("pin");
    ImGui::TableSetColumnIndex(1);
    ImGui::TextDisabled("pid");
    ImGui::TableSetColumnIndex(2);
    ImGui::TextDisabled("track");
    ImGui::TableSetColumnIndex(3);
    {
        // Draw the ruler in this cell. First read the cell's width so we can
        // size the virtual timeline against it. Capture the cell left edge
        // for the input block below — all timeline cells share the same X.
        ImVec2 cellTL = ImGui::GetCursorScreenPos();
        float cellW = ImGui::GetContentRegionAvail().x;
        timelineCellW = std::max(8.f, cellW);
        timelineCellLeft = cellTL.x;

        // Rescale the horizontal scroll when the cell width changes (e.g.
        // a vertical resize made the window grow/lose its scrollbar) so
        // the on-screen time range stays put instead of drifting.
        if (app.lastTimelineCellW > 0.f &&
            std::abs(timelineCellW - app.lastTimelineCellW) > 0.5f) {
            app.timelineScrollX *= timelineCellW / app.lastTimelineCellW;
        }
        app.lastTimelineCellW = timelineCellW;

        // Capture invisible button for ruler-area input. Hovering the ruler
        // also pans/zooms the whole timeline.
        ImGui::InvisibleButton("##ruler", ImVec2(timelineCellW, kRulerH));
        bool rulerHovered = ImGui::IsItemHovered();
        anyTimelineHovered = anyTimelineHovered || rulerHovered;

        const float virtW = timelineCellW * std::max(1.f, app.timelineZoom);
        app.timelineScrollX = std::clamp(app.timelineScrollX, 0.f,
                                         std::max(0.f, virtW - timelineCellW));
        const float scrollX = app.timelineScrollX;

        // Click-and-drag measurement on the ruler. We capture both
        // endpoints in ns so the markers track scroll/zoom changes
        // mid-drag (and stay correct after release).
        auto rulerNsAtMouse = [&]() {
            float mx = std::clamp(mouse.x - cellTL.x, 0.f, timelineCellW);
            return t0Ns + static_cast<int64_t>((double(scrollX) + double(mx)) *
                                               double(totalNs) / double(virtW));
        };
        if (rulerHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int64_t ns = rulerNsAtMouse();
            app.measureStartNs = ns;
            app.measureEndNs = ns;
            app.measuring = true;
        }
        if (app.measuring) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                app.measureEndNs = rulerNsAtMouse();
            } else {
                app.measuring = false;
            }
        }

        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(
            cellTL, ImVec2(cellTL.x + timelineCellW, cellTL.y + kRulerH), true);
        double nsPerPixel = double(totalNs) / double(virtW);
        // Pick a pattern whose major-tick spacing is at least ~120px, the
        // same MIN_PX_PER_STEP Perfetto uses. Pattern length encodes how
        // many subdivisions sit between two major ticks.
        constexpr double kMinPxPerMajor = 120.0;
        int64_t minPatternNs = int64_t(kMinPxPerMajor * nsPerPixel);
        const RulerPattern *picked =
            &kRulerPatterns[IM_ARRAYSIZE(kRulerPatterns) - 1];
        for (const auto &p : kRulerPatterns) {
            if (p.size >= minPatternNs) {
                picked = &p;
                break;
            }
        }
        const int64_t patternSize = picked->size;
        const int patternLen = int(strlen(picked->pattern));
        const int64_t stepSize = patternSize / int64_t(patternLen);

        // Pick a single unit from the major step so the whole ruler reads
        // consistently (e.g. "0ms 200ms 400ms" rather than "0s 200ms 400ms").
        struct Unit {
            double scale;
            const char *suffix;
        };
        const Unit units[] = {
            {1e9, "s"}, {1e6, "ms"}, {1e3, "us"}, {1.0, "ns"}};
        const Unit *labelUnit = &units[3];
        for (const auto &cand : units) {
            if (double(patternSize) >= cand.scale) {
                labelUnit = &cand;
                break;
            }
        }
        const double stepInUnit = double(patternSize) / labelUnit->scale;

        int64_t startNs = int64_t(scrollX * nsPerPixel);
        int64_t firstPattern = (startNs / patternSize) * patternSize;
        if (firstPattern > startNs)
            firstPattern -= patternSize;
        int patternIdx = 0;
        for (int64_t ns = firstPattern; ns <= totalNs; ns += stepSize) {
            char ch = picked->pattern[patternIdx % patternLen];
            patternIdx++;
            float x = cellTL.x - scrollX +
                      float(double(ns) / double(totalNs) * virtW);
            if (x < cellTL.x - 10)
                continue;
            if (x > cellTL.x + timelineCellW)
                break;
            // '|' MAJOR full-height + label, ':' MEDIUM mid-height,
            // '.' MINOR short tick at the bottom.
            bool isMajor = ch == '|';
            bool isMedium = ch == ':';
            float tickTop = isMajor    ? cellTL.y
                            : isMedium ? cellTL.y + kRulerH * 0.35f
                                       : cellTL.y + kRulerH * 0.65f;
            ImU32 color = isMajor    ? IM_COL32(110, 110, 130, 255)
                          : isMedium ? IM_COL32(80, 80, 100, 255)
                                     : IM_COL32(60, 60, 80, 255);
            dl->AddLine(ImVec2(x, tickTop), ImVec2(x, cellTL.y + kRulerH),
                        color);
            if (isMajor)
                dl->AddText(ImVec2(x + 3, cellTL.y + 2),
                            IM_COL32(180, 180, 200, 255),
                            FormatNsInUnit(double(ns), labelUnit->scale,
                                           labelUnit->suffix, stepInUnit)
                                .c_str());
        }
        dl->AddLine(ImVec2(cellTL.x, cellTL.y + kRulerH),
                    ImVec2(cellTL.x + timelineCellW, cellTL.y + kRulerH),
                    IM_COL32(70, 70, 90, 255));
        dl->PopClipRect();
    }

    // --- Track rows --------------------------------------------------------
    // Yellow is reserved for whichever side the user actually clicked.
    // The other side gets cyan as a "linked" hint when it
    // cross-references the manual pick.
    const bool manualFrame = app.manualSelect == AppState::ManualSelect::Frame;
    const bool manualTxn =
        app.manualSelect == AppState::ManualSelect::Txn &&
        app.selectedTransactionIdx >= 0 &&
        app.selectedTransactionIdx < static_cast<int>(txns.size());
    const int selTxnFrame =
        manualTxn
            ? static_cast<int>(txns[app.selectedTransactionIdx].frameIndex)
            : -1;

    // virtW for depth assignment uses the same cellW the rows will use
    // (carried over from the ruler row). Slices are point events; their
    // visual width in ns is the pixel width × nsPerPixel, so two events
    // overlap when the next event's start time falls before the prior
    // one's tail. Greedy depth assignment per track.
    const float virtW = timelineCellW * std::max(1.f, app.timelineZoom);
    const double nsPerPx = double(totalNs) / double(virtW);

    // Sub-row height scales with zoom: full kRowH when zoomed in enough
    // to read labels (visible <= 500ms), shrinking down to a thin band
    // when way zoomed out — keeps tall stacked tracks from dominating
    // the screen at trace-wide scales.
    const double visibleNsForH =
        double(totalNs) * double(timelineCellW) / double(virtW);
    constexpr float kSubRowMin = 4.f;
    constexpr double kFullAtNs = 500.0e6;
    constexpr double kMinAtNs = 5.0e9;
    const double tShrink = std::clamp(
        (visibleNsForH - kFullAtNs) / (kMinAtNs - kFullAtNs), 0.0, 1.0);
    const float subRowH = static_cast<float>(
        double(kRowH) + tShrink * (kSubRowMin - double(kRowH)));

    // Viewport time range (with a generous pixel margin so partial
    // slices at the edges still get drawn). Events outside this range
    // are skipped entirely — no label format, no draw.
    const float scrollXForCull = app.timelineScrollX;
    const int64_t cullMarginNs = static_cast<int64_t>(120.0 * nsPerPx);
    const int64_t viewportStartNs =
        t0Ns + static_cast<int64_t>(double(scrollXForCull) * nsPerPx) -
        cullMarginNs;
    const int64_t viewportEndNs =
        t0Ns +
        static_cast<int64_t>(double(scrollXForCull + timelineCellW) * nsPerPx) +
        cullMarginNs;

    struct Placement {
        int idx;
        int depth;
    };
    std::vector<Placement> placements;
    std::vector<int64_t> tailNs;

    for (int ti = 0; ti < numTracks; ti++) {
        const auto &tr = tracks[ti];

        // --- Phase 1: depth-stack the slices using a fixed 1ms time
        // width per slice so each event's start time is preserved
        // exactly. Greedy: each slice goes to the lowest sub-row
        // whose tail end is <= this slice's start. Walks ALL slices
        // (not just the viewport) so each slice's depth is a stable,
        // global property.
        placements.clear();
        tailNs.clear();
        int maxDepth = 0;
        constexpr int64_t halfNs = kSliceDurationNs / 2;
        auto place = [&](int idx, int64_t centerNs) {
            int64_t startNs = centerNs - halfNs;
            int64_t endNs = centerNs + halfNs;
            int d = 0;
            while (d < static_cast<int>(tailNs.size()) && tailNs[d] > startNs)
                d++;
            if (d == static_cast<int>(tailNs.size()))
                tailNs.push_back(0);
            tailNs[d] = endNs;
            placements.push_back({idx, d});
            if (d > maxDepth)
                maxDepth = d;
        };
        // Greedy depth assumes time-sorted input. Frames are already
        // sorted (one per vsync); txns aren't guaranteed to be — SF
        // can pack multiple producers' transactions into one trace
        // entry in arrival order, not post order. Sort indices by
        // postTimeNs before placing so unrelated txns don't get pushed
        // onto deeper rows by an earlier-but-later-encountered neighbour.
        if (tr.pid == -1) {
            placements.reserve(n);
            for (int i = 0; i < n; i++)
                place(i, frames[i].tsNs);
        } else {
            std::vector<int> sortedIdx;
            for (int i = 0; i < static_cast<int>(txns.size()); i++) {
                if (txns[i].pid != tr.pid)
                    continue;
                sortedIdx.push_back(i);
            }
            std::sort(sortedIdx.begin(), sortedIdx.end(), [&](int a, int b) {
                return txns[a].postTimeNs < txns[b].postTimeNs;
            });
            for (int idx : sortedIdx)
                place(idx, txns[idx].postTimeNs);
        }

        const float trackH = (maxDepth + 1) * subRowH;
        ImGui::TableNextRow(0, trackH);

        // Col 0: pin checkbox. Frames (pid -1) is always pinned so we
        // skip the checkbox and just show a dim indicator.
        ImGui::TableSetColumnIndex(0);
        if (tr.pid == -1) {
            ImGui::TextDisabled("*");
        } else {
            ImGui::PushID(tr.pid);
            bool pinned = app.pinnedTrackPids.count(tr.pid) > 0;
            if (ImGui::Checkbox("##pin", &pinned)) {
                if (pinned)
                    app.pinnedTrackPids.insert(tr.pid);
                else
                    app.pinnedTrackPids.erase(tr.pid);
            }
            ImGui::PopID();
        }

        // Col 1: pid.
        ImGui::TableSetColumnIndex(1);
        if (tr.pid == -1)
            ImGui::TextUnformatted("");
        else
            ImGui::Text("%d", tr.pid);

        // Col 2: track name. ImGui's own text rendering + cell clip handles
        // truncation at the column border; hover the cell for the full name.
        ImGui::TableSetColumnIndex(2);
        std::string display = (tr.pid == -1) ? std::string("Frames") : tr.name;
        if (display.empty())
            display = "(unknown)";
        ImGui::TextUnformatted(display.c_str());
        if (ImGui::IsItemHovered() && tr.pid != -1)
            ImGui::SetTooltip("pid %d  %s", tr.pid, display.c_str());

        // Col 3: timeline slices for this track.
        ImGui::TableSetColumnIndex(3);
        ImVec2 cellTL = ImGui::GetCursorScreenPos();
        float cellW = ImGui::GetContentRegionAvail().x;
        if (cellW < 1.f)
            continue;
        timelineCellW = std::max(8.f, cellW); // same as ruler's

        ImGui::PushID(ti);
        ImGui::InvisibleButton("##row", ImVec2(timelineCellW, trackH));
        bool rowHovered = ImGui::IsItemHovered();
        anyTimelineHovered = anyTimelineHovered || rowHovered;
        bool rowClicked =
            rowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        ImGui::PopID();

        const float scrollX = app.timelineScrollX; // clamped in ruler cell

        ImDrawList *dl = ImGui::GetWindowDrawList();
        // Alt-row background.
        if (ti & 1)
            dl->AddRectFilled(
                cellTL, ImVec2(cellTL.x + timelineCellW, cellTL.y + trackH),
                IM_COL32(26, 26, 32, 255));
        dl->PushClipRect(
            cellTL, ImVec2(cellTL.x + timelineCellW, cellTL.y + trackH), true);

        // `rowH` is the *sub-row* height — DrawTimelineSlice combines it
        // with each placement's depth to land in the right strip.
        TimelineRow row{dl,    cellTL, timelineCellW, subRowH,    scrollX,
                        virtW, t0Ns,   totalNs,       rowHovered, mouse};

        // --- Phase 2: draw using the precomputed depths --------------------
        // Viewport culling here only — depths are global so panning
        // doesn't reshuffle anyone's layer.
        if (tr.pid == -1) {
            for (const auto &p : placements) {
                const layerviewer::CapturedFrame &f = frames[p.idx];
                if (f.tsNs < viewportStartNs || f.tsNs > viewportEndNs)
                    continue;
                int busy = std::min(255, 60 + f.txnCount * 18);
                ImU32 col = IM_COL32(40, busy, 90, 255);
                std::string label =
                    android::base::StringPrintf("%d (%d)", p.idx, f.txnCount);
                bool isCurrent = manualFrame && p.idx == app.frameIndex;
                bool isLinked = !isCurrent && p.idx == selTxnFrame;
                if (DrawTimelineSlice(row, f.tsNs, col, label, isCurrent,
                                      isLinked, p.depth)) {
                    hoverFrameIdx = p.idx;
                    if (rowClicked)
                        anyTimelineClicked = true;
                }
            }
        } else {
            ImU32 col = PidTrackColor(tr.pid);
            for (const auto &p : placements) {
                const layerviewer::CapturedTransaction &t = txns[p.idx];
                if (t.postTimeNs < viewportStartNs ||
                    t.postTimeNs > viewportEndNs)
                    continue;
                std::string label = android::base::StringPrintf(
                    "%u", static_cast<uint32_t>(t.transactionId));
                bool isCurrent =
                    manualTxn && p.idx == app.selectedTransactionIdx;
                bool isLinked =
                    !isCurrent && manualFrame &&
                    static_cast<int>(t.frameIndex) == app.frameIndex;
                if (DrawTimelineSlice(row, t.postTimeNs, col, label, isCurrent,
                                      isLinked, p.depth)) {
                    hoverTxnIdx = p.idx;
                    if (rowClicked)
                        anyTimelineClicked = true;
                }
            }
        }

        // Current-frame marker line spans the full (possibly stacked) track.
        {
            float x = cellTL.x - scrollX +
                      float(double(frames[app.frameIndex].tsNs - t0Ns) /
                            double(totalNs) * virtW);
            if (x >= cellTL.x && x <= cellTL.x + timelineCellW)
                dl->AddLine(ImVec2(x, cellTL.y), ImVec2(x, cellTL.y + trackH),
                            IM_COL32(255, 200, 60, 140), 1.f);
        }
        dl->PopClipRect();
    }

    ImGui::EndTable();
    ImGui::PopStyleVar();

    // Measurement overlay. Drawn after EndTable so the vertical lines
    // can span every track row (no per-cell clip in scope here).
    if (app.measureStartNs >= 0 && app.measureStartNs != app.measureEndNs) {
        int64_t a = std::min(app.measureStartNs, app.measureEndNs);
        int64_t b = std::max(app.measureStartNs, app.measureEndNs);
        double virtWLocal = double(virtW);
        float xa =
            timelineCellLeft - app.timelineScrollX +
            static_cast<float>(double(a - t0Ns) / double(totalNs) * virtWLocal);
        float xb =
            timelineCellLeft - app.timelineScrollX +
            static_cast<float>(double(b - t0Ns) / double(totalNs) * virtWLocal);
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 wsize = ImGui::GetWindowSize();
        float top = wpos.y;
        float bot = wpos.y + wsize.y;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImU32 line = IM_COL32(255, 230, 120, 230);
        const ImU32 fill = IM_COL32(255, 230, 120, 25);
        // Translucent fill between the two lines makes the range obvious
        // even when both endpoints scroll out together.
        dl->AddRectFilled(ImVec2(xa, top), ImVec2(xb, bot), fill);
        dl->AddLine(ImVec2(xa, top), ImVec2(xa, bot), line, 1.f);
        dl->AddLine(ImVec2(xb, top), ImVec2(xb, bot), line, 1.f);
        std::string label = FormatNs(double(b - a), 1);
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        ImVec2 lp((xa + xb) * 0.5f - ts.x * 0.5f, top + 2.f);
        // Background pill so the label is readable against any track.
        dl->AddRectFilled(ImVec2(lp.x - 4, lp.y - 1),
                          ImVec2(lp.x + ts.x + 4, lp.y + ts.y + 1),
                          IM_COL32(20, 20, 25, 220));
        dl->AddText(lp, IM_COL32(255, 240, 200, 255), label.c_str());
    }

    // --- Input --------------------------------------------------------------
    //  * plain wheel (trackpad pan)             → pan
    //  * shift/ctrl/super + wheel               → zoom around cursor
    //  * WASD (keyboard, while window focused)  → zoom/pan
    //  * left click                             → select slice under cursor
    ImGuiIO &io = ImGui::GetIO();
    app.timelineScrollX = std::clamp(app.timelineScrollX, 0.f,
                                     std::max(0.f, virtW - timelineCellW));
    const float scrollX = app.timelineScrollX;

    // Pixel offset of the mouse into the timeline cell (clamped to its
    // width). Used as the zoom anchor so the virtual-x under the cursor
    // stays put through a zoom.
    const float mouseCellX =
        std::clamp(mouse.x - timelineCellLeft, 0.f, timelineCellW);

    // Wheel. Perfetto uses a log-compressed deltaY with WHEEL_ZOOM_SPEED =
    // -0.02 (see timeline_interactions.ts) so one mouse-wheel notch lands
    // around ~13% zoom-out. ImGui's wheelY is already per-notch (1.0 on a
    // mouse, fractional on a trackpad), so feed |wheelY|*100 through the
    // same log2 to line up with browser deltaY magnitudes.
    // Cap zoom so the visible window never shrinks below 10ms — past
    // that the labels stop being meaningful for SF transactions.
    const float zoomMax =
        std::max(1.f, static_cast<float>(double(totalNs) / 1.0e7));
    if (anyTimelineHovered) {
        float wheelY = io.MouseWheel;
        float wheelX = io.MouseWheelH;
        bool zoomMod = io.KeyShift || io.KeyCtrl || io.KeySuper;
        if (wheelY != 0.f && zoomMod) {
            // ImGui MouseWheel is +ve for scroll-up. Convention: scroll-up
            // (with the zoom modifier) zooms IN, so multiply zoom by a
            // factor > 1 when wheelY > 0.
            float sign = wheelY < 0.f ? -1.f : 1.f;
            float compressed = sign * std::log2(1.f + std::abs(wheelY) * 100.f);
            float factor = 1.f + compressed * 0.02f; // perfetto window scale
            float newZoom = std::clamp(app.timelineZoom * factor, 1.f, zoomMax);
            ZoomTimeline(newZoom, mouseCellX, app.timelineZoom,
                         app.timelineScrollX);
        } else if (wheelY != 0.f) {
            // No modifier → scroll the track list vertically. The window
            // has NoScrollWithMouse set, so we drive scroll manually here.
            // WASD owns horizontal pan, so wheel doesn't pan-the-timeline.
            ImGui::SetScrollY(ImGui::GetScrollY() - wheelY * 60.f);
        }

        if (anyTimelineClicked) {
            // A click on a slice clears any sticky measurement —
            // user has moved on to inspecting something else.
            app.measureStartNs = -1;
            app.measureEndNs = -1;
            if (hoverFrameIdx >= 0) {
                app.frameIndex = hoverFrameIdx;
                app.manualSelect = AppState::ManualSelect::Frame;
            } else if (hoverTxnIdx >= 0) {
                app.selectedTransactionIdx = hoverTxnIdx;
                app.manualSelect = AppState::ManualSelect::Txn;
                if (app.autoSyncTimeline)
                    app.frameIndex =
                        static_cast<int>(txns[hoverTxnIdx].frameIndex);
                RaiseDockedTab("Transaction Inspector");
            }
        }

        // Hover tooltip — full details for whichever slice the cursor is
        // on. Always on (no wait), so you don't need to zoom+squint to
        // read the narrow slice label.
        if (hoverFrameIdx >= 0) {
            const layerviewer::CapturedFrame &hf = frames[hoverFrameIdx];
            ImGui::SetTooltip("Frame #%d\nvsync %lld\nts %.3fs\n"
                              "%d txns  +%d layers  -%d handles\n"
                              "%zu reachable snapshots%s",
                              hoverFrameIdx, (long long)hf.vsyncId,
                              hf.tsNs / 1e9, hf.txnCount, hf.addedCount,
                              hf.destroyedHandleCount, hf.snapshots.size(),
                              hf.displaysChanged ? "\ndisplays changed" : "");
        } else if (hoverTxnIdx >= 0) {
            const layerviewer::CapturedTransaction &ht = txns[hoverTxnIdx];
            std::unordered_map<int32_t, std::string>::const_iterator it =
                app.trace->pidNames.find(ht.pid);
            const char *proc =
                it != app.trace->pidNames.end() ? it->second.c_str() : "?";
            ImGui::SetTooltip(
                "txn #%d  (id=%llu)\npid %d  %s  uid %d\n"
                "postTime %.6fs  vsyncId %lld\n"
                "%d layer change%s  %d display change%s  frame %zu",
                hoverTxnIdx, (unsigned long long)ht.transactionId, ht.pid, proc,
                ht.uid, ht.postTimeNs / 1e9, (long long)ht.vsyncId,
                ht.layerChanges, ht.layerChanges == 1 ? "" : "s",
                ht.displayChanges, ht.displayChanges == 1 ? "" : "s",
                ht.frameIndex);
        }
    }

    // Keyboard nav — ported from perfetto's wasd_navigation_handler.ts.
    // Spring-with-acceleration: while the key is held, `target` grows by
    // at least MIN_PER_FRAME * velocity each frame, `current` chases it
    // with SNAP * (target - current). Releasing the key stops acceleration
    // but the chase keeps finishing, so motion eases out.
    const bool windowFocused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (windowFocused) {
        constexpr float kSnap = 0.4f;
        constexpr float kAccelPerMs = 1.f / 50.f;
        constexpr float kPanPxPerFrame = 8.f;
        constexpr float kZoomRatioPerFrame = 0.008f;
        constexpr float kInitialPanStepPx = 50.f;
        constexpr float kInitialZoomStep = 0.1f;

        int newPanDir = ImGui::IsKeyDown(ImGuiKey_D)   ? 1
                        : ImGui::IsKeyDown(ImGuiKey_A) ? -1
                                                       : 0;
        int newZoomDir = ImGui::IsKeyDown(ImGuiKey_W)   ? 1
                         : ImGui::IsKeyDown(ImGuiKey_S) ? -1
                                                        : 0;

        // Direction changes reset the spring so the first press feels
        // immediate (INITIAL_*_STEP kicks current toward the target
        // straight away on frame 0).
        if (newPanDir != 0 && newPanDir != app.panDir) {
            app.panTarget = float(newPanDir) * kInitialPanStepPx;
            app.panCurrent = 0.f;
            app.panMsElapsed = 0.f;
        }
        if (newZoomDir != 0 && newZoomDir != app.zoomDir) {
            app.zoomTargetRatio = float(newZoomDir) * kInitialZoomStep;
            app.zoomCurrentRatio = 0.f;
            app.zoomMsElapsed = 0.f;
        }
        app.panDir = newPanDir;
        app.zoomDir = newZoomDir;

        const float dtMs = io.DeltaTime * 1000.f;

        // Pan step.
        float panStep = (app.panTarget - app.panCurrent) * kSnap;
        if (app.panDir != 0) {
            app.panMsElapsed += dtMs;
            float v = 1.f + app.panMsElapsed * kAccelPerMs;
            float mag = std::max(kPanPxPerFrame * v, std::abs(panStep));
            app.panTarget += float(app.panDir) * mag;
        }
        app.panCurrent += panStep;
        if (std::abs(panStep) > 0.1f)
            app.timelineScrollX += panStep;
        else if (app.panDir == 0) {
            app.panTarget = 0.f;
            app.panCurrent = 0.f;
        }

        // Zoom step. `step` is a per-frame increment in perfetto's "visible
        // window scale" space; convert it back to our zoom factor (larger
        // zoom = smaller window) by dividing the old zoom by (1 - step).
        float zoomStep = (app.zoomTargetRatio - app.zoomCurrentRatio) * kSnap;
        if (app.zoomDir != 0) {
            app.zoomMsElapsed += dtMs;
            float v = 1.f + app.zoomMsElapsed * kAccelPerMs;
            float mag = std::max(kZoomRatioPerFrame * v, std::abs(zoomStep));
            app.zoomTargetRatio += float(app.zoomDir) * mag;
        }
        app.zoomCurrentRatio += zoomStep;
        if (std::abs(zoomStep) > 1e-6f) {
            const float keyAnchorPx =
                anyTimelineHovered ? mouseCellX : timelineCellW * 0.5f;
            float newZoom =
                std::clamp(app.timelineZoom / (1.f - zoomStep), 1.f, zoomMax);
            ZoomTimeline(newZoom, keyAnchorPx, app.timelineZoom,
                         app.timelineScrollX);
        } else if (app.zoomDir == 0) {
            app.zoomTargetRatio = 0.f;
            app.zoomCurrentRatio = 0.f;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            app.frameIndex = std::max(0, app.frameIndex - 1);
            app.manualSelect = AppState::ManualSelect::Frame;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            app.frameIndex = std::min(n - 1, app.frameIndex + 1);
            app.manualSelect = AppState::ManualSelect::Frame;
        }
    }

    // Capture hover frame index for next-frame info row.
    app.timelineHoverIdx = hoverFrameIdx;
}

// ---------------------------------------------------------------------------
// Preview: offscreen Skia surface → GL texture → ImGui::Image
// ---------------------------------------------------------------------------

// Owns a GL texture + FBO and wraps it as an SkSurface. Recreated on resize;
// the texture id is what we hand to ImGui::Image.
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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texId, 0);
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
            SkRect cell =
                SkRect::MakeXYWH(rect.left() + c * cellPx,
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
    if (ImGui::IsItemActive() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        app.wireframe3D.yawDeg += delta.x * 0.4f;
        app.wireframe3D.pitchDeg += delta.y * 0.4f;
        // Clamp yaw strictly inside (−90°, +90°) so we never rotate past the
        // edge of the stack — looking from behind makes no sense for an
        // exploded layer view and just flips the sort direction.
        app.wireframe3D.yawDeg =
            std::clamp(app.wireframe3D.yawDeg, -85.f, 85.f);
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
        MakeRotation(app.wireframe3D.yawDeg, app.wireframe3D.pitchDeg,
                     cw * 0.5f, ch * 0.5f, czd);
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
        Rotate(R, 0.5f * (b.left + b.right), 0.5f * (b.top + b.bottom), z, rx,
               ry, p.viewZ);
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
        ImU32 stroke = selected
                           ? IM_COL32(255, 215, 50, 255)
                           : (p.l->isVisible ? strokeVisible : strokeHidden);
        dl->AddQuad(p.c[0], p.c[1], p.c[2], p.c[3], stroke,
                    selected ? 2.5f : 1.f);
    }

    if (hovered && hoverId != kUnassignedLayerId) {
        if (const Snap *snap = frame.snapshot(hoverId)) {
            ImGui::BeginTooltip();
            ImGui::Text("#%u %s", hoverId, snap->name.c_str());
            const auto &b = snap->transformedBounds;
            ImGui::Text("bounds: (%.1f, %.1f) → (%.1f, %.1f)", b.left, b.top,
                        b.right, b.bottom);
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
    ImGuiID right = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.28f,
                                                nullptr, &main);

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
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                        SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);

    SDL_Window *window =
        SDL_CreateWindow("layerviewer", 1600, 1000,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                             SDL_WINDOW_HIGH_PIXEL_DENSITY);
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

        SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType,
                                             kPremul_SkAlphaType);
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
                if ((static_cast<int>(x / cell) + static_cast<int>(y / cell)) &
                    1)
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
        font.measureText(label.c_str(), label.size(), SkTextEncoding::kUTF8,
                         &tb);
        c->drawString(label.c_str(), w * 0.5f - tb.centerX(),
                      h * 0.5f - tb.centerY(), font, textPaint);

        SkPixmap pm;
        if (!surface->peekPixels(&pm))
            return;

        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glBindTexture(GL_TEXTURE_2D, glTexId);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pm.addr());
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
            std::fprintf(stderr, "trace load failed: %s\n",
                         app.trace->error.c_str());
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
            app.frameIndex = std::clamp(app.frameIndex, 0,
                                        (int)app.trace->frames.size() - 1);
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
        if ((io.KeyCtrl || io.KeySuper) &&
            ImGui::IsKeyPressed(ImGuiKey_O, false))
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
                ImGui::Text("SF snapshots: %d",
                            app.trace->layerSnapshotPacketCount);
                ImGui::Text("Replay frames: %zu", app.trace->frames.size());
                if (!app.trace->frames.empty()) {
                    const auto &f = app.trace->frames[app.frameIndex];
                    int withBuffer = 0;
                    for (const auto &snap : f.snapshots)
                        if (snap.externalTexture)
                            withBuffer++;
                    ImGui::Separator();
                    ImGui::Text("Current vsync: %lld", (long long)f.vsyncId);
                    ImGui::Text("Display:       %dx%d", f.displayWidth,
                                f.displayHeight);
                    ImGui::Text("Reachable:     %zu layers (%d with buffer)",
                                f.snapshots.size(), withBuffer);
                }
            }
        }
        ImGui::End();

        // HorizontalScrollbar: long layer paths (full FQCN + #id) exceed the
        // panel width easily. Without this flag the WorkRect's right edge is
        // pinned to the visible window, so SpanAvailWidth selections stop
        // short and any text past the edge is simply clipped. With it,
        // WorkRect grows with content, the selection highlight reaches the
        // end of the widest row, and we get a real scrollbar.
        if (ImGui::Begin("Layers", nullptr,
                         ImGuiWindowFlags_HorizontalScrollbar)) {
            if (app.trace && !app.trace->frames.empty())
                DrawLayerTreePane(app.trace->frames[app.frameIndex], app);
        }
        ImGui::End();

        if (ImGui::Begin("Timeline", nullptr,
                         ImGuiWindowFlags_NoScrollWithMouse))
            DrawTimeline(app);
        ImGui::End();

        if (ImGui::Begin("Transactions"))
            DrawTransactions(app);
        ImGui::End();

        if (ImGui::Begin("Preview")) {
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
                // Compose this frame through CE + SkiaRenderEngine. Returns the
                // GL texture name of the composed output buffer (0 on failure).
                unsigned int composedTex =
                    compositor.composeFrame(app.trace->frames[app.frameIndex]);

                // RE's own Ganesh context touched a bunch of GL state — restore
                // the default framebuffer + viewport so ImGui renders into the
                // window.
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

                // Aspect-preserving fit of the composed display rect into the
                // window.
                float rw = static_cast<float>(
                    app.trace->frames[app.frameIndex].displayWidth);
                float rh = static_cast<float>(
                    app.trace->frames[app.frameIndex].displayHeight);
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
                    dl->AddImage(static_cast<ImTextureID>(composedTex), imgMin,
                                 imgMax);
                } else {
                    dl->AddText(ImVec2(winMin.x + 12, winMin.y + 12),
                                IM_COL32(200, 200, 220, 220),
                                "(no display info in this frame)");
                }
                dl->AddRect(imgMin, imgMax, IM_COL32(160, 160, 200, 220), 0.f,
                            0, 1.f);
                dl->PopClipRect();

                // Plain wheel pans (matches macOS two-finger scroll);
                // ⌘/Ctrl+wheel zooms around the cursor; dragging pans.
                if (ImGui::IsItemHovered()) {
                    float wheelY = io.MouseWheel;
                    float wheelX = io.MouseWheelH;
                    bool zoomMod = io.KeyCtrl || io.KeySuper;
                    if (wheelY != 0.f && zoomMod) {
                        float oldZoom = app.preview.zoom;
                        float newZoom = std::clamp(
                            oldZoom * std::pow(1.15f, wheelY), 0.1f, 30.f);
                        float ratio = newZoom / oldZoom;
                        ImVec2 m = ImGui::GetMousePos();
                        app.preview.panX = (m.x - winCx) * (1.f - ratio) +
                                           app.preview.panX * ratio;
                        app.preview.panY = (m.y - winCy) * (1.f - ratio) +
                                           app.preview.panY * ratio;
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
