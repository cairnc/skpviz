#include <ui/GraphicBuffer.h>

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
// Minimal GL declarations — we only need glGenTextures / glDeleteTextures /
// glBindTexture / glTexImage2D. Avoids dragging in a full GL loader here.
#include <cstddef>
typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
extern "C" {
void glGenTextures(GLsizei, GLuint *);
void glDeleteTextures(GLsizei, const GLuint *);
void glBindTexture(GLenum, GLuint);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                  const void *);
void glTexParameteri(GLenum, GLenum, GLint);
}
#endif

namespace android {

GraphicBuffer::~GraphicBuffer() {
  if (mTextureId) {
    GLuint t = mTextureId;
    glDeleteTextures(1, &t);
    mTextureId = 0;
  }
}

unsigned int GraphicBuffer::getOrCreateGLTexture() {
  if (mTextureId)
    return mTextureId;
  GLuint t = 0;
  glGenTextures(1, &t);
  if (!t)
    return 0;
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)mWidth, (GLsizei)mHeight, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  mTextureId = t;
  return t;
}

} // namespace android
