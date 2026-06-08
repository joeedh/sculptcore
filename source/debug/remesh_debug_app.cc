// Interactive quad-remesh debug app. Loads/imports/generates assets, runs the
// standalone remesh_cli as a subprocess (so the remesher can be rebuilt without
// restarting this app), and shows the shaded result with a quad-edge wireframe.
// Orbit/pan/zoom with the mouse; an agent can drive it over the named pipe
// \\.\pipe\sculpt-remesh-debug (see tools/remesh_dbg.mjs, or send `help`).
//
// All overlays (quad wireframe, curvature field, cross field, field edges,
// streamlines) are emitted as world-space line geometry through the Scene's
// depth-tested overlay hook, so geometry on the far side of the surface is
// occluded by the front faces. The line pipeline has no alpha blending, so
// overlays use solid opaque colors (distinguished by hue/brightness, never
// alpha) and a small toward-eye nudge to clear coincident surface depth.

#include "input.h"
#include "remesh_app.h"
#include "remesh_pipe_server.h"
#include "remesh_ui.h"
#include "scene.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/curvature.h"
#include "window/window.h"

#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace sculptcore::debug_app;
using litestl::math::float2;
using litestl::math::float3;
using litestl::math::float4;
using litestl::math::mat4;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::BuiltinAttr;

namespace {

constexpr int kMaxWireEdges = 250000;  // bound the per-frame wireframe overlay
constexpr int kMaxFieldVerts = 200000; // bound the per-frame field overlays
constexpr float kHalfPi = 1.57079632679f;
// Fraction of the eye->point distance to pull each overlay point toward the
// eye, so coincident overlay geometry clears the surface depth (LEQUAL) without
// floating in front of nearer faces.
constexpr float kOverlayNudge = 0.0015f;

// Orbit/pan/zoom controller for the read-only remesh viewer: plain LMB or RMB
// orbits, Shift+LMB or MMB pans, scroll zooms. No sculpting (unlike debug_app's
// InteractiveController). Camera math mirrors interactive.cc.
struct OrbitController : InputHandler {
  OrbitController(Scene *scene) : scene_(scene) {}

  bool handle(const InputEvent &e) override
  {
    switch (e.kind) {
    case InputKind::CursorPos: {
      float2 cur(e.u.cursor.x, e.u.cursor.y);
      float2 d(cur[0] - cursor_[0], cur[1] - cursor_[1]);
      cursor_ = cur;
      if (orbit_) {
        doOrbit(d);
      } else if (pan_) {
        doPan(d);
      }
      return false;
    }
    case InputKind::MouseButton: {
      bool press = e.u.mb.action == ButtonAction::Press;
      cursor_ = float2(e.u.mb.x, e.u.mb.y);
      bool shift = (e.u.mb.mods.bits & 0x1) != 0;
      if (e.u.mb.button == MouseButton::Left) {
        lmb_ = press;
        orbit_ = press && !shift;
        pan_ = press && shift;
      } else if (e.u.mb.button == MouseButton::Right) {
        orbit_ = press;
      } else if (e.u.mb.button == MouseButton::Middle) {
        pan_ = press;
      }
      if (!press) {
        // clear whichever modes the released button could have set
        if (e.u.mb.button == MouseButton::Left) {
          orbit_ = pan_ = false;
        } else if (e.u.mb.button == MouseButton::Right) {
          orbit_ = false;
        } else if (e.u.mb.button == MouseButton::Middle) {
          pan_ = false;
        }
      }
      return false;
    }
    case InputKind::Scroll:
      doZoom(e.u.scroll.dy);
      return true;
    case InputKind::FramebufferSize:
      scene_->handleResize();
      return false;
    default:
      return false;
    }
  }

private:
  void fbSize(int &w, int &h) const
  {
    w = scene_->swapchain.width > 0 ? scene_->swapchain.width : scene_->width;
    h = scene_->swapchain.height > 0 ? scene_->swapchain.height : scene_->height;
  }

  void doOrbit(float2 delta)
  {
    constexpr float kRadPerPx = 0.005f;
    float3 worldUp(0, 0, 1);
    auto rot = [](float3 v, float3 a, float ang) -> float3 {
      float c = std::cos(ang), s = std::sin(ang);
      return v * c + a.cross(v) * s + a * (a.dot(v) * (1.0f - c));
    };
    float3 rel = scene_->camera.eye - scene_->camera.target;
    rel = rot(rel, worldUp, -delta[0] * kRadPerPx);
    float3 fwd = rel * -1.0f;
    fwd.normalize();
    float3 right = fwd.cross(worldUp);
    if (right.lengthSqr() < 1e-8f) {
      right = float3(1, 0, 0);
    } else {
      right.normalize();
    }
    rel = rot(rel, right, -delta[1] * kRadPerPx);
    scene_->camera.eye = scene_->camera.target + rel;
  }

  void doPan(float2 delta)
  {
    float3 fwd = scene_->camera.target - scene_->camera.eye;
    float dist = fwd.length();
    if (dist < 1e-6f) {
      return;
    }
    fwd.normalize();
    float3 right = fwd.cross(scene_->camera.up);
    right.normalize();
    float3 up = right.cross(fwd);
    up.normalize();
    int w = 0, h = 0;
    fbSize(w, h);
    if (h <= 0) {
      return;
    }
    float worldPerPx =
        (2.0f * dist * std::tan(scene_->camera.fovy * 0.5f)) / float(h);
    float3 ofs = right * (-delta[0] * worldPerPx) + up * (delta[1] * worldPerPx);
    scene_->camera.eye = scene_->camera.eye + ofs;
    scene_->camera.target = scene_->camera.target + ofs;
  }

  void doZoom(float dy)
  {
    float3 rel = scene_->camera.eye - scene_->camera.target;
    float dist = rel.length();
    if (dist < 1e-6f) {
      return;
    }
    float newDist = dist * std::pow(1.1f, -dy);
    if (newDist < 0.05f) {
      newDist = 0.05f;
    }
    scene_->camera.eye = scene_->camera.target + rel * (newDist / dist);
  }

  Scene *scene_;
  float2 cursor_{0, 0};
  bool lmb_ = false, orbit_ = false, pan_ = false;
};

// Accumulates world-space line segments for the depth-tested overlay pass. Each
// point is nudged slightly toward the eye (see kOverlayNudge) so overlay
// geometry coincident with the surface still clears its depth.
struct LineSink {
  litestl::util::Vector<float3> pos;
  litestl::util::Vector<float4> clr;
  float3 eye{0, 0, 0};

  float3 nudge(float3 p) const { return p + (eye - p) * kOverlayNudge; }
  void seg(float3 a, float3 b, float4 c)
  {
    pos.append(nudge(a));
    pos.append(nudge(b));
    clr.append(c);
    clr.append(c);
  }
  void clear()
  {
    pos.clear();
    clr.clear();
  }
};

// User payload for the Scene overlay hook. Both members are owned by main (in
// the Scene scope), so the sink's heap buffers are freed before the end-of-main
// leak check — a function-static sink would still be alive there and falsely
// trip it.
struct OverlayCtx {
  RemeshApp *app;
  LineSink *sink;
};

// --- Field helpers (geometry only; no rendering) ---

// True for an interior, manifold (exactly two distinct faces) edge; fills fa/fb.
bool interiorEdgeFaces(sculptcore::mesh::Mesh &m, int e, int &fa, int &fb)
{
  int c1 = m.e.c[e];
  if (c1 == ELEM_NONE) {
    return false;
  }
  int c2 = m.c.radial_next[c1];
  if (c2 == c1 || m.c.radial_next[c2] != c1) {
    return false;
  }
  fa = m.l.f[m.c.l[c1]];
  fb = m.l.f[m.c.l[c2]];
  return fa != fb;
}

// Mesh AABB via an explicit min/max loop (calcAABB seeds max to FLT_MIN).
bool meshBBox(sculptcore::mesh::Mesh &m, float3 &bmin, float3 &bmax)
{
  bool have = false;
  for (int v : m.v) {
    float3 c = m.v.co[v];
    if (!have) {
      bmin = bmax = c;
      have = true;
      continue;
    }
    for (int i = 0; i < 3; i++) {
      bmin[i] = c[i] < bmin[i] ? c[i] : bmin[i];
      bmax[i] = c[i] > bmax[i] ? c[i] : bmax[i];
    }
  }
  return have;
}

// Centroid + tangent frame of face f (frame matches the solver's faceFrame, so
// the θ stored per face is expressed in this same basis).
void faceCenterFrame(sculptcore::mesh::Mesh &m, int f, float3 &c, float3 &X,
                     float3 &Y, float3 &N)
{
  sculptcore::remesh::faceFrame(m, f, X, Y, N);
  c = float3(0, 0, 0);
  int n = 0, c0 = m.l.c[m.f.l[f]], cc = c0;
  do {
    c = c + m.v.co[m.c.v[cc]];
    n++;
    cc = m.c.next[cc];
  } while (cc != c0 && n < 64);
  if (n > 0) {
    c = c * (1.0f / float(n));
  }
}

// Project face f's verts into its (X,Y) tangent plane about c; returns vert count.
int facePoly2D(sculptcore::mesh::Mesh &m, int f, const float3 &c, const float3 &X,
               const float3 &Y, float *xs, float *ys, int maxn)
{
  int c0 = m.l.c[m.f.l[f]], cc = c0, n = 0;
  do {
    float3 d = m.v.co[m.c.v[cc]] - c;
    xs[n] = d.dot(X);
    ys[n] = d.dot(Y);
    n++;
    cc = m.c.next[cc];
  } while (cc != c0 && n < maxn);
  return n;
}

// Even-odd point-in-polygon (PNPOLY) in 2D.
bool pnpoly(int n, const float *xs, const float *ys, float px, float py)
{
  bool in = false;
  for (int i = 0, j = n - 1; i < n; j = i++) {
    if (((ys[i] > py) != (ys[j] > py)) &&
        (px < (xs[j] - xs[i]) * (py - ys[i]) / (ys[j] - ys[i]) + xs[i])) {
      in = !in;
    }
  }
  return in;
}

// Among face f's four cross arms (built from θ + frame), the one best aligned
// with `want` — the 4-RoSy direction match used when a streamline crosses faces.
float3 snapCross(float t, const float3 &X, const float3 &Y, float3 want)
{
  float best = -1e30f;
  float3 bd = want;
  for (int k = 0; k < 4; k++) {
    float a = t + float(k) * kHalfPi;
    float3 d = X * std::cos(a) + Y * std::sin(a);
    float dt = d.dot(want);
    if (dt > best) {
      best = dt;
      bd = d;
    }
  }
  return bd;
}

// The edge-adjacent face of f that contains world point p (ELEM_NONE if none).
int neighborContaining(sculptcore::mesh::Mesh &m, int f, float3 p)
{
  int c0 = m.l.c[m.f.l[f]], cc = c0, n = 0;
  do {
    int cr = m.c.radial_next[cc];
    if (cr != cc) {
      int nf = m.l.f[m.c.l[cr]];
      if (nf != f) {
        float3 c2, X2, Y2, N2;
        faceCenterFrame(m, nf, c2, X2, Y2, N2);
        float xs[64], ys[64];
        int np = facePoly2D(m, nf, c2, X2, Y2, xs, ys, 64);
        float3 pp = p - N2 * ((p - c2).dot(N2));
        if (pnpoly(np, xs, ys, (pp - c2).dot(X2), (pp - c2).dot(Y2))) {
          return nf;
        }
      }
    }
    cc = m.c.next[cc];
    n++;
  } while (cc != c0 && n < 64);
  return ELEM_NONE;
}

// Ensure the 4-RoSy cross field (.remesh.f.theta / .e.period / .v.pole_index)
// exists; compute it with default params on first use. False if no faces.
bool ensureCrossField(sculptcore::mesh::Mesh &m)
{
  BuiltinAttr<float, ".remesh.f.theta", AttrFlag::TEMP> theta;
  if (theta.ensure(m.f.attrs)) {
    sculptcore::remesh::CrossFieldParams cfp;
    sculptcore::remesh::computeCrossField(m, cfp);
  }
  return m.f.count > 0;
}

// --- Overlay emitters (append world-space segments to the sink) ---

// Real mesh edges (the renderer fans quads to tris for the shaded pass, so this
// overlay is the only way to see the quad topology). Dim solid color.
void emitWireframe(Scene &scene, LineSink &sink)
{
  auto &m = *scene.mesh;
  const float4 col(0.10f, 0.12f, 0.16f, 1.0f);
  int drawn = 0;
  for (int e : m.e) {
    auto ev = m.e.vs[e];
    sink.seg(m.v.co[ev[0]], m.v.co[ev[1]], col);
    if (++drawn >= kMaxWireEdges) {
      break;
    }
  }
}

// Per-vertex principal-curvature cross: blue = kmin direction, red = kmax. The
// estimator is run in prepareOverlays; here we just read the TEMP layers.
void emitCurvature(Scene &scene, RemeshApp &app, LineSink &sink)
{
  auto &m = *scene.mesh;
  BuiltinAttr<float3, ".remesh.v.kmin_dir", AttrFlag::TEMP> kmin_dir;
  BuiltinAttr<float3, ".remesh.v.kmax_dir", AttrFlag::TEMP> kmax_dir;
  kmin_dir.ensure(m.v.attrs);
  kmax_dir.ensure(m.v.attrs);

  float3 bmin, bmax;
  bool have = meshBBox(m, bmin, bmax);
  float diag = have ? (bmax - bmin).length() : 1.0f;
  float half = 0.5f * app.curvatureScale * (diag > 1e-8f ? diag : 1.0f);
  if (half <= 0.0f) {
    return;
  }
  const float4 colMin(0.24f, 0.47f, 1.0f, 1.0f); // kmin: blue
  const float4 colMax(1.0f, 0.27f, 0.20f, 1.0f); // kmax: red
  int drawn = 0;
  for (int v : m.v) {
    float3 p = m.v.co[v];
    sink.seg(p - kmin_dir[v] * half, p + kmin_dir[v] * half, colMin);
    sink.seg(p - kmax_dir[v] * half, p + kmax_dir[v] * half, colMax);
    if (++drawn >= kMaxFieldVerts) {
      break;
    }
  }
}

// Cross-field overlay: per-face 4-RoSy "+" glyphs (teal) reconstructed from the
// face frame + .remesh.f.theta, plus camera-facing singularity rings at verts
// whose .remesh.v.pole_index != 0 (orange = positive, blue = negative). With
// app.crossAnisotropy, glyph length and brightness scale with local curvature
// anisotropy (brightness, not alpha — the line pipeline has no blending).
void emitCrossField(Scene &scene, RemeshApp &app, LineSink &sink)
{
  auto &m = *scene.mesh;
  BuiltinAttr<float, ".remesh.f.theta", AttrFlag::TEMP> theta;
  theta.ensure(m.f.attrs);
  BuiltinAttr<short, ".remesh.v.pole_index", AttrFlag::TEMP> pole;
  pole.ensure(m.v.attrs);
  BuiltinAttr<float2, ".remesh.v.k", AttrFlag::TEMP> kk;
  if (app.crossAnisotropy) {
    kk.ensure(m.v.attrs);
  }

  float3 bmin, bmax;
  bool have = meshBBox(m, bmin, bmax);
  float diag = have ? (bmax - bmin).length() : 1.0f;
  float half = 0.5f * app.crossScale * (diag > 1e-8f ? diag : 1.0f);
  if (half <= 0.0f) {
    return;
  }

  int drawn = 0;
  for (int f : m.f) {
    float3 c, X, Y, N;
    faceCenterFrame(m, f, c, X, Y, N);
    float h = half;
    float bright = 1.0f;
    if (app.crossAnisotropy) {
      double aniso = 0.0;
      int an = 0, c0 = m.l.c[m.f.l[f]], cc = c0;
      do {
        float2 kv = kk[m.c.v[cc]];
        double num = std::fabs(double(kv[1]) - double(kv[0]));
        double den = std::fabs(double(kv[0])) + std::fabs(double(kv[1])) + 1e-6;
        aniso += num / den;
        an++;
        cc = m.c.next[cc];
      } while (cc != c0 && an < 64);
      if (an > 0) {
        aniso /= double(an);
      }
      h *= float(0.25 + 0.75 * aniso);
      bright = float(0.35 + 0.65 * aniso);
    }
    const float4 col(0.31f * bright, 0.90f * bright, 0.70f * bright, 1.0f);
    float t = theta[f];
    float3 d0 = X * std::cos(t) + Y * std::sin(t);
    float3 d1 = X * std::cos(t + kHalfPi) + Y * std::sin(t + kHalfPi);
    sink.seg(c - d0 * h, c + d0 * h, col);
    sink.seg(c - d1 * h, c + d1 * h, col);
    if (++drawn >= kMaxFieldVerts) {
      break;
    }
  }

  // Singularity markers: small camera-facing rings (12 segments) at each
  // non-regular vertex; radius grows with |pole_index|.
  const float ringR = 0.35f * half;
  for (int v : m.v) {
    int pi = pole[v];
    if (pi == 0) {
      continue;
    }
    float3 p = m.v.co[v];
    float3 toEye = sink.eye - p;
    if (toEye.lengthSqr() < 1e-12f) {
      continue;
    }
    float3 n = toEye;
    n.normalize();
    float3 right = n.cross(float3(0, 0, 1));
    if (right.lengthSqr() < 1e-8f) {
      right = float3(1, 0, 0);
    }
    right.normalize();
    float3 up = right.cross(n);
    up.normalize();
    float r = ringR * (1.0f + 0.5f * float(pi < 0 ? -pi : pi));
    const float4 col = pi > 0 ? float4(1.0f, 0.55f, 0.15f, 1.0f)  // +: orange
                              : float4(0.24f, 0.62f, 1.0f, 1.0f); // -: blue
    const int seg = 12;
    float3 prev;
    for (int i = 0; i <= seg; i++) {
      float a = float(i) / float(seg) * 6.2831853f;
      float3 q = p + (right * std::cos(a) + up * std::sin(a)) * r;
      if (i > 0) {
        sink.seg(prev, q, col);
      }
      prev = q;
    }
  }
}

// Per-edge field overlay. Mode 0 colours interior edges by their period jump
// (.remesh.e.period 0..3, 0 dim grey); mode 1 by the per-edge curl residual
// reduce(θ_b − θ_a − ρ) (blue ≈ smooth, red ≈ high curl).
void emitFieldEdges(Scene &scene, RemeshApp &app, LineSink &sink)
{
  auto &m = *scene.mesh;
  int drawn = 0;

  if (app.fieldEdgeMode == 0) {
    BuiltinAttr<short, ".remesh.e.period", AttrFlag::TEMP> period;
    period.ensure(m.e.attrs);
    const float4 cols[4] = {
        float4(0.28f, 0.28f, 0.32f, 1.0f), // 0: no jump (dim grey)
        float4(0.31f, 0.86f, 0.47f, 1.0f), // 1: green
        float4(0.94f, 0.78f, 0.24f, 1.0f), // 2: gold
        float4(0.94f, 0.31f, 0.78f, 1.0f), // 3: magenta
    };
    for (int e : m.e) {
      int fa, fb;
      if (!interiorEdgeFaces(m, e, fa, fb)) {
        continue;
      }
      int p = period[e];
      p = p < 0 ? 0 : (p > 3 ? 3 : p);
      auto ev = m.e.vs[e];
      sink.seg(m.v.co[ev[0]], m.v.co[ev[1]], cols[p]);
      if (++drawn >= kMaxWireEdges) {
        break;
      }
    }
    return;
  }

  // Curl-residual mode needs per-face frames.
  BuiltinAttr<float, ".remesh.f.theta", AttrFlag::TEMP> theta;
  theta.ensure(m.f.attrs);
  int fcap = int(m.f.capacity());
  litestl::util::Vector<float3> FX, FY, FN;
  FX.resize(fcap);
  FY.resize(fcap);
  FN.resize(fcap);
  for (int f : m.f) {
    sculptcore::remesh::faceFrame(m, f, FX[f], FY[f], FN[f]);
  }
  auto edgeAngle = [&](int e, int f) -> float {
    float3 d = m.v.co[m.e.vs[e][1]] - m.v.co[m.e.vs[e][0]];
    return std::atan2(d.dot(FY[f]), d.dot(FX[f]));
  };
  auto reduceQ = [](float x) -> float {
    return x - kHalfPi * std::round(x / kHalfPi);
  };

  for (int e : m.e) {
    int fa, fb;
    if (!interiorEdgeFaces(m, e, fa, fb)) {
      continue;
    }
    float rho = edgeAngle(e, fb) - edgeAngle(e, fa);
    float r = std::fabs(reduceQ(theta[fb] - theta[fa] - rho));
    float t = r / (kHalfPi * 0.5f); // normalize against π/4
    t = t > 1.0f ? 1.0f : t;
    const float4 col(0.24f + 0.74f * t, 0.35f, 0.86f - 0.70f * t, 1.0f);
    auto ev = m.e.vs[e];
    sink.seg(m.v.co[ev[0]], m.v.co[ev[1]], col);
    if (++drawn >= kMaxWireEdges) {
      break;
    }
  }
}

// Trace streamlines along the cross field across faces (4-RoSy: at each face
// crossing pick the arm best aligned with the incoming direction). Seeds are a
// strided subsample of faces, traced forward and backward; drawn as yellow
// polylines previewing quad-edge flow.
void emitStreamlines(Scene &scene, RemeshApp &app, LineSink &sink)
{
  auto &m = *scene.mesh;
  int fc = m.f.count;
  if (fc <= 0) {
    return;
  }
  BuiltinAttr<float, ".remesh.f.theta", AttrFlag::TEMP> theta;
  theta.ensure(m.f.attrs);

  float3 bmin, bmax;
  bool have = meshBBox(m, bmin, bmax);
  float diag = have ? (bmax - bmin).length() : 1.0f;
  if (diag < 1e-8f) {
    diag = 1.0f;
  }
  float step = app.streamlineScale * diag;
  if (step <= 0.0f) {
    return;
  }
  const int kMaxSteps = 220;
  const float4 col(0.98f, 0.94f, 0.47f, 1.0f); // yellow

  // March from (f, pos) along dir, hopping faces, appending world segments.
  auto trace = [&](int f, float3 pos, float3 dir) {
    float3 c, X, Y, N;
    faceCenterFrame(m, f, c, X, Y, N);
    float3 prev = pos;
    for (int s = 0; s < kMaxSteps; s++) {
      float3 np = pos + dir * step;
      float xs[64], ys[64];
      int npn = facePoly2D(m, f, c, X, Y, xs, ys, 64);
      float3 ppl = np - N * ((np - c).dot(N));
      bool inside = pnpoly(npn, xs, ys, (ppl - c).dot(X), (ppl - c).dot(Y));
      if (!inside) {
        int nf = neighborContaining(m, f, np);
        if (nf == ELEM_NONE) {
          break;
        }
        f = nf;
        faceCenterFrame(m, f, c, X, Y, N);
        dir = snapCross(theta[f], X, Y, dir);
        np = np - N * ((np - c).dot(N));
      } else {
        np = ppl;
      }
      sink.seg(prev, np, col);
      prev = np;
      pos = np;
    }
  };

  int want = app.streamlineSeeds > 0 ? app.streamlineSeeds : 1;
  int stride = fc > want ? fc / want : 1;
  int fi = 0, seeded = 0;
  for (int f : m.f) {
    if ((fi++ % stride) != 0) {
      continue;
    }
    if (seeded >= want) {
      break;
    }
    seeded++;
    float3 c, X, Y, N;
    faceCenterFrame(m, f, c, X, Y, N);
    float t = theta[f];
    float3 arm = X * std::cos(t) + Y * std::sin(t);
    trace(f, c, arm);
    trace(f, c, arm * -1.0f);
  }
}

// Heavy field computation, run once per frame on the MAIN thread before the
// render pass records the overlay (the emit* fns run inside the pass and must
// stay cheap). Creates and fills the TEMP field layers on first use.
void prepareOverlays(Scene &scene, RemeshApp &app)
{
  if (!scene.mesh) {
    return;
  }
  auto &m = *scene.mesh;
  if (app.showCrossField || app.showFieldEdges || app.showStreamlines) {
    ensureCrossField(m);
  }
  // computeCurvature fills kmin_dir/kmax_dir AND .remesh.v.k, so a single
  // ensure-gate on kmin_dir covers both the curvature overlay and anisotropy.
  if (app.showCurvature || (app.showCrossField && app.crossAnisotropy)) {
    BuiltinAttr<float3, ".remesh.v.kmin_dir", AttrFlag::TEMP> kmin_dir;
    bool created = kmin_dir.ensure(m.v.attrs);
    if (created ||
        app.curvatureOverlayIters != app.params.curvature_smooth_iters ||
        app.curvatureOverlayLambda != app.params.curvature_smooth_lambda) {
      sculptcore::remesh::computeCurvature(
          m, sculptcore::remesh::CurvatureParams{
                 app.params.curvature_smooth_iters,
                 app.params.curvature_smooth_lambda});
      app.curvatureOverlayIters = app.params.curvature_smooth_iters;
      app.curvatureOverlayLambda = app.params.curvature_smooth_lambda;
    }
  }
}

// Scene overlay hook: fires inside the swapchain render pass, after the
// axes/cursor overlays and before the ImGui post-draw hook. Accumulates every
// enabled overlay into one world-space line batch and submits it through the
// depth-tested line pipeline, so back-facing field elements are occluded by the
// front surface. Reused statically — called once per frame on the main thread.
void emitOverlays(void *user, sculptcore::gpu::GPUManager &mgr,
                  sculptcore::vulkan::VulkanBackend &backend, const mat4 &vp)
{
  OverlayCtx &ctx = *static_cast<OverlayCtx *>(user);
  RemeshApp &app = *ctx.app;
  Scene &scene = app.scene();
  if (!scene.mesh) {
    return;
  }
  LineSink &sink = *ctx.sink;
  sink.clear();
  sink.eye = scene.camera.eye;

  if (app.showWireframe) {
    emitWireframe(scene, sink);
  }
  if (app.showCurvature) {
    emitCurvature(scene, app, sink);
  }
  if (app.showFieldEdges) {
    emitFieldEdges(scene, app, sink);
  }
  if (app.showCrossField) {
    emitCrossField(scene, app, sink);
  }
  if (app.showStreamlines) {
    emitStreamlines(scene, app, sink);
  }
  if (sink.pos.size() >= 2) {
    scene.overlay.drawLines(mgr, backend, vp, sink.pos.data(), sink.clr.data(),
                            int(sink.pos.size()));
  }
}

void usage()
{
  std::fprintf(stderr,
               "remesh_debug_app [--asset NAME] [--width N] [--height N]\n"
               "  --asset NAME   load this asset (OBJ stem) on startup\n"
               "  Mouse: LMB/RMB orbit, Shift+LMB/MMB pan, scroll zoom.\n"
               "  Pipe:  \\\\.\\pipe\\sculpt-remesh-debug (send 'help').\n");
}

} // namespace

int main(int argc, char **argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  int width = 1280, height = 800;
  const char *asset = nullptr;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    auto next = [&](const char *n) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", n);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(a, "--asset") == 0) {
      asset = next("--asset");
    } else if (std::strcmp(a, "--width") == 0) {
      width = std::atoi(next("--width"));
    } else if (std::strcmp(a, "--height") == 0) {
      height = std::atoi(next("--height"));
    } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", a);
      usage();
      return 2;
    }
  }

  {
    Scene scene(width, height, /*headless=*/false);
    RemeshApp app(scene);
    app.rescanAssets();

    std::string err;
    if (asset) {
      if (!app.loadAsset(asset, err)) {
        std::fprintf(stderr, "load %s: %s\n", asset, err.c_str());
      }
    } else if (!scene.mesh && app.selected >= 0) {
      app.loadAsset(app.assets[app.selected], err);
    }

    if (!scene.ensureGPU() || !scene.window) {
      std::fprintf(stderr, "failed to bring up window/GPU\n");
      return 1;
    }

    InputDispatcher dispatcher;
    RemeshUi ui(&scene, &app);
    OrbitController controller(&scene);
    dispatcher.addHandler(&ui); // first: short-circuits the controller on ImGui focus
    dispatcher.addHandler(&controller);
    dispatcher.attach(scene.window->handle());

    if (!ui.init()) {
      std::fprintf(stderr, "Ui::init failed; continuing without panel\n");
    }

    LineSink overlaySink;
    OverlayCtx overlayCtx{&app, &overlaySink};
    scene.setOverlayDrawCB(emitOverlays, &overlayCtx);

    PipeServer pipe;
    if (!pipe.start(L"\\\\.\\pipe\\sculpt-remesh-debug",
                    [&app](const std::string &line) {
                      return app.handleCommand(line);
                    })) {
      std::fprintf(stderr, "warning: pipe server failed to start\n");
    } else {
      std::printf("pipe server: \\\\.\\pipe\\sculpt-remesh-debug\n");
    }

    while (!scene.window->shouldClose()) {
      scene.window->poll();
      pipe.drainMainThreadQueue(); // run queued pipe commands on this thread
      app.update();                // parse subprocess stdout, apply results
      ui.beginFrame();             // build the ImGui panel (rendered in renderWindow)
      prepareOverlays(scene, app); // heavy field compute, outside the render pass
      scene.renderWindow();        // shaded mesh + depth-tested overlays + panel
    }

    scene.setOverlayDrawCB(nullptr, nullptr);
    pipe.stop();
    ui.shutdown();
    dispatcher.detach();
  }

  if (litestl::alloc::getMemorySize() > 0) {
    std::printf("=== memory leaks: ===\n");
  }
  litestl::alloc::print_blocks(false);
  std::fflush(stdout);
  return 0;
}
