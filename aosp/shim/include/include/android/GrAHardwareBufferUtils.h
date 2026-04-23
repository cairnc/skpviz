// GrAHardwareBufferUtils shim. Upstream Skia's Android build imports AHBs by
// creating EGLImages; our GraphicBuffer stores the texture directly, so
// MakeGLBackendTexture just reads mTextureId off our GraphicBuffer and wraps
// it with GrBackendTextures::MakeGL.

#pragma once

#include <include/core/SkColorType.h>
#include <include/core/SkImageInfo.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <include/gpu/ganesh/gl/GrGLTypes.h>

#include <android/hardware_buffer.h>
#include <cstdint>

class GrDirectContext;

// Include our GraphicBuffer so we can reach into it for the GL texture name.
// Included unqualified (matching layerviewer's libui include root).
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>

namespace GrAHardwareBufferUtils {

using TexImageCtx = void *;
using DeleteImageProc = void (*)(void *);
using UpdateImageProc = void (*)(void *, GrDirectContext *);

inline SkColorType GetSkColorTypeFromBufferFormat(uint32_t format) {
  switch (format) {
  case 1: /* RGBA_8888 */
    return kRGBA_8888_SkColorType;
  case 2: /* RGBX_8888 */
    return kRGB_888x_SkColorType;
  case 3: /* RGB_888   */
    return kRGB_888x_SkColorType;
  case 4: /* RGB_565   */
    return kRGB_565_SkColorType;
  case 5: /* BGRA_8888 */
    return kBGRA_8888_SkColorType;
  case 0x16: /* RGBA_FP16 */
    return kRGBA_F16_SkColorType;
  case 0x2b: /* RGBA_1010102 */
    return kRGBA_1010102_SkColorType;
  default:
    return kRGBA_8888_SkColorType;
  }
}

inline GrBackendFormat GetGLBackendFormat(GrDirectContext *,
                                          uint32_t /*format*/,
                                          bool /*requireKnownFormat*/) {
  // Our textures are always GL_RGBA8 regardless of the requested format —
  // the color mapping happens in Skia during draw. Returning a known format
  // keeps AutoBackendTexture happy.
  return GrBackendFormats::MakeGL(0x8058 /*GL_RGBA8*/,
                                  0x0DE1 /*GL_TEXTURE_2D*/);
}

inline GrBackendTexture
MakeGLBackendTexture(GrDirectContext *, AHardwareBuffer *buffer, int width,
                     int height, DeleteImageProc *outDelete,
                     UpdateImageProc *outUpdate, TexImageCtx *outCtx,
                     bool /*isProtectedContent*/, const GrBackendFormat &,
                     bool /*isRenderable*/) {
  if (outDelete)
    *outDelete = [](void *) {};
  if (outUpdate)
    *outUpdate = [](void *, GrDirectContext *) {};
  if (outCtx)
    *outCtx = nullptr;

  auto *gb = android::GraphicBuffer::fromAHardwareBuffer(buffer);
  if (!gb)
    return {};

  // Allocate the texture on-demand — RE's AutoBackendTexture creates the
  // backend texture before making an SkSurface/SkImage from it, and the GL
  // context is current for all of that.
  unsigned int texId = gb->getOrCreateGLTexture();
  if (!texId)
    return {};

  GrGLTextureInfo info;
  info.fTarget = 0x0DE1; // GL_TEXTURE_2D
  info.fID = texId;
  info.fFormat = 0x8058; // GL_RGBA8
  info.fProtected = skgpu::Protected::kNo;

  return GrBackendTextures::MakeGL(width ? width : (int)gb->getWidth(),
                                   height ? height : (int)gb->getHeight(),
                                   skgpu::Mipmapped::kNo, info);
}

// Vulkan variants declared to satisfy call sites in Vk branches (never taken
// in our desktop-GL port).
inline GrBackendFormat
GetVulkanBackendFormat(GrDirectContext *, AHardwareBuffer *, uint32_t, bool) {
  return {};
}
inline GrBackendTexture
MakeVulkanBackendTexture(GrDirectContext *, AHardwareBuffer *, int, int,
                         DeleteImageProc *, UpdateImageProc *, TexImageCtx *,
                         bool, const GrBackendFormat &, bool) {
  return {};
}

} // namespace GrAHardwareBufferUtils
