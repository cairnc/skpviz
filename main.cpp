#define GL_SILENCE_DEPRECATION
#include "gl_extras.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_opengl.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "skia_backend.h"

#include <cstdarg>
#include <cstdio>
#include <memory>

struct Frame
{
    PictureFrame pic;
    std::unique_ptr<DebugCanvas> debug;
    int selected_cmd = -1;
    // Per-command flag: true if this Save/SaveLayer is folded in the UI.
    // Index matches CommandInfo index; non-Save entries are always false.
    std::vector<bool> collapsed;
    // Dense list of currently-visible command indices, recomputed each
    // frame from `collapsed`. Ordered; monotonically increasing.
    std::vector<int> visible_cmds;
};

static struct AppState
{
    SDL_Window *window = nullptr;
    SDL_GLContext gl_ctx = nullptr;
    bool should_quit = false;
    SkiaBackend skia;
    std::vector<Frame> frames;
    std::string base_name;
    int current_frame = 0;
    int rendered_frame = -1;
    ImageViewer viewport_viewer;

    int rendered_cmd = -2;
    uint32_t selected_shader_id = 0; // 0 = none (uniqueID is never 0)
    SkM44 current_matrix;

    bool scroll_to_cmd = false;
    bool rendered_clip = false;
    bool show_commands = true;
    bool show_detail = true;
    bool show_resources = true;
    bool reset_layout = false;
    bool show_clip = false;
    bool indent_save_restore = true;
    bool hide_annotations = false;
    bool highlight_geometry = false;
    bool rendered_highlight = false;

    Frame *cur()
    {
        return frames.empty() ? nullptr : &frames[current_frame];
    }
} g;

// SDL3 file dialogs are async: they invoke the callback on the main thread
// after the user dismisses the dialog. We stash a "what to do on complete"
// function so each caller can supply its own behavior.
using DialogCallback = std::function<void(const char *path)>;
static DialogCallback g_pending_dialog;

static void SDLCALL DialogResultCallback(void *, const char *const *files,
                                         int /*filter*/)
{
    if (!g_pending_dialog)
        return;
    const char *path = (files && files[0]) ? files[0] : nullptr;
    g_pending_dialog(path);
    g_pending_dialog = nullptr;
}

static const SDL_DialogFileFilter kSkpFilters[] = {
    {"SKP / MSKP files", "skp;mskp"},
    {"All files", "*"},
};
static const SDL_DialogFileFilter kPngFilters[] = {{"PNG files", "png"}};
static const SDL_DialogFileFilter kTxtFilters[] = {{"Text files", "txt"}};

static std::string g_error_message;
static bool g_open_error_popup = false;

#ifdef _MSC_VER
static void ShowError(const char *fmt, ...);
#else
static void ShowError(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
#endif
static void ShowError(const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_error_message = buf;
    g_open_error_popup = true;
}

static void DrawErrorPopup()
{
    if (g_open_error_popup)
    {
        ImGui::OpenPopup("Error");
        g_open_error_popup = false;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Error", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("%s", g_error_message.c_str());
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static ImageViewer *ActiveViewer()
{
    return &g.viewport_viewer;
}

static void SetCurrentFrame(int idx)
{
    if (g.frames.empty())
        return;
    g.current_frame = idx;
    g.rendered_cmd = -2;
    g.rendered_frame = -1;
    g.selected_shader_id = 0;
    g_selected_image = 0;
    // RenderIfNeeded allocates the target now — its size depends on
    // whether the selection lives in the main picture or an offscreen
    // layer, and the layer routing can change per command.
}

static void LoadPicture(const char *path)
{
    std::vector<PictureFrame> pics = LoadPictures(path);
    if (pics.empty())
    {
        ShowError("Failed to load: %s", path);
        return;
    }

    // Extract base filename (strip directory + extension).
    std::string name = path;
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    g.base_name = name;

    ResetImageTiles();
    g.frames.clear();
    g.frames.reserve(pics.size());
    for (const auto &p : pics)
    {
        Frame f;
        f.pic = p;
        f.debug = std::make_unique<DebugCanvas>();
        f.debug->CollectFromPicture(p.picture.get());
        f.selected_cmd = f.debug->GetCommandCount() - 1;
        f.collapsed.assign(f.debug->GetCommandCount(), false);
        for (const auto &r : f.debug->GetResources())
            if (r.type == ResourceInfo::Image && r.image)
                AddImageTile(r.id, r.image.get());
        g.frames.push_back(std::move(f));
    }
    g.viewport_viewer.Reset();
    SetCurrentFrame(0);
}

static void RenderIfNeeded()
{
    Frame *f = g.cur();
    if (!f)
        return;
    if (f->selected_cmd == g.rendered_cmd && g.show_clip == g.rendered_clip &&
        g.current_frame == g.rendered_frame &&
        g.highlight_geometry == g.rendered_highlight)
        return;

    // If the selected command lives inside an offscreen layer, redirect
    // replay to the layer's captured SkPicture and size the render target
    // to the layer's cullRect. Commands outside any layer go through the
    // main picture at frame size.
    int layer_idx = -1;
    int sel = f->selected_cmd;
    auto &cmds = f->debug->GetCommands();
    if (sel >= 0 && sel < (int)cmds.size())
        layer_idx = cmds[sel].layer;

    if (layer_idx >= 0)
    {
        const LayerSpan &L = f->debug->GetLayers()[layer_idx];
        SkIRect lb = L.picture->cullRect().roundOut();
        g.skia.AllocateTarget(lb.width(), lb.height());
    }
    else
    {
        g.skia.AllocateTarget((int)f->pic.width, (int)f->pic.height);
    }

    g.skia.context->resetContext();
    g.skia.RebindSurface();

    SkCanvas *canvas = g.skia.GetCanvas();
    if (!canvas)
        return;

    canvas->clear(0xFF333333);
    int stop =
        (sel >= 0 && sel < (int)cmds.size()) ? cmds[sel].play_index : sel;
    if (layer_idx >= 0)
    {
        const LayerSpan &L = f->debug->GetLayers()[layer_idx];
        f->debug->DrawLayerToPicture(canvas, L, stop, g.show_clip,
                                     g.highlight_geometry);
    }
    else
    {
        f->debug->DrawToPicture(canvas, f->pic.picture.get(), stop, g.show_clip,
                                g.highlight_geometry);
    }
    g.current_matrix = canvas->getLocalToDevice();
    g.skia.Flush();
    g.skia.ReadbackPixels();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g.rendered_cmd = f->selected_cmd;
    g.rendered_clip = g.show_clip;
    g.rendered_frame = g.current_frame;
    g.rendered_highlight = g.highlight_geometry;
}

static void ExportPNG()
{
    if (g.skia.pixels.empty() || g.skia.tex_width <= 0)
        return;
    std::string suggested =
        sfmt("%s_%d.png", g.base_name.c_str(), g.current_frame);
    // Capture what we need now; SDL fires the callback later.
    int tex_w = g.skia.tex_width;
    int tex_h = g.skia.tex_height;
    std::vector<uint8_t> pixels = g.skia.pixels; // copy
    g_pending_dialog =
        [tex_w, tex_h, pixels = std::move(pixels)](const char *path)
    {
        if (!path)
            return;
        SkPixmap pm(SkImageInfo::Make(tex_w, tex_h, kRGBA_8888_SkColorType,
                                      kPremul_SkAlphaType),
                    pixels.data(), tex_w * 4);
        SkFILEWStream out(path);
        if (!out.isValid() || !SkPngEncoder::Encode(&out, pm, {}))
            ShowError("Failed to write %s", path);
    };
    SDL_ShowSaveFileDialog(DialogResultCallback, nullptr, g.window, kPngFilters,
                           1, suggested.c_str());
}

static void ExportDump()
{
    if (g.frames.empty())
        return;
    std::string suggested = g.base_name + ".txt";
    g_pending_dialog = [](const char *path)
    {
        if (!path)
            return;
        FILE *f = fopen(path, "w");
        if (!f)
        {
            ShowError("Failed to open %s", path);
            return;
        }
        for (size_t fi = 0; fi < g.frames.size(); fi++)
        {
            const Frame &fr = g.frames[fi];
            fprintf(f, "=== Frame %zu/%zu  %dx%d  %d commands ===\n", fi + 1,
                    g.frames.size(), (int)fr.pic.width, (int)fr.pic.height,
                    fr.debug->GetCommandCount());
            const std::vector<CommandInfo> &cmds = fr.debug->GetCommands();
            for (size_t i = 0; i < cmds.size(); i++)
            {
                fprintf(f, "[%zu] %s\n", i, cmds[i].name.c_str());
                std::string body = RenderDetailText(cmds[i].text);
                if (!body.empty())
                    fputs(body.c_str(), f);
                fprintf(f, "\n");
            }
        }
        fclose(f);
    };
    SDL_ShowSaveFileDialog(DialogResultCallback, nullptr, g.window, kTxtFilters,
                           1, suggested.c_str());
}

static void DrawMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open...", "Cmd+O"))
            {
                g_pending_dialog = [](const char *path)
                {
                    if (path)
                        LoadPicture(path);
                };
                SDL_ShowOpenFileDialog(DialogResultCallback, nullptr, g.window,
                                       kSkpFilters, 2, nullptr, false);
            }
            if (ImGui::MenuItem("Export PNG...", nullptr, false,
                                !g.skia.pixels.empty()))
                ExportPNG();
            if (ImGui::MenuItem("Export Dump...", nullptr, false,
                                !g.frames.empty()))
                ExportDump();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Cmd+Q"))
                g.should_quit = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            ImageViewer *v = ActiveViewer();
            if (ImGui::MenuItem("Zoom In", "+"))
                v->ZoomBy(2.0f);
            if (ImGui::MenuItem("Zoom Out", "-"))
                v->ZoomBy(0.5f);
            if (ImGui::MenuItem("Fit", "F"))
                v->Fit(v->last_view_w, v->last_view_h, v->last_img_w,
                       v->last_img_h);
            if (ImGui::MenuItem("1:1", "1"))
                v->zoom = 1.0f;
            if (ImGui::MenuItem("Reset View", "0"))
                v->Reset();
            ImGui::Separator();
            ImGui::MenuItem("Show Clip Rect", "C", &g.show_clip);
            ImGui::Separator();
            ImGui::MenuItem("Commands", nullptr, &g.show_commands);
            ImGui::MenuItem("Command Detail", nullptr, &g.show_detail);
            ImGui::MenuItem("Resources", nullptr, &g.show_resources);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout"))
                g.reset_layout = true;
            ImGui::EndMenu();
        }
        if (Frame *f = g.cur())
        {
            ImGui::Separator();
            ImGui::Text("%.0fx%.0f", f->pic.width, f->pic.height);
            ImGui::Separator();
            ImGui::Text("%.1fx", ActiveViewer()->zoom);
            if (g.frames.size() > 1)
            {
                ImGui::Separator();
                ImGui::Text("Frame %d/%d", g.current_frame + 1,
                            (int)g.frames.size());
            }
        }
        ImGui::EndMainMenuBar();
    }
}

static void HandleArrowNav();
static int FindVisibleFor(const std::vector<int> &vis, int cmd);

static void DrawViewportPanel(const uint8_t *px)
{
    constexpr float panel_w = 180.0f;
    ImGui::BeginGroup();

    ImGui::Checkbox("Show Clip", &g.show_clip);
    ImGui::Checkbox("Highlight Geometry", &g.highlight_geometry);
    ImGui::Text("Zoom: %.2fx", g.viewport_viewer.zoom);

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
#ifdef __APPLE__
    ImGui::TextWrapped("Hold Option to sample");
#else
    ImGui::TextWrapped("Hold Alt to sample");
#endif
    ImGui::PopStyleColor();

    int &hx = g.viewport_viewer.hover_px;
    int &hy = g.viewport_viewer.hover_py;

    if (hx >= 0 && px)
    {
        int tw = g.skia.tex_width;
        int th = g.skia.tex_height;
        uint32_t c = 0;
        if (hx < tw && hy >= 0 && hy < th)
            memcpy(&c, px + (hy * tw + hx) * 4, 4);
        uint8_t r = c & 0xFF;
        uint8_t g = (c >> 8) & 0xFF;
        uint8_t b = (c >> 16) & 0xFF;
        uint8_t a = (c >> 24) & 0xFF;

        ImGui::Text("X: %d  Y: %d", hx, hy);
        ImVec4 col(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
        ImGui::ColorButton("##pxcol", col, ImGuiColorEditFlags_AlphaPreview,
                           ImVec2(20, 20));
        ImGui::SameLine();
        ImGui::Text("#%02X%02X%02X%02X", r, g, b, a);
        ImGui::SameLine();
        if (ImGui::Button("Copy"))
            ImGui::SetClipboardText(
                sfmt("#%02X%02X%02X%02X", r, g, b, a).c_str());

        ImGui::Separator();
        DrawPixelMagnifier(hx, hy, tw, th, px, 11, panel_w);
    }
    else
    {
        ImGui::TextDisabled("No sample");
    }

    ImGui::EndGroup();
}

static void DrawViewport()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("Viewport"))
    {
        HandleArrowNav();
        if (g.skia.color_tex && g.skia.tex_width > 0)
        {
            const uint8_t *px =
                g.skia.pixels.empty() ? nullptr : g.skia.pixels.data();

            ImVec2 avail = ImGui::GetContentRegionAvail();
            constexpr float panel_w = 180.0f;

            // Image area on the left
            ImGui::BeginChild("##imgarea", ImVec2(avail.x - panel_w, 0));
            g.viewport_viewer.DrawImage(g.skia.color_tex, g.skia.tex_width,
                                        g.skia.tex_height, nullptr);
            ImGui::EndChild();

            // Panel on the right
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
            ImGui::BeginChild("##viewopts", ImVec2(0, 0), true);
            DrawViewportPanel(px);
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

static void DrawCommandList()
{
    if (!g.show_commands)
        return;
    if (ImGui::Begin("Commands", &g.show_commands))
    {
        HandleArrowNav();
        Frame *f = g.cur();
        if (f && f->debug)
        {
            int nframes = (int)g.frames.size();
            if (nframes > 1)
            {
                ImGui::BeginDisabled(g.current_frame <= 0);
                if (ImGui::Button("< Prev"))
                    SetCurrentFrame(g.current_frame - 1);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(g.current_frame >= nframes - 1);
                if (ImGui::Button("Next >"))
                    SetCurrentFrame(g.current_frame + 1);
                ImGui::EndDisabled();
                ImGui::SameLine();
                int frame = g.current_frame;
                if (ImGui::SliderInt("##frame", &frame, 0, nframes - 1,
                                     "Frame %d"))
                    SetCurrentFrame(frame);
                ImGui::Separator();
            }
            ImGui::Checkbox("Indent save/restore", &g.indent_save_restore);
            ImGui::SameLine();
            ImGui::Checkbox("Hide annotations", &g.hide_annotations);
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Up/Down: prev/next command");
            ImGui::TextWrapped("Left/Right: close/expand group");
            ImGui::PopStyleColor();
            if (ImGui::BeginTable("CmdTable", 2,
                                  ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_BordersInnerH))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed,
                                        35);
                ImGui::TableSetupColumn("Command");
                ImGui::TableHeadersRow();

                std::vector<CommandInfo> &cmds = f->debug->GetCommands();
                const std::vector<int> &vis = f->visible_cmds;
                for (int vi = 0; vi < (int)vis.size(); vi++)
                {
                    int i = vis[vi];
                    bool is_active = (i <= f->selected_cmd);
                    bool is_selected = (i == f->selected_cmd);
                    ImVec4 dim(0.5f, 0.5f, 0.5f, 1.0f);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(i);
                    std::string idx = std::to_string(i);
                    // Make the Selectable input-only: suppress its own
                    // hover/active/selected fill. We paint the row bg
                    // ourselves via TableSetBgColor (RowBg layer) which
                    // is guaranteed to render behind column content.
                    ImGui::PushStyleColor(ImGuiCol_Header,
                                          IM_COL32(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                          IM_COL32(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                          IM_COL32(0, 0, 0, 0));
                    if (!is_active)
                        ImGui::PushStyleColor(ImGuiCol_Text, dim);
                    bool pressed = ImGui::Selectable(
                        idx.c_str(), is_selected,
                        ImGuiSelectableFlags_SpanAllColumns |
                            ImGuiSelectableFlags_AllowOverlap);
                    // IsItemHovered() immediately after Selectable runs
                    // before the fold button is submitted, so the
                    // AllowOverlap check can't see the button yet and
                    // would report the row hovered even when the mouse
                    // is over the button. Capture the raw "over-rect"
                    // hover here, subtract out the button's own hover
                    // after it's drawn.
                    bool row_over_rect = ImGui::IsItemHovered(
                        ImGuiHoveredFlags_AllowWhenOverlappedByItem);
                    if (!is_active)
                        ImGui::PopStyleColor();
                    ImGui::PopStyleColor(3);
                    if (pressed)
                        f->selected_cmd = i;
                    if (is_selected && g.scroll_to_cmd)
                    {
                        ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
                        g.scroll_to_cmd = false;
                    }
                    ImGui::TableNextColumn();
                    if (g.indent_save_restore && cmds[i].indent > 0)
                    {
                        ImGui::Dummy(ImVec2(cmds[i].indent *
                                                ImGui::GetStyle().IndentSpacing,
                                            0));
                        ImGui::SameLine(0, 0);
                    }
                    bool btn_hovered = false;
                    if (cmds[i].range_end >= 0)
                    {
                        bool col = (i < (int)f->collapsed.size())
                                       ? (bool)f->collapsed[i]
                                       : false;
                        // ArrowButton sizes itself to GetFrameHeight
                        // (font + 2*FramePadding.y). Zero out vertical
                        // padding so the button matches text height and
                        // the row doesn't grow past the Selectable's
                        // hit rect.
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                            ImVec2(0, 0));
                        if (ImGui::ArrowButton("##fold", col ? ImGuiDir_Right
                                                             : ImGuiDir_Down))
                        {
                            f->collapsed[i] = !col;
                            if (!col && f->selected_cmd > i &&
                                f->selected_cmd <= cmds[i].range_end)
                                f->selected_cmd = cmds[i].range_end;
                        }
                        btn_hovered = ImGui::IsItemHovered();
                        ImGui::PopStyleVar();
                        ImGui::SameLine();
                    }
                    if (!is_active)
                        ImGui::PushStyleColor(ImGuiCol_Text, dim);
                    ImGui::TextUnformatted(cmds[i].name.c_str());
                    if (!is_active)
                        ImGui::PopStyleColor();
                    ImGui::PopID();
                    if (is_selected)
                        ImGui::TableSetBgColor(
                            ImGuiTableBgTarget_RowBg1,
                            ImGui::GetColorU32(ImGuiCol_Header));
                    else if (row_over_rect && !btn_hovered)
                        ImGui::TableSetBgColor(
                            ImGuiTableBgTarget_RowBg1,
                            ImGui::GetColorU32(ImGuiCol_HeaderHovered));
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::End();
}

struct DecomposedMatrix
{
    float translate_x, translate_y;
    float scale_x, scale_y;
    float rotation_deg;
    float shear;
};

// Decompose the 2D affine portion of a Skia matrix. SkMatrix's raw getters
// return matrix entries (e.g. cos θ for rotation), not the underlying
// transform components. This extracts them via QR-style decomposition:
//   M = Translate * Rotate(θ) * Scale(sx,sy) * Shear(sh)
static DecomposedMatrix DecomposeMatrix(const SkMatrix &m)
{
    // SkMatrix layout: [ a b tx ; c d ty ; 0 0 1 ]
    float a = m.getScaleX(), b = m.getSkewX();
    float c = m.getSkewY(), d = m.getScaleY();
    DecomposedMatrix out;
    out.translate_x = m.getTranslateX();
    out.translate_y = m.getTranslateY();
    out.scale_x = sqrtf(a * a + c * c);
    float det = a * d - b * c;
    out.scale_y =
        (out.scale_x > 1e-6f) ? det / out.scale_x : sqrtf(b * b + d * d);
    float rot_rad = atan2f(c, a);
    out.rotation_deg = rot_rad * (180.0f / 3.14159265358979323846f);
    out.shear = (out.scale_x > 1e-6f) ? (a * b + c * d) / out.scale_x : 0;
    return out;
}

static void DrawCommandDetail()
{
    if (!g.show_detail)
        return;
    if (ImGui::Begin("Command Detail", &g.show_detail))
    {
        Frame *f = g.cur();
        if (f && f->debug && f->selected_cmd >= 0 &&
            f->selected_cmd < f->debug->GetCommandCount())
        {
            CommandInfo &cmd = f->debug->GetCommands()[f->selected_cmd];
            ImGui::Text("#%d  %s", f->selected_cmd, cmd.name.c_str());
            ImGui::Separator();
            if (cmd.ui)
                cmd.ui();

            ImGui::Separator();
            if (ImGui::TreeNodeEx("Local to Device",
                                  ImGuiTreeNodeFlags_DefaultOpen))
            {
                SkMatrix m = g.current_matrix.asM33();
                DecomposedMatrix d = DecomposeMatrix(m);
                ImGui::Text("Translate: (%.2f, %.2f)", d.translate_x,
                            d.translate_y);
                ImGui::Text("Scale:     (%.3f, %.3f)", d.scale_x, d.scale_y);
                ImGui::Text("Rotation:  %.2f\u00b0", d.rotation_deg);
                ImGui::Text("Shear:     %.3f", d.shear);
                if (m.hasPerspective())
                    ImGui::Text("Perspective: (%.4f, %.4f, %.4f)",
                                m.getPerspX(), m.getPerspY(),
                                m.get(SkMatrix::kMPersp2));
                ImGui::TreePop();
            }
        }
        else
        {
            ImGui::TextDisabled("No command selected.");
        }
    }
    ImGui::End();
}

static void DrawResources()
{
    if (!g.show_resources)
        return;
    if (ImGui::Begin("Resources", &g.show_resources))
    {
        Frame *f = g.cur();
        if (!f || !f->debug)
        {
            ImGui::TextDisabled("No picture loaded.");
        }
        else
        {
            std::vector<ResourceInfo> &res = f->debug->GetResources();
            if (res.empty())
            {
                ImGui::TextDisabled("No resources found.");
            }
            else
            {
                if (ImGui::BeginChild("ResList", ImVec2(0, 200),
                                      ImGuiChildFlags_Borders))
                {
                    if (ImGui::BeginTable("ResTable", 3,
                                          ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_BordersInnerH))
                    {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn(
                            "ID", ImGuiTableColumnFlags_WidthFixed, 40);
                        ImGui::TableSetupColumn(
                            "Type", ImGuiTableColumnFlags_WidthFixed, 30);
                        ImGui::TableSetupColumn("Name");
                        ImGui::TableHeadersRow();

                        for (int i = 0; i < (int)res.size(); i++)
                        {
                            ResourceInfo &r = res[i];
                            bool is_selected =
                                (r.type == ResourceInfo::Image &&
                                 r.id == g_selected_image) ||
                                (r.type == ResourceInfo::Shader &&
                                 r.id == g.selected_shader_id);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::PushID(i);
                            std::string label = std::to_string(r.id);
                            if (ImGui::Selectable(
                                    label.c_str(), is_selected,
                                    ImGuiSelectableFlags_SpanAllColumns))
                            {
                                if (r.type == ResourceInfo::Image)
                                {
                                    g_selected_image = r.id;
                                    g.selected_shader_id = 0;
                                }
                                else
                                {
                                    g.selected_shader_id = r.id;
                                    g_selected_image = 0;
                                }
                            }
                            ImGui::PopID();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(
                                r.type == ResourceInfo::Shader ? "S" : "I");
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(r.type_name.c_str());
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();

                const ResourceInfo *sel = nullptr;
                for (const auto &r : res)
                    if ((r.type == ResourceInfo::Image &&
                         r.id == g_selected_image) ||
                        (r.type == ResourceInfo::Shader &&
                         r.id == g.selected_shader_id))
                    {
                        sel = &r;
                        break;
                    }
                if (sel)
                {
                    const ResourceInfo &r = *sel;
                    ImGui::Separator();
                    ImGui::Text("ID: %u", r.id);
                    ImGui::Text("Type: %s", r.type == ResourceInfo::Shader
                                                ? "Shader"
                                                : "Image");
                    ImGui::Text("Name: %s", r.type_name.c_str());

                    if (r.type == ResourceInfo::Shader && r.effect)
                    {
                        const SkFlattenable *f =
                            (const SkFlattenable *)r.effect;
                        if (HasSksl(f))
                        {
                            if (ImGui::Button("View Shader"))
                                OpenEffectWindow(f);
                            ImGui::SameLine();
                            if (ImGui::Button("Copy"))
                                SDL_SetClipboardText(ExtractSksl(f).c_str());
                            ImGui::SameLine();
                            if (ImGui::Button("Save..."))
                            {
                                std::string suggested =
                                    sfmt("%s_shader_%u.sksl",
                                         g.base_name.c_str(), r.id);
                                std::string source = ExtractSksl(f);
                                g_pending_dialog = [source = std::move(source)](
                                                       const char *path)
                                {
                                    if (!path)
                                        return;
                                    FILE *out = fopen(path, "w");
                                    if (!out)
                                        return;
                                    fwrite(source.data(), 1, source.size(),
                                           out);
                                    fclose(out);
                                };
                                SDL_ShowSaveFileDialog(
                                    DialogResultCallback, nullptr, g.window,
                                    nullptr, 0, suggested.c_str());
                            }
                        }
                    }
                    else if (r.type == ResourceInfo::Image && r.image)
                    {
                        ImGui::Text("Size: %dx%d", r.image->width(),
                                    r.image->height());
                        if (ImGui::Button("View Image"))
                            FocusImageTile(r.id, r.image.get());
                        ImGui::SameLine();
                        if (ImGui::Button("Copy"))
                        {
                            // Encode to PNG and put on clipboard with MIME
                            // type "image/png".
                            int w = r.image->width(), h = r.image->height();
                            SkBitmap bm;
                            bm.allocPixels(SkImageInfo::MakeN32Premul(w, h));
                            if (r.image->readPixels(bm.pixmap(), 0, 0))
                            {
                                SkDynamicMemoryWStream buf;
                                if (SkPngEncoder::Encode(&buf, bm.pixmap(), {}))
                                {
                                    sk_sp<SkData> png = buf.detachAsData();
                                    static sk_sp<SkData> s_png_keep;
                                    s_png_keep = png;
                                    auto provide =
                                        [](void *userdata, const char *,
                                           size_t *size) -> const void *
                                    {
                                        SkData *d = (SkData *)userdata;
                                        *size = d->size();
                                        return d->data();
                                    };
                                    const char *mime[] = {"image/png"};
                                    SDL_SetClipboardData(provide, nullptr,
                                                         s_png_keep.get(), mime,
                                                         1);
                                }
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Save..."))
                        {
                            sk_sp<SkImage> img = r.image;
                            std::string suggested = sfmt(
                                "%s_image_%u.png", g.base_name.c_str(), r.id);
                            g_pending_dialog = [img](const char *path)
                            {
                                if (!path)
                                    return;
                                int w = img->width(), h = img->height();
                                SkBitmap bm;
                                bm.allocPixels(
                                    SkImageInfo::MakeN32Premul(w, h));
                                if (!img->readPixels(bm.pixmap(), 0, 0))
                                    return;
                                SkFILEWStream out(path);
                                if (out.isValid())
                                    SkPngEncoder::Encode(&out, bm.pixmap(), {});
                            };
                            SDL_ShowSaveFileDialog(
                                DialogResultCallback, nullptr, g.window,
                                kPngFilters, 1, suggested.c_str());
                        }
                    }
                }
            }
        }
    }
    ImGui::End();
}

static void HandleShortcuts()
{
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureKeyboard)
        return;

    bool cmd = io.KeySuper || io.KeyCtrl;
    if (cmd && ImGui::IsKeyPressed(ImGuiKey_Q))
    {
        g.should_quit = true;
        return;
    }
    if (cmd && ImGui::IsKeyPressed(ImGuiKey_O))
    {
        g_pending_dialog = [](const char *path)
        {
            if (path)
                LoadPicture(path);
        };
        SDL_ShowOpenFileDialog(DialogResultCallback, nullptr, g.window,
                               kSkpFilters, 2, nullptr, false);
        return;
    }

    ImageViewer *v = ActiveViewer();
    if (ImGui::IsKeyPressed(ImGuiKey_F))
        v->Fit(v->last_view_w, v->last_view_h, v->last_img_w, v->last_img_h);
    if (ImGui::IsKeyPressed(ImGuiKey_Equal))
        v->ZoomBy(2.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus))
        v->ZoomBy(0.5f);
    if (ImGui::IsKeyPressed(ImGuiKey_1))
        v->zoom = 1.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_0))
        v->Reset();
    if (ImGui::IsKeyPressed(ImGuiKey_C))
        g.show_clip = !g.show_clip;
}

// Arrow-key navigation. Call from inside a window's Begin block so the
// focus check gates input to that window. RootAndChildWindows lets focus
// survive clicks into tables/list clippers (which create child windows).
// Walk commands, skipping the interior of any collapsed Save/SaveLayer.
// The Save row itself stays visible; its range [i+1 .. range_end] is hidden.
static void RebuildVisible(Frame &f)
{
    f.visible_cmds.clear();
    const auto &cmds = f.debug->GetCommands();
    int n = (int)cmds.size();
    f.visible_cmds.reserve(n);
    int i = 0;
    while (i < n)
    {
        if (!(g.hide_annotations && cmds[i].kind == CommandKind::Annotation))
            f.visible_cmds.push_back(i);
        if (cmds[i].range_end >= 0 && i < (int)f.collapsed.size() &&
            f.collapsed[i])
            i = cmds[i].range_end + 1;
        else
            i++;
    }
    // If the current selection just got hidden, slide it onto the nearest
    // still-visible command so the viewport render and row highlight stay
    // coherent without a redundant scroll.
    if (!f.visible_cmds.empty() && f.selected_cmd >= 0)
    {
        bool present = false;
        int vi = FindVisibleFor(f.visible_cmds, f.selected_cmd);
        if (f.visible_cmds[vi] == f.selected_cmd)
            present = true;
        if (!present)
        {
            f.selected_cmd = f.visible_cmds[vi];
            g.scroll_to_cmd = true;
        }
    }
}

// Find the largest visible entry <= cmd. Returns 0 if cmd precedes all
// visible entries; visible_cmds.size()-1 if it follows them.
static int FindVisibleFor(const std::vector<int> &vis, int cmd)
{
    if (vis.empty())
        return 0;
    int lo = 0, hi = (int)vis.size() - 1;
    while (lo < hi)
    {
        int mid = (lo + hi + 1) / 2;
        if (vis[mid] <= cmd)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static void HandleArrowNav()
{
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        return;
    Frame *f = g.cur();
    if (!f)
        return;

    const auto &vis = f->visible_cmds;
    if (vis.empty())
        return;
    auto &cmds = f->debug->GetCommands();
    int prev = f->selected_cmd;
    int cur_vis = FindVisibleFor(vis, f->selected_cmd);
    int last_vis = (int)vis.size() - 1;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && cur_vis > 0)
        f->selected_cmd = vis[cur_vis - 1];
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && cur_vis < last_vis)
        f->selected_cmd = vis[cur_vis + 1];
    if (ImGui::IsKeyPressed(ImGuiKey_Home))
        f->selected_cmd = vis.front();
    if (ImGui::IsKeyPressed(ImGuiKey_End))
        f->selected_cmd = vis.back();
    // Left/Right behave like a tree view: Left closes the current row's
    // group; if we're not on a group header ourselves, it closes the
    // enclosing one and moves the selection onto it. Right opens the
    // current row if it's a folded group header.
    int sel = f->selected_cmd;
    bool has_range =
        sel >= 0 && sel < (int)cmds.size() && cmds[sel].range_end >= 0;
    bool is_collapsed =
        has_range && sel < (int)f->collapsed.size() && f->collapsed[sel];
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) && has_range &&
        is_collapsed)
        f->collapsed[sel] = false;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && sel >= 0)
    {
        if (has_range && !is_collapsed)
            f->collapsed[sel] = true;
        else
        {
            // Walk backwards for the nearest group whose range covers us.
            for (int i = sel - 1; i >= 0; i--)
                if (cmds[i].range_end >= sel)
                {
                    if (i < (int)f->collapsed.size())
                        f->collapsed[i] = true;
                    f->selected_cmd = i;
                    break;
                }
        }
    }
    if (f->selected_cmd != prev)
        g.scroll_to_cmd = true;
}

int main(int argc, char **argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "skpviz", SDL_GetError(),
                                 nullptr);
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                        SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);

    g.window = SDL_CreateWindow("skpviz", 1440, 900,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                    SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g.window)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "skpviz", SDL_GetError(),
                                 nullptr);
        SDL_Quit();
        return 1;
    }

    g.gl_ctx = SDL_GL_CreateContext(g.window);
    if (!g.gl_ctx)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "skpviz", SDL_GetError(),
                                 g.window);
        SDL_DestroyWindow(g.window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(g.window, g.gl_ctx);
    SDL_GL_SetSwapInterval(1);
#ifndef __APPLE__
    LoadGLExtras();
#endif

    if (!g.skia.Init())
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "skpviz",
                                 "Failed to initialize Skia GPU context.",
                                 g.window);
        SDL_GL_DestroyContext(g.gl_ctx);
        SDL_DestroyWindow(g.window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "imgui.ini";
    // ProggyForever — scalable vector variant that matches ProggyClean's
    // look but rasterizes cleanly at any size (required for the image
    // tile labels whose font size tracks the Images panel zoom).
    io.Fonts->AddFontDefaultVector(nullptr);
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(g.window, g.gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 150");

    bool first_frame = true;
    if (argc > 1)
        LoadPicture(argv[1]);

    while (!g.should_quit)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT)
                g.should_quit = true;
            else if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                     ev.window.windowID == SDL_GetWindowID(g.window))
                g.should_quit = true;
            else if (ev.type == SDL_EVENT_DROP_FILE)
                LoadPicture(ev.drop.data);
        }
        RenderIfNeeded();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        if (Frame *f = g.cur())
            RebuildVisible(*f);

        HandleShortcuts();
        DrawMenuBar();

        ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();

        bool build_layout = false;
        if (first_frame)
        {
            first_frame = false;
            ImGuiDockNode *node = ImGui::DockBuilderGetNode(dockspace_id);
            if (!node || node->IsLeafNode())
                build_layout = true;
        }
        if (g.reset_layout)
        {
            g.reset_layout = false;
            g.show_commands = true;
            g.show_detail = true;
            g.show_resources = true;
            build_layout = true;
        }
        if (build_layout)
        {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id,
                                      ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id,
                                          ImGui::GetMainViewport()->Size);

            ImGuiID left, center;
            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.22f,
                                        &left, &center);
            ImGuiID viewport, right;
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, &right,
                                        &viewport);
            ImGuiID left_top, left_bottom;
            ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.4f, &left_bottom,
                                        &left_top);

            // Tab order in the viewport node = order docked here.
            // Viewport is docked first so it appears as the leftmost tab.
            ImGui::DockBuilderDockWindow("Viewport", viewport);
            ImGui::DockBuilderDockWindow("Images", viewport);
            ImGui::DockBuilderDockWindow("Shaders", viewport);
            ImGui::DockBuilderDockWindow("Commands", left_top);
            ImGui::DockBuilderDockWindow("Command Detail", left_bottom);
            ImGui::DockBuilderDockWindow("Resources", right);
            ImGui::DockBuilderFinish(dockspace_id);
        }

        // Order matters for tab focus: when multiple windows share a dock
        // node, the LAST Begin'd one becomes the active tab on first frame.
        // Viewport must come last to win against Images/Shaders.
        DrawCommandList();
        DrawCommandDetail();
        DrawResources();
        if (Frame *fr = g.cur())
        {
            DrawShadersPanel(fr->debug->GetResources());
            DrawImageWindows(fr->debug->GetResources());
        }
        else
        {
            DrawShadersPanel({});
            DrawImageWindows({});
        }
        DrawViewport();
        DrawErrorPopup();

        ImGui::Render();
        int fb_w = 0, fb_h = 0;
        SDL_GetWindowSizeInPixels(g.window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(g.window);
    }

    g.frames.clear();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    g.skia.Shutdown();
    SDL_GL_DestroyContext(g.gl_ctx);
    SDL_DestroyWindow(g.window);
    SDL_Quit();
    return 0;
}
