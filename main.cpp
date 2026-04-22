#include <SDL3/SDL.h>

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include "imgui_impl_opengl3_loader.h"
#endif

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

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

// Skia draws into the default framebuffer (FBO 0). ImGui draws on top in the
// same frame. Surface is recreated on resize.
static sk_sp<SkSurface> MakeWindowSurface(GrDirectContext *ctx, int w, int h)
{
    GrGLFramebufferInfo fb_info;
    fb_info.fFBOID = 0;
    fb_info.fFormat = GL_RGBA8;
    GrBackendRenderTarget target =
        GrBackendRenderTargets::MakeGL(w, h, 0, 8, fb_info);
    return SkSurfaces::WrapBackendRenderTarget(
        ctx, target, kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
        SkColorSpace::MakeSRGB(), nullptr);
}

int main(int, char **)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
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
        SDL_CreateWindow("layerviewer", 1440, 900,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                             SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "layerviewer",
                                 SDL_GetError(), nullptr);
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "layerviewer",
                                 SDL_GetError(), window);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);

    sk_sp<const GrGLInterface> gl_interface = GrGLMakeNativeInterface();
    if (!gl_interface)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "layerviewer",
                                 "GrGLMakeNativeInterface failed", window);
        return 1;
    }
    sk_sp<GrDirectContext> gr_ctx = GrDirectContexts::MakeGL(gl_interface);
    if (!gr_ctx)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "layerviewer",
                                 "GrDirectContexts::MakeGL failed", window);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 150");

    sk_sp<SkSurface> surface;
    int surf_w = 0, surf_h = 0;

    bool quit = false;
    while (!quit)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT)
                quit = true;
            else if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                     ev.window.windowID == SDL_GetWindowID(window))
                quit = true;
        }

        int fb_w, fb_h;
        SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
        if (fb_w != surf_w || fb_h != surf_h || !surface)
        {
            surface = MakeWindowSurface(gr_ctx.get(), fb_w, fb_h);
            surf_w = fb_w;
            surf_h = fb_h;
        }

        SkCanvas *canvas = surface->getCanvas();
        canvas->clear(SkColorSetARGB(255, 30, 30, 34));

        // Placeholder Skia drawing so we can verify the GPU path.
        SkPaint p;
        p.setAntiAlias(true);
        p.setColor(SkColorSetARGB(255, 200, 120, 60));
        canvas->drawCircle(fb_w * 0.5f, fb_h * 0.5f, 120.f, p);

        gr_ctx->flushAndSubmit();
        // Skia leaves GL state dirty; restore what ImGui expects.
        gr_ctx->resetContext();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport();
        ImGui::ShowDemoWindow();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);
    }

    surface.reset();
    gr_ctx.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
