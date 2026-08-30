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

// Compiler-internal ids for the `@use(<category>)` attr tag; each names one
// mesh::_AttrUse bit. The emitters write the symbolic enumerator rather than a
// number, so mesh/attribute_enums.h stays the single value source.
namespace AttrUseId {
enum : int { None = 0, Unit, Color, Uv, Polygroup, Select, SculptLayer };
}
int parseAttrUse(stringref name);

enum class BinOp : int {
  Add,
  Sub,
  Mul,
  Div,
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,
  And,
  Or,
  BitAnd,
  BitOr,
  BitXor,
};

enum class UnaryOp : int {
  Neg,
  Not,
};

enum class AssignOp : int {
  Assign,
  AddAssign,
  SubAssign,
  MulAssign,
  DivAssign,
};

const char *binOpCSym(BinOp op);
const char *unaryOpCSym(UnaryOp op);
const char *assignOpCSym(AssignOp op);

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

enum class ExprKind : int {
  LitFloat,
  LitInt,
  LitBool,
  Ident,
  Member, // base.field
  Index,  // base[index]
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
  Expr(ExprKind k) : kind(k)
  {
  }
};

enum class StmtKind : int {
  Block,
  DeclLocal, // <type> <name> = <init>;
  Assign,    // <lvalue> [op]= <rvalue>;
  If,        // if (<cond>) <then> [else <else>]
  For,       // for (<init>; <cond>; <step>) <body>
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
  Stmt(StmtKind k) : kind(k)
  {
  }
};

enum class StageKind : int {
  Vertex,
  Reduce, // Wave 4
  Host,   // Wave 4
  Face,   // per-face stage (boundary-conditions wave)
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
  string name; // e.g. "apply"
  TypeKind returnType = TypeKind::Void;
  Vector<Param> params;
  StmtPtr body;
};

enum class FieldKind : int {
  Uniform,
  Ctx,
  Attr, // typed mesh attribute, bound to a layer by name at runtime
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
  // FieldKind::Attr only: `@use(<category>)`, an AttrUseId naming the
  // mesh::_AttrUse bit a host uses to retarget this handle. 0 = untagged.
  int use = AttrUseId::None;
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

// Texture parameter kinds (`param <kind> <name> ...;` inside a texture block).
// Non-@const params occupy slots in the texture's dense float slab: a Float is
// one slot, a Ramp is kTexRampSize slots. Int params must be @const (folded to
// literals at compile time) and hold no slot.
enum class TexParamKind : int {
  Float,
  Int,
  Ramp,
};

// Slots one ramp occupies in a texture's parameter slab. The runtime constant
// in brush/texture_eval.h must match; generated headers static_assert it.
inline constexpr int kTexRampSize = 256;

struct TexParam {
  TexParamKind kind = TexParamKind::Float;
  string name;
  bool hasDefault = false;
  double defaultValue = 0.0; // Float/Int only; ramps default to identity
  bool hasRange = false;
  double rangeMin = 0.0;
  double rangeMax = 0.0;
  // `@const`: folded into the generated code as a literal; not in the slab and
  // not runtime-tweakable (T3 re-specializes on change). Required for Int,
  // forbidden for Ramp.
  bool isConst = false;
  // First slot in the owning texture's param slab, -1 for @const params.
  int offset = -1;
  int line = 0;
};

// `sampler float <name>(float3 p[, float3 n]);` — a host-provided sample
// callback slot. T1 parses and records these; calling one from any emitted
// backend is an error until T4 wires host samplers through the eval ctx.
struct SamplerDecl {
  string name;
  TypeKind returnType = TypeKind::Float;
  Vector<Param> params;
  int line = 0;
};

// A procedural texture: `texture <Name> { ... float eval(float3 p, float3 n) { ... } }`.
// Either inline in a brush or standalone in a .stex unit (imported via
// `use texture <Name>;`). Callable within the owning brush as `<Name>.eval(p, n)`.
// Lowered to a free function (tex<Cap>Eval in C++, tex_<name>_eval in WGSL).
struct TextureDef {
  string name;                           // e.g. "Rings"
  TypeKind returnType = TypeKind::Float; // eval return type
  Vector<Param> params;                  // eval params
  StmtPtr body;                          // eval body
  Vector<TexParam> texParams;            // `param ...;` decls, decl order
  // Total float slots the non-@const params occupy (slab size).
  int slabSize = 0;
  // True when the body calls mapPoint() — the eval needs the map matrix
  // threaded in (TexEvalCtx on C++, an extra mat4x4 param on WGSL).
  bool usesMap = false;
  // Names of unit-scope samplers the body calls (T4 runtime dependency list).
  Vector<string> samplerDeps;
  // True for a `use texture` import resolved from a .stex unit. The cpp
  // emitter include-guards imported definitions: two brushes importing the
  // same texture emit byte-identical code (same unit parse), and brushes/all.h
  // aggregates every gen header into one TU.
  bool imported = false;
  int line = 0;
};

// A parsed .stex file: standalone texture definitions plus the unit-scope
// sampler declarations they may reference. sbrushc compiles one unit per
// --texture-unit invocation; brush compiles resolve `use texture` names
// against units passed via --texture=<path>.
struct TextureUnit {
  Vector<SamplerDecl> samplers;
  Vector<TextureDef> textures;
  string sourceFile;
};

struct Brush {
  string attrName; // from @brush("draw") -> "draw"
  string cppName;  // brush Draw { ... } -> "Draw"
  Vector<Field> fields;
  Vector<StructDef> structs;
  Vector<TextureDef> textures;
  // Unit-scope sampler decls carried through for texture-unit emission — the
  // C/WGSL backends need each host-sampler call site's declared arity.
  Vector<SamplerDecl> samplers;
  Vector<Stage> stages;
  // `use texture <Name>;` imports. Resolved after parse against the .stex units
  // supplied on the command line; each resolved def is moved into `textures`
  // so backends treat imports exactly like inline textures.
  Vector<string> useTextures;
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
  // A kernel marked `@unbounded` has a field with unbounded support that is its
  // own falloff, so `strength()` is forbidden in it (sema errors) and
  // `unbounded_window()` is required — the window makes the field vanish at the
  // host's node-filter radius instead of tearing on a leaf boundary. It is
  // non-accumulate-ineligible for now, unlike `@incremental`, whose
  // non-accumulability is structural. See the Open list in the wave-5 plan.
  bool isUnbounded = false;
  // `@incremental`: a stage input is a per-dab *delta* rather than an absolute
  // stroke quantity (snakehook's `grabTo` is the step since the last dab), so
  // there is no stroke-start base to re-derive the dab from — replaying it from
  // base would discard every earlier dab's drag. Codegen emits
  // def.accumulable = false, which makes the executor ignore nonAccum.
  bool isIncremental = false;
  // `@tool NAME[, NAME...]`: the SculptBrushes enum items this kernel implements.
  // One kernel can serve several tools (plane.sbrush is CLAY/SCRAPE/FILL — the
  // host varies uniforms, not code). Empty means the kernel is compiled but not
  // dispatched by any tool, which is what the built-in registry generator uses to
  // tell a live kernel from a scratch one.
  Vector<string> tools;
  // `@fulltopo`: the kernel (or its host pre-pass) walks live topology links every
  // dab, so the stroke can neither freeze topology nor read neighbors from a
  // stroke-start CSR snapshot.
  bool isFullTopo = false;
  // `@gpu`: the kernel has a GPU port — the built-in registry publishes its stem
  // per tool in kBuiltinBrushGpuKernel, which gpu_marshal dispatches on.
  bool isGpu = false;
};

/** True when any stage body contains a `for_neighbor` loop — such brushes are
 * emitted with the extra NbrSource template parameter (CsrNbr/LiveDiskNbr). */
bool brushUsesNeighborLoop(const Brush &brush);

/** How a kernel stores to a named member, from scanning its stage bodies.
 * `Nested` is a store *through* a swizzle or index of the member
 * (`v.color.x = ...`), which a top-level-name test reads as "no write" — so it
 * is reported separately and rejected by codegen rather than silently
 * mis-classified. */
enum class MemberWriteKind : int {
  None,
  TopLevel,
  Nested,
};

/** Scan every stage body for stores to a `.<field>` member. Used to emit the
 * host-visible write flags — BrushCommandDef::writesMask for the builtin
 * Vertex field, and BrushAttrManifestEntry::kernelWrites for each declared
 * `attr` handle (which the manifest's `materialize` bit cannot answer, since
 * that is set for every attr entry). */
MemberWriteKind brushScanMemberWrites(const Brush &brush, const char *field);

/** True when any stage body assigns to a `.<field>` member, at any depth. */
bool brushWritesMember(const Brush &brush, const char *field);

/** True when the brush declares a `face` stage, i.e. it is dispatched per-face
 * rather than per-vertex. */
bool brushHasFaceStage(const Brush &brush);

} // namespace sculptcore::brush::sbrush
