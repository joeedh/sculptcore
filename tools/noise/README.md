# Brush-noise metric fixtures

Fixtures for the displacement-base A/B
(`documentation/plans/2026-07-26-0909-brush-displacement-base-attribute.md`
§9.1). Run one with:

```
build/native/source/debug/debug_app.exe --script sculptcore/tools/noise/<f>.txt
```

`stroke_path ... rough=1` prints the per-dab trace; the `roughness` verb scores
the last stroke's swept region on demand. Each line reports the one-ring normal
roughness of the **live** surface and of the derived **base** point set
(`co - disp`, else the live position), plus the live-only fidelity guard
(`maxdisp`, `vol`).

| script | surface | dyntopo | tangential smooth |
|---|---|---|---|
| `noise_grid_smooth` | flat grid | on | on (app default) |
| `noise_grid` | flat grid | on | off |
| `noise_grid_nodyntopo` | flat grid | off | — |
| `noise_sphere` | sphere | on | on |
| `noise_sphere_nosmooth` | sphere | on | off |
| `noise_sphere_nodyntopo` | sphere | off | — |

Baselines on the pre-M1 absolute-snapshot path (`.brush.orig.co`, since deleted):
`documentation/research/2026-07-26-brush-base-noise-baseline.md`.
The primary fixture also runs as a ctest, `test_brush_noise`.
