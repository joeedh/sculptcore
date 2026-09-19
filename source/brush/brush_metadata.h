/** Not a standalone header: included once, from inside `namespace
 * sculptcore::brush { ... }`, at the point in brush_executor.h where
 * CommandExecutor's declaration ends (same convention as
 * brush_executor_templates.inl). Depends on CommandExecutor, so it cannot be
 * included ahead of it or on its own. */

/** Declared in accum_mode.h; AccumKind::Grab write-back uses it. First image to
 * write vert `v` this dab â†’ stamp curDabGen and return true (re-base from orig);
 * an already-stamped vert returns false (later image adds). The page was
 * pre-materialized in exec(), so this only reads/writes an existing slot. With no
 * stamp attr (single-image stroke, dab counter idle) every write re-bases. */
inline bool grabClaimFirstTouch(const CommandExecutor &exec, int v)
{
  mesh::AttrData<int> *dabGen = exec.ctx.dabGen;
  if (!dabGen) {
    return true;
  }
  if ((*dabGen)[v] == int(exec.ctx.curDabGen)) {
    return false;
  }
  (*dabGen)[v] = int(exec.ctx.curDabGen);
  return true;
}

/** Build a kernel's BrushCommandDef without a live stroke. csrNeighbors=false:
 * the manifest and the flags are identical for both neighbor policies. The
 * scratch Brush is what lets an extra (out-of-repo) kernel report its manifest â€”
 * createExtraBrush seeds uniform defaults into the Brush it is handed, so a null
 * one made every extra kernel report unhandled (and therefore empty), which is
 * precisely the case the grid-attr capability rule has to answer for. False when
 * `brushType` matched nothing. */
inline bool buildBrushDef(SculptBrushes brushType, CommandExecutor::brush_command &def)
{
  Brush scratch;
  return CommandExecutor::createCommandSwitch<AccumLive>(brushType, false, &scratch, def);
}

/** A kernel's declared attribute layers, into caller-owned storage. Prefer this
 * over BrushMetadata's bound instance methods in engine code: those park the
 * result on the query object (the binding runtime hands bound structs back by
 * pointer and cannot marshal a Vector), so two interleaved queries clobber each
 * other. False when `brushType` matched nothing; `out` is cleared either way. */
inline bool brushAttrManifestFor(SculptBrushes brushType,
                                 Vector<BrushAttrManifestEntry> &out)
{
  out.clear();
  CommandExecutor::brush_command def;
  if (!buildBrushDef(brushType, def)) {
    return false;
  }
  for (const auto &a : def.attrs) {
    out.append(a);
  }
  return true;
}

/** The manifest entry for one handle, or null. */
inline const BrushAttrManifestEntry *
findBrushAttrEntry(const Vector<BrushAttrManifestEntry> &manifest, const char *handle)
{
  util::string want(handle);
  for (const auto &a : manifest) {
    if (a.handle.operator==(want)) {
      return &a;
    }
  }
  return nullptr;
}

/** A kernel's codegen-set policy bits, queried by tool id without a live stroke.
 * All-false for an unknown or out-of-repo (extra) kernel. */
inline BrushDefFlags brushDefFlagsFor(SculptBrushes brushType)
{
  BrushDefFlags flags;
  CommandExecutor::brush_command def;
  if (!buildBrushDef(brushType, def)) {
    return flags;
  }
  flags.needsCoPrev = def.needsCoPrev;
  flags.accumulable = def.accumulable;
  flags.relaxesBase = def.relaxesBase;
  flags.grabModeCapable = def.grabModeCapable;
  flags.unbounded = def.unbounded;
  flags.incremental = def.incremental;
  flags.writesMask = def.writesMask;
  flags.faceMode = def.faceMode;
  for (const auto &a : def.attrs) {
    if (a.kernelWrites && (a.use & int(mesh::AttrUse::COLOR))) {
      flags.writesColor = true;
    }
    if (a.boundName.operator==(util::string(".boundary.vert.class"))) {
      flags.readsVclass = true;
    }
  }
  return flags;
}

/**
 * Stateless query object for a kernel's codegen-set metadata. Default-constructible
 * and stroke-independent (unlike CommandExecutor, which needs a spatial tree and a
 * live Brush), so a host can ask about any tool before â€” or without â€” a stroke and
 * drive its dab shaping / GPU kernel choice off the answer instead of a per-brush
 * conditional. Everything is addressed by index: the binding runtime can't marshal
 * a JS string into a `util::string` method arg.
 */
struct BrushMetadata {
  Vector<BrushAttrManifestEntry> queriedAttrs;
  // Result slots â€” the binding runtime hands bound structs back by pointer, so
  // the values have to outlive the call.
  BrushDefFlags queriedFlags;

  static litestl::binding::types::Struct<BrushMetadata> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<BrushMetadata> *st = new types::Struct<BrushMetadata>(
        "sculptcore::brush::BrushMetadata", sizeof(BrushMetadata));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, queryAttrManifest, MARGS("brushType"));
    BIND_STRUCT_METHOD(st, queriedAttrEntry, MARGS("idx"));
    BIND_STRUCT_METHOD(st, queryBrushFlags, MARGS("brushType"));
    return st;
  }

  /** Enumerate a kernel's declared attribute layers so a host can retarget each
   * retargetable handle (empty boundName, non-zero `use`) at the mesh layer active
   * for that AttrUse category. Returns the entry count. */
  int queryAttrManifest(int brushType)
  {
    brushAttrManifestFor(static_cast<SculptBrushes>(brushType), queriedAttrs);
    return int(queriedAttrs.size());
  }

  BrushAttrManifestEntry *queriedAttrEntry(int idx)
  {
    if (idx < 0 || idx >= int(queriedAttrs.size())) {
      return nullptr;
    }
    return &queriedAttrs[idx];
  }

  /** The kernel's policy bits. Valid until the next call; all-false for an
   * unknown or out-of-repo (extra) kernel. */
  BrushDefFlags *queryBrushFlags(int brushType)
  {
    queriedFlags = brushDefFlagsFor(static_cast<SculptBrushes>(brushType));
    return &queriedFlags;
  }
};
