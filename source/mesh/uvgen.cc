#include "uvgen.h"

#include "attribute.h"
#include "attribute_bool.h"
#include "boundary.h"
#include "mesh.h"
#include "mesh_iter.h"

#include "litestl/util/vector.h"

#include <cmath>

namespace sculptcore::mesh {

using litestl::math::float2;
using litestl::math::float3;
using litestl::util::Vector;

namespace {

// Area-weighted face normal via Newell's method (robust for non-planar faces).
float3 faceAreaNormal(MeshBase *m, int f)
{
  float3 n(0.0f, 0.0f, 0.0f);
  int l = m->f.l[f];
  while (l != ELEM_NONE) {
    int cstart = m->l.c[l];
    int c = cstart;
    do {
      int cn = m->c.next[c];
      if (cn == ELEM_NONE) cn = cstart;
      float3 cur = m->v.co[m->c.v[c]];
      float3 nxt = m->v.co[m->c.v[cn]];
      n[0] += (cur[1] - nxt[1]) * (cur[2] + nxt[2]);
      n[1] += (cur[2] - nxt[2]) * (cur[0] + nxt[0]);
      n[2] += (cur[0] - nxt[0]) * (cur[1] + nxt[1]);
      c = cn;
    } while (c != cstart);
    l = m->l.next[l];
  }
  return n;
}

} // namespace

int generateUVFromSeams(MeshBase *m, const char *uvName, float margin)
{
  const int nf = m->f.count;
  const int nc = m->c.count;

  // 1. Flood-fill faces into charts, not crossing seam edges. Resolve the seam
  // bool view once instead of a string-keyed lookup per corner per face.
  BoolAttrView *seam = boundary::findBoolEdgeView(m, boundary::EDGE_SEAM);
  Vector<int> chartId;
  chartId.resize(nf);
  for (int f = 0; f < nf; f++) chartId[f] = -1;

  int nCharts = 0;
  Vector<int> stack;
  for (int f0 = 0; f0 < nf; f0++) {
    if (chartId[f0] != -1) continue;
    chartId[f0] = nCharts;
    stack.clear();
    stack.append(f0);
    while (stack.size() > 0) {
      int f = stack.pop_back();
      int l = m->f.l[f];
      while (l != ELEM_NONE) {
        int cstart = m->l.c[l];
        int c = cstart;
        do {
          int e = m->c.e[c];
          if (!(seam && seam->get(e))) {
            int rc0 = m->e.c[e];
            if (rc0 != ELEM_NONE) {
              int rc = rc0;
              do {
                int rf = m->l.f[m->c.l[rc]];
                if (chartId[rf] == -1) {
                  chartId[rf] = nCharts;
                  stack.append(rf);
                }
                rc = m->c.radial_next[rc];
              } while (rc != rc0 && rc != ELEM_NONE);
            }
          }
          c = m->c.next[c];
        } while (c != cstart && c != ELEM_NONE);
        l = m->l.next[l];
      }
    }
    nCharts++;
  }
  if (nCharts == 0) return 0;

  // 2. Per-chart group normal + tangent basis.
  Vector<float3> nrmSum, t1s, t2s;
  nrmSum.resize(nCharts);
  t1s.resize(nCharts);
  t2s.resize(nCharts);
  for (int c = 0; c < nCharts; c++) nrmSum[c] = float3(0.0f, 0.0f, 0.0f);
  for (int f = 0; f < nf; f++) nrmSum[chartId[f]] += faceAreaNormal(m, f);
  for (int c = 0; c < nCharts; c++) {
    float3 nrm = nrmSum[c];
    float len = nrm.length();
    nrm = len > 1e-12f ? nrm / len : float3(0.0f, 0.0f, 1.0f);
    float3 up = std::abs(nrm[2]) < 0.999f ? float3(0.0f, 0.0f, 1.0f)
                                          : float3(1.0f, 0.0f, 0.0f);
    float3 t1 = up.cross(nrm);
    float tl = t1.length();
    t1 = tl > 1e-12f ? t1 / tl : float3(1.0f, 0.0f, 0.0f);
    t1s[c] = t1;
    t2s[c] = nrm.cross(t1);
  }

  // 3. Project every corner onto its chart's plane; track per-chart bbox.
  Vector<float2> rawUV;
  rawUV.resize(nc);
  Vector<float> minX, minY, maxX, maxY;
  minX.resize(nCharts);
  minY.resize(nCharts);
  maxX.resize(nCharts);
  maxY.resize(nCharts);
  for (int c = 0; c < nCharts; c++) {
    minX[c] = minY[c] = 1e30f;
    maxX[c] = maxY[c] = -1e30f;
  }
  for (int c = 0; c < nc; c++) {
    int ch = chartId[m->l.f[m->c.l[c]]];
    float3 p = m->v.co[m->c.v[c]];
    float u = p.dot(t1s[ch]);
    float v = p.dot(t2s[ch]);
    rawUV[c] = float2(u, v);
    if (u < minX[ch]) minX[ch] = u;
    if (v < minY[ch]) minY[ch] = v;
    if (u > maxX[ch]) maxX[ch] = u;
    if (v > maxY[ch]) maxY[ch] = v;
  }

  // 4. Shelf box-pack chart bounding boxes (world scale preserved), wrapping at
  //    a square-ish row width, then scale the whole packing into [0,1].
  Vector<float> offX, offY;
  offX.resize(nCharts);
  offY.resize(nCharts);
  double sumArea = 0.0;
  for (int c = 0; c < nCharts; c++) {
    float w = (maxX[c] - minX[c]) + 2.0f * margin;
    float h = (maxY[c] - minY[c]) + 2.0f * margin;
    sumArea += double(w) * h;
  }
  float rowLimit = float(std::sqrt(sumArea));
  if (!(rowLimit > 0.0f)) rowLimit = 1.0f;

  float cx = 0.0f, cy = 0.0f, rowH = 0.0f, packW = 0.0f;
  for (int c = 0; c < nCharts; c++) {
    float w = (maxX[c] - minX[c]) + 2.0f * margin;
    float h = (maxY[c] - minY[c]) + 2.0f * margin;
    if (cx > 0.0f && cx + w > rowLimit) {
      cx = 0.0f;
      cy += rowH;
      rowH = 0.0f;
    }
    offX[c] = cx + margin; // chart bbox-min maps here
    offY[c] = cy + margin;
    cx += w;
    if (h > rowH) rowH = h;
    if (cx > packW) packW = cx;
  }
  float packH = cy + rowH;
  float scale = 1.0f / std::max(std::max(packW, packH), 1e-6f);

  // 5. Write packed per-corner UVs.
  AttrRef &uref = m->c.attrs.ensure(AttrType::FLOAT2, uvName, /*materialize=*/true);
  uref.use = AttrUse::UV;
  auto *uv = uref.get_data<float2>();
  for (int c = 0; c < nc; c++) {
    int ch = chartId[m->l.f[m->c.l[c]]];
    float u = (rawUV[c][0] - minX[ch] + offX[ch]) * scale;
    float v = (rawUV[c][1] - minY[ch] + offY[ch]) * scale;
    (*uv)[c] = float2(u, v);
  }

  return nCharts;
}

} // namespace sculptcore::mesh
