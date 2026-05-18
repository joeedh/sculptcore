#include "gl_overlay.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace sculptcore::opengl {

using litestl::math::float3;
using litestl::math::float4;
using litestl::math::mat4;

static const char *kVS = R"glsl(
#version 120
uniform mat4 uMat;
attribute vec3 aPos;
attribute vec4 aColor;
varying vec4 vColor;
void main() {
  gl_Position = uMat * vec4(aPos, 1.0);
  vColor = aColor;
}
)glsl";

static const char *kFS = R"glsl(
#version 120
uniform vec4 uColor;
varying vec4 vColor;
void main() {
  gl_FragColor = vColor * uColor;
}
)glsl";

static GLuint compile(GLenum stage, const char *src)
{
  GLuint sh = glCreateShader(stage);
  glShaderSource(sh, 1, &src, nullptr);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    GLsizei n = 0;
    glGetShaderInfoLog(sh, sizeof(log) - 1, &n, log);
    log[n] = 0;
    fprintf(stderr, "Overlay shader compile failed:\n%s\n", log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

bool Overlay::ensure()
{
  if (prog_) {
    return true;
  }
  GLuint vs = compile(GL_VERTEX_SHADER, kVS);
  GLuint fs = compile(GL_FRAGMENT_SHADER, kFS);
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return false;
  }
  prog_ = glCreateProgram();
  glAttachShader(prog_, vs);
  glAttachShader(prog_, fs);
  glBindAttribLocation(prog_, 0, "aPos");
  glBindAttribLocation(prog_, 1, "aColor");
  glLinkProgram(prog_);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint ok = 0;
  glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024];
    GLsizei n = 0;
    glGetProgramInfoLog(prog_, sizeof(log) - 1, &n, log);
    log[n] = 0;
    fprintf(stderr, "Overlay program link failed:\n%s\n", log);
    glDeleteProgram(prog_);
    prog_ = 0;
    return false;
  }

  loc_pos_ = 0;
  loc_color_ = 1;
  loc_uMat_ = glGetUniformLocation(prog_, "uMat");
  loc_uColor_ = glGetUniformLocation(prog_, "uColor");

  glGenBuffers(1, &vbo_);
  return true;
}

void Overlay::release()
{
  if (vbo_) {
    glDeleteBuffers(1, &vbo_);
    vbo_ = 0;
  }
  if (prog_) {
    glDeleteProgram(prog_);
    prog_ = 0;
  }
}

namespace {
struct Vert {
  float pos[3];
  float color[4];
};

void issue(GLuint prog,
           GLuint vbo,
           GLint loc_uMat,
           GLint loc_uColor,
           const mat4 &m,
           const float4 &color,
           const std::vector<Vert> &verts,
           GLenum prim)
{
  if (verts.empty()) {
    return;
  }
  glUseProgram(prog);
  glUniformMatrix4fv(loc_uMat, 1, GL_FALSE, static_cast<const float *>(m));
  glUniform4f(loc_uColor, color[0], color[1], color[2], color[3]);

  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER,
               GLsizeiptr(verts.size() * sizeof(Vert)),
               verts.data(),
               GL_DYNAMIC_DRAW);

  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(
      0, 3, GL_FLOAT, GL_FALSE, sizeof(Vert), reinterpret_cast<void *>(0));
  glVertexAttribPointer(1,
                        4,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(Vert),
                        reinterpret_cast<void *>(sizeof(float) * 3));
  glDrawArrays(prim, 0, GLsizei(verts.size()));
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
}
} // namespace

void Overlay::drawAxes(const mat4 &drawMatrix, float scale)
{
  if (!ensure()) {
    return;
  }
  std::vector<Vert> v;
  auto add = [&](float3 a, float3 b, float4 c) {
    v.push_back({{a[0], a[1], a[2]}, {c[0], c[1], c[2], c[3]}});
    v.push_back({{b[0], b[1], b[2]}, {c[0], c[1], c[2], c[3]}});
  };
  add({0, 0, 0}, {scale, 0, 0}, {1.0f, 0.2f, 0.2f, 1.0f});
  add({0, 0, 0}, {0, scale, 0}, {0.2f, 1.0f, 0.2f, 1.0f});
  add({0, 0, 0}, {0, 0, scale}, {0.2f, 0.4f, 1.0f, 1.0f});
  issue(prog_, vbo_, loc_uMat_, loc_uColor_, drawMatrix, {1, 1, 1, 1}, v, GL_LINES);
}

void Overlay::drawBrushCursor(
    const mat4 &drawMatrix, float3 center, float3 normal, float radius, float4 color)
{
  if (!ensure()) {
    return;
  }
  /* Build a basis (u, v) spanning the plane perpendicular to normal. */
  float3 n = normal;
  n.normalize();
  float3 ref = std::fabs(n[0]) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
  float3 u = n.cross(ref);
  u.normalize();
  float3 vAx = n.cross(u);
  vAx.normalize();

  const int segments = 48;
  std::vector<Vert> verts;
  verts.reserve(segments * 2);
  for (int i = 0; i < segments; i++) {
    float a0 = float(i) / segments * 6.28318530718f;
    float a1 = float(i + 1) / segments * 6.28318530718f;
    float3 p0 = center + (u * std::cos(a0) + vAx * std::sin(a0)) * radius;
    float3 p1 = center + (u * std::cos(a1) + vAx * std::sin(a1)) * radius;
    verts.push_back({{p0[0], p0[1], p0[2]}, {1, 1, 1, 1}});
    verts.push_back({{p1[0], p1[1], p1[2]}, {1, 1, 1, 1}});
  }
  issue(prog_, vbo_, loc_uMat_, loc_uColor_, drawMatrix, color, verts, GL_LINES);
}

} // namespace sculptcore::opengl
