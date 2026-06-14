#include "test_util.h"

#include "litestl/math/vector.h"
#include "mesh/mesh.h"
#include "meshlog/attr_saver.h"

#include <cstdio>

test_init;

#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace sculptcore::meshlog;
using litestl::math::float3;

namespace {

/* A few loose verts is all the gate needs — it only touches the stamp column. */
void make_verts(Mesh &m, int n)
{
  for (int i = 0; i < n; i++) {
    m.make_vertex(float3(float(i), 0.0f, 0.0f));
  }
}

} // namespace

int main()
{
  /* add() maps categories to bits and accumulates a combined mask. */
  {
    Mesh m;
    make_verts(m, 4);
    AttrSaver<ElemType::VERTEX> saver;
    TASSERT(saver.ensure(m));        // freshly created
    TASSERT(!saver.ensure(m));       // idempotent: already bound
    int co = saver.add(m.v.co, CO);
    int no = saver.add(m.v.no, NO);
    TASSERT(co == CO && no == NO);
    TASSERT(saver.mask() == (CO | NO));
  }

  /* First touch of a stroke needs data; after updateSaved it doesn't. */
  {
    Mesh m;
    make_verts(m, 4);
    AttrSaver<ElemType::VERTEX> saver;
    saver.ensure(m);
    int mask = saver.add(m.v.co, CO) | saver.add(m.v.no, NO);

    const int stroke = 1;
    TASSERT(saver.needsData(2, stroke, mask));   // never saved -> needs data
    saver.updateSaved(2, stroke, mask);
    TASSERT(!saver.needsData(2, stroke, mask));  // saved this stroke -> skip
    /* A sibling element is independent. */
    TASSERT(saver.needsData(3, stroke, mask));
  }

  /* Incrementing the stroke id re-arms every element (flags treated cleared). */
  {
    Mesh m;
    make_verts(m, 2);
    AttrSaver<ElemType::VERTEX> saver;
    saver.ensure(m);
    int mask = saver.add(m.v.co, CO);

    saver.updateSaved(0, 1, mask);
    TASSERT(!saver.needsData(0, 1, mask));
    TASSERT(saver.needsData(0, 2, mask));        // new stroke -> needs data again
    saver.updateSaved(0, 2, mask);
    TASSERT(!saver.needsData(0, 2, mask));
  }

  /* Per-attr flags: saving CO alone leaves NO still pending; the combined mask
   * reports pending until both bits are set. */
  {
    Mesh m;
    make_verts(m, 2);
    AttrSaver<ElemType::VERTEX> saver;
    saver.ensure(m);
    int co = saver.add(m.v.co, CO);
    int no = saver.add(m.v.no, NO);
    int both = saver.mask();

    const int stroke = 5;
    saver.updateSaved(1, stroke, co);
    TASSERT(!saver.needsData(1, stroke, co));    // CO done
    TASSERT(saver.needsData(1, stroke, no));     // NO still pending
    TASSERT(saver.needsData(1, stroke, both));   // combined still pending
    saver.updateSaved(1, stroke, no);
    TASSERT(!saver.needsData(1, stroke, both));  // both now saved
  }

  /* resetElem clears the stamp so a recycled id re-arms even within a stroke. */
  {
    Mesh m;
    make_verts(m, 2);
    AttrSaver<ElemType::VERTEX> saver;
    saver.ensure(m);
    int mask = saver.add(m.v.co, CO);

    const int stroke = 7;
    saver.updateSaved(0, stroke, mask);
    TASSERT(!saver.needsData(0, stroke, mask));
    saver.resetElem(0);
    TASSERT(saver.needsData(0, stroke, mask));
  }

  /* Face-domain saver binds its own stamp column independently. */
  {
    Mesh m;
    make_verts(m, 3);
    int tri[3] = {0, 1, 2};
    m.make_face(std::span<int>(tri, 3));

    AttrSaver<ElemType::FACE> fsaver;
    TASSERT(fsaver.ensure(m));
    int mask = fsaver.add(m.f.no, NO);
    TASSERT(fsaver.needsData(0, 1, mask));
    fsaver.updateSaved(0, 1, mask);
    TASSERT(!fsaver.needsData(0, 1, mask));
  }

  return retval;
}
