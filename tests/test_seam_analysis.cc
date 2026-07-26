/* Forensic harness for masked-stroke seam bugs. Reads a `writeMesh` blob
 * (extract the SCULPT00 container from a .wproj saved with LiteMesh's "Save
 * Temp Attributes" checkbox on) from the SC_SEAM_BLOB env var and correlates
 * every per-edge discontinuity — displacement-from-orig, stamped automask
 * factor, orig-normal angle, live-normal angle — against the saved
 * `.spatial.v.node` leaf-ownership boundaries, plus a single-ray least-squares
 * fit over the in-ramp factor population. No-ops (passes) when the env var is
 * unset so the suite stays green. */
#include "test_util.h"

#include "litestl/math/vector.h"
#include "mesh/mesh.h"
#include "mesh/mesh_serialize.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using litestl::math::float3;

template <typename T> static AttrData<T> *findAttr(Mesh &m, AttrType t, const char *name)
{
  if (!m.v.attrs.has(t, name)) {
    return nullptr;
  }
  return static_cast<AttrData<T> *>(m.v.attrs.find_attribute(t, name).data);
}

static void runAnalysis(const char *path)
{
  std::ifstream in(path, std::ios::binary);
  test_assert(bool(in));
  Mesh m;
  test_assert(serial::readMesh(m, in));
  printf("mesh: v=%d e=%d f=%d\n", m.v.count, m.e.count, m.f.count);

  for (auto &attr : m.v.attrs.attrs) {
    printf("  vattr '%s' type=%d flags=0x%x\n", attr.name.c_str(), int(attr.type),
           int(attr.flag));
  }

  auto *origCo = findAttr<float3>(m, AttrType::FLOAT3, ".brush.orig.co");
  auto *origNo = findAttr<float3>(m, AttrType::FLOAT3, ".brush.orig.no");
  auto *origGen = findAttr<int>(m, AttrType::INT, ".brush.orig.gen");
  // Older saves used ".brush.automask.factor" (combined product); current ones
  // carry the cavity-only cache under ".brush.automask.cavity".
  auto *fac = findAttr<float>(m, AttrType::FLOAT, ".brush.automask.factor");
  if (!fac) {
    fac = findAttr<float>(m, AttrType::FLOAT, ".brush.automask.cavity");
  }
  auto *facGen = findAttr<int>(m, AttrType::INT, ".brush.automask.gen");
  auto *vnode = findAttr<int>(m, AttrType::INT, ".spatial.v.node");
  printf("attrs: origCo=%d origNo=%d origGen=%d fac=%d facGen=%d vnode=%d\n",
         !!origCo, !!origNo, !!origGen, !!fac, !!facGen, !!vnode);

  if (origGen) {
    std::map<int, int> hist;
    for (int v : m.v) {
      hist[origGen->safe_get(v)]++;
    }
    printf("orig.gen histogram:");
    for (auto &kv : hist) {
      printf(" [%d]=%d", kv.first, kv.second);
    }
    printf("\n");
  }
  int modalGen = 0;
  if (origGen) {
    std::map<int, int> hist;
    for (int v : m.v) {
      hist[origGen->safe_get(v)]++;
    }
    int best = 0;
    for (auto &kv : hist) {
      if (kv.first != 0 && kv.second > best) {
        best = kv.second;
        modalGen = kv.first;
      }
    }
  }
  printf("modal orig gen = %d\n", modalGen);

  int nodeBoundaryEdges = 0, totalEdges = 0;
  struct Offender {
    float score;
    int e, a, b;
  };
  auto correlate = [&](const char *label, auto valueOf) {
    // valueOf(v) -> float or NaN (skip). Reports the boundary share among the
    // top-1% jump edges vs the base boundary share.
    struct EdgeJump {
      float jump;
      bool boundary;
      int e;
    };
    std::vector<EdgeJump> jumps;
    for (int e : m.e) {
      int a = m.e.vs[e][0], b = m.e.vs[e][1];
      float va = valueOf(a), vb = valueOf(b);
      if (std::isnan(va) || std::isnan(vb)) {
        continue;
      }
      bool boundary = vnode && vnode->safe_get(a) != vnode->safe_get(b);
      jumps.push_back({std::fabs(va - vb), boundary, e});
    }
    if (jumps.empty()) {
      printf("%s: no data\n", label);
      return;
    }
    int nBoundary = 0;
    for (auto &j : jumps) {
      nBoundary += j.boundary;
    }
    std::sort(jumps.begin(), jumps.end(),
              [](const EdgeJump &x, const EdgeJump &y) { return x.jump > y.jump; });
    int top = std::max(1, int(jumps.size() / 100));
    int topBoundary = 0;
    for (int i = 0; i < top; i++) {
      topBoundary += jumps[i].boundary;
    }
    printf("%s: edges=%d base-boundary=%.1f%% | top-1%% (n=%d) max=%g "
           "boundary=%.1f%%\n",
           label, int(jumps.size()), 100.0f * nBoundary / jumps.size(), top,
           jumps[0].jump, 100.0f * topBoundary / top);
    for (int i = 0; i < std::min(5, top); i++) {
      int e = jumps[i].e;
      int a = m.e.vs[e][0], b = m.e.vs[e][1];
      printf("   top%d e=%d jump=%g a=%d(n%d) b=%d(n%d) co.a=(%.4f %.4f %.4f)\n", i,
             e, jumps[i].jump, a, vnode ? vnode->safe_get(a) : -1, b,
             vnode ? vnode->safe_get(b) : -1, m.v.co[a][0], m.v.co[a][1],
             m.v.co[a][2]);
    }
  };

  if (vnode) {
    for (int e : m.e) {
      totalEdges++;
      if (vnode->safe_get(m.e.vs[e][0]) != vnode->safe_get(m.e.vs[e][1])) {
        nodeBoundaryEdges++;
      }
    }
    printf("ownership: %d/%d edges are node-boundary\n", nodeBoundaryEdges, totalEdges);
  }

  if (origCo && origGen) {
    correlate("disp |co-origCo|", [&](int v) -> float {
      if (origGen->safe_get(v) != modalGen) {
        return NAN;
      }
      return (m.v.co[v] - origCo->safe_get(v)).length();
    });
  }
  if (fac && facGen) {
    correlate("automask factor", [&](int v) -> float {
      if (facGen->safe_get(v) != modalGen) {
        return NAN;
      }
      return fac->safe_get(v);
    });
  }
  if (origNo && origGen) {
    correlate("orig-normal z", [&](int v) -> float {
      if (origGen->safe_get(v) != modalGen) {
        return NAN;
      }
      float3 n = origNo->safe_get(v);
      float l = n.length();
      return l > 1e-9f ? n[2] / l : NAN;
    });
  }
  m.recalc_normals();
  correlate("live-normal z", [&](int v) -> float {
    float3 n = m.v.no[v];
    float l = n.length();
    return l > 1e-9f ? n[2] / l : NAN;
  });

  // Per-seam forensics: for the worst factor-jump edges, print both sides'
  // stamped factor, the FULL angle between their orig normals, and each side's
  // displacement from its orig position. Smooth orig normals + near-binary
  // factor jumps would prove the two sides were stamped with different inputs
  // (e.g. a stroke-gen collision reusing stale stamps).
  if (fac && facGen && origNo && origCo && origGen) {
    struct FE {
      float jump;
      int e;
    };
    std::vector<FE> fes;
    for (int e : m.e) {
      int a = m.e.vs[e][0], b = m.e.vs[e][1];
      if (facGen->safe_get(a) != modalGen || facGen->safe_get(b) != modalGen) {
        continue;
      }
      fes.push_back({std::fabs(fac->safe_get(a) - fac->safe_get(b)), e});
    }
    std::sort(fes.begin(), fes.end(),
              [](const FE &x, const FE &y) { return x.jump > y.jump; });
    printf("seam forensics (top factor-jump edges):\n");
    for (int i = 0; i < std::min<size_t>(8, fes.size()); i++) {
      int e = fes[i].e;
      int a = m.e.vs[e][0], b = m.e.vs[e][1];
      float3 na = origNo->safe_get(a), nb = origNo->safe_get(b);
      float la = na.length(), lb = nb.length();
      float ang = 0.0f;
      if (la > 1e-9f && lb > 1e-9f) {
        float d = na.dot(nb) / (la * lb);
        d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
        ang = std::acos(d) * 180.0f / 3.14159265f;
      }
      float dispA = (m.v.co[a] - origCo->safe_get(a)).length();
      float dispB = (m.v.co[b] - origCo->safe_get(b)).length();
      printf("  e=%d n%d|n%d fac %.4f|%.4f origNoAngle=%.2fdeg disp %.4f|%.4f\n", e,
             vnode ? vnode->safe_get(a) : -1, vnode ? vnode->safe_get(b) : -1,
             fac->safe_get(a), fac->safe_get(b), ang, dispA, dispB);
    }
  }

  // Ray forensics. Assume the interactive settings (limit 90deg, falloff
  // 75deg): an in-ramp factor determines the angle to the ray, so the in-ramp
  // population overdetermines a single ray (linear least squares on
  // n-hat . r = -cos(theta_v)). The fit residual says whether ONE ray explains
  // the real-valued block; evaluating the fitted ray on the factor==1.0
  // population says whether those verts could have come from the same ray.
  if (fac && facGen && origNo) {
    const float kLimit = 3.14159265f / 2.0f;
    const float kFalloff = 75.0f * 3.14159265f / 180.0f;
    double AtA[3][3] = {}, Atb[3] = {};
    int nRamp = 0, nOne = 0, nZeroish = 0, nStamped = 0;
    for (int v : m.v) {
      if (facGen->safe_get(v) != modalGen) {
        continue;
      }
      nStamped++;
      float f = fac->safe_get(v);
      if (f >= 0.9999f) {
        nOne++;
      } else if (f <= 0.02f) {
        nZeroish++;
      }
      if (f <= 0.02f || f >= 0.98f) {
        continue;
      }
      float3 n = origNo->safe_get(v);
      float l = n.length();
      if (l < 1e-9f) {
        continue;
      }
      n = n / l;
      double c = std::cos(double(kLimit) - double(f) * double(kFalloff));
      // n . r = -c
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          AtA[i][j] += double(n[i]) * double(n[j]);
        }
        Atb[i] += double(n[i]) * (-c);
      }
      nRamp++;
    }
    printf("factor census: stamped=%d exact-1.0=%d near-0=%d in-ramp=%d\n", nStamped,
           nOne, nZeroish, nRamp);
    if (nRamp > 100) {
      // Solve the 3x3 normal equations by Cramer's rule.
      auto det3 = [](double a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
               a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
      };
      double D = det3(AtA);
      float3 ray{0, 0, 0};
      if (std::fabs(D) > 1e-12) {
        for (int k = 0; k < 3; k++) {
          double M[3][3];
          std::memcpy(M, AtA, sizeof(M));
          for (int i = 0; i < 3; i++) {
            M[i][k] = Atb[i];
          }
          ray[k] = float(det3(M) / D);
        }
      }
      float rl = ray.length();
      printf("fitted ray = (%.4f %.4f %.4f) |r|=%.4f\n", ray[0], ray[1], ray[2], rl);
      if (rl > 1e-6f) {
        float3 rhat = ray / rl;
        auto predict = [&](const float3 &noRaw) {
          float l = noRaw.length();
          if (l < 1e-9f) {
            return 1.0f;
          }
          float d = -noRaw.dot(rhat) / l;
          d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
          d = std::fabs(d); // cull off
          float angle = std::acos(d);
          if (angle >= kLimit) {
            return 0.0f;
          }
          float rampStart = kLimit - kFalloff;
          if (angle <= rampStart) {
            return 1.0f;
          }
          return (kLimit - angle) / kFalloff;
        };
        double rampErr = 0, oneSum = 0;
        int rampN = 0, oneN = 0;
        for (int v : m.v) {
          if (facGen->safe_get(v) != modalGen) {
            continue;
          }
          float f = fac->safe_get(v);
          float p = predict(origNo->safe_get(v));
          if (f > 0.02f && f < 0.98f) {
            rampErr += double(f - p) * double(f - p);
            rampN++;
          } else if (f >= 0.9999f) {
            oneSum += p;
            oneN++;
          }
        }
        printf("in-ramp fit RMS=%.4f (n=%d) | exact-1.0 verts: mean predicted "
               "factor under fitted ray = %.4f (n=%d)\n",
               rampN ? std::sqrt(rampErr / rampN) : -1.0, rampN,
               oneN ? oneSum / oneN : -1.0, oneN);
      }
    }
  }
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char *path = std::getenv("SC_SEAM_BLOB");
  if (!path) {
    printf("SC_SEAM_BLOB unset; diagnostic skipped\n");
    return test_end();
  }
  // Scoped in a function so the Mesh destructs before test_end()'s leak gate.
  runAnalysis(path);
  return test_end();
}
