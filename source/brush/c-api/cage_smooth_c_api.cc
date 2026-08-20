/** Cage-dab stroke c-api (CS2): the session surface a host drives when a
 * colour-smoothing stroke runs over multires — dabs execute on the cage colour
 * column at cage resolution and the touched grids re-derive per dab. See
 * brush/cage_smooth.h for the model. */

#include "brush/cage_smooth.h"

#include "litestl/util/alloc.h"

using namespace sculptcore;
using brush::CageSmoothSession;

extern "C" {

/** Open a cage-smooth session for `level` of `mr` (both outlive the session;
 * `brush` is the host's engine brush). Null on a bad level. */
CageSmoothSession *CageSmooth_new(subdiv::Multires *mr, int level, brush::Brush *brush)
{
  if (!mr || !brush || level < 1 || level > mr->maxLevel()) {
    return nullptr;
  }
  return litestl::alloc::New<CageSmoothSession>("cage smooth session", mr, level, brush);
}

void CageSmooth_free(CageSmoothSession *s)
{
  if (s) {
    litestl::alloc::Delete(s);
  }
}

/** Stroke-begin setup against the cage colour column `name` (ensured, and
 * white-flooded when fresh). 1 on success; 0 when the level has no
 * materialized slot mesh + tree, or the session is already begun. */
int CageSmooth_begin(CageSmoothSession *s, const char *name)
{
  if (!s) {
    return 0;
  }
  return s->begin(name) ? 1 : 0;
}

/** Batch of spaced dabs: MeshStroke_dabBatch's prop cycle and mirror loop over
 * the cage visit set. `dabs` = n x 7 {center.xyz, normal.xyz, radius}; `signs`
 * = mirrorCount x 3. Returns total visited-vert count, -1 on a bad call. */
int CageSmooth_dabBatch(CageSmoothSession *s,
                        int tool,
                        int n,
                        const float *dabs,
                        float strength,
                        int invert,
                        float pressure,
                        int usePressure,
                        const float *signs,
                        int mirrorCount)
{
  if (!s) {
    return -1;
  }
  return s->dabBatch(tool, n, dabs, strength, invert != 0, pressure, usePressure != 0,
                     signs, mirrorCount);
}

/** Stroke-end: close the executor step and restore the brush's policy bits.
 * Idempotent; CageSmooth_free calls it too. */
void CageSmooth_end(CageSmoothSession *s)
{
  if (s) {
    s->end();
  }
}

} // extern "C"
