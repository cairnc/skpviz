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
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h" // DockBuilder

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"

#include "layer_trace.h"

namespace {

constexpr uint32_t kUnassignedLayerId = 0xffffffffu;

struct View3D {
  float yawDeg = 25.f;        // rotation around vertical screen axis
  float pitchDeg = 10.f;      // rotation around horizontal screen axis
  float depthSpacing = 200.f; // per-layer Z step in device pixels
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
  bool resetLayoutOnce = true;     // first-run default layout
  bool requestResetLayout = false; // View → Reset Layout
  View3D wireframe3D;
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
// Layer tree
// ---------------------------------------------------------------------------

// Recursively draw one node, and push visible rows into `flat` in the order
// they appear — used for up/down arrow key navigation.
void DrawLayerTreeNode(const layerviewer::CapturedFrame &frame, uint32_t id,
                       AppState &app, std::vector<uint32_t> &flat) {
  auto it = frame.layersById.find(id);
  if (it == frame.layersById.end())
    return;
  const auto &layer = it->second;

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_SpanFullWidth |
                             ImGuiTreeNodeFlags_DefaultOpen;
  if (layer.childIds.empty())
    flags |= ImGuiTreeNodeFlags_Leaf;
  const bool selected = app.selectedLayerId == layer.id;
  if (selected)
    flags |= ImGuiTreeNodeFlags_Selected;

  ImGui::PushID(static_cast<int>(layer.id));
  // Selection came from outside the tree (3D wireframe click, arrow-key
  // nav etc.): force this branch open so the row is reachable, scroll to
  // it, consume the flag.
  if (selected && app.scrollTreeToSelection)
    ImGui::SetNextItemOpen(true);
  bool open =
      ImGui::TreeNodeEx("##n", flags, "#%u %s", layer.id, layer.name.c_str());
  if (selected && app.scrollTreeToSelection) {
    ImGui::SetScrollHereY(0.5f);
    app.scrollTreeToSelection = false;
  }
  flat.push_back(layer.id);
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    app.selectedLayerId = layer.id;
  if (open) {
    for (uint32_t childId : layer.childIds)
      DrawLayerTreeNode(frame, childId, app, flat);
    ImGui::TreePop();
  }
  ImGui::PopID();
}

void DrawLayerTreePane(const layerviewer::CapturedFrame &frame, AppState &app) {
  if (frame.layersById.empty()) {
    ImGui::TextUnformatted("(no layers in this frame)");
    return;
  }
  std::vector<uint32_t> visible;
  visible.reserve(frame.layersById.size());
  for (uint32_t rootId : frame.rootIds)
    DrawLayerTreeNode(frame, rootId, app, visible);

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
  auto it = frame.layersById.find(app.selectedLayerId);
  if (it == frame.layersById.end()) {
    ImGui::Text("Layer #%u not in this frame.", app.selectedLayerId);
    return;
  }
  const auto &l = it->second;

  if (ImGui::BeginTable("inspector", 2,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_RowBg)) {
    auto row = [](const char *k, const std::string &v) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(k);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(v.c_str());
    };
    auto rowf = [](const char *k, const char *fmt, auto v) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(k);
      ImGui::TableSetColumnIndex(1);
      ImGui::Text(fmt, v);
    };

    row("name", l.name);
    rowf("id", "%u", l.id);
    rowf("parent", "%u", l.parentId);
    rowf("globalZ", "%d", l.z);
    rowf("layerStack", "%u", l.layerStack);
    rowf("visible", "%s", l.isVisible ? "yes" : "no");
    rowf("hiddenByPolicy", "%s", l.isHiddenByPolicy ? "yes" : "no");
    rowf("contentOpaque", "%s", l.contentOpaque ? "yes" : "no");
    rowf("isOpaque", "%s", l.isOpaque ? "yes" : "no");
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("bounds");
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("(%.1f, %.1f) → (%.1f, %.1f)", l.geomLayerBounds.left,
                l.geomLayerBounds.top, l.geomLayerBounds.right,
                l.geomLayerBounds.bottom);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("color");
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("r=%.2f g=%.2f b=%.2f a=%.2f", l.colorR, l.colorG, l.colorB,
                l.colorA);
    rowf("alpha", "%.2f", l.alpha);
    if (l.hasBuffer)
      rowf("bufferFrame", "%llu", (unsigned long long)l.bufferFrame);
    ImGui::EndTable();
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

  ImGui::Text("Frame %d / %d   vsync=%lld   ts=%.3fs", app.frameIndex + 1, n,
              (long long)frames[app.frameIndex].vsyncId,
              frames[app.frameIndex].tsNs / 1e9);
  ImGui::SameLine();
  ImGui::Checkbox("show invisible", &app.showInvisible);
  ImGui::SameLine();
  if (ImGui::Button("<"))
    app.frameIndex = std::max(0, app.frameIndex - 1);
  ImGui::SameLine();
  if (ImGui::Button(">"))
    app.frameIndex = std::min(n - 1, app.frameIndex + 1);

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
      char buf[32];
      snprintf(buf, sizeof buf, "%.0fms", ms);
      dl->AddText(ImVec2(x + 2, origin.y + 2), IM_COL32(160, 160, 180, 255),
                  buf);
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
    ImGui::Text("layers    %zu reachable", hf.layersById.size());
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
  constexpr float kPi = 3.14159265f;
  const float kDepthSpacing = app.wireframe3D.depthSpacing;

  ImGui::TextDisabled("click and drag to rotate");
  ImGui::SameLine();
  ImGui::Checkbox("show all layers", &app.showInvisible);
  ImGui::SameLine();
  ImGui::PushItemWidth(160);
  ImGui::SliderFloat("spacing", &app.wireframe3D.depthSpacing, 0.f, 800.f,
                     "%.0fpx");
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

  // Filter drawable layers — same rule as the Preview.
  std::vector<const layerviewer::CapturedLayer *> layers;
  layers.reserve(frame.layersById.size());
  for (const auto &[_, l] : frame.layersById) {
    if (!app.showInvisible && !l.isVisible)
      continue;
    const auto &b = l.geomLayerBounds;
    if (b.right <= b.left || b.bottom <= b.top)
      continue;
    if (b.left < 0 || b.top < 0 || b.right > cw || b.bottom > ch)
      continue;
    layers.push_back(&l);
  }
  std::sort(layers.begin(), layers.end(),
            [](const auto *a, const auto *b) { return a->z < b->z; });

  // 3D transform: rotate yaw around Y, then pitch around X, then ortho.
  // The rotation is around the *center* of the layer stack so the view
  // pivots in place when you drag.
  const float cy_ = std::cos(app.wireframe3D.yawDeg * kPi / 180.f);
  const float sy_ = std::sin(app.wireframe3D.yawDeg * kPi / 180.f);
  const float cp_ = std::cos(app.wireframe3D.pitchDeg * kPi / 180.f);
  const float sp_ = std::sin(app.wireframe3D.pitchDeg * kPi / 180.f);
  const float cxd = cw * 0.5f;
  const float cyd = ch * 0.5f;
  const float czd =
      0.5f * std::max<int>(0, (int)layers.size() - 1) * kDepthSpacing;
  // Returns the post-rotation (X, Y, Z) — we use XY for screen, Z for depth
  // sort and hit-testing order.
  auto rotate = [&](float x, float y, float z, float &ox, float &oy,
                    float &oz) {
    float X = (x - cxd) * cy_ - (z - czd) * sy_;
    float Y = (y - cyd);
    float Z = (x - cxd) * sy_ + (z - czd) * cy_;
    float Y2 = Y * cp_ - Z * sp_;
    float Z2 = Y * sp_ + Z * cp_;
    ox = X;
    oy = Y2;
    oz = Z2;
  };

  // Fixed scale: fit the *unrotated* device rect into the window. Scale
  // stays constant regardless of yaw/pitch so rotation doesn't zoom in or
  // out — rotated corners may extend past the window, that's fine.
  const float pad = 80.f;
  float scale = std::min((avail.x - 2 * pad) / cw, (avail.y - 2 * pad) / ch);
  scale = std::max(0.01f, scale);
  float screenCx = origin.x + avail.x * 0.5f;
  float screenCy = origin.y + avail.y * 0.5f;
  auto project = [&](float wx, float wy, float wz) {
    float rx, ry, rz;
    rotate(wx, wy, wz, rx, ry, rz);
    return ImVec2(screenCx + rx * scale, screenCy + ry * scale);
  };

  // Device-rect (at z=0) as a reference plane.
  {
    ImVec2 p00 = project(0.f, 0.f, 0.f);
    ImVec2 p10 = project(cw, 0.f, 0.f);
    ImVec2 p11 = project(cw, ch, 0.f);
    ImVec2 p01 = project(0.f, ch, 0.f);
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
    const layerviewer::CapturedLayer *l;
    int rank; // index in z-ascending `layers`: 0 = bottom of stack.
    ImVec2 c[4];
    float viewZ; // larger = closer to viewer (see View3D comment).
  };
  std::vector<Proj> projs;
  projs.reserve(layers.size());
  for (size_t k = 0; k < layers.size(); k++) {
    const auto &b = layers[k]->geomLayerBounds;
    float z = static_cast<float>(k) * kDepthSpacing;
    Proj p;
    p.l = layers[k];
    p.rank = static_cast<int>(k);
    p.c[0] = project(b.left, b.top, z);
    p.c[1] = project(b.right, b.top, z);
    p.c[2] = project(b.right, b.bottom, z);
    p.c[3] = project(b.left, b.bottom, z);
    float rx, ry;
    rotate(0.5f * (b.left + b.right), 0.5f * (b.top + b.bottom), z, rx, ry,
           p.viewZ);
    projs.push_back(p);
  }
  // Sort by RANK, not by post-rotation viewZ of the center. Center-based
  // sort wobbled when a layer's (x, y) contribution to viewZ swamped the
  // rank delta for adjacent layers. Yaw is clamped to ±85° so the stack
  // direction is stable — largest rank first (farthest back in painter's
  // sense), rank 0 drawn last (visually on top in overlap). Hit-testing
  // iterates projs in reverse so the frontmost drawn quad wins.
  std::sort(projs.begin(), projs.end(),
            [](const Proj &a, const Proj &b) { return a.rank > b.rank; });

  // Point-in-convex-quad via cross-product signs.
  auto pointInQuad = [](const ImVec2 &m, const ImVec2 &a, const ImVec2 &b,
                        const ImVec2 &c, const ImVec2 &d) {
    auto cross = [](ImVec2 p, ImVec2 q, ImVec2 r) {
      return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    float s1 = cross(a, b, m), s2 = cross(b, c, m);
    float s3 = cross(c, d, m), s4 = cross(d, a, m);
    return (s1 >= 0 && s2 >= 0 && s3 >= 0 && s4 >= 0) ||
           (s1 <= 0 && s2 <= 0 && s3 <= 0 && s4 <= 0);
  };

  // Hit-test first, in near→far order (projs is far→near, so iterate back).
  uint32_t hoverId = kUnassignedLayerId;
  ImVec2 mouse = ImGui::GetMousePos();
  if (hovered) {
    for (auto it = projs.rbegin(); it != projs.rend(); ++it) {
      if (pointInQuad(mouse, it->c[0], it->c[1], it->c[2], it->c[3])) {
        hoverId = it->l->id;
        break;
      }
    }
  }

  // Deterministic distinct color per layer id — golden-ratio hue spacing so
  // adjacent ids never land on similar hues. Two pre-baked complementary
  // tints per layer: the base and a deeper shade used for the checkerboard.
  auto hsvToRgb = [](float h, float s, float v, float &r, float &g, float &b) {
    h = h - std::floor(h);
    float i = std::floor(h * 6.f);
    float f = h * 6.f - i;
    float p = v * (1.f - s);
    float q = v * (1.f - s * f);
    float t = v * (1.f - s * (1.f - f));
    switch (static_cast<int>(i) % 6) {
    case 0:
      r = v;
      g = t;
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
      b = t;
      return;
    case 3:
      r = p;
      g = q;
      b = v;
      return;
    case 4:
      r = t;
      g = p;
      b = v;
      return;
    case 5:
      r = v;
      g = p;
      b = q;
      return;
    }
  };
  auto colorsFor = [&](uint32_t id, ImU32 &light, ImU32 &dark) {
    float hue = static_cast<float>(id) * 0.61803398875f; // golden ratio
    float r, g, b;
    hsvToRgb(hue, 0.55f, 0.92f, r, g, b);
    light = IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255),
                     static_cast<int>(b * 255), 200);
    hsvToRgb(hue, 0.75f, 0.55f, r, g, b);
    dark = IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255),
                    static_cast<int>(b * 255), 200);
  };

  // Low-freq checkerboard — big cells (128 device-space pixels) so overlapping
  // layers with similar colors can still be told apart by the checker
  // phase. We project each cell corner into screen space so the checker
  // rotates with the layer.
  const float kCheckerCell = 128.f;

  // Draw far → near.
  for (const auto &p : projs) {
    bool selected = app.selectedLayerId == p.l->id;
    ImU32 light, dark;
    colorsFor(p.l->id, light, dark);

    // Fill base (light color), then overlay dark-tinted cells to build the
    // checkerboard directly as rotated sub-quads.
    dl->AddQuadFilled(p.c[0], p.c[1], p.c[2], p.c[3], light);
    const auto &b = p.l->geomLayerBounds;
    float z = static_cast<float>(p.rank) * kDepthSpacing;
    for (float cy = std::floor(b.top / kCheckerCell) * kCheckerCell;
         cy < b.bottom; cy += kCheckerCell) {
      for (float cx = std::floor(b.left / kCheckerCell) * kCheckerCell;
           cx < b.right; cx += kCheckerCell) {
        int ix = static_cast<int>(std::floor(cx / kCheckerCell));
        int iy = static_cast<int>(std::floor(cy / kCheckerCell));
        if (((ix + iy) & 1) == 0)
          continue; // only dark cells overlaid
        float x0 = std::max(cx, b.left);
        float y0 = std::max(cy, b.top);
        float x1 = std::min(cx + kCheckerCell, b.right);
        float y1 = std::min(cy + kCheckerCell, b.bottom);
        ImVec2 q0 = project(x0, y0, z);
        ImVec2 q1 = project(x1, y0, z);
        ImVec2 q2 = project(x1, y1, z);
        ImVec2 q3 = project(x0, y1, z);
        dl->AddQuadFilled(q0, q1, q2, q3, dark);
      }
    }

    ImU32 stroke =
        selected ? IM_COL32(255, 215, 50, 255) : IM_COL32(40, 40, 50, 220);
    dl->AddQuad(p.c[0], p.c[1], p.c[2], p.c[3], stroke, selected ? 2.5f : 1.f);

    // Big centered buffer-id watermark so overlapping layers can still be
    // told apart by their underlying GraphicBuffer. Fall back to layer id
    // when the layer has no buffer (container / effect layers).
    ImVec2 center =
        project(0.5f * (b.left + b.right), 0.5f * (b.top + b.bottom), z);
    char buf[32];
    if (p.l->bufferId)
      snprintf(buf, sizeof buf, "%llu",
               static_cast<unsigned long long>(p.l->bufferId));
    else
      snprintf(buf, sizeof buf, "#%u", p.l->id);
    // Scale the watermark text with the projected quad height so small
    // layers don't get a giant label.
    float px0 = std::min({p.c[0].x, p.c[1].x, p.c[2].x, p.c[3].x});
    float py0 = std::min({p.c[0].y, p.c[1].y, p.c[2].y, p.c[3].y});
    float px1 = std::max({p.c[0].x, p.c[1].x, p.c[2].x, p.c[3].x});
    float py1 = std::max({p.c[0].y, p.c[1].y, p.c[2].y, p.c[3].y});
    float size = std::clamp(0.35f * std::min(px1 - px0, py1 - py0), 10.f, 64.f);
    ImFont *font = ImGui::GetFont();
    ImVec2 textSz = font->CalcTextSizeA(size, FLT_MAX, 0.f, buf);
    dl->AddText(font, size,
                ImVec2(center.x - textSz.x * 0.5f, center.y - textSz.y * 0.5f),
                IM_COL32(15, 15, 20, 230), buf);
  }

  if (hovered && hoverId != kUnassignedLayerId) {
    auto it = frame.layersById.find(hoverId);
    if (it != frame.layersById.end()) {
      ImGui::BeginTooltip();
      ImGui::Text("#%u %s", it->second.id, it->second.name.c_str());
      const auto &b = it->second.geomLayerBounds;
      ImGui::Text("bounds: (%.1f, %.1f) → (%.1f, %.1f)", b.left, b.top, b.right,
                  b.bottom);
      ImGui::Text("z=%d  alpha=%.2f", it->second.z, it->second.alpha);
      ImGui::EndTooltip();
    }
    if (ImGui::IsItemClicked()) {
      app.selectedLayerId = hoverId;
      app.scrollTreeToSelection = true;
    }
  }
}

void DrawPreviewCanvas(SkCanvas *canvas, int fbW, int fbH,
                       const layerviewer::CapturedFrame &frame,
                       const AppState &app) {
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

  // Gather drawable layers in paint order (ascending z). Filter to ones
  // whose bounds actually sit within the display rect — the ±10800
  // layer-space root bounds otherwise swamp every child.
  std::vector<const layerviewer::CapturedLayer *> layers;
  layers.reserve(frame.layersById.size());
  for (const auto &[_, l] : frame.layersById) {
    if (!app.showInvisible && !l.isVisible)
      continue;
    const auto &b = l.geomLayerBounds;
    if (b.right <= b.left || b.bottom <= b.top)
      continue;
    if (b.left < 0 || b.top < 0 || b.right > cw || b.bottom > ch)
      continue;
    layers.push_back(&l);
  }
  std::sort(layers.begin(), layers.end(),
            [](const auto *a, const auto *b) { return a->z < b->z; });

  SkPaint fill, stroke, textP;
  fill.setStyle(SkPaint::kFill_Style);
  fill.setAntiAlias(true);
  stroke.setStyle(SkPaint::kStroke_Style);
  stroke.setAntiAlias(true);
  textP.setAntiAlias(true);
  SkFont font(nullptr, 11.f);
  font.setEdging(SkFont::Edging::kAntiAlias);

  for (const auto *l : layers) {
    const auto &b = l->geomLayerBounds;
    SkRect r =
        SkRect::MakeLTRB(t.tx + b.left * t.scale, t.ty + b.top * t.scale,
                         t.tx + b.right * t.scale, t.ty + b.bottom * t.scale);

    // Faint fill tinted by the layer's requested color (clamped — negative
    // values are the "no color fill" sentinel).
    SkColor4f c = {l->colorR >= 0 ? l->colorR : 0.5f,
                   l->colorG >= 0 ? l->colorG : 0.6f,
                   l->colorB >= 0 ? l->colorB : 0.95f, 0.10f};
    fill.setColor4f(c, nullptr);
    canvas->drawRect(r, fill);

    SkColor4f sc = {c.fR, c.fG, c.fB, 0.55f};
    stroke.setStrokeWidth(1.f);
    if (app.selectedLayerId == l->id) {
      sc = {1.f, 0.85f, 0.2f, 1.f};
      stroke.setStrokeWidth(2.5f);
    }
    stroke.setColor4f(sc, nullptr);
    canvas->drawRect(r, stroke);

    // Layer name inside the rect. Fade unless selected. Only show if the
    // rect is tall enough — avoids text exploding over tiny strips.
    if (r.height() >= 16.f && r.width() >= 40.f) {
      SkColor4f tc = app.selectedLayerId == l->id
                         ? SkColor4f{1.f, 0.95f, 0.6f, 1.f}
                         : SkColor4f{1.f, 1.f, 1.f, 0.65f};
      textP.setColor4f(tc, nullptr);
      std::string label = "#" + std::to_string(l->id) + " " + l->name;
      canvas->save();
      canvas->clipRect(r);
      canvas->drawString(label.c_str(), r.left() + 4.f, r.top() + 13.f, font,
                         textP);
      canvas->restore();
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
  ImGuiID right =
      ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.28f, nullptr, &main);

  ImGui::DockBuilderDockWindow("Timeline", bottom);
  ImGui::DockBuilderDockWindow("Layers", left);
  ImGui::DockBuilderDockWindow("Inspector", right);
  ImGui::DockBuilderDockWindow("Trace Info", right);
  ImGui::DockBuilderDockWindow("Preview", main);
  ImGui::DockBuilderDockWindow("Wireframe", main); // tabbed with Preview
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
  sk_sp<GrDirectContext> grCtx = GrDirectContexts::MakeGL(glInterface);
  if (!grCtx) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "layerviewer",
                             "GrDirectContexts::MakeGL failed", window);
    return 1;
  }

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

  PreviewTarget preview;

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
          ImGui::Separator();
          ImGui::Text("Current vsync: %lld", (long long)f.vsyncId);
          ImGui::Text("Display:       %dx%d", f.displayWidth, f.displayHeight);
          ImGui::Text("Reachable:     %zu layers", f.layersById.size());
        }
      }
    }
    ImGui::End();

    if (ImGui::Begin("Layers")) {
      if (app.trace && !app.trace->frames.empty())
        DrawLayerTreePane(app.trace->frames[app.frameIndex], app);
    }
    ImGui::End();

    if (ImGui::Begin("Inspector")) {
      if (app.trace && !app.trace->frames.empty())
        DrawInspector(app.trace->frames[app.frameIndex], app);
    }
    ImGui::End();

    if (ImGui::Begin("Timeline"))
      DrawTimeline(app);
    ImGui::End();

    if (ImGui::Begin("Preview")) {
      ImVec2 avail = ImGui::GetContentRegionAvail();
      int pxW = static_cast<int>(avail.x * io.DisplayFramebufferScale.x);
      int pxH = static_cast<int>(avail.y * io.DisplayFramebufferScale.y);
      if (pxW > 0 && pxH > 0 && app.trace && !app.trace->frames.empty() &&
          preview.ensure(grCtx.get(), pxW, pxH)) {
        SkCanvas *canvas = preview.surface->getCanvas();
        DrawPreviewCanvas(canvas, pxW, pxH, app.trace->frames[app.frameIndex],
                          app);
        grCtx->flushAndSubmit();
        grCtx->resetContext();
        // Rebind the window framebuffer — ImGui draws into FBO 0.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fbW, fbH);

        ImGui::Image(static_cast<ImTextureID>(preview.texId), avail);
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

  preview.destroy();
  grCtx.reset();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(glCtx);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
