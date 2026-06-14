#pragma once

/**
this system is for quickly saving only attributes changed by
brushes.  it works by associating a list of attribute references
with bitmask flags, allowing us to detect if an element needs to
be stored in undo storage (either on the mesh itself or externally).

it is the *gate* only: it answers "has element e's attribute(s) already been
saved this stroke?" without owning the saved bytes — the caller stores those
wherever it likes (the per-step element store, or a mesh attribute for the
blender-style anchored brush mode).

it works by stamping a per-element builtin int attribute (`.strokeid.<domain>`):
the low 16 bits hold the current stroke id, the high 16 bits a set of saved
flags. when the stroke id increments the flags are treated as cleared (a stamp
whose stored id != the current id means "nothing saved this stroke yet").

example:

  AttrSaver<ElemType::VERTEX> saver;
  saver.ensure(mesh);                 // bind/create the stamp column
  int co = saver.add(mesh.v.co, CO);  // register attrs -> stamp bits
  int no = saver.add(mesh.v.no, NO);
  int m  = saver.mask();              // == co | no
  ...for a vertex v:
  if (saver.needsData(v, strokeId, m)) {
    ...store mesh.v.co[v] / mesh.v.no[v] in undo (or a mesh attribute)
    saver.updateSaved(v, strokeId, m);
  }

this gate is element-keyed, so it stays correct when the spatial tree
restructures mid-stroke: the stamp lives on the element, not the node.

see documentation/plans/attr-saver.md.
*/

#include "../mesh/mesh.h"

namespace sculptcore::meshlog {
using namespace sculptcore::mesh;
using litestl::util::Vector;

// else-if chain so exactly one return survives — the branches return
// differently-sized StrLiteral<N>, which a trailing live return would clash with
// during return-type deduction.
template <ElemType type> constexpr auto attrName()
{
  if constexpr (type == ElemType::VERTEX) {
    return util::StrLiteral(".strokeid.vertex");
  } else if constexpr (type == ElemType::EDGE) {
    return util::StrLiteral(".strokeid.edge");
  } else if constexpr (type == ElemType::CORNER) {
    return util::StrLiteral(".strokeid.corner");
  } else if constexpr (type == ElemType::LIST) {
    return util::StrLiteral(".strokeid.list");
  } else {
    return util::StrLiteral(".strokeid.face");
  }
}

// AttrSaver::add maps standard attributes to these flags by category; other
// attrs start at 1<<CUSTOM_START and go up from there. Bits must fit in the
// 16-bit flags field (so bits 0..15; CUSTOM_START..15 is room for custom layers).
enum StandardUndoAttrs {
  CO = 1 << 0,
  NO = 1 << 1,
  COLOR = 1 << 2,
  MASK = 1 << 3,
  CUSTOM_START = 6,
};

template <ElemType type, util::StrLiteral strokeIdName = attrName<type>()>
struct AttrSaver {
  struct AttrRefWithFlag {
    AttrRef ref;
    int flag;
  };

  // low 16 bits = stroke id, high 16 bits = saved flags.
  static constexpr int SID_MASK = 0xffff;

  BuiltinAttr<int, strokeIdName, AttrFlag::NOCOPY | AttrFlag::TEMP | AttrFlag::NOINTERP>
      strokeId;
  Vector<AttrRefWithFlag> attrs;
  int mask_ = 0; // OR of every registered flag

  // Bind (or create) the stamp column on `group`. Idempotent: safe to call once
  // per dab. Returns true if the column was freshly created.
  bool ensure(AttrGroup &group)
  {
    return strokeId.ensure(group);
  }
  bool ensure(mesh::Mesh &m)
  {
    return ensure(groupForDomain(m));
  }

  // Register an attribute under `flag` (a StandardUndoAttrs bit / custom bit).
  // Returns `flag` so callers can OR up a combined mask inline.
  int add(const AttrRef &ref, int flag)
  {
    attrs.append(AttrRefWithFlag{ref, flag});
    mask_ |= flag;
    return flag;
  }

  // Combined mask of all registered attrs (the all-or-nothing capture flag).
  int mask() const
  {
    return mask_;
  }

  void clearRegistered()
  {
    attrs.clear();
    mask_ = 0;
  }

  // True if any bit in `flag` has not yet been saved for element `e` this
  // stroke. A stamp from a different stroke id means nothing is saved yet.
  bool needsData(int e, int curStrokeId, int flag) const
  {
    int s = strokeId[e];
    if ((s & SID_MASK) != (curStrokeId & SID_MASK)) {
      return true;
    }
    int flags = (s >> 16) & SID_MASK;
    return (flags & flag) != flag;
  }

  // Mark `flag` saved for element `e` at `curStrokeId`. Resets the flags when
  // the stored stroke id differs (first save of a new stroke).
  void updateSaved(int e, int curStrokeId, int flag)
  {
    int s = strokeId[e];
    int flags = ((s & SID_MASK) == (curStrokeId & SID_MASK)) ? ((s >> 16) & SID_MASK) : 0;
    flags |= flag;
    strokeId[e] = ((flags & SID_MASK) << 16) | (curStrokeId & SID_MASK);
  }

  // Clear an element's stamp (e.g. when its id is recycled for new geometry).
  void resetElem(int e)
  {
    strokeId[e] = 0;
  }

  static AttrGroup &groupForDomain(mesh::Mesh &m)
  {
    if constexpr (type == ElemType::VERTEX) {
      return m.v.attrs;
    } else if constexpr (type == ElemType::EDGE) {
      return m.e.attrs;
    } else if constexpr (type == ElemType::CORNER) {
      return m.c.attrs;
    } else if constexpr (type == ElemType::LIST) {
      return m.l.attrs;
    } else {
      return m.f.attrs; // FACE
    }
  }
};

} // namespace sculptcore::meshlog
