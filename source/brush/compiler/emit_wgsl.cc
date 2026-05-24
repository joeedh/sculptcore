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

  // Reduce-stage out/inout params (struct or scalar) are lowered to WGSL
  // `ptr<function, T>`, so identifier references to them have to be
  // dereferenced inline — `s.a = x` becomes `(*s).a = x`, `w = 1.0`
  // becomes `(*w) = 1.0`.
  bool isOutPtrParam(stringref name) const
  {
    if (!currentStage) return false;
    for (const auto &p : currentStage->params) {
      if (!string(p.name).operator==(string(name.c_str()))) continue;
      return p.dir == ParamDir::Out || p.dir == ParamDir::InOut;
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
      if (isOutPtrParam(nm)) {
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
    case ExprKind::Index:
      emitExpr(*e.lhs);
      out += "[";
      emitExpr(*e.rhs);
      out += "]";
      break;
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
    case StmtKind::For: {
      writeIndent();
      out += "for (";
      auto renderFrag = [&](const Stmt &child, bool stripSemi) {
        string saved = out;
        out = string("");
        int savedIndent = indent;
        indent = 0;
        emitStmt(child);
        indent = savedIndent;
        string frag = out;
        out = saved;
        int n = (int)frag.size();
        while (n > 0 && frag[n - 1] == '\n') n--;
        if (stripSemi && n > 0 && frag[n - 1] == ';') n--;
        for (int i = 0; i < n; i++) {
          char tmp[2] = {frag[i], 0};
          out += tmp;
        }
      };
      if (s.forInit) renderFrag(*s.forInit, /*stripSemi=*/false);
      out += " ";
      emitExpr(*s.cond);
      out += "; ";
      if (s.forStep) renderFrag(*s.forStep, /*stripSemi=*/true);
      out += ") ";
      if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
        out += "{\n";
        indent++;
        int savedLocals = (int)locals.size();
        for (const auto &c : s.thenBranch->stmts) emitStmt(*c);
        while ((int)locals.size() > savedLocals) locals.pop_back();
        indent--;
        writeIndent(); out += "}\n";
      } else if (s.thenBranch) {
        out += "{\n";
        indent++;
        emitStmt(*s.thenBranch);
        indent--;
        writeIndent(); out += "}\n";
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
             std::strcmp(n, "spacing") == 0  || std::strcmp(n, "invert") == 0 ||
             std::strcmp(n, "falloff_kind") == 0 ||
             std::strcmp(n, "falloff_shape") == 0 ||
             std::strcmp(n, "falloff_dir") == 0 ||
             std::strcmp(n, "coord_space") == 0 ||
             std::strcmp(n, "tex_repeat") == 0 ||
             std::strcmp(n, "stroke_path_count") == 0;
    };
    auto isBuiltinCtxName = [](const char *n) {
      return std::strcmp(n, "surfacePos") == 0 || std::strcmp(n, "surfaceNo") == 0 ||
             std::strcmp(n, "render_matrix") == 0;
    };

    // Lowers a DSL field's type to its WGSL uniform-block spelling.
    // Array<T,N> becomes `array<T, N>`; note that `array<vec3<f32>, N>`
    // has stride 16 in the uniform address space (vec3 is padded), so
    // any future C++ marshaling has to match — deferred until a WGSL
    // dispatcher actually consumes these blocks.
    auto writeFieldType = [&](const Field &f) {
      if (f.type == TypeKind::Array) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", f.arraySize);
        out += "array<";
        out += wgslType(f.arrayElem);
        out += ", ";
        out += buf;
        out += ">";
      } else {
        out += wgslType(f.type);
      }
    };

    write("struct BrushUniforms {\n");
    write("  strength: f32,\n");
    write("  radius: f32,\n");
    write("  spacing: f32,\n");
    write("  invert: u32,\n");
    // Selector for `brush_falloff` — values must match the C++ enum
    // FalloffKind in brush.h. The C++ side packs this as a `u8`; on the
    // GPU it widens to u32 to keep std140 happy (and the host marshaler,
    // when it lands, must pad to u32 to match).
    write("  falloff_kind: u32,\n");
    // Spatial falloff metric (FalloffShape in brush.h), widened to u32.
    write("  falloff_shape: u32,\n");
    // Direction for FalloffShape::Linear. vec3 needs 16-byte alignment in
    // the uniform address space; the host marshaler (when it lands) must
    // match the padding here.
    write("  falloff_dir: vec3<f32>,\n");
    // Brush-texture UV mapping selector (TexCoordSpace in brush.h), widened
    // to u32, plus the tiling factor for ViewRepeat. `brush_sample_tex`
    // branches on coord_space to match CommandCtx::sampleBrushTex.
    write("  coord_space: u32,\n");
    write("  tex_repeat: f32,\n");
    // Live length of the StrokePath storage buffer for STROKE_CURVED. Mirrors
    // Brush::strokePathCount; the (future) dispatcher writes exactly this many
    // StrokeSample entries into the binding-10 buffer.
    write("  stroke_path_count: u32,\n");
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
      writeFieldType(f);
      write(",\n");
    }
    write("};\n\n");

    write("struct CtxUniforms {\n");
    write("  surfacePos: vec3<f32>,\n");
    write("  surfaceNo: vec3<f32>,\n");
    // View/render transform consumed by brush_sample_tex for the ViewPlane
    // and ViewRepeat coord spaces. Mirrors CommandCtxBase::renderMatrix.
    write("  render_matrix: mat4x4<f32>,\n");
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Ctx) continue;
      if (isBuiltinCtxName(f.name.c_str())) continue;
      write("  ");
      write(f.name);
      write(": ");
      writeFieldType(f);
      write(",\n");
    }
    write("};\n\n");

    write("struct NodeMeta {\n");
    write("  vert_offset: u32,\n");
    write("  vert_count: u32,\n");
    write("};\n\n");

    // One StrokePath sample — mirrors Brush::StrokeSample (pos, normal,
    // arclen). Consumed by brush_stroke_uv for STROKE_CURVED texture mapping.
    write("struct StrokeSample {\n");
    write("  pos: vec3<f32>,\n");
    write("  normal: vec3<f32>,\n");
    write("  arclen: f32,\n");
    write("};\n\n");

    write("@group(0) @binding(0) var<storage, read_write> co_buf: array<vec3<f32>>;\n");
    write("@group(0) @binding(1) var<storage, read_write> no_buf: array<vec3<f32>>;\n");
    write("@group(0) @binding(2) var<storage, read_write> mask_buf: array<f32>;\n");
    write("@group(0) @binding(3) var<storage, read>       unique_verts: array<u32>;\n");
    write("@group(0) @binding(4) var<storage, read>       nodes: array<NodeMeta>;\n");
    write("@group(0) @binding(5) var<uniform>             brush_u: BrushUniforms;\n");
    write("@group(0) @binding(6) var<uniform>             ctx_u: CtxUniforms;\n");
    // Curve LUT for FalloffKind::Curve. Sized to match Brush::falloff_curve
    // (kFalloffCurveSize = 256 in brush.h); when the WGSL dispatcher lands,
    // its marshaler should write exactly that many f32s into this binding.
    // The buffer is bake-produced from Brush::falloffCurve (a props::CurveGen)
    // via bake_curve_lut, so this LUT-fetch is bit-identical to the C++
    // Curve-branch interpolation by construction.
    write("@group(0) @binding(7) var<storage, read>       falloff_lut: array<f32, 256>;\n");
    // Brush texture + sampler. When no texture is bound the host binds a 1x1
    // white texel so `brush_sample_tex` returns 1.0 (matching the C++
    // no-texture path). The sampler is expected to be linear + clamp-to-edge
    // to mirror sampleTexBilinear.
    write("@group(0) @binding(8) var                       brush_tex: texture_2d<f32>;\n");
    write("@group(0) @binding(9) var                       brush_samp: sampler;\n");
    // StrokePath ring buffer for STROKE_CURVED — mirrors Brush::strokePath.
    // Uniform-resident on CPU; a storage buffer here so the length can vary.
    write("@group(0) @binding(10) var<storage, read>      stroke_path: array<StrokeSample>;\n\n");

    // Falloff selector — kept in lockstep with Brush::falloffEval in
    // brush.h. Each branch is the same closed form as its C++ twin;
    // changing one without the other is a regression on the WGSL/CPU
    // bit-equality contract that the upcoming backend A/B framework
    // will rely on.
    write("fn brush_falloff(t: f32) -> f32 {\n");
    write("  if (brush_u.falloff_kind == 1u) {\n");
    write("    return t;\n");
    write("  } else if (brush_u.falloff_kind == 2u) {\n");
    write("    let sb_u = 1.0 - t;\n");
    write("    return exp(-9.0 * sb_u * sb_u);\n");
    write("  } else if (brush_u.falloff_kind == 3u) {\n");
    write("    let sb_c = clamp(t, 0.0, 1.0);\n");
    write("    let sb_s = sb_c * 255.0;\n");
    write("    let sb_i = i32(floor(sb_s));\n");
    write("    if (sb_i >= 255) { return falloff_lut[255]; }\n");
    write("    let sb_f = sb_s - f32(sb_i);\n");
    write("    return falloff_lut[sb_i] * (1.0 - sb_f) + falloff_lut[sb_i + 1] * sb_f;\n");
    write("  }\n");
    write("  return t * t * (3.0 - 2.0 * t);\n");
    write("}\n\n");
    // Spatial falloff metric — kept in lockstep with Brush::falloffDist.
    // Values must match the C++ enum FalloffShape in brush.h.
    write("fn brush_falloff_dist(delta: vec3<f32>) -> f32 {\n");
    write("  let sb_inv_r = 1.0 / brush_u.radius;\n");
    write("  if (brush_u.falloff_shape == 1u) {\n");
    write("    let sb_a = abs(delta);\n");
    write("    return max(sb_a.x, max(sb_a.y, sb_a.z)) * sb_inv_r;\n");
    write("  } else if (brush_u.falloff_shape == 2u) {\n");
    write("    return abs(dot(delta, brush_u.falloff_dir)) * sb_inv_r;\n");
    write("  }\n");
    write("  return length(delta) * sb_inv_r;\n");
    write("}\n\n");
    write("fn brush_strength(p: vec3<f32>) -> f32 {\n");
    write("  let sb_t = 1.0 - min(brush_falloff_dist(p - ctx_u.surfacePos), 1.0);\n");
    write("  return brush_u.strength * brush_falloff(sb_t) * brush_u.radius * 0.1;\n");
    write("}\n\n");
    // Brush-texture modulation — kept in lockstep with
    // CommandCtx::sampleBrushTex. `no` is part of the DSL signature but
    // currently unused by the matrix-driven coord spaces; the phony
    // assignment keeps tint from flagging it.
    // Curvilinear stroke UV — kept in lockstep with Brush::sampleStrokeUV.
    // Projects `co` onto the StrokePath polyline: uv.x = arc length at the
    // nearest point, uv.y = lateral distance from the centerline.
    write("fn brush_stroke_uv(co: vec3<f32>) -> vec2<f32> {\n");
    write("  if (brush_u.stroke_path_count == 0u) { return vec2<f32>(0.0, 0.0); }\n");
    write("  if (brush_u.stroke_path_count == 1u) {\n");
    write("    return vec2<f32>(stroke_path[0].arclen, length(co - stroke_path[0].pos));\n");
    write("  }\n");
    write("  var sb_best_dist = 3.402823e+38;\n");
    write("  var sb_best_arc = 0.0;\n");
    write("  var sb_best_lat = 0.0;\n");
    write("  for (var i = 0u; i + 1u < brush_u.stroke_path_count; i = i + 1u) {\n");
    write("    let sb_a = stroke_path[i].pos;\n");
    write("    let sb_ab = stroke_path[i + 1u].pos - sb_a;\n");
    write("    let sb_len2 = dot(sb_ab, sb_ab);\n");
    write("    var sb_t = 0.0;\n");
    write("    if (sb_len2 > 0.0) { sb_t = dot(co - sb_a, sb_ab) / sb_len2; }\n");
    write("    sb_t = clamp(sb_t, 0.0, 1.0);\n");
    write("    let sb_d = length(co - (sb_a + sb_ab * sb_t));\n");
    write("    if (sb_d < sb_best_dist) {\n");
    write("      sb_best_dist = sb_d;\n");
    write("      sb_best_arc = stroke_path[i].arclen + (stroke_path[i + 1u].arclen - stroke_path[i].arclen) * sb_t;\n");
    write("      sb_best_lat = sb_d;\n");
    write("    }\n");
    write("  }\n");
    write("  return vec2<f32>(sb_best_arc, sb_best_lat);\n");
    write("}\n\n");
    write("fn brush_sample_tex(co: vec3<f32>, no: vec3<f32>) -> f32 {\n");
    write("  _ = no;\n");
    write("  var sb_uv: vec2<f32>;\n");
    write("  if (brush_u.coord_space == 1u) {\n");
    write("    let sb_p = (ctx_u.render_matrix * vec4<f32>(co, 1.0)).xyz;\n");
    write("    sb_uv = sb_p.xy;\n");
    write("  } else if (brush_u.coord_space == 2u) {\n");
    write("    let sb_p = (ctx_u.render_matrix * vec4<f32>(co, 1.0)).xyz;\n");
    write("    sb_uv = sb_p.xy * brush_u.tex_repeat;\n");
    write("  } else if (brush_u.coord_space == 3u) {\n");
    write("    sb_uv = brush_stroke_uv(co);\n");
    write("  } else {\n");
    write("    sb_uv = co.xy;\n");
    write("  }\n");
    write("  return textureSampleLevel(brush_tex, brush_samp, sb_uv, 0.0).r;\n");
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

  // Emit one reduce stage as a WGSL function. out/inout params (struct
  // or scalar) become `ptr<function, T>` so the callee can write back;
  // `in` params pass by value. The body emitter dereferences ptr params
  // automatically — see isOutPtrParam.
  void emitReduceStage(const Stage &st)
  {
    write("fn ");
    write(st.name);
    write("(");
    bool first = true;
    for (const auto &p : st.params) {
      if (!first) write(", ");
      first = false;
      const char *typeSpelling = (p.type == TypeKind::Struct)
                                     ? p.structName.c_str()
                                     : wgslType(p.type);
      if (p.dir == ParamDir::Out || p.dir == ParamDir::InOut) {
        write(p.name);
        write(": ptr<function, ");
        write(typeSpelling);
        write(">");
      } else {
        write(p.name);
        write(": ");
        write(typeSpelling);
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

    // Declare locals for the vertex stage's extra params (struct or
    // scalar) and call each reduce stage on them. The naive per-thread
    // reduce matches the C++ executor's one-per-node call: both pay
    // O(stages*params) ops up-front before the per-vertex code runs.
    for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
      const auto &p = vertexStage->params[pi];
      write("  var ");
      write(p.name);
      write(": ");
      if (p.type == TypeKind::Struct) write(p.structName);
      else write(wgslType(p.type));
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
        // Match by name+type to the vertex-stage local declared above.
        bool found = false;
        for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
          const auto &vp = vertexStage->params[pi];
          if (vp.type != rp.type) continue;
          if (!string(vp.name).operator==(string(rp.name.c_str()))) continue;
          if (rp.type == TypeKind::Struct &&
              !string(vp.structName).operator==(string(rp.structName.c_str()))) continue;
          if (rp.dir == ParamDir::Out || rp.dir == ParamDir::InOut) {
            out += "&";
          }
          write(vp.name);
          found = true;
          break;
        }
        if (!found) {
          errf("reduce param '%s' has no matching vertex-stage local of the same type",
               rp.name.c_str());
          write("/*unmatched*/");
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
