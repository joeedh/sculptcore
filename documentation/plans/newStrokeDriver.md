# C++ brush stroke driver in sculptcore

## Context

The sculpt stroke sampler — pointer events → evenly-spaced dab samples along a
centripetal Catmull-Rom spline — lives entirely in TypeScript
(`scripts/editors/view3d/tools/stroke_driver.ts`, 658 lines, plus the
dependency-free math in `scripts/util/stroke_math.ts`). sculptcore itself has
only `source/brush/stroke_spacing.h` (`StrokeSpacer`: constant world-space
spacing with residual carry, no spline, no screen space, no stroke methods), so
every non-browser host — the headless `debug/interactive.cc` controller,
`debug/script.cc`'s `stroke` verb, the Python addon — reimplements a weaker
sampler and produces dab sequences that don't match the app.

Moving the sampler into C++ behind the litestl binding system gives one
implementation shared by TS, Python and native tests, and makes the stroke
sampler testable in `ctest` without a browser.

**Scope (confirmed with the user):** sampler only. Symmetry mirroring, the
per-dab raycast, filter-radius/grab discipline, dyntopo cadence, preview/rollback
and the GPU path all stay in `SculptPaintOp.applyDab` / `applyDabOne`
(`sculptcore_ops.ts`) and are not touched. Pressure→params dynamics stay in TS
(`StrokeDriverOp.makeParamProvider`) and are pushed in already-resolved. Rollout
is behind a feature flag with a parity test against the TS driver.

---

## Design

### 1. C++ curve math — `sculptcore/source/brush/stroke_curve.h` (new)

Direct port of `scripts/util/stroke_math.ts`, header-only, templated on
`math::Vec<double, N>` so both the 2D screen curve and the 3D world curve use one
implementation:

- `crToBezier(P0,P1,P2,P3, alpha=0.5) -> Cubic<N>` — centripetal Catmull-Rom →
  cubic Bezier control points, with the one-sided-tangent endpoint clamping
  (`t01 < EPS` / `t23 < EPS`) that the caller relies on for stroke ends.
- `evalCubic`, `deCasteljau` (internal), `subCubic(B, t0, t1)`.
- `arcLengthWalk(B, spacingDist, carryIn, fine=32) -> {Vector<double> ts; double carryOut}`.

`EPS = 1e-7` and `fine = 32` must match TS exactly.

**Use `double`, not `float`, throughout the driver's internals.** `walkCarry`
accumulates across every segment of a stroke; in `float` the drift shifts dab
counts near spacing thresholds and the parity test becomes flaky. Only the
emitted `DabSample` is `float`.

`cubicDeriv` is unused by the driver — skip it.

### 2. The driver — `sculptcore/source/brush/stroke_driver.{h,cc}` (new)

Namespace `sculptcore::brush`. Enums mirror the TS numeric values exactly
(`scripts/brush/brush_base.ts:61,71`):

```cpp
enum class StrokeSpaceMode { Screen = 0, World = 1 };
enum class StrokeMethod    { Path = 0, Anchored = 1, DragDot = 2 };
enum class AnchoredLiveMode{ Radius = 0, Angle = 1 };
```

#### `struct StrokeView`
Camera/object snapshot, refreshed by the host before each `poll()` — the C++
equivalent of `poll()`'s batch snapshot (`stroke_driver.ts:212-235`):
`mat4 rendermat` (world→clip), `mat4 obmat` (local→world) + `bool hasObjectMatrix`,
`float3 cameraPos`, `float2 viewSize` (logical, = `view3d.size`), `float2 glSize`
(device px), `float camNear`. `update()` derives `irendermat`, `iobmat`,
`iobmatDir` (translation cleared), `localRendermat = rendermat * obmat`,
`localIrendermat`.

It must also carry exact ports of:
- `view3dProject` / `view3dUnproject` (`scripts/editors/view3d/view3d_base.ts:16,47`)
  — including the `tmp[3] !== 0` guard, the `y` flip and the "co.length" arity
  behaviour (the driver only ever calls the 3- and 4-component forms).
- `getViewVec(x, y)` (`view3d.ts:720`) — unproject `(x, y, -camNear - 0.001)`
  through the **camera's own** `irendermat` (not `localIrendermat`), subtract
  `cameraPos`, normalize.

#### `struct DabSample`
Bound POD carrying every field the TS driver writes onto `PaintSample`
(`pbvh_paintsample.ts`): `p` (float4, `[3] = w`), `dp`, `screenP`, `dScreenP`,
`strokeS`, `dstrokeS`, `isInterp`, `angle`, `futureAngle`, `vec`, `color`,
`viewvec`, `vieworigin`, `viewPlane`, `strength`, `radius`, `w`, `invert`,
`pressure`, `hit`, `useAltBrush`, `anchorVec`, `liveAngle`, `tiltX/tiltY/twist`,
plus `bool hasCurve` and `float3 curve0..curve3` (the object-local
`subCubic(worldB, t±0.15)` slice; TS rebuilds `Bezier(...).createQuads()` from
them). `rendermat`/`irendermat`/`view3dSize` are constant per batch and are read
back off the driver, not duplicated per sample.

Nothing in `scripts/` currently reads `ps.curve` — carry it for fidelity, but it
is not on the critical path.

#### `struct BrushStrokeDriver`
Members (bound, host-writable before the stroke): `spaceMode`, `strokeMethod`,
`anchoredLiveMode`, `radiusIsWorld`.

Bound methods:
```cpp
BrushStrokeDriver();                            // screen-only, no raycast
BrushStrokeDriver(spatial::SpatialTree *tree);  // "main" ctor; raycasts internally

void setViewRow(int matId, int row, float x, float y, float z, float w);
void setViewParams(float camX, float camY, float camZ,
                   float viewW, float viewH, float glW, float glH,
                   float camNear, bool hasObjectMatrix);

void push(float x, float y,                     // LOCAL view3d px
          float pressure, float tiltX, float tiltY, float twist,
          bool invert, bool useAltBrush,
          float radius, float strength, float spacing);
void pushColor(float r, float g, float b, float a);   // applies to the next push
void end();
void reset();

int  poll();                 // -> number of ready samples this batch
DabSample *sampleAt(int i);  // manifest-query idiom; avoids Vector<struct> marshalling
bool finished() const;
bool hasAnchorScreen() const;  float anchorScreenX() const;  float anchorScreenY() const;
bool hasPreviewScreen() const; float previewScreenX() const; float previewScreenY() const;
```

Why `setViewRow` instead of a `Vector<float>` matrix argument: `Vector<float>`
in-params from TS have no proven write path — only `setBoundIntVector` exists
(`typescript/api/wasm.ts:601`), and `CommandExecutor::setRenderMatrix(Vector<float>&)`
(`brush_executor.h:341`) is bound but never actually called from the app. Eight
6-arg calls per poll is trivial next to the raycast, needs no N-API change, and
stays inside `MARGS`'s 8-slot inline capacity. (A `setBoundFloatVector` +
`floatVectorAssign` pair is the nicer long-term fix — worth doing later, out of
scope here.)

**Raycasting is internal.** The TS `makeRayCast` adapter (`sculptcore_ops.ts:353`)
converts a world ray to object-local, calls `mesh.rayCast`, converts the hit back
to world. The driver has `obmat`, so it does that itself against
`SpatialTree::castRay(orig, dir, CastRayIsect&)` (`source/spatial/spatial.h:234`)
— which removes the callback-across-the-binding-seam problem entirely. With the
default constructor (no tree), `rayCast` always misses and the driver takes the
screen-mode fallback branch.

#### Internal state and algorithm

`ControlPoint` (`screen[2]`, `world[3]`, `normal[3]`, `viewvec[3]`, `hit`,
pressure/tilt/twist, invert/useAltBrush, params) in a `util::Vector`. The TS
driver never trims `cps` but only ever reads indices `L-3..L`; keep the same
append-only vector for a 1:1 port and note the memory cost is negligible.

Port, statement for statement:
- `ingest` (`stroke_driver.ts:249-340`) — position-resolution ladder including
  all four fallbacks and the two `return`-discard cases (ANCHORED miss before
  anchor; WORLD miss before first hit).
- `ingestAnchored` / `emitAnchored` / `emitDot` / `emitRaw`.
- `emitSegment` (`:450-518`) — spacing/radius from `p2` only, WORLD/SCREEN
  driving-curve fork, `spacingDist = max(spacing * 2 * radius, 1e-5)`,
  `walkCarry` continuity, `strokeS += spacing` (a stroke-fraction, **not** a
  distance), `prev.futureAngle` back-patching.
- `synthesizeMiss`, `projectOntoAnchorPlane`, `worldRadiusAt`, `screenRadiusAt`,
  `toLocal`, `toLocalDir`, `makeSample` (t<0.5 step for `invert`/`useAltBrush`/
  `hit`; lerp for everything else).

### 3. Bindings

- `defineBindings()` for `DabSample` and `BrushStrokeDriver` defined
  out-of-line in `source/brush/bindings.cc` (the file already exists and already
  hosts the brush enum `Binder<>` specializations, `source/brush/bindings.cc:5-51`);
  `Binder<StrokeSpaceMode|StrokeMethod|AnchoredLiveMode>::bind()` alongside them.
- `BIND_STRUCT_DEFAULT_CONSTRUCTOR` + `BIND_STRUCT_CONSTRUCTOR(st, "main", spatial::SpatialTree *)`
  on the driver; `BIND_STRUCT_DEFAULT_CONSTRUCTOR` + `BIND_STRUCT_COPY_CONSTRUCTOR`
  on `DabSample`.
- Register both in `sculptcore::brush::registerBindings` (already wired into
  `source/core/bindings.cc`).
- Add `stroke_driver.cc` to `source/brush/CMakeLists.txt`.
- **No N-API change is needed** — `NapiRuntime::construct`/`methodInvoker` are
  generic over the descriptors. Regen with `node sculptcore/make.mjs build wasm`
  (emits `typescript/sculptcore/brush/{BrushStrokeDriver,DabSample}.ts` + the
  `AllBoundTypes` entry in `typescript/index.ts`; both are committed) and
  `node sculptcore/make.mjs build node` for the native backend.
- Model to copy: `source/dyntopo/bindings.cc` and `source/spatial/bindings.cc:17-95`.
- Watch the known trap: `defineBindings()` must never reference its own type
  (`source/brush/brush.h:481`), and `MARGS` aborts at `require()` time on an
  arity mismatch.

### 4. TS integration

**`scripts/editors/view3d/tools/stroke_driver.ts`**
- Extract `interface IBrushStrokeDriver` — `push(input) / end() / reset() /
  poll(): PaintSample[] / readonly finished / getAnchorScreen() / getPreviewScreen()`.
  `BrushStrokeDriver` implements it unchanged apart from one refactor below.
- Move param resolution to push time: `StrokeInput` gains
  `radius / strength / spacing / color`, and `StrokeDriverOptions.getParams` goes
  away. Both drivers then consume identical inputs, which is what makes the
  parity test meaningful.

**`scripts/editors/view3d/tools/stroke_driver_native.ts`** (new)
`class NativeStrokeDriver implements IBrushStrokeDriver` — constructs the bound
object via `wasm.manager.constructWith(ctor, mesh.spatial)`, pushes the view
snapshot (`setViewRow` ×8 + `setViewParams`) at the top of `poll()`, then
`for (i < driver.poll()) convert(driver.sampleAt(i))` into a real `PaintSample`.
Keeping `PaintSample` as the boundary type means `SculptPaintOp`,
`inputs.samples` serialization and undo/redo replay are all untouched.

**`scripts/editors/view3d/tools/stroke_paint_op.ts`**
- `driver` field becomes `IBrushStrokeDriver | undefined`.
- `modalStart` (`:186`) picks by feature flag.
- New overridable `getSpatialTree(): unknown | undefined` (default `undefined`);
  `SculptPaintOp` returns `mesh.spatial`. When it's undefined the native driver
  is not eligible and the TS driver is used regardless of the flag.
- `on_pointermove` resolves params through `makeParamProvider()` and converts
  window→local coords via `view3d.getLocalMouse` before pushing (the native
  driver takes local px directly).

**`scripts/core/feature-flag.ts`** — add `sculptcore.cpp_stroke_driver`
(default `false`) next to the other `sculptcore.*` entries (`:208-226`), then
`pnpm gen:paths`.

---

## Verification

1. **C++ unit test** — `sculptcore/tests/test_stroke_driver.cc`, wired via the
   `test(...)` macro in `tests/CMakeLists.txt` (model:
   `tests/test_brush_mirror_grab.cc`). Cover: constant spacing along a straight
   drag (dab count = path length / (spacing·2·radius) ± 1); `walkCarry`
   continuity across segments (no clustering at control points); ANCHORED emits
   one dab per input all at the anchor with a growing `anchorVec`; DRAG_DOT
   follows the cursor; the first PATH dab is raw with `isInterp == false`;
   WORLD-mode inputs before the first hit are discarded. No view3d needed —
   feed a synthetic `mat4` and a `Scene`-built cube (`script::run` +
   `build_spatial`, as the other brush tests do).

2. **Parity test** — `tests/integration/stroke_driver_parity.test.ts` (NW.js,
   runs under both `SC_TEST_BACKEND=wasm` and `native`). Replay a fixed recorded
   pointer path + fixed camera matrices through both drivers and assert equal
   sample counts and per-field agreement to ~1e-4, for PATH / ANCHORED /
   DRAG_DOT × SCREEN / WORLD × `radiusIsWorld` on/off. This is the gate for
   flipping the flag default.

3. **`pnpm test`** plus the existing stroke regressions —
   `tests/integration/sculptcore_anchored_dragdot.test.ts` (the no-compounding
   invariant) must pass with the flag on.

4. **Live check** — `pnpm run nwjs`, enable the flag, sculpt with Draw / Grab /
   Snake Hook / Clay at both radius modes; confirm no dab clustering at stroke
   direction changes and that Anchored still previews and rolls back correctly.
   Drive it headlessly via `window._sculptcoreStrokeTester` and over CDP with
   `node nwjs/cdp.mjs` if a visual diff is needed.

5. **`npx tsgo --noEmit`** (not `tsc`), and commit the regenerated
   `sculptcore/typescript/**` files together with the C++ change, bumping the
   parent gitlink in the same logical commit.

## Follow-ups (not in this change)

- Port `debug/interactive.cc` and `debug/script.cc`'s `stroke` verb onto the new
  driver so headless C++ strokes match the app; retire `stroke_spacing.h` once
  nothing uses it.
- Add `setBoundFloatVector` / `floatVectorAssign` so bound `Vector<float>`
  in-params work, then collapse `setViewRow` into a single matrix call and fix
  `CommandExecutor::setRenderMatrix`.
- Regenerate the Python stubs (`node make.mjs build python && python -m sculptcore._gen`)
  and expose the driver to the Blender addon.
- Update `documentation/strokeDriverReport.md` (its symbol line numbers are
  already stale) and `sculptcore/documentation/strokeDriverGuide.md` §1 to
  describe the in-engine sampler.
