
/**
this system is for quickly saving only attributes changed by
brushes.  it works by associating a list of attribute references
with bitmask flags, allowing us to detect if an element needs to
be stored in undo storage (either on the mesh itself or externally).

this is done by storing a stroke id and a set of bitmask flags of whether
attributes have been updated (for this id, flags are cleared when strokeid
increments).

we will reuse this a blender-style anchored brush mode too, that's the case that 
would be backed with mesh attributes.

example:

AttrSaver saver;
...build and initialize attributes to save
...for a vertex v:
if (saver.needsData(m.v.co, v, strokeId)) {
  ...store m.v.co[v] in either undo or mesh attribute or something
  saver.updateSaved(m.v.co, v, strokeId);
}

this system will be used by whatever replaces LogChunkSimple.
it would be nice if aforementioned replacement avoided hash tables.

see emit_cpp.cc:1087 and 1102-1106.
or one of the generated files, source\brush\kernels\generated\draw.brush.gen.h

*/

#include "../mesh/mesh.h"

namespace sculptcore::meshlog {
using namespace sculptcore::mesh;
using litestl::util::Vector;

template <ElemType type> constexpr auto attrName()
{
  if constexpr (type == ElemType::VERTEX) {
    return util::StrLiteral(".strokeid.vertex");
  }
  if constexpr (type == ElemType::EDGE) {
    return util::StrLiteral(".strokeid.edge");
  }
  if constexpr (type == ElemType::CORNER) {
    return util::StrLiteral(".strokeid.corner");
  }
  if constexpr (type == ElemType::LIST) {
    return util::StrLiteral(".strokeid.list");
  }
  if constexpr (type == ElemType::FACE) {
    return util::StrLiteral(".strokeid.face");
  }
  return util::StrLiteral("error in attr_saver.h");
}

// AttrSave.add (when implemented) should always map standard
// attributes to these flags,
// other attrs should start at 1<<CUSTOM_START and 
// go up from there
enum StandardUndoAttrs { CO = 1 << 0, NO = 1 << 1, COLOR = 1 << 2, MASK = 1 << 3, CUSTOM_START = 6 };

template <ElemType type, typename T, util::StrLiteral strokeIdName = attrName<type>()>
struct AttrSaver {
  struct AttrRefWithFlag {
    AttrRef ref;
    int flag;
  };

  // first 16 bits are stroke id, second 16 bits are flags
  BuiltinAttr<int, strokeIdName, AttrFlag::NOCOPY | AttrFlag::TEMP | AttrFlag::NOINTERP>
      strokeId;
  Vector<AttrRefWithFlag> attrs;

  bool needsData(AttrRef &ref, int e, int curStrokeId, int flag) const
  {
    int s = this->strokeId[e];
    int strokeId = s & (65535);
    if (strokeId != curStrokeId) {
      return true;
    }
    int flags = s >> 16;
    return flags & flag;
  }

  void needsData(AttrRef &ref, int e, int curStrokeId) const
  {
    for (auto &a : this->attrs) {
      if (a.ref == ref) {
        this->needsData(a.ref, e, a.flag);
      }
    }
  }

  void updateSaved(AttrRef &ref, int e, int curStrokeId, int flag)
  {
    int s = ref.get_data<int>()->operator[](e);
    int strokeId = s & (65535);
    int flags = curStrokeId == strokeId ? (s >> 16) | flag : flag;
    this->strokeId[e] = (flags << 16) | curStrokeId;
  }

  void updateSaved(AttrRef &ref, int e, int curStrokeId)
  {
    for (auto &a : this->attrs) {
      if (a.ref == ref) {
        this->updateSaved(a.ref, e, a.flag);
      }
    }
  }
};


} // namespace sculptcore::meshlog