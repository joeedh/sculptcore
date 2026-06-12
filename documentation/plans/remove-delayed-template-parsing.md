# Remove `-fdelayed-template-parsing`: migrate `Bind<T>()` to a `Binder<T>` trait

**Goal.** Build sculptcore (WASM, native, Node addon) without clang's
`-fdelayed-template-parsing` MSVC-compat extension, under conforming C++20
two-phase name lookup.

**Why it currently can't.** `Bind<T>()` is an open overload set of
zero-argument function templates, extended by reopening
`namespace litestl::binding` across litestl *and* downstream modules (mesh,
gpu, spatial, props, brush, dyntopo, math). Calls inside template bodies —
`MethodBuilder` / `ConstructorBuilder` (`binding_method.h`,
`binding_constructor_builder.h`), `Struct::_addMethod`
(`binding_struct.h`), the `IsVector` binder (`binding.h`) — resolve their
candidate set by ordinary lookup **at the template's definition point**;
`Bind()` has no arguments, so ADL contributes nothing at instantiation.
Every overload declared downstream of those headers is invisible to them
under conforming lookup. Delayed parsing defers the body parse to the
instantiation point, which is what makes the current design work.
(`BIND_STRUCT_MEMBER` is unaffected: it expands `binding::Bind<...>()`
directly at the macro-expansion site, a non-template context.)

**Fix (Option A).** Replace the overload set with a class-template
customization point. `Binder<T>::bind()` is a dependent *qualified* name —
resolved at instantiation — and a specialization only has to be declared
before the first use that instantiates it in each TU, which is exactly the
include discipline call sites already follow. All ~124 `Bind<X>()` call
sites keep their spelling; only the ~30 overload definitions migrate.

## Design

New header `source/litestl/binding/binding_bind.h` (includes only
`binding_base.h`):

```cpp
namespace litestl::binding {

/* Primary is intentionally undefined: unbound types fail with
 * "implicit instantiation of undefined template Binder<X>", and
 * specializations may be declared in a header / defined in a .cc. */
template <typename T> struct Binder;

template <typename T> auto Bind()
{
  return Binder<T>::bind();   // dependent qualified name -> late lookup
}

} // namespace litestl::binding
```

Decisions:

- **No cvref stripping in the dispatcher.** Today `T&` / `T*` dispatch to
  Reference / Pointer overloads via constraints; that moves into partial
  specializations `Binder<T&>` / `Binder<T*>`. `Bind<const X>` is a
  compile error today (no matching overload) and stays one (undefined
  `Binder<const X>`).
- **`auto` return on the dispatcher and `static auto bind()` (or the
  concrete type) on specializations** so derived return types survive —
  call sites rely on `types::Enum *`, `types::Struct<T> *`,
  `types::Boolean *`, etc. (verified: nothing takes `&Bind<...>`, so the
  non-uniform signature is safe).
- **Explicit specialization beats partial specialization**, so
  `Binder<void *>` and `Binder<gpu::Buffer *>` win over the generic
  `Binder<T *>` automatically — the current `!std::same_as<T, void *>`
  exclusion on the pointer overload is dropped.
- **C++20 constrained partial specializations** carry the concept-based
  overloads unchanged: `template <ClassBindingReq CLS> struct Binder<CLS>`,
  `template <IsVector VEC> struct Binder<VEC>`,
  `template <math::isMathVec T>`, `template <math::isMathAABB T>`.
  Constraint-subsumption partial ordering matches today's overload
  partial ordering; any new ambiguity surfaces as a compile error and is
  resolved by tightening a concept.
- **Declare-in-header / define-in-cc** keeps working:
  `template <> struct Binder<X> { static const BindingBase *bind(); };`
  in the header, member definition in the `.cc` (the `gpu/types.h` →
  `manager.cc` pattern).
- Side benefit: today's downstream overloads are `static` function
  templates in headers (internal linkage), so builder instantiations in
  different TUs formally bind different functions — an ODR violation.
  Class specializations with inline/external-linkage members erase it.

## Inventory

### litestl-internal overloads → specializations (phase 1)

| File | Overload today | Becomes |
| --- | --- | --- |
| `binding_number.h` | 11 per-type `_()` macro overloads | same macro, emitting `template <> struct Binder<ctype>` (no consolidation — out of scope) |
| `binding_utils.h` | `bool`; `T*` (non-void); `T&` | explicit spec; partial spec `Binder<T *>` (drop void exclusion); partial spec `Binder<T &>` |
| `binding_struct.h` | `ClassBindingReq CLS` | constrained partial spec |
| `binding.h` | `void *`; `IsVector VEC`; `util::string` | explicit; constrained partial; explicit |
| `math/math_bindings.h` | `isMathVec T`; `isMathAABB T` | constrained partial specs |

### Downstream overloads → specializations (phase 2)

| File | Types | Notes |
| --- | --- | --- |
| `mesh/attribute_enums.h` | `AttrUse`, `AttrFlag`, `AttrType` | in-header, mechanical |
| `gpu/vbo.h` | `GPUBufferType`, `GPUBufferHint` | in-header, mechanical |
| `gpu/types.h` + `gpu/manager.cc` | `GPUCmdType`, `GPUType`, `GPUFetchMode` | spec declared in header, `bind()` defined in cc |
| `gpu/vbo.cc` | `Buffer *` | cc-local today (latent ODR hazard: another TU binding a `Buffer *` member silently got the generic pointer overload). **Hoist the spec declaration into a gpu header** so every TU agrees |
| `props/prop_base.h` | `Prop` | in-header |
| `spatial/spatial_enums.h` | `NodeFlags` | in-header |
| `brush/brushes/types.h` | `SculptBrushes` | in-header |
| `dyntopo/bindings.cc` | `DynTopoMode` | declare the spec in `dyntopo/bindings.h`, define in the cc |

Call sites (~124 across 26 files, incl. `manager.h`, `core/bindings.cc`,
module `bindings.cc` files, `litestl/tests/`) are untouched.

## Phases

Phases 1–2 are flag-compatible: trait-based code compiles identically under
delayed parsing, so every step lands green before the flag flips.

### Phase 0 — sizing (scratch, not committed)

- [ ] Locally delete `-fdelayed-template-parsing
      -Wno-delayed-template-parsing-in-cxx20` from both
      `add_compile_options` lines in the root `CMakeLists.txt`
      (~line 14 WASM branch, ~line 37 native branch), configure + build
      native, capture the error inventory, revert.
- [ ] Confirm the fallout is dominated by `Bind` lookup; list any
      non-binding stragglers (never-instantiated template bodies elsewhere
      — `util/` containers are the likely hotspot) for phase 3.

### Phase 1 — core trait infrastructure (flag stays on)

- [ ] Add `binding/binding_bind.h` (primary `Binder` + `Bind` dispatcher).
      Include it at the top of every `binding_*.h` header so builder bodies
      see the dispatcher regardless of the
      `binding_method.h ↔ binding_utils.h ↔ binding_struct.h` include
      cycle.
- [ ] Migrate the litestl-internal overloads per the table above.
      `BIND_STRUCT_*` macros and builder bodies are unchanged (their
      `Bind<...>` spellings now route through the dispatcher).
- [ ] Grep-verify no function-template `Bind` definitions remain under
      `source/litestl/` (pattern: `\*\s*Bind\s*\(\)` and `Bind\s*\(\)\s*\{`).
- [ ] **Gate 1:** `node make.mjs build native` + `node make.mjs test`
      green; WASM build green.

### Phase 2 — downstream modules (flag stays on)

- [ ] Migrate per the downstream table: mesh, gpu (incl. hoisting the
      `Buffer *` spec declaration), props, spatial, brush, dyntopo.
- [ ] Audit grep over all of `source/` and `tests/` for leftover `Bind`
      overload definitions or declarations (incl. test-local ones in
      `litestl/tests/test_binding_system.cc`, `tests/test_binding.cc`).
- [ ] **Gate 2:** native build + ctest, WASM build, `node make.mjs node
      --smoke` all green. Regenerate the TS bindings (WASM build, then
      `pnpm build` in `tools/`) and confirm a **zero diff** under
      `typescript/` — descriptors must be byte-identical. Surface any diff
      before accepting it.

### Phase 3 — flip the flag

- [ ] Remove `-fdelayed-template-parsing` and
      `-Wno-delayed-template-parsing-in-cxx20` from both CMakeLists lines.
- [ ] Rebuild native, WASM, and the Node addon; fix residual fallout from
      phase 0's straggler list (expect mechanical fixes: missing includes,
      typos in never-instantiated bodies, non-dependent names not visible
      at definition).
- [ ] **Gate 3:** all three builds green; full ctest green (known
      pre-existing failure: `test_debug_script` smooth-brush assert —
      unrelated, don't chase); `--smoke` passes; TS regen still zero-diff.
      Optionally run the parent repo's `sculptcore_parity.test.ts`.

### Phase 4 — docs + cleanup

- [ ] Update `sculptcore/CLAUDE.md` (drop "the code relies on Clang's
      delayed-template-parsing extension"; clang stays the required
      toolchain), the `make.mjs` comment (~line 224), and
      `litestl/documentation/binding.md` (customization point is now
      `Binder<T>` specialization — update the "bind a new enum" recipe;
      `litestl/CLAUDE.md`'s macro-level instructions are unchanged).
- [ ] Remove every `CLAUDENOTE:` comment added during the work, promoting
      any still-useful ones to ≤3-line permanent comments.

## Risks

- **Derived-return-type drift.** A spec accidentally returning
  `const BindingBase *` where the old overload returned `types::Enum *`
  breaks call sites — caught at compile time; keep spec return types
  identical to the old overloads.
- **Partial-spec ambiguity.** A type satisfying two constrained partial
  specs (e.g. a `Vector`-like class that also grows `defineBindings()`)
  is ambiguous — same as today's overloads; resolve by concept tightening
  (`ClassBindingReq<T> && !IsVector<T>` style), only if it actually fires.
- **Spec visibility (IFNDR).** A TU instantiating `Bind<X>` before
  `Binder<X>`'s declaration silently gets the undefined primary → hard
  error (good) — except for types also matched by a partial spec
  (`Buffer *` vs `T *`), where it would silently bind differently. That
  hazard exists today; the hoisted header declarations close it. Keep the
  rule: every spec for a type visible outside one cc lives in a header
  next to the type.
- **Unknown non-binding fallout** once the flag is off. Phase 0 bounds it
  before any migration work starts; iterate on native (fast) before WASM.
