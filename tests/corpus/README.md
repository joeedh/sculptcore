# Quad-remesh corpus

A small, fixed set of assets the quad remesher is measured against, run-over-run,
so every tier of [`plans/quad-remeshing-filtering.md`](../../documentation/plans/quad-remeshing-filtering.md)
has an honest baseline to diff. This is Tier 0c.

## Running it

```
node make.mjs build native            # build remesh_cli first
node tools/remesh_corpus.mjs          # run every available asset
node tools/remesh_corpus.mjs --list   # show the corpus + which assets resolve
node tools/remesh_corpus.mjs --only simple-closed,anime-girl
```

Outputs land in `tests/remesher-results/corpus/` (gitignored):

- **`metrics.csv`** — one row per asset, the *deterministic* quality columns
  only (no timestamps, paths, or durations). A fixed seed reproduces this file
  byte-for-byte, so this is the artifact to diff between tiers.
- **`results.json`** — the full per-asset manifest (durations, the min-angle
  histogram, the per-stage run report) for deep inspection.

The runner shells out to `remesh_cli` once per asset and reads the JSON manifest
it writes (`remesh-manifest/1`). Per-asset remesh params come from each entry's
`params` block (keys are `remesh_cli` flag names without the `--`).

## `corpus.json`

```jsonc
{
  "schema": "remesh-corpus/1",
  "seed": 1,                       // corpus-wide determinism seed (--seed overrides)
  "assets": [
    {
      "name": "simple-closed",     // stable row key
      "asset": "Simple.obj",       // resolved against tests/assets, or an abs path
      "category": "clean-baseline",
      "note": "...",
      "params": { "target": 0.2 }, // remesh_cli flags (no leading --)
      "available": true            // false => placeholder, always skipped
    }
  ]
}
```

## The intended ugly-asset set

Tier 0c calls for ~6–10 deliberately ugly assets spanning the failure modes the
later tiers target. Large/3rd-party meshes are **kept out of git** — only the two
small checked-in controls ship here; the rest are `available:false` placeholders.
To add one, drop the OBJ into `tests/assets/` (or point `asset` at an absolute
path) and flip `available` to `true` (or delete the flag).

| name              | category            | what it stresses                                          | status      |
|-------------------|---------------------|-----------------------------------------------------------|-------------|
| `simple-closed`   | clean-baseline      | sanity control — any regression here is real              | checked in  |
| `anime-girl`      | meshy-character     | dense Meshy character; **currently fails extraction** (folds past the gate) — the headline target for later tiers | checked in  |
| `raw-scan`        | photogrammetry-scan | noisy normals, non-manifold edges, floating speckles       | placeholder |
| `thin-clothing`   | thin-sheet          | near-coincident double walls folding the cross field       | placeholder |
| `holed-face`      | holed-organic       | eye/mouth holes + non-manifold fan verts → boundary loops  | placeholder |
| `multi-accessory` | multi-component     | disconnected shells of different scale → per-component / area ratio | placeholder |

## Metric definitions (see `mesh_validate.h`)

The columns mean exactly what the validation header documents. A few that are
easy to misread:

- **`regular_frac`** = `1 − irregular_interior_verts / interior_vert_count`.
  Uses the *interior* vert count as the denominator (boundary verts are excluded),
  so it is meaningful on open character meshes — unlike `1 − irregular/total`.
- **`components`** = connected components over the **face-adjacency** graph (faces
  adjacent iff they share an edge). Wire/isolated verts are ignored.
- **`holes`** (`boundary_loop_count`) = number of **boundary loops** — connected
  components of the boundary-edge graph (edges with exactly one incident face).
  **Not** genus handles, **not** the M6 output residual cap loops.
- **`param_folds`** = pre-extraction parametrization folds on the *solve* mesh's
  `(u,v)` (faces with `det(grad u, grad v) ≤ 0`). Distinct from the output mesh's
  `inverted_faces`.
- **`min_angle`** = smallest interior corner angle over all output faces (radians);
  the per-face min-angle histogram (10° bins over [0,90)) lives in `results.json`.
