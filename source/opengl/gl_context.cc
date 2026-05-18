#include "gl_context.h"

#include <cstdio>

namespace sculptcore::opengl {

bool OffscreenTarget::create(int w, int h)
{
  release();
  width = w;
  height = h;

  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(
      GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  glFramebufferRenderbuffer(
      GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "OffscreenTarget: incomplete framebuffer 0x%x\n", status);
    release();
    return false;
  }
  return true;
}

void OffscreenTarget::release()
{
  if (fbo) {
    glDeleteFramebuffers(1, &fbo);
    fbo = 0;
  }
  if (color) {
    glDeleteTextures(1, &color);
    color = 0;
  }
  if (depth) {
    glDeleteRenderbuffers(1, &depth);
    depth = 0;
  }
  width = height = 0;
}

void OffscreenTarget::bind()
{
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glViewport(0, 0, width, height);
}

void OffscreenTarget::unbind()
{
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace sculptcore::opengl
