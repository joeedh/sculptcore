# Quad Remesher

A feature-aligned, global **MIQ-style quad remesher** in `source/remesh/`, plus a
standalone interactive **debug app** (`source/debug/remesh_*`) for driving,
inspecting, and tuning it outside the full app or the unit tests.

`QuadRemesh(input, params)` takes a triangle (or mixed) mesh and returns a
freshly-allocated **all-quad** mesh whose edge loops follow curvature and sharp
features and which is **provably free of spiraling iso-lines** (the integer
quantization step is what guarantees that). The input is never mutated, so the
host keeps it for undo.

This is a *global, one-shot* operation, not a per-dab brush — it borrows
dyntopo's module packaging, never its local/incremental use-case. The full design
rationale, literature references, and milestone history live in
[plans/quad-remeshing.md](plans/quad-remeshing.md); this document describes the
shipped implementation and how to drive it.

---

## The pipeline

`QuadRemesh` (`remesh/remesh.cc`) runs a synchronous sequence of stages. Each
fires the optional `RemeshProgressFn` at its boundary with a monotonic `0..100`
percentage and a stable lowercase tag:

| pct | stage         | what happens |
|----:|---------------|--------------|
|   0 | `copy`        | `buildTriCopy` deep-copies positions + face topology into a fresh working mesh, triangulates it, recomputes normals. The caller's input is left intact. |
|  10 | `decimate`    | *(only if `solve_edge_length > 0`)* `decimateForSolve` runs a dyntopo uniform-remesh pre-pass (Botsch-Kobbelt collapse/split/flip/tangential-smooth over a whole-mesh sphere) to coarsen the **solve** mesh so dense inputs stay tractable. Geometry only. |
|  25 | `cross_field` | M2: solve a smooth per-face 4-RoSy cross field (Diamanti 2014 complex-polynomial 4-PolyVector). Computes curvature (M1) and sharp/feature tags first as needed. |
|  45 | `singularity` | M3: re-solve the smoothest *phase* field with period jumps (hence singularities) held fixed — one real SPD Poisson solve on the face dual graph that provably lowers per-edge curl. |
|  65 | `quantize`    | M5: build the seamless parametrization (M4: cut graph + seamless map) **internally**, then snap every cut-edge translation to an integer (integer-grid map). This is the spiral-elimination step. |
|  80 | `extract`     | M6: extract the integer lattice's preimage QEx-style (Ebke 2013) — weld lattice points into grid vertices, trace iso-line arcs, walk the rotation system to emit one quad per grid cell. Returns `nullptr` on a clean failure (no lattice map) → progress `100 failed`. |
|  92 | `reproject`   | *(only if `reproject`)* snap each output vertex onto the **original full-res** input surface via a BVH closest-point query, with optional Laplacian smoothing between snaps. When the solve mesh was decimated, this re-copies the input so detail dropped by decimation is recovered. |
| 100 | `done`        | success; returns the heap-allocated quad mesh (caller owns, frees via the mesh allocator / `freeMesh`). |

Stage attrs are all written as `.remesh.*` **TEMP** layers (never serialized):

- `.remesh.f.theta` (float) — cross angle in the face frame, in `(−π/4, π/4]`
- `.remesh.e.period` (short) — period jump `0..3`
- `.remesh.v.pole_index` (short) — quarter-index per interior vertex (Σ == 4χ)
- `.remesh.v.kmin_dir`, `.remesh.v.kmax_dir` (float3) — principal-curvature
  unit tangents; `.remesh.v.k` (float2) = `(kmin, kmax)`
- `.remesh.c.uv` (float2) — the integer-grid map
- `.remesh.e.translation_q` (int2) — quantized cut translation

### Curvature estimation

`computeCurvature(mesh::Mesh&)` (`remesh/field/curvature.cc`) estimates discrete
principal curvature via the Cohen-Steiner & Morvan **normal-cycle shape
operator**: per vertex it accumulates the signed-dihedral rank-1 tensor over the
incident manifold edges, divides by the barycentric vertex area, and takes the
tangent eigenpairs (Eigen `SelfAdjointEigenSolver`). It runs `recalc_normals()`
first, so `m.v.no` is valid afterward. Note the deliberate **kmin/kmax direction
swap**: the most-bent edge direction (large eigenvalue) is the direction of
*minimum* surface curvature, so `kmax_dir = eigenvector(small)`,
`kmin_dir = eigenvector(large)` — verified on a cylinder. It is invoked on demand
from the cross-field stage (`field/constraints.cc`) gated on `use_curvature`, and
also directly by the debug app's curvature overlay (below).

### Quantization rounding (M5)

`computeQuantization` (`quantize/quantize_ilp.cc`) is a **penalty-formulation
MIQ rounder**: seam consistency and integer locks are `1e6` soft quadratics on
the `2M` corner-class system, solved with CHOLMOD LDL' + incremental
`cholmod_updown` rank updates natively, or per-round Eigen `SimplicialLLT`
refactorization under WASM. Design rationale, A/B history, and the measured
rejections live in [`plans/miq.md`](plans/miq.md) (the CoMISo comparison) — read
it before touching the rounding loop. The moving parts:

- **Greedy rounding (default).** Vertex-independent, most-confident-first
  batches: every unfixed cut side whose fractional distance to its nearest
  integer is within `confidence_radius` (`tau = 0.3` — measured better than
  CoMISo's cumulative error budget, miq.md Q3) locks per round, then the system
  re-solves. Side keys live in a generation-validated lazy min-heap with an
  explicit `(frac, side)` tie-break; after a converged local-GS round only
  sides incident to touched classes are re-keyed (a class→sides CSR), full
  re-keys happen only on direct-solve rounds (miq.md Q2).
- **Local Gauss-Seidel tier** (`QuantizeParams::use_local_gs`, default on).
  Each lock batch first tries a local GS relaxation seeded at the locked
  classes, escalating to a direct solve when it can't drain within the visit
  cap; the final solve is always direct. Under the `1e6` coupling the
  relaxation in practice spreads globally (miq.md Q1) — the tier's payoff is
  removing per-round refactorizations, chiefly under WASM.
- **`RoundingStrategy {GREEDY, DIRECT}`** (`QuantizeParams::rounding`, surfaced
  as `RemeshParams::quantize_direct_rounding`, miq.md Q4). DIRECT runs zero
  greedy rounds — every side locks at once off the initial seamless/ARAP-settled
  solve, one re-solve (`stats.iters == 0`). It is a fast path for clean inputs
  and the quality oracle GREEDY must never lose to (ctest
  `testDirectRounding`); on messy corpus inputs its single re-solve does not
  reach integrality (`feasible=false`) — it is never the quality default. A
  third, exact tier (CoMISo's Gurobi/CPLEX slot) is deliberately unbuilt; the
  enum comment in `quantize_ilp.h` documents the slot.
- **Stats.** `QuantizeStats` carries deterministic counters (`full_refactors`,
  `updowns`, `back_solves`, `tier1b_probes`, the `gs_*` local-GS profile, the
  `resort_*` re-key profile) plus volatile `*_ms` phase wall-clocks; both are
  written to the CLI manifest's `run.quantize` block, and only the counters may
  ever feed `metrics.csv`.

## Parameters (`RemeshParams`, `remesh/remesh_params.h`)

| field | default | meaning |
|-------|---------|---------|
| `target_edge_length` | `0.1` | target quad edge length (world units); drives the param scale → output face count |
| `solve_edge_length`  | `0.0` | solve-mesh edge length; `0` = solve on the raw input. `>0` coarsens the working copy first (pick a touch finer than `target_edge_length`) |
| `use_curvature`      | `true` | soft-align the field to principal curvature |
| `use_sharp_features` | `true` | hard-pin the field to sharp edges + open boundaries |
| `sharp_angle`        | `0.785` (~45°) | dihedral threshold (radians) for "sharp" |
| `use_density`        | `false` | scale quad spacing by the per-vertex `.remesh.v.density` map |
| `quantize_direct_rounding` | `false` | one-shot DIRECT rounding instead of greedy batches (see *Quantization rounding* below) |
| `reproject`          | `true` | snap output back onto the input surface (off = debugging) |
| `cap_odd_holes`      | `false` | close odd-length cap rims with one triangle each (trades the all-quad guarantee for watertightness on organic inputs) |
| `smooth_iterations`  | `2` | Laplacian passes interleaved with reprojection |
| `smooth_strength`    | `0.5` | per-iteration smoothing step `0..1` |
| `seed`               | `1` | determinism seed (M3 iteration / M5 tie-breaks); fixed input+seed → byte-identical output (cross-backend parity prerequisite) |

The struct is binding-header-free (out-of-line `defineBindings()` in
`remesh/bindings.cc`) and crosses the WASM/N-API seam by value.

## Source layout (`source/remesh/`)

```
remesh.{h,cc}          QuadRemesh orchestrator + tri-copy / decimate pre-pass
remesh_params.h        RemeshParams
bindings.{h,cc}        binding registration (Mesh_quadRemesh, param struct)
c-api/remesh_c_api.cc  Mesh *Mesh_quadRemesh(Mesh*, RemeshParams*) — WASM/N-API entry
field/curvature.*      M1 principal-curvature estimator
field/feature_tag.*    M1 sharp-edge / boundary tagging
field/cross_field.*    M2 4-RoSy cross-field solve
field/constraints.*    M1/M2 constraint assembly (curvature + features)
field/singularity_adjust.*  M3 fixed-period Poisson curl reduction
param/cut_graph.*      M4 cut-graph construction
param/seamless_param.* M4 seamless (u,v) parametrization
quantize/quantize_ilp.*  M5 integer-grid quantization (spiral elimination)
quantize/t_mesh.*      M5 T-mesh / class-space support
extract/quad_extract.* M6 QEx-style quad extraction
extract/reproject.*    M6 BVH snap-to-surface + Laplacian relax
cli/remesh_cli.cc      standalone CLI (see below)
```

OBJ I/O lives in the `mesh` lib: `mesh::loadObj(path, keepNgons)` /
`mesh::writeObj(mesh, path)` (`source/mesh/utils/obj_io.{h,cc}`) — `keepNgons`
preserves native quads; the tests-only shim fan-triangulates.

## Entry points

- **C++ direct:** `remesh::QuadRemesh(in, params, progress=nullptr, user=nullptr)`.
  The progress hook is kept out of `RemeshParams` so the binding system never
  sees a function pointer.
- **Cross-seam:** `Mesh_quadRemesh(Mesh* in, RemeshParams* params)` — the
  whole-mesh C entry (no progress), exported to WASM and wrapped by the N-API
  runtime.
- **Host TS:** `LiteMesh.quadRemesh` builds a `RemeshParams`, calls
  `Mesh_quadRemesh`, and swaps in the result; the `litemesh.quad_remesh()` ToolOp
  makes it undoable. Guarded by `tests/integration/litemesh_quad_remesh.test.ts`
  (boots both backends, checks topology fingerprints / undo-redo / GPU buffers /
  native↔WASM parity).

The C++ core is gated by `tests/test_remesh_extract.cc` under `node make.mjs test`
(grid / cylinder / torus / sphere / capped-cylinder + reprojection + end-to-end
`QuadRemesh` + the `Simple.obj` ARAP-untangle fixture).

---

## The debug app

`remesh_debug_app` is an interactive GUI for loading/importing/generating assets,
configuring params, running the remesher, watching live progress, and orbiting
the shaded result with quad edges drawn. It can also be driven **headlessly** over
a named pipe so an agent can inspect and control it.

### Architecture

- **`RemeshApp` (`debug/remesh_app.{h,cc}`)** is the single source of truth for all
  non-rendering state and actions (params, asset list, selection, job status, last
  result). Both the ImGui panel and the named-pipe channel drive the app through
  this one object, so a button click and `node tools/remesh_dbg.mjs run_remesh` do
  exactly the same thing. Everything here runs on the **main thread**.
- **The remesher runs as a separate process.** `runRemesh` spawns the standalone
  `remesh_cli.exe` (built as its own self-contained exe — every module is a STATIC
  lib) and parses its stdout. This means the remesher can be **rebuilt without
  restarting the app**: rebuild just the `remesh_cli` target (its exe is unlocked)
  and click Run again to pick up the new binary.
- **Threading discipline:** the pipe accept/reader thread and the subprocess
  reader thread only *enqueue*; every `Scene` / camera / mesh mutation happens on
  the main thread (the pipe marshals each command via a `promise`/`future`). No
  locking inside `RemeshApp`.
- **Overlays** (quad wireframe, curvature field) are ImGui background-draw-list
  lines projected with `scene.camera.viewProj(aspect)`. They are non-depth-tested
  and **window-only** — they do not appear in the offscreen `screenshot` capture
  (which is the shaded mesh only).

`debug/remesh_debug_config.h.in` (a `configure_file`) bakes in the assets dir,
results dir, repo-root `tools/` and `keys/` paths, the built `remesh_cli` path,
and the git commit.

### Building and running

From the worktree (load the sccache env first so builds are cache hits):

```powershell
cd C:\dev\webgl-app-framework-quad-remeshing
. .\worktree-env.ps1
cd sculptcore
node make.mjs configure native      # one-time / after CMake changes
node make.mjs build native          # builds remesh_cli.exe + remesh_debug_app.exe
build\native\source\debug\remesh_debug_app.exe   # launch
```

Rebuild **only** the CLI while the app stays open (the rebuild-without-restart
loop):

```powershell
cd sculptcore\build\native
node ..\..\configureEnv.mjs cmake --build . --target remesh_cli
```

`remesh_debug_app [--asset NAME] [--width N] [--height N]`. Mouse: LMB/RMB orbit,
Shift+LMB or MMB pan, scroll zoom.

### The UI panel (`debug/remesh_ui.cc`)

A single ImGui window, top to bottom:

- **Asset** — combo of OBJ stems in the assets dir; **Load** / **Import…**
  (`GetOpenFileNameW` → copy into assets) / **Rescan**.
- **Generate (Meshy text-to-3D)** — a prompt textbox + **Generate** (spawns
  `meshy_gen.mjs`, streams its progress to the same bar, selects the result).
- **Params** — one widget per `RemeshParams` field.
- **Run** — **Run Remesh** (disabled while busy or with no asset) + the **quad
  wireframe** toggle, a progress bar, and the last `STATS` line + manifest path.
- **View** — **Fit camera**, **axes**, the **curvature field** overlay controls,
  vert/face counts, fps.

### Curvature-field overlay

The **View** section has a **curvature field** checkbox + a **field line scale**
slider (`drawCurvatureField` in `remesh_debug_app.cc`). When enabled it draws each
vertex's principal-curvature directions as short centered segments — **blue for
kmin, red for kmax** (centered because principal directions are sign-ambiguous
lines). The field is **auto-computed on demand**: `BuiltinAttr::ensure()` returns
`true` when it just created the `.remesh.v.k{min,max}_dir` TEMP layers, which
triggers `computeCurvature` (the same estimator the cross-field stage runs, and
which also runs `recalc_normals`). A freshly loaded or remeshed mesh is a new
`Mesh` lacking the layers, so it recomputes; the same mesh keeps the cached field
across frames. Line length = `curvatureScale × bbox_diagonal` (scale-independent
across assets). Backface-culled to declutter; capped at 200k vertices/frame.

### Quad-wireframe overlay

The renderer fans quads to triangles, so quad edges are invisible in the shaded
pass. `drawQuadWireframe` projects every real mesh edge and draws it into the
ImGui background list (thin, semi-transparent), capped at 250k edges/frame. Toggle
it in the **Run** section.

---

## The standalone CLI (`remesh_cli`)

`remesh_cli.cc` loads one OBJ, runs `QuadRemesh`, validates the result
(`mesh::remeshValidate`), writes the quad OBJ + a JSON manifest, and streams a
line protocol to stdout (unbuffered, so a parent reads progress live). It is a
fresh process per run.

```
remesh_cli --input <obj> [options]
  --input <path>          input OBJ (bare name resolves against the assets dir)
  --outdir <dir>          output dir (default: tests/remesher-results)
  --name <base>           output basename (default: input stem)
  --target <float>        target quad edge length (default 0.1)
  --solve <float>         solve-mesh edge length, 0=off (default 0)
  --curvature <0|1>       align field to curvature (default 1)
  --sharp <0|1>           pin field to sharp edges/boundaries (default 1)
  --sharp-angle <float>   sharp dihedral threshold, radians (default 0.785)
  --density <0|1>         use per-vertex density map (default 0)
  --reproject <0|1>       snap output onto input surface (default 1)
  --cap-odd <0|1>         close odd holes w/ one tri each (default 0)
  --smooth <int>          reprojection smoothing iterations (default 2)
  --smooth-strength <f>   per-iteration smoothing step 0..1 (default 0.5)
  --seed <uint>           determinism seed (default 1)
```

**stdout protocol** (one record per line):

```
PROGRESS <pct> <stage>   monotonic 0..100; stages copy..done, or "failed"
RESULT <obj-path>        the written quad mesh
MANIFEST <json-path>     the written manifest
STATS k=v k=v ...        verts/edges/faces/quads/tris/ngons/allquad/manifold/
                         euler/inverted/boundary/spiral/irr + the Tier-0 metrics
                         regular_frac/components/holes/folds/min_angle/
                         area_ratio + duration_ms
ERROR <message>          fatal (also exit != 0; carries reason=<tag> on a clean
                         pipeline failure, which still writes a manifest)
```

Output files are `<name>_<YYYYMMDD-HHMMSS>.{obj,json}` in the output dir. Paths are
emitted with forward slashes (valid in JSON / uniform for the parent reader).

### Manifest schema (`remesh-manifest/1`)

Written by hand (no JSON lib in the deps). One file per run next to the OBJ, same
stem. Top-level keys: `schema`, `timestamp`, `git_commit` (baked at configure
time), `duration_ms`, and the blocks:

- `input` — `name`, `path`, `verts`, `faces`, `components`, `holes`, `manifold`,
  `aabb_min`, `aabb_max` (the input is itself run through `remeshValidate` so the
  manifest can compare input vs output topology)
- `params` — every `RemeshParams` field
- `output` — `path`, `verts`, `edges`, `faces`, `quads`, `tris`, `ngons`
- `validation` — `manifold`(+`manifold_error`), `euler`, `consistent_winding`,
  `non_manifold_edges`, `boundary_edges`, `degenerate_faces`, `inverted_faces`,
  `all_quad`, `irregular_interior_verts`, plus the **Tier-0 quality metrics**
  (`plans/quad-remeshing-filtering.md`): `interior_vert_count`,
  `regular_interior_frac`, `component_count`, `boundary_loop_count`,
  `max_component_irregular`, `max_adjacent_area_ratio`, `max_adjacent_edge_ratio`,
  `min_interior_angle`, `min_angle_hist` (9 bins), `parametrization_folds`, then
  `isolines_checked`, `spiral_isolines`, `open_isolines`, `closed_isolines`
- `run` — the `RemeshRunReport` (`remesh/remesh_report.h`): `success`,
  `failure_reason`, `pipeline_ms`, the cross-field stats (`num_singularities`,
  `index_sum`, `field_solved_eigen`), the quantize stats
  (`parametrization_folds`, `min_jacobian`, `quantize_feasible`, plus a
  `quantize` sub-block with the deterministic rounding-loop counters —
  `rounds`, `residual`, `full_refactors`, `updowns`, `back_solves`,
  `tier1b_probes`, the local-GS profile `gs_*`, the re-sort profile
  `resort_*` — and the volatile `*_ms` phase wall-clocks, which stay
  manifest-only, never in `metrics.csv`), and a `stages`
  map of per-stage status (`copy`/`decimate`/.../`reproject` → `ok`/`failed`/`skipped`)

A clean pipeline failure (no output mesh, e.g. `extract_no_lattice`) still writes
a manifest: the `validation`/`output` blocks are default-valued and the `run`
block carries the `failure_reason` + per-stage status, so a batch run records the
failed attempt rather than losing it.

The metric definitions (what counts as a "component" vs a "boundary loop" vs a
"fold") live next to the fields in `mesh/utils/mesh_validate.h`; read those before
comparing numbers across runs.

The input AABB is computed with an explicit min/max loop, **not** `mesh::calcAABB`
(which seeds max to `FLT_MIN` and mis-handles all-negative meshes).

### Corpus batch runner (`tools/remesh_corpus.mjs`)

Tier 0c. Runs `remesh_cli` over every available asset in
`tests/corpus/corpus.json` and aggregates the manifests into a metrics table so
each tier's review gate has a baseline to diff. Two artifacts in
`tests/remesher-results/corpus/`: `metrics.csv` (one row per asset, the
*deterministic* quality columns only — fixed seed reproduces it byte-for-byte)
and `results.json` (the full per-asset manifests). Assets that don't resolve (the
`available:false` placeholders) are logged as skipped, never silently dropped. See
[`tests/corpus/README.md`](../tests/corpus/README.md).

```
node tools/remesh_corpus.mjs            # run every available asset
node tools/remesh_corpus.mjs --list     # corpus + which assets resolve
node tools/remesh_corpus.mjs --only simple-closed
```

---

## Headless control: the named pipe

`PipeServer` (`debug/remesh_pipe_server.{h,cc}`) listens on
`\\.\pipe\sculpt-remesh-debug`. The accept thread reads one command line per
connection, hands it to the main thread (drained each frame), waits for the reply,
and writes it back framed with a lone `.` terminator line (`<body>\n.\n`). The
handler runs on the main thread, so it can freely touch the scene.

**`node tools/remesh_dbg.mjs <command> [args...]`** is the agent's CLI into the
running app — it connects to the pipe, sends `args.join(' ')`, and prints the
framed reply. Commands (`help` lists them):

| command | effect |
|---------|--------|
| `help` | list commands |
| `list_assets` | OBJ stems in the assets dir |
| `load_asset <name>` | load an asset into the viewport |
| `import_asset <path>` | copy an external file into assets + load it |
| `meshy_gen <prompt>` | text-to-3D a new asset (async) |
| `get_params` | current params as `name=value` lines |
| `set_param <name> <v>` | set one param |
| `run_remesh` | remesh the selected asset (async) |
| `last_result` | obj / manifest / stats of the last run |
| `get_state` | JSON: asset, mesh counts, job (`running`/`progress`/`stage`), camera, last result, status |
| `camera_fit` / `camera_get` | frame the camera to the AABB / read eye·target·up·fov |
| `screenshot <path>` | write a shaded PNG of the offscreen view |
| `quit` | close the app |

> A job is async: `run_remesh` / `meshy_gen` return `OK started` immediately. Poll
> `get_state` and wait for `"running": false` (the idle state is `running:false`,
> not a `job:none` field).

Example session:

```sh
node tools/remesh_dbg.mjs list_assets
node tools/remesh_dbg.mjs load_asset Simple
node tools/remesh_dbg.mjs set_param target_edge_length 0.05
node tools/remesh_dbg.mjs run_remesh
node tools/remesh_dbg.mjs get_state          # poll until running:false
node tools/remesh_dbg.mjs last_result
node tools/remesh_dbg.mjs screenshot out.png
```

## Meshy asset generation (`tools/meshy_gen.mjs`)

Zero-dependency Node ESM (built-in `fetch`). Creates a Meshy **text-to-3D**
preview task (`mode: 'preview'`, `ai_model: 'meshy-5'` — the untextured base mesh
is what we want to remesh), polls until `SUCCEEDED` forwarding `PROGRESS`, then
downloads `model_urls.obj` into `sculptcore/tests/assets/<slug>.obj` and emits
`RESULT <path>`. It streams the same `PROGRESS`/`RESULT`/`ERROR` protocol the app
parses.

**Security:** the API key lives in `keys/meshy.txt` (gitignored at the repo root —
`/keys/`). The script **refuses to run if that path is git-tracked** and never
prints the key. Never commit, echo, or log the key.

---

## Conventions / gotchas

- Overlays are **window-only**; `screenshot` captures the shaded offscreen mesh
  without them.
- The **solve/reproject split** is deliberate: `solve_edge_length` coarsens only
  the *solve* mesh; reprojection always targets the original full-res surface, so
  detail is preserved.
- `RemeshProgressFn` is intentionally **not** a `RemeshParams` field (keeps the
  reflected/bound struct free of function pointers); the CLI passes a
  stdout-streaming callback, the app/N-API path passes none.
- The WASM main-thread stack must be enlarged (`-sSTACK_SIZE`); the Eigen-heavy
  pipeline overflows the 64 KB default **silently**. Native is immune. See
  [plans/quad-remeshing.md](plans/quad-remeshing.md) for the full account.
