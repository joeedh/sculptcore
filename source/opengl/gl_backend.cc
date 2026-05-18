#include "gl_backend.h"

#include "gpu/batch.h"
#include "gpu/command.h"
#include "gpu/manager.h"
#include "gpu/shader.h"
#include "gpu/vbo.h"

#include "litestl/util/string.h"

#include <cstdio>
#include <cstring>

namespace sculptcore::opengl {

using namespace sculptcore::gpu;
using litestl::util::string;

static const char *gl_type_name(GLenum t)
{
  switch (t) {
  case GL_FLOAT: return "float";
  case GL_INT: return "int";
  case GL_UNSIGNED_INT: return "uint";
  case GL_SHORT: return "short";
  case GL_UNSIGNED_SHORT: return "ushort";
  case GL_BYTE: return "byte";
  case GL_UNSIGNED_BYTE: return "ubyte";
  }
  return "?";
}

static GLenum gpu_type_to_gl(GPUType t)
{
  switch (t) {
  case GPUType::FLOAT32: return GL_FLOAT;
  case GPUType::INT32: return GL_INT;
  case GPUType::UINT32: return GL_UNSIGNED_INT;
  case GPUType::INT16: return GL_SHORT;
  case GPUType::UINT16: return GL_UNSIGNED_SHORT;
  case GPUType::INT8: return GL_BYTE;
  case GPUType::UINT8: return GL_UNSIGNED_BYTE;
  default: return GL_FLOAT;
  }
}

static GLenum cmd_type_to_gl(GPUCmdType t)
{
  switch (t) {
  case GPUCmdType::DRAW_TRIS: return GL_TRIANGLES;
  case GPUCmdType::DRAW_TRI_STRIP: return GL_TRIANGLE_STRIP;
  case GPUCmdType::DRAW_LINES: return GL_LINES;
  case GPUCmdType::DRAW_POINTS: return GL_POINTS;
  }
  return GL_TRIANGLES;
}

/* The engine's shaders are written in GLSL-ES style (precision qualifier,
 * `attribute` / `varying` / `gl_FragColor`). Desktop GL 2.1's GLSL 1.20
 * accepts that surface once `#version 120` is prepended and the precision
 * qualifier is neutralized. */
static string preprocess_glsl(const string &src)
{
  std::string s(src.c_str(), src.size());
  /* Strip every "precision <p> <type>;" line. Replace with a comment so
   * line numbers stay aligned with the original source. */
  for (;;) {
    auto pos = s.find("precision ");
    if (pos == std::string::npos) {
      break;
    }
    auto end = s.find(';', pos);
    if (end == std::string::npos) {
      break;
    }
    /* Replacement must NOT contain "precision " or the next iteration of
     * the loop matches its own output. */
    s.replace(pos, end - pos + 1, "/* p_stripped */");
  }
  std::string out = "#version 120\n";
  out += s;
  return string(out.c_str());
}

static GLuint compile_shader(GLenum stage, const string &src, const char *name)
{
  string processed = preprocess_glsl(src);
  GLuint sh = glCreateShader(stage);
  const char *cstr = processed.c_str();
  GLint len = GLint(processed.size());
  glShaderSource(sh, 1, &cstr, &len);
  glCompileShader(sh);

  GLint status = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
  if (!status) {
    char log[2048];
    GLsizei loglen = 0;
    glGetShaderInfoLog(sh, sizeof(log) - 1, &loglen, log);
    log[loglen] = 0;
    fprintf(stderr,
            "GLBackend: %s shader '%s' failed:\n%s\n--- source ---\n%s\n",
            stage == GL_VERTEX_SHADER ? "vertex" : "fragment",
            name,
            log,
            processed.c_str());
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

GLBackend::GLBackend(GPUManager *mgr) : mgr_(mgr) {}

GLBackend::~GLBackend() { invalidate(); }

void GLBackend::invalidate()
{
  /* Caller invokes invalidate() right before destroying this backend so the
   * GL context can still see the handles. We only need to release the GL
   * objects; the maps die with the GLBackend. */
  for (auto &kv : vbo_cache_) {
    GLuint id = kv.value;
    if (id) {
      glDeleteBuffers(1, &id);
    }
  }
  for (auto &kv : prog_cache_) {
    GLuint p = kv.value;
    if (p) {
      glDeleteProgram(p);
    }
  }
}

GLuint GLBackend::ensureBuffer(Buffer *buf)
{
  if (!buf || !buf->data || buf->size <= 0) {
    return 0;
  }

  GLuint *cached = vbo_cache_.lookup_ptr(buf);
  GLuint id = cached ? *cached : 0;

  if (!id) {
    glGenBuffers(1, &id);
    vbo_cache_[buf] = id;
    buf->update_buffer = true;
  }

  if (buf->update_buffer) {
    GLenum target = (buf->target == BUFFER_INDEX) ? GL_ELEMENT_ARRAY_BUFFER
                                                  : GL_ARRAY_BUFFER;
    GLenum usage = (buf->hint == HINT_STATIC) ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW;
    GLsizeiptr bytes = GLsizeiptr(buf->size) * buf->elemsize * gpu_sizeof(buf->type);
    glBindBuffer(target, id);
    glBufferData(target, bytes, buf->data, usage);
    buf->update_buffer = false;
    buf->uploaded = true;
  }
  return id;
}

GLuint GLBackend::ensureProgram(ShaderDef *def)
{
  if (!def) {
    return 0;
  }
  GLuint *cached = prog_cache_.lookup_ptr(def);
  if (cached && *cached) {
    return *cached;
  }

  GLuint vs = compile_shader(GL_VERTEX_SHADER, def->vertexSource, def->name.c_str());
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, def->fragmentSource, def->name.c_str());
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    prog_cache_[def] = 0;
    return 0;
  }

  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  /* Bind attribute locations by definition order so we can set them up
   * without querying the program after link. */
  for (int i = 0; i < int(def->attrs.size()); i++) {
    glBindAttribLocation(prog, GLuint(i), def->attrs[i].name.c_str());
  }
  glLinkProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint status = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &status);
  if (!status) {
    char log[2048];
    GLsizei loglen = 0;
    glGetProgramInfoLog(prog, sizeof(log) - 1, &loglen, log);
    log[loglen] = 0;
    fprintf(stderr, "GLBackend: program '%s' link failed:\n%s\n", def->name.c_str(), log);
    glDeleteProgram(prog);
    prog_cache_[def] = 0;
    return 0;
  }

  prog_cache_[def] = prog;
  return prog;
}

void GLBackend::issue(DrawCommand *cmd, const DrawUniforms &u)
{
  if (!cmd || !cmd->shader) {
    return;
  }
  GLuint prog = ensureProgram(cmd->shader);
  if (!prog) {
    return;
  }

  glUseProgram(prog);

  /* Standard uniforms: drawMatrix, normalMatrix, uColor. Queried by name so
   * we don't tie the backend to a particular shader's uniform layout. */
  GLint loc;
  loc = glGetUniformLocation(prog, "drawMatrix");
  if (loc >= 0) {
    glUniformMatrix4fv(loc, 1, GL_FALSE, static_cast<const float *>(u.drawMatrix));
  }
  loc = glGetUniformLocation(prog, "normalMatrix");
  if (loc >= 0) {
    glUniformMatrix4fv(loc, 1, GL_FALSE, static_cast<const float *>(u.normalMatrix));
  }
  loc = glGetUniformLocation(prog, "uColor");
  if (loc >= 0) {
    glUniform4f(loc, u.uColor[0], u.uColor[1], u.uColor[2], u.uColor[3]);
  }

  /* Bind each attribute buffer at the location matching its index in the
   * shader's attrs[] list. cmd->attrs is expected to be in the same order. */
  const auto &shader_attrs = cmd->shader->attrs;
  int n = int(cmd->attrs.size());
  if (n > int(shader_attrs.size())) {
    n = int(shader_attrs.size());
  }
  for (int i = 0; i < n; i++) {
    Buffer *buf = cmd->attrs[i];
    GLuint vbo = ensureBuffer(buf);
    if (!vbo) {
      continue;
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(GLuint(i));
    glVertexAttribPointer(GLuint(i),
                          shader_attrs[i].elemSize,
                          gpu_type_to_gl(shader_attrs[i].type),
                          GL_FALSE,
                          0,
                          nullptr);
  }

  GLenum prim = cmd_type_to_gl(cmd->type);
  GLsizei count = cmd->end - cmd->start;
  if (count > 0) {
    glDrawArrays(prim, cmd->start, count);
  }

  for (int i = 0; i < n; i++) {
    glDisableVertexAttribArray(GLuint(i));
  }
}

void GLBackend::draw(DrawBatch *batch, const DrawUniforms &u)
{
  if (!batch) {
    return;
  }
  for (DrawCommand *cmd : batch->commands) {
    issue(cmd, u);
  }
}

} // namespace sculptcore::opengl
