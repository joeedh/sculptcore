#include "emit_wgsl.h"
#include "../kernels/ir/intrinsics.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

namespace {

// WGSL spelling for primitive types. Matches the IR's TypeKind set; the
// engine has no WGSL-specific types yet, so anything unknown lowers to
// `f32` to keep tint moving — typed parsing rejects truly bogus input
// upstream.
const char *wgslType(TypeKind k)
{
  switch (k) {
  case TypeKind::Void: return "void";
  case TypeKind::Bool: return "bool";
  case TypeKind::Int: return "i32";
  case TypeKind::Float: return "f32";
  case TypeKind::Float2: return "vec2<f32>";
  case TypeKind::Float3: return "vec3<f32>";
  case TypeKind::Float4: return "vec4<f32>";
  default: return "f32";
  }
}

bool hasNeighborLoop(const Stmt *s)
{
  if (!s) return false;
  if (s->kind == StmtKind::NeighborLoop) return true;
  for (const auto &c : s->stmts) {
    if (hasNeighborLoop(c.get())) return true;
  }
  if (hasNeighborLoop(s->thenBranch.get())) return true;
  if (hasNeighborLoop(s->elseBranch.get())) return true;
  return false;
}

struct Emit {
  const Brush *brush;
  const Stage *vertexStage = nullptr;
  string vertexParamName;  // e.g. "v"

  // Stage currently being lowered — drives stage-param identifier
  // resolution (so reduce-body `s` and vertex-body `v` route correctly).
  const Stage *currentStage = nullptr;

  string out;
  Vector<string> errors;
  int indent = 0;

  Vector<string> locals;

  void err(const char *msg) { errors.append(string(msg)); }
  void errf(const char *fmt, const char *arg)
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, arg);
    errors.append(string(buf));
  }

  void writeIndent()
  {
    for (int i = 0; i < indent; i++) out += "  ";
  }
  void write(const char *s) { out += s; }
  void write(const string &s) { out += s; }

  bool isLocal(stringref name) const
  {
    for (const auto &l : locals) {
      if (string(l).operator==(string(name.c_str()))) return true;
    }
    return false;
  }

  const Field *findField(stringref name) const
  {
    for (const auto &f : brush->fields) {
      if (string(f.name).operator==(string(name.c_str()))) return &f;
    }
    return nullptr;
  }

  bool isStageParam(stringref name) const
  {
    if (!currentStage) return false;
    for (const auto &p : currentStage->params) {
      if (string(p.name).operator==(string(name.c_str()))) return true;
    }
    return false;
  }

  // Reduce-stage out/inout struct params are lowered to WGSL `ptr<function, T>`,
  // so identifier references to them have to be dereferenced inline — e.g.
  // `s.a = x` lowers to `(*s).a = x`.
  bool isStructPtrParam(stringref name) const
  {
    if (!currentStage) return false;
    for (const auto &p : currentStage->params) {
      if (!string(p.name).operator==(string(name.c_str()))) continue;
      return p.type == TypeKind::Struct &&
             (p.dir == ParamDir::Out || p.dir == ParamDir::InOut);
    }
    return false;
  }

  bool isVertexParam(stringref name) const
  {
    if (!vertexStage) return false;
    // Only the first param of the vertex stage is the Vertex bundle; the
    // rest are struct-typed locals which share the regular ident path.
    if (vertexStage->params.size() == 0) return false;
    const auto &p = vertexStage->params[0];
    return string(p.name).operator==(string(name.c_str()));
  }

  // === expression emitter ===

  void emitExpr(const Expr &e)
  {
    switch (e.kind) {
    case ExprKind::LitFloat: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", e.fvalue);
      bool hasDot = false;
      for (const char *p = buf; *p; p++) {
        if (*p == '.' || *p == 'e' || *p == 'E') { hasDot = true; break; }
      }
      out += buf;
      if (!hasDot) out += ".0";
      // No 'f' suffix — older WGSL didn't accept it, and untyped float
      // literals coerce fine in every position we emit.
      break;
    }
    case ExprKind::LitInt: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%lld", e.ivalue);
      out += buf;
      break;
    }
    case ExprKind::LitBool:
      out += e.bvalue ? "true" : "false";
      break;
    case ExprKind::Ident: {
      stringref nm(e.name.c_str());
      if (isStructPtrParam(nm)) {
        out += "(*";
        out += e.name;
        out += ")";
      } else if (isLocal(nm) || isStageParam(nm)) {
        out += e.name;
      } else if (auto *f = findField(nm)) {
        if (f->kind == FieldKind::Uniform) {
          out += "brush_u.";
          out += e.name;
        } else {
          out += "ctx_u.";
          out += e.name;
        }
      } else {
        out += e.name;
      }
      break;
    }
    case ExprKind::Member: {
      // Vertex-param member access (`v.co`, `v.no`, `v.mask`) targets
      // local mutable vars seeded from the per-thread storage loads.
      if (e.lhs && e.lhs->kind == ExprKind::Ident && isVertexParam(stringref(e.lhs->name.c_str()))) {
        out += e.lhs->name;
        out += "_";
        out += e.name;
      } else {
        emitExpr(*e.lhs);
        out += ".";
        out += e.name;
      }
      break;
    }
    case ExprKind::Binary:
      out += "(";
      emitExpr(*e.lhs);
      out += " ";
      out += binOpCSym(e.binop);
      out += " ";
      emitExpr(*e.rhs);
      out += ")";
      break;
    case ExprKind::Unary:
      out += "(";
      out += unaryOpCSym(e.unaryop);
      emitExpr(*e.lhs);
      out += ")";
      break;
    case ExprKind::Paren:
      out += "(";
      emitExpr(*e.lhs);
      out += ")";
      break;
    case ExprKind::Call: {
      // float2/3/4 type-constructor calls in the source become WGSL
      // vec*<f32>() constructors. The IR doesn't tag these specially
      // (parser just sees Ident + LParen), so the emitter has to match
      // by name.
      const char *n = e.name.c_str();
      if (std::strcmp(n, "float2") == 0 || std::strcmp(n, "float3") == 0 ||
          std::strcmp(n, "float4") == 0) {
        out += "vec";
        out += n[5];
        out += "<f32>(";
        for (int i = 0; i < (int)e.args.size(); i++) {
          if (i > 0) out += ", ";
          emitExpr(*e.args[i]);
        }
        out += ")";
        break;
      }

      const IntrinsicDef *intr = findIntrinsic(stringref(e.name.c_str()));
      if (intr) {
        const char *pat = intr->emit[(int)BackendKind::Wgsl].pattern;
        if (!pat) {
          string msg = string("intrinsic '") + e.name + "' has no WGSL emit pattern";
          errors.append(msg);
          out += "/*missing-wgsl-intrinsic*/";
          break;
        }
        Vector<string> rendered;
        for (const auto &a : e.args) {
          string saved = out;
          out = string("");
          emitExpr(*a);
          rendered.append(out);
          out = saved;
        }
        for (const char *p = pat; *p; ) {
          if (*p == '$' && std::isdigit((unsigned char)p[1])) {
            int idx = p[1] - '0';
            p += 2;
            if (idx < (int)rendered.size()) {
              out += rendered[idx];
            } else {
              out += "/*bad-arg*/";
            }
          } else {
            char tmp[2] = {*p, 0};
            out += tmp;
            p++;
          }
        }
      } else {
        // Unknown name — defer to WGSL's resolver and let tint flag it.
        out += e.name;
        out += "(";
        for (int i = 0; i < (int)e.args.size(); i++) {
          if (i > 0) out += ", ";
          emitExpr(*e.args[i]);
        }
        out += ")";
      }
      break;
    }
    }
  }

  // === statement emitter ===

  // WGSL has no `continue` outside a loop — and our kernel body is the
  // bare per-vertex code, no enclosing for-loop — so a source `continue;`
  // (i.e. "skip this vertex") lowers to `return;`. That also matches the
  // C++ executor's effect: the post-iteration `affected_verts.append`
  // doesn't run for skipped verts there either.
  void emitStmt(const Stmt &s)
  {
    switch (s.kind) {
    case StmtKind::Block: {
      writeIndent(); out += "{\n";
      indent++;
      int savedLocals = (int)locals.size();
      for (const auto &c : s.stmts) emitStmt(*c);
      while ((int)locals.size() > savedLocals) locals.pop_back();
      indent--;
      writeIndent(); out += "}\n";
      break;
    }
    case StmtKind::DeclLocal:
      writeIndent();
      out += "var ";
      out += s.name;
      out += ": ";
      if (s.declType == TypeKind::Struct) out += s.declStructName;
      else out += wgslType(s.declType);
      if (s.expr) {
        out += " = ";
        emitExpr(*s.expr);
      }
      out += ";\n";
      locals.append(s.name);
      break;
    case StmtKind::Assign:
      writeIndent();
      emitExpr(*s.lvalue);
      out += " ";
      out += assignOpCSym(s.assignOp);
      out += " ";
      emitExpr(*s.rvalue);
      out += ";\n";
      break;
    case StmtKind::If: {
      writeIndent();
      out += "if (";
      emitExpr(*s.cond);
      out += ") ";
      if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
        out += "{\n";
        indent++;
        int savedLocals = (int)locals.size();
        for (const auto &c : s.thenBranch->stmts) emitStmt(*c);
        while ((int)locals.size() > savedLocals) locals.pop_back();
        indent--;
        writeIndent(); out += "}";
      } else if (s.thenBranch) {
        out += "{\n";
        indent++;
        emitStmt(*s.thenBranch);
        indent--;
        writeIndent(); out += "}";
      }
      if (s.elseBranch) {
        out += " else ";
        if (s.elseBranch->kind == StmtKind::Block) {
          out += "{\n";
          indent++;
          int savedLocals = (int)locals.size();
          for (const auto &c : s.elseBranch->stmts) emitStmt(*c);
          while ((int)locals.size() > savedLocals) locals.pop_back();
          indent--;
          writeIndent(); out += "}\n";
        } else if (s.elseBranch->kind == StmtKind::If) {
          emitStmt(*s.elseBranch);
        } else {
          out += "{\n";
          indent++;
          emitStmt(*s.elseBranch);
          indent--;
          writeIndent(); out += "}\n";
        }
      } else {
        out += "\n";
      }
      break;
    }
    case StmtKind::Continue:
      writeIndent(); out += "return;\n";
      break;
    case StmtKind::Return:
      writeIndent();
      out += "return";
      if (s.expr) { out += " "; emitExpr(*s.expr); }
      out += ";\n";
      break;
    case StmtKind::ExprStmt:
      writeIndent();
      emitExpr(*s.expr);
      out += ";\n";
      break;
    case StmtKind::NeighborLoop:
      // Should have been caught up-front in run(); reaching here would
      // mean emit_wgsl was called on a brush we explicitly skip.
      err("for_neighbor reached WGSL emitter (should have been skipped earlier)");
      break;
    }
  }

  // === top-level emitter ===

  void emitPrelude()
  {
    // Fixed schema — every kernel binds the same layout, so the host can
    // share a single bind-group setup across brushes. Vec3 fields land on
    // their natural 16-byte boundaries in uniform address space; no
    // explicit padding needed for this layout.
    write("// AUTO-GENERATED by sbrushc (WGSL backend) — DO NOT EDIT.\n");
    write("// Source: ");
    write(brush->sourceFile);
    write("\n\n");

    // User-defined struct decls. WGSL allows them at module scope and
    // they're nameable from both reduce and vertex functions.
    for (const auto &sd : brush->structs) {
      write("struct ");
      write(sd.name);
      write(" {\n");
      for (const auto &f : sd.fields) {
        write("  ");
        write(f.name);
        write(": ");
        write(wgslType(f.type));
        write(",\n");
      }
      write("};\n\n");
    }

    // Names that are already members of the fixed BrushUniforms /
    // CtxUniforms blocks. DSL fields with these names re-bind to the
    // existing slot rather than getting re-emitted (and tripping tint).
    auto isBuiltinBrushName = [](const char *n) {
      return std::strcmp(n, "strength") == 0 || std::strcmp(n, "radius") == 0 ||
             std::strcmp(n, "spacing") == 0  || std::strcmp(n, "invert") == 0;
    };
    auto isBuiltinCtxName = [](const char *n) {
      return std::strcmp(n, "surfacePos") == 0 || std::strcmp(n, "surfaceNo") == 0;
    };

    write("struct BrushUniforms {\n");
    write("  strength: f32,\n");
    write("  radius: f32,\n");
    write("  spacing: f32,\n");
    write("  invert: u32,\n");
    // Spill brush-uniform fields declared by the DSL into the uniform
    // block so reduce/vertex can reference them. Wave 4 slice keeps the
    // packing trivial — scalars and vec3/vec4 align naturally on 16-byte
    // boundaries in the uniform address space.
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Uniform) continue;
      if (isBuiltinBrushName(f.name.c_str())) continue;
      write("  ");
      write(f.name);
      write(": ");
      write(wgslType(f.type));
      write(",\n");
    }
    write("};\n\n");

    write("struct CtxUniforms {\n");
    write("  surfacePos: vec3<f32>,\n");
    write("  surfaceNo: vec3<f32>,\n");
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Ctx) continue;
      if (isBuiltinCtxName(f.name.c_str())) continue;
      write("  ");
      write(f.name);
      write(": ");
      write(wgslType(f.type));
      write(",\n");
    }
    write("};\n\n");

    write("struct NodeMeta {\n");
    write("  vert_offset: u32,\n");
    write("  vert_count: u32,\n");
    write("};\n\n");

    write("@group(0) @binding(0) var<storage, read_write> co_buf: array<vec3<f32>>;\n");
    write("@group(0) @binding(1) var<storage, read_write> no_buf: array<vec3<f32>>;\n");
    write("@group(0) @binding(2) var<storage, read_write> mask_buf: array<f32>;\n");
    write("@group(0) @binding(3) var<storage, read>       unique_verts: array<u32>;\n");
    write("@group(0) @binding(4) var<storage, read>       nodes: array<NodeMeta>;\n");
    write("@group(0) @binding(5) var<uniform>             brush_u: BrushUniforms;\n");
    write("@group(0) @binding(6) var<uniform>             ctx_u: CtxUniforms;\n\n");

    // Inlined falloff: same shape as CommandCtx::strength in
    // brush_command.h:55. The C++ reference is the source of truth — if
    // that formula changes, this needs to follow.
    write("fn brush_strength(p: vec3<f32>) -> f32 {\n");
    write("  let sb_t1 = 1.0 - min(length(p - ctx_u.surfacePos) / brush_u.radius, 1.0);\n");
    write("  let sb_t = sb_t1 * sb_t1 * (3.0 - 2.0 * sb_t1);\n");
    write("  return brush_u.strength * sb_t * brush_u.radius * 0.1;\n");
    write("}\n\n");
  }

  void emitSkipStub(const char *reason)
  {
    write("// AUTO-GENERATED by sbrushc (WGSL backend) — DO NOT EDIT.\n");
    write("// Source: ");
    write(brush->sourceFile);
    write("\n");
    write("// SKIPPED: ");
    write(reason);
    write("\n\n");
    // Tint refuses an entirely empty module; this is the minimum valid
    // WGSL so the build's `tint <file>` step still passes.
    write("@compute @workgroup_size(1) fn nop() {}\n");
  }

  // Emit one reduce stage as a WGSL function. Struct params become `ptr`
  // params for out/inout (so the callee can write back), value params
  // for `in`. Scalars unsupported in this slice — error if seen.
  void emitReduceStage(const Stage &st)
  {
    write("fn ");
    write(st.name);
    write("(");
    bool first = true;
    for (const auto &p : st.params) {
      if (!first) write(", ");
      first = false;
      if (p.type == TypeKind::Struct) {
        if (p.dir == ParamDir::Out || p.dir == ParamDir::InOut) {
          write(p.name);
          write(": ptr<function, ");
          write(p.structName);
          write(">");
        } else {
          write(p.name);
          write(": ");
          write(p.structName);
        }
      } else {
        errf("reduce scalar param '%s' not yet supported in WGSL emit",
             p.name.c_str());
        write(p.name);
        write(": f32");
      }
    }
    write(") {\n");
    indent = 1;
    currentStage = &st;
    if (st.body && st.body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : st.body->stmts) emitStmt(*c);
      while ((int)locals.size() > savedLocals) locals.pop_back();
    }
    currentStage = nullptr;
    indent = 0;
    write("}\n\n");
  }

  void run()
  {
    if (!vertexStage) {
      err("brush has no vertex stage");
      return;
    }
    if (vertexStage->params.size() < 1) {
      err("vertex stage must take at least one parameter (the Vertex bundle)");
    }

    if (hasNeighborLoop(vertexStage->body.get())) {
      emitSkipStub("brush uses for_neighbor — WGSL lowering needs mesh-edge buffers (later wave)");
      return;
    }

    emitPrelude();

    // Reduce stages — WGSL has no templates, so we just name-mangle by
    // the DSL stage name (which is brush-local in practice).
    Vector<const Stage *> reduceStages;
    for (const auto &st : brush->stages) {
      if (st.kind == StageKind::Reduce) reduceStages.append(&st);
    }
    for (const auto *st : reduceStages) {
      emitReduceStage(*st);
    }

    // Per-thread vertex kernel. Workgroup size 64 is a reasonable WebGPU
    // default; the host dispatches ceil(node.vert_count / 64) workgroups
    // per node. The conventional shape is one workgroup per node, with
    // node-internal threads strided — using gid.x to look up the node
    // entry is fine for tint validation and keeps the kernel readable.
    write("@compute @workgroup_size(64)\n");
    write("fn main(\n");
    write("    @builtin(local_invocation_index) lid: u32,\n");
    write("    @builtin(workgroup_id) gid: vec3<u32>)\n");
    write("{\n");
    write("  let sb_node = nodes[gid.x];\n");
    write("  if (lid >= sb_node.vert_count) { return; }\n");
    write("  let sb_vidx = unique_verts[sb_node.vert_offset + lid];\n");
    write("  var ");
    write(vertexParamName); write("_co: vec3<f32> = co_buf[sb_vidx];\n");
    write("  var ");
    write(vertexParamName); write("_no: vec3<f32> = no_buf[sb_vidx];\n");
    write("  var ");
    write(vertexParamName); write("_mask: f32 = mask_buf[sb_vidx];\n");

    // Declare struct-typed locals for vertex stage's extra params and
    // call each reduce stage on them. The naive per-thread reduce
    // matches the C++ executor's one-per-node call: both pay
    // O(stages*params) ops up-front before the per-vertex code runs.
    for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
      const auto &p = vertexStage->params[pi];
      if (p.type != TypeKind::Struct) {
        errf("vertex param '%s' must be a struct type (Wave 4 slice)",
             p.name.c_str());
        continue;
      }
      write("  var ");
      write(p.name);
      write(": ");
      write(p.structName);
      write(";\n");
    }
    for (const auto *st : reduceStages) {
      write("  ");
      write(st->name);
      write("(");
      bool first = true;
      for (const auto &rp : st->params) {
        if (!first) write(", ");
        first = false;
        if (rp.type == TypeKind::Struct) {
          if (rp.dir == ParamDir::Out || rp.dir == ParamDir::InOut) {
            out += "&";
          }
          // Match by name+type to the vertex-stage local declared above.
          bool found = false;
          for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
            const auto &vp = vertexStage->params[pi];
            if (vp.type == TypeKind::Struct &&
                string(vp.name).operator==(string(rp.name.c_str())) &&
                string(vp.structName).operator==(string(rp.structName.c_str()))) {
              write(vp.name);
              found = true;
              break;
            }
          }
          if (!found) write("/*unmatched*/");
        } else {
          write("/*scalar-unsupported*/");
        }
      }
      write(");\n");
    }
    write("\n");

    indent = 1;
    currentStage = vertexStage;
    if (vertexStage->body && vertexStage->body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : vertexStage->body->stmts) emitStmt(*c);
      while ((int)locals.size() > savedLocals) locals.pop_back();
    }
    currentStage = nullptr;
    indent = 0;

    write("\n");
    write("  co_buf[sb_vidx] = ");
    write(vertexParamName); write("_co;\n");
    write("  no_buf[sb_vidx] = ");
    write(vertexParamName); write("_no;\n");
    write("  mask_buf[sb_vidx] = ");
    write(vertexParamName); write("_mask;\n");
    write("}\n");
  }
};

} // namespace

EmitResult emitWgsl(const Brush &brush)
{
  Emit em;
  em.brush = &brush;
  for (const auto &st : brush.stages) {
    if (st.kind == StageKind::Vertex) { em.vertexStage = &st; break; }
  }
  if (em.vertexStage && em.vertexStage->params.size() > 0) {
    em.vertexParamName = em.vertexStage->params[0].name;
  } else {
    em.vertexParamName = string("v");
  }
  em.run();
  EmitResult r;
  r.text = std::move(em.out);
  r.errors = std::move(em.errors);
  return r;
}

} // namespace sculptcore::brush::sbrush
