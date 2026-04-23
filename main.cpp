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
#include "include/core/SkSurface.h"
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

struct AppState {
  std::unique_ptr<layerviewer::ReplayedTrace> trace;
  int frameIndex = 0;
  uint32_t selectedLayerId = kUnassignedLayerId;
  bool showInvisible = false;
  bool resetLayoutOnce = true;     // first-run default layout
  bool requestResetLayout = false; // View → Reset Layout
};

// Skia draws into the default framebuffer (FBO 0). ImGui draws on top in the
// same frame.
sk_sp<SkSurface> MakeWindowSurface(GrDirectContext *ctx, int w, int h) {
  GrGLFramebufferInfo fb;
  fb.fFBOID = 0;
  fb.fFormat = GL_RGBA8;
  GrBackendRenderTarget target = GrBackendRenderTargets::MakeGL(w, h, 0, 8, fb);
  return SkSurfaces::WrapBackendRenderTarget(
      ctx, target, kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
      SkColorSpace::MakeSRGB(), nullptr);
}

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

void DrawLayerTreeNode(const layerviewer::CapturedFrame &frame, uint32_t id,
                       AppState &app) {
  auto it = frame.layersById.find(id);
  if (it == frame.layersById.end())
    return;
  const auto &layer = it->second;

  ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
  if (layer.childIds.empty())
    flags |= ImGuiTreeNodeFlags_Leaf;
  if (app.selectedLayerId == layer.id)
    flags |= ImGuiTreeNodeFlags_Selected;
  if (!layer.isVisible)
    ImGui::PushStyleColor(ImGuiCol_Text, 0xff808080);

  ImGui::PushID(static_cast<int>(layer.id));
  bool open =
      ImGui::TreeNodeEx("##n", flags, "#%u %s", layer.id, layer.name.c_str());
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    app.selectedLayerId = layer.id;
  if (open) {
    for (uint32_t childId : layer.childIds)
      DrawLayerTreeNode(frame, childId, app);
    ImGui::TreePop();
  }
  ImGui::PopID();

  if (!layer.isVisible)
    ImGui::PopStyleColor();
}

void DrawLayerTreePane(const layerviewer::CapturedFrame &frame, AppState &app) {
  if (frame.layersById.empty()) {
    ImGui::TextUnformatted("(no layers in this frame)");
    return;
  }
  for (uint32_t rootId : frame.rootIds)
    DrawLayerTreeNode(frame, rootId, app);
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
// Preview canvas (Skia)
// ---------------------------------------------------------------------------

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

  SkPaint border;
  border.setStyle(SkPaint::kStroke_Style);
  border.setAntiAlias(true);
  border.setStrokeWidth(1.5f);
  border.setColor(SkColorSetARGB(180, 120, 120, 140));
  canvas->drawRect(SkRect::MakeXYWH(t.tx, t.ty, cw * t.scale, ch * t.scale),
                   border);

  // Gather drawable layers in paint order (ascending z).
  std::vector<const layerviewer::CapturedLayer *> layers;
  layers.reserve(frame.layersById.size());
  for (const auto &[_, l] : frame.layersById) {
    if (!app.showInvisible && !l.isVisible)
      continue;
    layers.push_back(&l);
  }
  std::sort(layers.begin(), layers.end(),
            [](const auto *a, const auto *b) { return a->z < b->z; });

  SkPaint fill, stroke;
  fill.setStyle(SkPaint::kFill_Style);
  fill.setAntiAlias(true);
  stroke.setStyle(SkPaint::kStroke_Style);
  stroke.setAntiAlias(true);

  for (const auto *l : layers) {
    const auto &b = l->geomLayerBounds;
    // Only draw layers whose bounds actually fit into the display rect —
    // avoids the ±10800 layerspace-root bounds spanning the whole preview.
    if (b.left < 0 || b.top < 0 || b.right > cw || b.bottom > ch)
      continue;
    if (b.right <= b.left || b.bottom <= b.top)
      continue;

    SkRect r =
        SkRect::MakeLTRB(t.tx + b.left * t.scale, t.ty + b.top * t.scale,
                         t.tx + b.right * t.scale, t.ty + b.bottom * t.scale);
    SkColor4f c = {l->colorR >= 0 ? l->colorR : 0.4f,
                   l->colorG >= 0 ? l->colorG : 0.6f,
                   l->colorB >= 0 ? l->colorB : 0.95f,
                   std::min(0.25f, std::max(0.04f, l->alpha * 0.2f))};
    fill.setColor4f(c, nullptr);
    canvas->drawRect(r, fill);

    SkColor4f sc = {c.fR, c.fG, c.fB, 0.7f};
    stroke.setStrokeWidth(1.f);
    if (app.selectedLayerId == l->id) {
      sc = {1.f, 0.85f, 0.2f, 1.f};
      stroke.setStrokeWidth(2.5f);
    }
    stroke.setColor4f(sc, nullptr);
    canvas->drawRect(r, stroke);
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
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();
  ImGui_ImplSDL3_InitForOpenGL(window, glCtx);
  ImGui_ImplOpenGL3_Init("#version 150");

  if (!initialTrace.empty()) {
    app.trace = layerviewer::LoadAndReplay(initialTrace);
    if (!app.trace->error.empty())
      std::fprintf(stderr, "trace load failed: %s\n", app.trace->error.c_str());
  }

  sk_sp<SkSurface> surface;
  int surfW = 0, surfH = 0;

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
    if (fbW != surfW || fbH != surfH || !surface) {
      surface = MakeWindowSurface(grCtx.get(), fbW, fbH);
      surfW = fbW;
      surfH = fbH;
    }

    // Clear the backing framebuffer — the Preview window draws into Skia
    // later and we composite ImGui on top in the same GL context.
    SkCanvas *canvas = surface->getCanvas();
    canvas->clear(SkColorSetARGB(255, 24, 24, 28));
    if (app.trace && !app.trace->frames.empty()) {
      app.frameIndex =
          std::clamp(app.frameIndex, 0, (int)app.trace->frames.size() - 1);
      DrawPreviewCanvas(canvas, fbW, fbH, app.trace->frames[app.frameIndex],
                        app);
    }
    grCtx->flushAndSubmit();
    grCtx->resetContext();

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
      ImGui::TextUnformatted("Preview is drawn to the main framebuffer "
                             "behind this window — dock/float as needed.");
    }
    ImGui::End();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  surface.reset();
  grCtx.reset();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(glCtx);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
