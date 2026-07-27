#pragma once

#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include <memory>

// sbrush IR — typed AST shared across all backend emitters.
//
// Wave 1 scope: enough to express draw.sbrush body. The IR is a tree,
// not strict SSA — backends that need SSA can build it from this. The
// emit_cpp backend lowers directly from this tree.

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

enum class TypeKind : int {
  Void,
  Bool,
  Int,
  Float,
  Float2,
  Float3,
  Float4,
  Vertex, // special: parameter type of vertex stage
  Face,   // special: parameter type of face stage
  Struct, // user-defined struct; resolve by Param/Field/Local.structName
  Array,  // fixed-size Array<elem, N>; carrier uses arrayElem/arraySize
  Unknown,
};

const char *typeKindName(TypeKind k);
TypeKind parseTypeKind(stringref name);

enum class BinOp : int {
  Add, Sub, Mul, Div,
  Eq, Ne, Lt, Le, Gt, Ge,
  And, Or,
  BitAnd, BitOr, BitXor,
};

enum class UnaryOp : int {
  Neg, Not,
};

enum class AssignOp : int {
  Assign, AddAssign, SubAssign, MulAssign, DivAssign,
};

const char *binOpCSym(BinOp op);
const char *unaryOpCSym(UnaryOp op);
const char *assignOpCSym(AssignOp op);

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

enum class ExprKind : int {
  LitFloat, LitInt, LitBool,
  Ident,
  Member,   // base.field
  Index,    // base[index]
  Binary,
  Unary,
  Call,
  Paren,
};

struct Expr {
  ExprKind kind;
  TypeKind type = TypeKind::Unknown;
  int line = 0;

  // LitFloat
  double fvalue = 0.0;
  // LitInt
  long long ivalue = 0;
  // LitBool
  bool bvalue = false;
  // Ident, Member.field, Call.name
  string name;
  // Binary
  BinOp binop = BinOp::Add;
  // Unary
  UnaryOp unaryop = UnaryOp::Neg;
  // Binary/Unary/Member/Paren
  ExprPtr lhs;
  ExprPtr rhs;
  // Call
  Vector<ExprPtr> args;

  Expr() = default;
  Expr(ExprKind k) : kind(k) {}
};

enum class StmtKind : int {
  Block,
  DeclLocal,    // <type> <name> = <init>;
  Assign,       // <lvalue> [op]= <rvalue>;
  If,           // if (<cond>) <then> [else <else>]
  For,          // for (<init>; <cond>; <step>) <body>
  Continue,
  Return,
  ExprStmt,
  NeighborLoop, // for_neighbor (<name> in <outer>) <body>
};

struct Stmt {
  StmtKind kind;
  int line = 0;

  // Block
  Vector<StmtPtr> stmts;
  // DeclLocal
  TypeKind declType = TypeKind::Unknown;
  // For DeclLocal of TypeKind::Struct, the user-defined struct name.
  string declStructName;
  string name;
  // DeclLocal.init / ExprStmt.expr / Return.value
  ExprPtr expr;
  // Assign
  AssignOp assignOp = AssignOp::Assign;
  ExprPtr lvalue;
  ExprPtr rvalue;
  // If / For
  // - If:  cond, thenBranch, optional elseBranch
  // - For: init (DeclLocal or Assign or ExprStmt) lives in `forInit`;
  //        cond uses the same `cond` slot; step uses `forStep`; body is
  //        `thenBranch`.
  ExprPtr cond;
  StmtPtr thenBranch;
  StmtPtr elseBranch;
  StmtPtr forInit;
  StmtPtr forStep;
  // NeighborLoop
  // - name: inner binding (becomes a Vertex-like local within body)
  // - lvalue: the outer Vertex expression to iterate around
  // - thenBranch: the loop body
  // (re-uses existing fields to avoid bloating Stmt)

  Stmt() = default;
  Stmt(StmtKind k) : kind(k) {}
};

enum class StageKind : int {
  Vertex,
  Reduce,  // Wave 4
  Host,    // Wave 4
  Face,    // per-face stage (boundary-conditions wave)
};

enum class ParamDir : int {
  In,    // default; read-only
  Out,   // write-only (caller passes uninitialized storage, callee writes)
  InOut, // both
};

struct Param {
  string name;
  TypeKind type = TypeKind::Unknown;
  // For TypeKind::Struct, the user-defined struct name.
  string structName;
  ParamDir dir = ParamDir::In;
};

struct Stage {
  StageKind kind;
  string name;          // e.g. "apply"
  TypeKind returnType = TypeKind::Void;
  Vector<Param> params;
  StmtPtr body;
};

enum class FieldKind : int {
  Uniform,
  Ctx,
  Attr,    // typed mesh attribute, bound to a layer by name at runtime
};

// Which mesh element domain an `attr` field lives on.
enum class AttrDomain : int {
  Vertex,
  Face,
  Edge,
  Corner,
};

struct Field {
  FieldKind kind;
  TypeKind type;
  // For TypeKind::Array, the element type and fixed length.
  TypeKind arrayElem = TypeKind::Unknown;
  int arraySize = 0;
  string name;
  // FieldKind::Attr only: which element domain the attribute lives on, and an
  // optional fixed layer name (`attr vertex float4 color = "Col";`). When
  // boundName is empty the runtime binds the layer named by the handle (`name`)
  // via the Brush attrBindings map.
  AttrDomain domain = AttrDomain::Vertex;
  string boundName;
  // FieldKind::Uniform metadata (scalar floats): `= <n>` default, `@range(a,b)`,
  // and `@static` to opt out of device dynamics. dynamicCapable defaults true so
  // any plain float uniform is drivable; non-float uniforms are never dynamic.
  bool hasDefault = false;
  double defaultValue = 0.0;
  bool hasRange = false;
  double rangeMin = 0.0;
  double rangeMax = 0.0;
  bool dynamicCapable = true;
};

// A `save <domain> <name>[, <name>...];` declaration: one entry per name. Tells
// the CPU undo-capture codegen (emit_cpp `*Pre`) which attribute to snapshot.
// `name` is a builtin bundle member (`co`/`no`/`mask`) or a declared `attr`
// field handle; resolution to a mesh AttrRef is deferred to codegen.
struct SaveAttr {
  AttrDomain domain = AttrDomain::Vertex;
  string name;
};

struct StructField {
  TypeKind type;
  string name;
};

struct StructDef {
  string name;
  Vector<StructField> fields;
};

// An inline procedural texture: `texture <Name> { float eval(float3 p, float3 n) { ... } }`.
// Callable within the owning brush as `<Name>.eval(p, n)`. Lowered to a free
// function (tex<Cap>Eval in C++, tex_<name>_eval in WGSL).
struct TextureDef {
  string name;                            // e.g. "Rings"
  TypeKind returnType = TypeKind::Float;  // eval return type
  Vector<Param> params;                   // eval params
  StmtPtr body;                           // eval body
  int line = 0;
};

struct Brush {
  string attrName;       // from @brush("draw") -> "draw"
  string cppName;        // brush Draw { ... } -> "Draw"
  Vector<Field> fields;
  Vector<StructDef> structs;
  Vector<TextureDef> textures;
  Vector<Stage> stages;
  // `save <domain> <name>,...;` declarations — the per-brush undo-capture set.
  // Empty means the legacy default (vertex co, vertex no, face no) applies.
  Vector<SaveAttr> saves;
  string sourceFile;
  // `@paint`: the kernel writes an attribute rather than displacing geometry, so
  // it is not non-accumulate-eligible (codegen emits def.accumulable = false) and
  // always takes the accumulate path. See plans/nonAccumMode.md.
  bool isPaint = false;
  // `@grabmode`: grab-class from-orig brush (grab/kelvinlet). The WGSL emit
  // mirrors CoProxy<AccumOrigGrab> (accum_mode.h): the stage derives each vert's
  // stroke-start base from the displacement field (binding 25) and the write-back
  // does per-dab first-touch arbitration via the dab-stamp buffer (binding 23).
  // The C++ leg ignores it — the executor selects AccumOrigGrab at runtime.
  bool isGrabMode = false;
  // `@relaxation`: the kernel relaxes the surface rather than displacing it, so
  // it never contributes to the accumulated brush displacement (§2 invariant of
  // plans/2026-07-26-0909-brush-displacement-base-attribute.md). Such a kernel
  // runs live-from-live even in a non-accumulate stroke; see def.relaxesBase.
  bool isRelaxation = false;
  // `@unbounded`: the kernel's field has unbounded support and is its own
  // falloff, so `strength()` is forbidden in it (sema errors) and
  // `unbounded_window()` is required — the window is what makes the field
  // vanish at the host's node-filter radius instead of tearing on a leaf
  // boundary. Non-accumulate-ineligible for now — unlike @incremental, whose
  // non-accumulability is structural. See the Open list in the wave-5 plan.
  bool isUnbounded = false;
  // `@incremental`: a stage input is a per-dab *delta* rather than an absolute
  // stroke quantity (snakehook's `grabTo` is the step since the last dab), so
  // there is no stroke-start base to re-derive the dab from — replaying it from
  // base would discard every earlier dab's drag. Codegen emits
  // def.accumulable = false, which makes the executor ignore nonAccum.
  bool isIncremental = false;
};

/** True when any stage body contains a `for_neighbor` loop — such brushes are
 * emitted with the extra NbrSource template parameter (CsrNbr/LiveDiskNbr). */
bool brushUsesNeighborLoop(const Brush &brush);

} // namespace sculptcore::brush::sbrush
