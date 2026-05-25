#include "emit_cpp.h"
#include "../kernels/ir/intrinsics.h"
#include <cctype>
#include <cstdio>
#include <cstring>

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::Vector;
using litestl::util::stringref;

namespace {

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

  // Locals declared in the current body — kept for diagnostics. The
  // emitter doesn't need to track types because C++ does, but knowing
  // a name is a local helps us route identifier resolution correctly.
  Vector<string> locals;

  // Set when a NeighborLoop is emitted — drives an extra #include in
  // the generated header so EdgeOfVertIter resolves.
  bool neighborLoopUsed = false;

  void err(const char *msg)
  {
    errors.append(string(msg));
  }

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

  // Resolve a dotted call name like "Rings.eval" to its texture def.
  const TextureDef *findTextureCall(stringref callName) const
  {
    for (const auto &t : brush->textures) {
      string full = t.name + ".eval";
      if (string(full).operator==(string(callName.c_str()))) return &t;
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

  // === expression emitter ===

  void emitExpr(const Expr &e)
  {
    switch (e.kind) {
    case ExprKind::LitFloat: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", e.fvalue);
      // Ensure we look like a float literal — %g can produce "0" or "1e10"
      // without a decimal point, which then makes "0f" / "1e10f" invalid.
      bool hasDot = false;
      for (const char *p = buf; *p; p++) {
        if (*p == '.' || *p == 'e' || *p == 'E') { hasDot = true; break; }
      }
      out += buf;
      if (!hasDot) out += ".0";
      out += "f";
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
      if (isLocal(nm) || isStageParam(nm)) {
        out += e.name;
      } else if (auto *f = findField(nm)) {
        // Uniforms live on Brush. Ctx-kind fields default to ctx.brush.X
        // too, so the DSL can name new per-stroke state without having
        // to extend CommandCtxBase. The exception is the hardcoded
        // CommandCtxBase members (surfacePos, surfaceNo, mouse, …),
        // which keep the legacy `ctx.<name>` spelling.
        const char *n = e.name.c_str();
        bool isCtxBase = (std::strcmp(n, "mouse") == 0) ||
                         (std::strcmp(n, "mousePos") == 0) ||
                         (std::strcmp(n, "surfacePos") == 0) ||
                         (std::strcmp(n, "surfaceNo") == 0) ||
                         (std::strcmp(n, "mouseDir") == 0) ||
                         (std::strcmp(n, "renderMatrix") == 0) ||
                         (std::strcmp(n, "isFirstOfStep") == 0) ||
                         (std::strcmp(n, "meshLog") == 0);
        // Host stages take `(CommandCtxBase &ctx, Brush &brush)` — there
        // is no `ctx.brush`, so uniforms/non-builtin ctx fields resolve
        // to bare `brush.X` instead. Builtin ctx-base fields still go
        // through `ctx.X` either way.
        bool inHost = (currentStage && currentStage->kind == StageKind::Host);
        if (f->kind == FieldKind::Ctx && isCtxBase) {
          out += "ctx.";
        } else if (inHost) {
          out += "brush.";
        } else {
          out += "ctx.brush.";
        }
        out += e.name;
      } else {
        // Could be an intrinsic referenced without a call — treat as bare
        // identifier and let the C++ compiler catch it.
        out += e.name;
      }
      break;
    }
    case ExprKind::Member:
      emitExpr(*e.lhs);
      out += ".";
      out += e.name;
      break;
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
      // Dotted call `Tex.eval(args)` -> inline texture's free function.
      if (const TextureDef *td = findTextureCall(stringref(e.name.c_str()))) {
        out += "tex";
        out += capitalize(td->name);
        out += "Eval(";
        for (int i = 0; i < (int)e.args.size(); i++) {
          if (i > 0) out += ", ";
          emitExpr(*e.args[i]);
        }
        out += ")";
        break;
      }
      const IntrinsicDef *intr = findIntrinsic(stringref(e.name.c_str()));
      if (intr) {
        const char *pat = intr->emit[(int)BackendKind::Cpp].pattern;
        if (!pat) {
          errf("intrinsic '%s' has no C++ emit pattern", e.name.c_str());
          out += "/*missing-intrinsic-pattern*/";
          break;
        }
        // Substitute $0..$N — lower each arg into a temporary string, then
        // copy with substitution.
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
        // No intrinsic — emit a direct call. Used for built-in math
        // free functions (length, dot, ...) once their intrinsics are
        // registered; for now any unknown name reaches here.
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

  // Emit the C++ spelling of an IR type. Struct types use their
  // user-defined name; everything else maps to the engine math types.
  void emitTypeRef(TypeKind ty, const string &structName)
  {
    if (ty == TypeKind::Struct) {
      out += structName;
    } else {
      out += typeKindName(ty);
    }
  }

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
      emitTypeRef(s.declType, s.declStructName);
      out += " ";
      out += s.name;
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
        out += "\n";
        indent++;
        emitStmt(*s.thenBranch);
        indent--;
        writeIndent();
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
          // else-if chaining
          emitStmt(*s.elseBranch);
        } else {
          out += "\n";
          indent++;
          emitStmt(*s.elseBranch);
          indent--;
        }
      } else {
        out += "\n";
      }
      break;
    }
    case StmtKind::For: {
      // `for (<init> <cond>; <step>) { <body> }`. Render init and step
      // into scratch buffers so we can trim the trailing newline (and,
      // for the step, the trailing semicolon — the C-for closes with
      // `)` instead).
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
        out += "\n";
        indent++;
        emitStmt(*s.thenBranch);
        indent--;
      } else {
        out += ";\n";
      }
      break;
    }
    case StmtKind::Continue:
      writeIndent(); out += "continue;\n";
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
    case StmtKind::NeighborLoop: {
      // for_neighbor (nb in <outer>) { body }
      // Expand to a scoped C++ block that walks EdgeOfVertIter around
      // <outer>'s vertex index, binding nb as a reference-bundle with
      // .co/.no/.v just like the main vertex iter target.
      neighborLoopUsed = true;
      writeIndent(); out += "{\n";
      indent++;
      writeIndent(); out += "int __outer_v = ";
      emitExpr(*s.lvalue);
      out += ".v;\n";
      writeIndent(); out += "auto *__m = ctx.node.data->m;\n";
      writeIndent(); out += "int __e0 = __m->v.e[__outer_v];\n";
      writeIndent(); out += "if (__e0 != ELEM_NONE) {\n";
      indent++;
      writeIndent();
      out += "for (int __e : sculptcore::mesh::EdgeOfVertIter(__m, __outer_v, __e0)) {\n";
      indent++;
      writeIndent();
      out += "int __nb_v = (__m->e.vs[__e][0] == __outer_v) ? __m->e.vs[__e][1] : __m->e.vs[__e][0];\n";
      writeIndent();
      // Neighbor co reads the pre-dab snapshot (Jacobi); no/v stay live.
      out += "struct { litestl::math::float3 &co; litestl::math::float3 &no; int v; } ";
      out += s.name;
      out += " {(*ctx.co_prev)[__nb_v], __m->v.no[__nb_v], __nb_v};\n";
      // Body: emit either a Block (inline) or a single statement.
      int savedLocals = (int)locals.size();
      locals.append(s.name);
      if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
        for (const auto &c : s.thenBranch->stmts) emitStmt(*c);
      } else if (s.thenBranch) {
        emitStmt(*s.thenBranch);
      }
      while ((int)locals.size() > savedLocals) locals.pop_back();
      indent--;
      writeIndent(); out += "}\n";
      indent--;
      writeIndent(); out += "}\n";
      indent--;
      writeIndent(); out += "}\n";
      break;
    }
    }
  }

  // === top-level file emitter ===

  static string capitalize(const string &s)
  {
    string r = s;
    if (r.size() > 0) r[0] = (char)std::toupper((unsigned char)r[0]);
    return r;
  }

  static string lower(const string &s)
  {
    string r = s;
    for (int i = 0; i < (int)r.size(); i++) {
      r[i] = (char)std::tolower((unsigned char)r[i]);
    }
    return r;
  }

  // Emit one host stage as a templated free function. Host runs once per
  // dab on CPU with direct access to ctx (CommandCtxBase) and the Brush
  // — there's intentionally no per-node CommandCtx here, since the per-
  // node loop hasn't started yet. Currently host stages take no params;
  // any param-passing happens via ctx state.
  void emitHostStage(const Stage &st, const string &lowerBrush)
  {
    write("template <CommandTypes TYPES>\n");
    write("static void ");
    write(lowerBrush);
    write(capitalize(st.name));
    write("(CommandCtxBase &ctx, Brush &brush)\n");
    write("{\n");
    write("  (void)ctx; (void)brush;\n");
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

  // Emit one reduce stage as a templated free function. The signature
  // mirrors the DSL source: each struct-typed param becomes a C++
  // reference param, scalars stay by-value (in) or by-reference (out/inout).
  void emitReduceStage(const Stage &st, const string &lowerBrush)
  {
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
        if (p.dir == ParamDir::In) write("const ");
        write(p.structName);
        write(" &");
      } else {
        // Scalars: by-ref for out/inout, by-value for in.
        write(typeKindName(p.type));
        if (p.dir == ParamDir::Out || p.dir == ParamDir::InOut) write(" &");
      }
      write(" ");
      write(p.name);
    }
    write(")\n");
    write("{\n");
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

  // Emit one inline texture's eval as a pure free function. It sees only
  // its own parameters and intrinsics — no ctx/brush state — so the same
  // text lowers identically on every backend.
  void emitTextureFn(const TextureDef &td)
  {
    write("static ");
    write(typeKindName(td.returnType));
    write(" tex");
    write(capitalize(td.name));
    write("Eval(");
    for (int i = 0; i < (int)td.params.size(); i++) {
      if (i > 0) write(", ");
      write(typeKindName(td.params[i].type));
      write(" ");
      write(td.params[i].name);
    }
    write(")\n{\n");
    write("  using namespace litestl::math;\n");
    for (const auto &p : td.params) {
      write("  (void)");
      write(p.name);
      write(";\n");
    }
    indent = 1;
    // A scratch stage so identifier resolution treats the eval params as
    // stage params (bare names) rather than brush fields.
    Stage scratch;
    scratch.kind = StageKind::Reduce;
    for (const auto &p : td.params) scratch.params.append(p);
    currentStage = &scratch;
    if (td.body && td.body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : td.body->stmts) emitStmt(*c);
      while ((int)locals.size() > savedLocals) locals.pop_back();
    }
    currentStage = nullptr;
    indent = 0;
    write("}\n\n");
  }

  void run()
  {
    string lowerName = lower(string(brush->attrName.size() > 0 ? brush->attrName : brush->cppName));
    string camelName = capitalize(lowerName);

    write("// AUTO-GENERATED by sbrushc — DO NOT EDIT.\n");
    write("// Source: ");
    write(brush->sourceFile);
    write("\n");
    write("#pragma once\n");
    write("#include \"brush/brush_command.h\"\n");
    write("#include \"spatial/spatial_enums.h\"\n");
    write("#include \"mesh/mesh_iter.h\"\n\n");
    write("namespace sculptcore::brush::command {\n\n");

    // Struct decls — at namespace scope so reduce/vertex functions can
    // both name them. Skip if the brush declared none.
    for (const auto &sd : brush->structs) {
      write("struct ");
      write(sd.name);
      write(" {\n");
      for (const auto &f : sd.fields) {
        write("  ");
        write(typeKindName(f.type));
        write(" ");
        write(f.name);
        write(";\n");
      }
      write("};\n\n");
    }

    // Inline texture eval functions — pure, at namespace scope so the
    // vertex/reduce bodies can call them.
    for (const auto &td : brush->textures) {
      emitTextureFn(td);
    }

    // pre-stage: meshlog setup. Universal for local-per-vertex brushes.
    write("template <CommandTypes TYPES>\n");
    write("static void ");
    write(lowerName);
    write("Pre(CommandCtxBase &ctx, std::span<spatial::SpatialNode *> nodes)\n");
    write("{\n");
    write("  if (ctx.meshLog) {\n");
    write("    for (auto *node : nodes) {\n");
    write("      if (ctx.meshLog->hasSimpleChunk(node->id)) continue;\n");
    write("      auto *simple = ctx.meshLog->getSimpleChunk(\n");
    write("          node->id, node->unique_verts().size(), 0, 0, node->unique_faces().size());\n");
    write("      auto *m = node->data->m;\n");
    write("      simple->v.ensureAttr(m->v.attrs, m->v.co);\n");
    write("      simple->v.ensureAttr(m->v.attrs, m->v.no);\n");
    write("      simple->f.ensureAttr(m->f.attrs, m->f.no);\n");
    write("      simple->v.cpyFrom(m->v.attrs, node->unique_verts());\n");
    write("      simple->f.cpyFrom(m->f.attrs, node->unique_faces());\n");
    write("    }\n");
    write("  }\n");
    write("}\n\n");

    // Host stages — CPU-only setup that runs before any per-node work
    // for a dab. Emitted first so reduce/vertex (which may read ctx
    // fields the host populated) can rely on its side effects.
    Vector<const Stage *> hostStages;
    for (const auto &st : brush->stages) {
      if (st.kind == StageKind::Host) hostStages.append(&st);
    }
    for (const auto *st : hostStages) {
      emitHostStage(*st, lowerName);
    }

    // Reduce stages — emitted before the vertex stage so the vertex
    // function can call them by name.
    Vector<const Stage *> reduceStages;
    for (const auto &st : brush->stages) {
      if (st.kind == StageKind::Reduce) reduceStages.append(&st);
    }
    for (const auto *st : reduceStages) {
      emitReduceStage(*st, lowerName);
    }

    // vertex stage: walks node's verts running the DSL body.
    if (!vertexStage) {
      err("brush has no vertex stage");
      return;
    }
    if (vertexStage->params.size() < 1) {
      err("vertex stage must take at least one parameter (the Vertex bundle)");
    }

    write("template <CommandTypes TYPES>\n");
    write("static void ");
    write(lowerName);
    write("(CommandCtx<TYPES> &ctx)\n");
    write("{\n");
    write("  using namespace sculptcore::spatial;\n");
    write("  using namespace litestl::math;\n");
    write("  bool any_moved = false;\n");

    // Non-Vertex vertex params get declared as locals and seeded by the
    // matching reduce-stage output (matched by param name). The vertex
    // body then sees them as ordinary stage params — struct, scalar,
    // or vector. Struct locals are default-constructed; scalars stay
    // uninitialized until the reduce call runs (the executor always
    // calls every reduce stage before the per-vertex loop).
    for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
      const auto &p = vertexStage->params[pi];
      write("  ");
      emitTypeRef(p.type, p.structName);
      write(" ");
      write(p.name);
      write(";\n");
    }
    // Call each reduce stage in source order. Argument matching is
    // by-name to a vertex-stage local declared above; both struct
    // and scalar params are passed by reference at the C++ level
    // (the reduce signature already declares scalars as `T &` for
    // out/inout, by value for in — the call site looks the same).
    for (const auto *st : reduceStages) {
      write("  ");
      write(lowerName);
      write(capitalize(st->name));
      write("<TYPES>(ctx");
      for (const auto &rp : st->params) {
        write(", ");
        bool found = false;
        for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
          const auto &vp = vertexStage->params[pi];
          if (vp.type != rp.type) continue;
          if (!string(vp.name).operator==(string(rp.name.c_str()))) continue;
          if (rp.type == TypeKind::Struct &&
              !string(vp.structName).operator==(string(rp.structName.c_str()))) continue;
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

    write("  for (auto &");
    write(vertexParamName);
    write(" : ctx.vertexIter(ctx.node)) {\n");
    indent = 2;
    currentStage = vertexStage;

    // user body
    if (vertexStage->body && vertexStage->body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : vertexStage->body->stmts) emitStmt(*c);
      while ((int)locals.size() > savedLocals) locals.pop_back();
    }

    // post-iteration side effects: ran only when body did not continue/return.
    writeIndent();
    write("ctx.node.affected_verts.append(");
    write(vertexParamName);
    write(".v);\n");
    writeIndent();
    write("any_moved = true;\n");

    currentStage = nullptr;
    indent = 0;
    write("  }\n");
    write("  if (any_moved) {\n");
    write("    ctx.node.update(Spatial_UpdateNormals | Spatial_UpdateGPU | Spatial_RegenBounds);\n");
    write("  }\n");
    write("}\n\n");

    // post-stage: empty for Wave 1.
    write("template <CommandTypes TYPES>\n");
    write("static void ");
    write(lowerName);
    write("Post(CommandCtxBase &ctx, std::span<spatial::SpatialNode *> nodes)\n");
    write("{\n");
    write("  (void)ctx; (void)nodes;\n");
    write("}\n\n");

    // wire-up function: identical shape to existing createDrawBrush.
    write("template <CommandTypes TYPES>\n");
    write("static void create");
    write(camelName);
    write("Brush(BrushCommandDef<CommandCtx<TYPES>> &def)\n");
    write("{\n");
    // Host stages, if any, are composed into a single lambda so multiple
    // hosts on one brush still flow through one execHost slot.
    if (hostStages.size() > 0) {
      write("  def.execHost = [](CommandCtxBase &ctx, Brush &brush) {\n");
      for (const auto *st : hostStages) {
        write("    ");
        write(lowerName);
        write(capitalize(st->name));
        write("<TYPES>(ctx, brush);\n");
      }
      write("  };\n");
    }
    write("  def.execPre  = ");
    write(lowerName); write("Pre<TYPES>;\n");
    write("  def.exec     = ");
    write(lowerName); write("<TYPES>;\n");
    write("  def.execPost = ");
    write(lowerName); write("Post<TYPES>;\n");
    // for_neighbor reads ctx.co_prev — tell the executor to snapshot it.
    if (neighborLoopUsed) {
      write("  def.needsCoPrev = true;\n");
    }
    write("}\n\n");

    write("} // namespace sculptcore::brush::command\n");
  }
};

} // namespace

EmitResult emitCpp(const Brush &brush)
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
