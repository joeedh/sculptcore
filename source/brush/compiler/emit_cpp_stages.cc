/** Stage bodies and the kernel scaffolding around them: host and reduce
 * stages, the face kernel, save descriptors, the pre stage, the GPU pack. */

#include "emit_cpp_internal.h"

namespace sculptcore::brush::sbrush::cpp_emit {

// Emit one host stage as a templated free function. Host runs once per
// dab on CPU with direct access to ctx (CommandCtxBase) and the Brush
// — there's intentionally no per-node CommandCtx here, since the per-
// node loop hasn't started yet. Currently host stages take no params;
// any param-passing happens via ctx state.
void Emit::emitHostStage(const Stage &st, const string &lowerBrush)
{
  certifiedPrelude = PreparedPreludeProof(*brush).host(st);
  write("template <CommandTypes TYPES>\n");
  write("static void ");
  write(lowerBrush);
  write(capitalize(st.name));
  write("(CommandCtxBase &ctx, Brush &brush)\n");
  write("{\n");
  write("  (void)ctx; (void)brush;\n");
  emitTexCtxLocal();
  indent = 1;
  currentStage = &st;
  if (st.body && st.body->kind == StmtKind::Block) {
    int savedLocals = (int)locals.size();
    for (const auto &c : st.body->stmts)
      emitStmt(*c);
    while ((int)locals.size() > savedLocals)
      locals.pop_back();
  }
  currentStage = nullptr;
  certifiedPrelude = false;
  indent = 0;
  write("}\n\n");
}

// Emit one reduce stage as a templated free function. The signature
// mirrors the DSL source: each struct-typed param becomes a C++
// reference param, scalars stay by-value (in) or by-reference (out/inout).
void Emit::emitReduceStage(const Stage &st, const string &lowerBrush)
{
  certifiedPrelude = PreparedPreludeProof(*brush).reduce(st);
  write("template <CommandTypes TYPES>\n");
  write("static void ");
  write(lowerBrush);
  write(capitalize(st.name));
  write("(CommandCtx<TYPES> &ctx");
  for (const auto &p : st.params) {
    write(", ");
    if (p.type == TypeKind::Struct) {
      // Pass struct params by reference; const for pure-in to express
      // intent (and to allow temporaries down the line).
      if (p.dir == ParamDir::In)
        write("const ");
      write(p.structName);
      write(" &");
    } else {
      // Scalars: by-ref for out/inout, by-value for in.
      write(typeKindName(p.type));
      if (p.dir == ParamDir::Out || p.dir == ParamDir::InOut)
        write(" &");
    }
    write(" ");
    write(p.name);
  }
  write(")\n");
  write("{\n");
  emitTexCtxLocal();
  indent = 1;
  currentStage = &st;
  if (st.body && st.body->kind == StmtKind::Block) {
    int savedLocals = (int)locals.size();
    for (const auto &c : st.body->stmts)
      emitStmt(*c);
    while ((int)locals.size() > savedLocals)
      locals.pop_back();
  }
  currentStage = nullptr;
  certifiedPrelude = false;
  indent = 0;
  write("}\n\n");
}

bool Emit::exprUsesGrad(const Expr *e)
{
  if (!e)
    return false;
  if (e->kind == ExprKind::Call && std::strcmp(e->name.c_str(), "grad") == 0)
    return true;
  if (exprUsesGrad(e->lhs.get()) || exprUsesGrad(e->rhs.get()))
    return true;
  for (const auto &a : e->args)
    if (exprUsesGrad(a.get()))
      return true;
  return false;
}

bool Emit::stmtUsesGrad(const Stmt *s)
{
  if (!s)
    return false;
  if (exprUsesGrad(s->expr.get()) || exprUsesGrad(s->cond.get()) ||
      exprUsesGrad(s->lvalue.get()) || exprUsesGrad(s->rvalue.get()))
    return true;
  for (const auto &c : s->stmts)
    if (stmtUsesGrad(c.get()))
      return true;
  return stmtUsesGrad(s->thenBranch.get()) || stmtUsesGrad(s->elseBranch.get()) ||
         stmtUsesGrad(s->forInit.get()) || stmtUsesGrad(s->forStep.get());
}

bool Emit::brushUsesGrad() const
{
  for (const auto &st : brush->stages)
    if (stmtUsesGrad(st.body.get()))
      return true;
  return false;
}

bool Emit::brushUsesNeighbor() const
{
  return brushUsesNeighborLoop(*brush);
}

// Emit a `face` stage as the brush's primary kernel: walk the node's faces
// (BasicFaceIter, which exposes f.center/f.no + bound face attrs) and run the
// DSL body. Face attribute writes don't move geometry, so the node is flagged
// for GPU re-upload only. Reduce/host/neighbor are not supported on the face
// stage yet (poly-group paint needs none).
void Emit::emitFaceKernel(const string &lowerName)
{
  // AccMode is unused by the face stage (no vertex proxy / for_neighbor) but
  // is part of the signature so the create-fn can stamp def.exec uniformly.
  write("template <CommandTypes TYPES, sculptcore::brush::AccumMode AccMode>\n");
  write("static void ");
  write(lowerName);
  write("(CommandCtx<TYPES> &ctx)\n");
  write("{\n");
  write("  using namespace sculptcore::spatial;\n");
  write("  using namespace litestl::math;\n");
  write("  bool any_changed = false;\n");
  emitTexCtxLocal();
  for (const auto &f : brush->fields) {
    if (f.kind != FieldKind::Attr || f.domain != AttrDomain::Face)
      continue;
    write("  auto *__fattr_");
    write(f.name);
    write(" = ctx.template boundAttr<");
    write(attrCppType(f.type));
    write(">(\"");
    write(f.name);
    write("\"); (void)__fattr_");
    write(f.name);
    write(";\n");
  }
  write("  for (auto &");
  write(faceParamName);
  write(" : ctx.faceIter(ctx.node)) {\n");
  indent = 2;
  currentStage = faceStage;
  if (faceStage->body && faceStage->body->kind == StmtKind::Block) {
    int savedLocals = (int)locals.size();
    for (const auto &c : faceStage->body->stmts)
      emitStmt(*c);
    while ((int)locals.size() > savedLocals)
      locals.pop_back();
  }
  writeIndent();
  write("any_changed = true;\n");
  currentStage = nullptr;
  indent = 0;
  write("  }\n");
  write("  if (any_changed) {\n");
  if (kernelWritesGeomOnly()) {
    write("    ctx.node.update(Spatial_UpdateGPUGeom | Spatial_RegenBounds);\n");
  } else {
    write("    ctx.node.update(Spatial_UpdateGPU | Spatial_RegenBounds);\n");
  }
  write("  }\n");
  write("}\n\n");
}

// True when the kernel's write set (its `save` declarations; empty defaults
// to {v.co, v.no, f.no}) touches only geometry — such kernels flag the
// narrower Spatial_UpdateGPUGeom so the slice update skips attr-stream fills.
// @paint brushes write attribute streams by definition (they may not `save`
// them, e.g. polygroup) and always take the full-refresh flag.
bool Emit::kernelWritesGeomOnly() const
{
  if (brush->isPaint) {
    return false;
  }
  if (brush->saves.size() == 0) {
    return true;
  }
  for (const auto &s : brush->saves) {
    const char *n = s.name.c_str();
    const bool geom = (s.domain == AttrDomain::Vertex &&
                       (std::strcmp(n, "co") == 0 || std::strcmp(n, "no") == 0)) ||
                      (s.domain == AttrDomain::Face && std::strcmp(n, "no") == 0);
    if (!geom) {
      return false;
    }
  }
  return true;
}

// Map a `save` name to its undo-flag bit expression. co/no/mask/color are
// standard categories; anything else gets a fresh custom bit. customIdx is
// bumped for each custom attr so bits stay distinct within the 16-bit field.
string Emit::saveFlagExpr(const char *nm, int &customIdx)
{
  if (std::strcmp(nm, "co") == 0)
    return string("sculptcore::meshlog::CO");
  if (std::strcmp(nm, "no") == 0)
    return string("sculptcore::meshlog::NO");
  if (std::strcmp(nm, "mask") == 0)
    return string("sculptcore::meshlog::MASK");
  if (std::strcmp(nm, "color") == 0)
    return string("sculptcore::meshlog::COLOR");
  char buf[96];
  std::snprintf(
      buf, sizeof(buf), "(1 << (sculptcore::meshlog::CUSTOM_START + %d))", customIdx++);
  return string(buf);
}

// Emit one CaptureSaveDesc initializer for a `save`: its field kind, the attr
// handle (declared attrs only), and the codegen-fixed undo-flag bits. The
// resolve/capture work itself lives in TYPES::capture_policy (the domain
// seam) — see brush/capture_policy.h.
void Emit::emitSaveDesc(const SaveAttr &sv, AttrDomain dom, int &customIdx)
{
  const char *nm = sv.name.c_str();
  string flag = saveFlagExpr(nm, customIdx);

  write("    {sculptcore::brush::CaptureField::");
  if (std::strcmp(nm, "co") == 0) {
    if (dom != AttrDomain::Vertex)
      err("save: 'co' is only valid on the vertex domain");
    write("Co, nullptr, ");
  } else if (std::strcmp(nm, "no") == 0) {
    write("No, nullptr, ");
  } else if (std::strcmp(nm, "mask") == 0) {
    write("Mask, nullptr, ");
  } else {
    if (!findAttrField(stringref(nm)))
      err("save: unknown attribute (not builtin co/no/mask, nor a declared attr)");
    write("Attr, \"");
    write(nm);
    write("\", ");
  }
  write("int(");
  write(flag);
  write(")},\n");
}

// Pre-stage: undo capture through TYPES::capture_policy. The `save` set
// lowers to per-domain CaptureSaveDesc arrays; the policy resolves them and
// captures (the mesh policy is the historical AttrSaver + parallelCapture
// block). An empty `save` set defaults to {vertex co, vertex no, face no}.
void Emit::emitPreStage(const string &lowerName)
{
  Vector<SaveAttr> saves;
  if (brush->saves.size() == 0) {
    saves.append(SaveAttr{AttrDomain::Vertex, string("co")});
    saves.append(SaveAttr{AttrDomain::Vertex, string("no")});
    saves.append(SaveAttr{AttrDomain::Face, string("no")});
  } else {
    for (const auto &s : brush->saves)
      saves.append(s);
  }

  write("template <CommandTypes TYPES>\n");
  write("static void ");
  write(lowerName);
  write("Pre(CommandCtxBase &ctx, std::span<typename TYPES::node_type *> nodes)\n");
  write("{\n");

  int customIdx = 0;
  for (AttrDomain dom : {AttrDomain::Vertex, AttrDomain::Face}) {
    Vector<const SaveAttr *> domSaves;
    for (const auto &s : saves) {
      if (s.domain == dom)
        domSaves.append(&s);
    }
    if (domSaves.size() == 0)
      continue;

    const char *domEnum = (dom == AttrDomain::Vertex) ? "VERTEX" : "FACE";
    const char *arr = (dom == AttrDomain::Vertex) ? "__vsaves" : "__fsaves";

    write("  static const sculptcore::brush::CaptureSaveDesc ");
    write(arr);
    write("[] = {\n");
    for (const SaveAttr *s : domSaves)
      emitSaveDesc(*s, dom, customIdx);
    write("  };\n");
    write("  TYPES::capture_policy::template capture<sculptcore::mesh::ElemType::");
    write(domEnum);
    write(">(\n      ctx, nodes, std::span<const sculptcore::brush::CaptureSaveDesc>(");
    write(arr);
    write("));\n");
  }
  for (const auto &s : saves) {
    if (s.domain != AttrDomain::Vertex && s.domain != AttrDomain::Face)
      err("save: only vertex and face domains are supported");
  }
  write("}\n\n");
}

/** GPU appended-uniform marshal for this kernel: writes each non-builtin
 * DSL uniform into the ComputeBrushUniforms byte region after the 72-byte
 * fixed prelude, at the offset the WGSL uniform address space gives it
 * (scalars align 4, float3/float4 align 16 -- the layout the emitted WGSL
 * BrushUniforms block gets implicitly). Emitted for every kernel so the
 * registry can build a total dispatch; kernels that append nothing get an
 * empty body. @range clamps apply here -- they mirror the kernel's CPU-side
 * host-stage clamps (e.g. kelvinlet clampParams), which never lower to a
 * GPU backend. */
void Emit::emitGpuPack(const string &camelName)
{
  // emit_wgsl.cc's isBuiltinBrushName: names bound to fixed prelude slots,
  // never appended, so they must not consume appended offsets here either.
  auto isBuiltinBrushName = [](const char *n) {
    return std::strcmp(n, "strength") == 0 || std::strcmp(n, "radius") == 0 ||
           std::strcmp(n, "spacing") == 0 || std::strcmp(n, "invert") == 0 ||
           std::strcmp(n, "falloff_kind") == 0 ||
           std::strcmp(n, "falloff_shape") == 0 || std::strcmp(n, "falloff_dir") == 0 ||
           std::strcmp(n, "falloff_extent") == 0 ||
           std::strcmp(n, "unbounded_extent") == 0 ||
           std::strcmp(n, "coord_space") == 0 || std::strcmp(n, "tex_repeat") == 0 ||
           std::strcmp(n, "stroke_path_count") == 0;
  };
  write("/** Marshal this kernel's appended DSL uniforms (offset 72+) into a\n");
  write(" * ComputeBrushUniforms buffer; offsets mirror the kernel's WGSL\n");
  if (extrasMode) {
    write(
        " * BrushUniforms block. Extra GPU runtime dispatch is not implemented. */\n");
  } else {
    write(" * BrushUniforms block. Called via builtinBrushGpuPack from\n");
    write(" * packBrushUniforms (gpu_marshal.cc). */\n");
  }
  write("inline void pack");
  write(camelName);
  write("GpuUniforms(sculptcore::brush::Brush &brush, unsigned char *out)\n");
  write("{\n");
  write("  (void)brush;\n");
  write("  (void)out;\n");
  if (brush->isUnbounded) {
    write("  // @unbounded cutoff radius rides its fixed prelude slot (offset 44).\n");
    write("  std::memcpy(out + 44, &brush.unboundedExtent, 4);\n");
  }
  int off = 72;
  for (const auto &f : brush->fields) {
    if (f.kind != FieldKind::Uniform)
      continue;
    if (isBuiltinBrushName(f.name.c_str()))
      continue;
    int align = 4, size = 4;
    switch (f.type) {
    case TypeKind::Float:
    case TypeKind::Int:
    case TypeKind::Bool:
      break;
    case TypeKind::Float2:
      align = size = 8;
      break;
    case TypeKind::Float3:
      align = 16;
      size = 12;
      break;
    case TypeKind::Float4:
      align = size = 16;
      break;
    default:
      errf("GPU uniform marshal: unsupported type for uniform '%s'", f.name.c_str());
      return;
    }
    off = (off + align - 1) & ~(align - 1);
    if (off + size > 112) {
      errf("appended DSL uniforms overflow ComputeBrushUniforms (112 bytes) "
           "at '%s'",
           f.name.c_str());
      return;
    }
    char loc[64];
    std::snprintf(loc, sizeof(loc), "out + %d", off);
    if (extrasMode && fieldUsesStore(f)) {
      write("  {\n    ");
      write(f.type == TypeKind::Bool  ? "uint32_t"
            : f.type == TypeKind::Int ? "int32_t"
                                      : "float");
      write(" v = brush.getNamed");
      write(storeSuffix(f.type));
      write("(kExtraSlot_");
      write(f.name);
      write(f.type == TypeKind::Bool ? ") ? 1u : 0u;\n" : ");\n");
      if (f.hasRange && f.type != TypeKind::Float) {
        const double lower = std::ceil(
            std::max(f.rangeMin, f.type == TypeKind::Bool ? 0.0 : double(INT32_MIN)));
        const double upper = std::floor(
            std::min(f.rangeMax, f.type == TypeKind::Bool ? 1.0 : double(INT32_MAX)));
        write("    v = v < ");
        write(integralLiteral(lower));
        write(" ? ");
        write(integralLiteral(lower));
        write(" : (v > ");
        write(integralLiteral(upper));
        write(" ? ");
        write(integralLiteral(upper));
        write(" : v);\n");
      } else if (f.hasRange) {
        write("    v = v < ");
        write(floatLit(f.rangeMin));
        write(" ? ");
        write(floatLit(f.rangeMin));
        write(" : (v > ");
        write(floatLit(f.rangeMax));
        write(" ? ");
        write(floatLit(f.rangeMax));
        write(" : v);\n");
      }
      write("    std::memcpy(");
      write(loc);
      write(", &v, 4);\n  }\n");
    } else if (f.hasRange && f.type == TypeKind::Float) {
      write("  {\n    float v = brush.");
      write(f.name);
      write(";\n    v = v < ");
      write(floatLit(f.rangeMin));
      write(" ? ");
      write(floatLit(f.rangeMin));
      write(" : (v > ");
      write(floatLit(f.rangeMax));
      write(" ? ");
      write(floatLit(f.rangeMax));
      write(" : v);\n    std::memcpy(");
      write(loc);
      write(", &v, 4);\n  }\n");
    } else {
      char sz[16];
      std::snprintf(sz, sizeof(sz), "%d", size);
      write("  std::memcpy(");
      write(loc);
      write(", &brush.");
      write(f.name);
      write(", ");
      write(sz);
      write(");\n");
    }
    off += size;
  }
  write("}\n\n");
}

} // namespace sculptcore::brush::sbrush::cpp_emit
