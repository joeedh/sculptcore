# Meshy.ai 3D Model Generation Guide

A practical guide to using Meshy.ai for generating game-ready 3D assets, drawn from lessons learned building the rpgtest bake pipeline.

---

## Overview

Meshy converts text prompts into rigged, animated GLB files through a multi-stage pipeline:

```
text prompt → preview mesh → refine (textures) → rig → animate (per clip)
```

Each stage produces an intermediate GLB you can inspect before the next stage runs. The bake pipeline (`pnpm bake`) handles all of this automatically with caching at every stage so you never pay for work you've already done.

---

## Quick Start

1. Get an API key from [meshy.ai](https://meshy.ai) and place it in `keys/meshy.txt`.
2. Add your character or prop to `assets/manifest.json5`.
3. Run `pnpm bake --id <your-id>`.

```json5
// assets/manifest.json5
{
  version: 1,
  entries: [
    {
      id    : 'hero',
      kind  : 'character',
      style : 'stylized',
      prompt: 'human male warrior, realistic human anatomy, slim athletic build, ...',
      clips : [
        { actionId: 0 },               // Idle → assets/hero.glb
        { actionId: 30, suffix: 'walk' }, // Casual_Walk → assets/hero.walk.glb
      ],
    },
    {
      id    : 'fountain',
      kind  : 'prop',
      style : 'stylized',
      prompt: 'stone town square fountain, JRPG, low-poly, weathered',
    },
  ],
}
```

---

## Prompt Writing

### Characters

Good character prompts are specific about anatomy, clothing silhouette, and topology:

```
human male warrior, realistic human anatomy, slim athletic build,
short brown hair, plain blue tunic with silver trim, brown trousers,
leather boots, clean game-ready topology, low-poly
```

Key tips:
- **Always say "full body"** if you want the whole model, not a bust.
- **Mention "low-poly"** — Meshy defaults to higher fidelity if you don't.
- **Describe anatomy first**, then clothing. The rigger classifies limbs from the mesh; loose-fitting sleeves or robes confuse it at limb boundaries.
- **Avoid vague style tags** like "fantasy" or "epic" — they bloat the prompt without improving the mesh.
- **Don't mention pose** in the prompt — `pose_mode: 't-pose'` is set automatically for characters and overrides the prompt.

Prompts that worked well in this repo:
```
young mage in red robes, JRPG companion, full body, T-pose, low-poly
older king with crown and beard, JRPG NPC, full body, T-pose, low-poly
wispy black shadow creature with glowing white eyes, JRPG enemy, low-poly
```

### Props and Environment

Props use a single preview stage (no rig/animate). Keep them short and concrete:

```
stone town square fountain, JRPG, low-poly, weathered
```

Tips:
- Include **material hints** ("weathered stone", "iron", "oak wood") — they drive the PBR texture bake.
- **"low-poly"** keeps polygon count in check for scene-placed instances.
- Avoid describing scale (e.g., "large fountain") — metric normalization handles size after the bake.

---

## API Parameters — What Actually Matters

### Preview Stage

```typescript
{
  mode            : 'preview',
  ai_model        : 'meshy-6',          // Always use the latest named model
  art_style       : 'realistic',        // 'realistic' for humanoids; 'sculpture' for stylized objects
  seed            : entry.seed,         // Integer — omit for random; fix to reproduce a result
  pose_mode       : 't-pose',           // Characters only — T-pose gives rigger the clearest limb view
  symmetry_mode   : 'on',              // Characters only — improves limb consistency
  should_remesh   : true,              // CRITICAL — see below
  target_polycount: 30_000,            // Characters: 30k; Props: 15k
}
```

**`should_remesh: true` is critical.** Without it, meshy-6 ignores `target_polycount` and can return a multi-million-polygon raw scan-style mesh. Always force remesh.

**T-pose vs A-pose:** T-pose is better for rigging. A-pose was found to mis-classify limbs at sleeve boundaries — the auto-rigger's limb detection works from silhouette and T-pose gives it unambiguous shoulder-to-wrist lines.

**`symmetry_mode: 'on'`** ensures limbs are mirror-symmetric, which the auto-rigger depends on for clean skinning weights.

### Refine Stage

```typescript
{
  mode           : 'refine',
  preview_task_id: previewTaskId,  // Reference by task ID, not a URL
  enable_pbr     : true,           // Always true — generates metallic/roughness maps
  hd_texture     : true,           // Higher resolution textures
}
```

Reference the preview by `preview_task_id`, not by downloading the GLB and re-uploading it. Meshy fetches the canonical asset directly; short-lived signed URLs can expire between stage completion and the next API call.

### Rig Stage

```typescript
{
  input_task_id: refineTaskId,  // Again: task ID, not URL
  height_meters: 1.8,           // Meshy will attempt to scale the rig to this height
}
```

Passing `height_meters` here is not fully reliable — the bake pipeline still applies its own post-bake metric normalization to guarantee the output is within ±5% of 1.8m. Treat it as a hint, not a guarantee.

### Animate Stage

Clips are applied to the rig by numeric `action_id`:

```typescript
{
  rig_task_id: rigTaskId,
  action_id  : 30,  // Casual_Walk
}
```

Common action IDs from the [Meshy animation library](https://docs.meshy.ai/api/animation-library):

| ID  | Name              | Use Case           |
|-----|-------------------|--------------------|
| 0   | Idle              | Stand-still default |
| 30  | Casual_Walk       | Normal locomotion  |
| 106 | Confident_Walk    | Hero entrance      |

Each clip generates a separate GLB. The rig is reused across clips — you only pay for the preview/refine/rig stages once.

---

## Response Schema Gotchas

The Meshy API response shape has changed across versions. Two things to handle defensively:

**Rigging task URL location:**
```typescript
// v1 nests the URL under `result`; older docs say it's top-level
function rigGlbUrl(t: RigTask): string | undefined {
  return t.rigged_character_glb_url ?? t.result?.rigged_character_glb_url
}
```

**Animation task URL location:**
```typescript
// Same pattern — check both locations
function animGlbUrl(t: AnimationTask): string | undefined {
  return t.animation_glb_url ?? t.result?.animation_glb_url
}
```

Always check both the top-level field and the nested `result` object. If you only read one, half your deploys will silently get no URL.

---

## Caching Strategy

Each stage is content-addressed by a hash of `{id, prompt, kind, style, seed}`. Task IDs are saved to `assets/cache/.tasks/<id>-<hash>.json` after each stage completes:

```json
{
  "previewId": "task_abc123",
  "refineId": "task_def456",
  "rigId": "task_ghi789"
}
```

This means:
- Interrupting a bake mid-pipeline is safe — the next run picks up where it left off.
- Changing the prompt invalidates the cache and triggers a fresh preview.
- Changing only the clip list reuses the existing rig and only re-runs animate.

**To re-run from a specific stage** (e.g., you want a better texture bake without regenerating the mesh):
```
pnpm bake --id hero --from refine
```

This clears the refine and rig task IDs and re-runs from the refine stage forward.

---

## Metric Normalization

Meshy does not guarantee consistent scale across generations. Post-bake normalization is mandatory for a consistent world:

- **Characters:** measured by Y-axis height in bind pose (T-pose), scaled to 1.8m. The rig's `height_meters: 1.8` param helps but is not precise enough — always re-measure.
- **Props:** measured by max bounding-box dimension, scaled to a target size. Set `expectedMaxDimMeters` in the manifest to pin the size; without it the pipeline asks Gemini to estimate the real-world size of the object.

Normalization is idempotent within ±5%, so re-running the bake on an already-correct cache is always safe.

**For skinned GLBs:** scale is applied to the scene root node's `scale` field, not to vertex data. This preserves inverse bind matrices and animation channels.

**For static props:** scale is baked into the POSITION accessor bytes. `loadGLBStatic` reads raw vertex data without honoring node TRS, so the scale must live in the geometry.

---

## Root Motion

Walk clips from Meshy bake forward translation onto the root joint. If you move the entity in code *and* let the animation play the root translation, the character either skates or moonwalks.

The fix used here is not to strip the channel, but to measure it:

1. After baking each clip, `writeClipMotionSidecar` measures net horizontal root displacement over the clip duration.
2. It writes `<clip>.motion.json` alongside the GLB: `{distance, duration, rootNode}`.
3. At runtime, the loader drops the root translation channel and stores the descriptor.
4. Gameplay code drives world-space motion and scales clip playback by `distance/duration` for footplant sync.

If you add a new animation clip file, run the sidecar generation explicitly — `pnpm bake` does it automatically, but manually-placed GLBs won't get a sidecar.

---

## Output File Layout

```
assets/
  hero.glb                   ← stable runtime path (mirrored from cache)
  hero.walk.glb              ← secondary clip; suffixed
  hero.walk.motion.json      ← root-motion sidecar

assets/cache/
  hero-<hash>-preview.glb    ← stage diagnostic (preview mesh)
  hero-<hash>-refine.glb     ← stage diagnostic (textured)
  hero-<hash>-rigged.glb     ← stage diagnostic (rigged, bind-pose)
  hero-<hash>.glb            ← content-addressed final (idle clip)
  hero-<hash>.walk.glb       ← content-addressed final (walk clip)
  hero-<hash>.walk.motion.json
  .tasks/
    hero-<hash>.json         ← cached task IDs for each stage
```

The runtime loads from `assets/<id>[.suffix].glb`. The cache files are deterministic — carry the whole `assets/cache/` directory between machines and no re-bake is needed.

---

## Common Failures

**"meshy preview create failed: 400"**
- `should_remesh` or `target_polycount` in a format the API version doesn't expect. Check the API changelog; parameter names change between meshy-4/5/6.

**Rigger mis-classifying limbs**
- Switch `pose_mode` to `t-pose` (not `a-pose`). Loose-fitting clothing at the sleeve boundary confuses the per-limb silhouette detector.
- Add `symmetry_mode: 'on'`.

**Animation URL missing in response**
- Check both `t.animation_glb_url` and `t.result?.animation_glb_url`. The nesting moved between API versions.

**Model comes out at wrong scale**
- The `height_meters` rig param is advisory. Run `pnpm bake --no-rescale` to see the raw output, then check if `expectedHeightMeters` or `expectedMaxDimMeters` in the manifest needs to be set.

**Cache collision after changing provider**
- The entry hash omits `provider` when it's `meshy` (legacy compat). If you switch the same entry to `tripo3d`, a new hash is generated automatically — you won't collide.

**Stuck task (no progress after 10+ minutes)**
- Tasks time out after ~240 polls (~20 minutes). If a task is genuinely stuck, use `--from <stage>` to invalidate and restart from that stage.

---

## CLI Reference

```
pnpm bake                        # Bake all manifest entries
pnpm bake --id hero              # Bake one entry
pnpm bake --id hero --force      # Re-bake even if cached output exists
pnpm bake --id hero --from refine  # Re-run from the refine stage onward
pnpm bake --id hero --action 106   # One-off: bake a single action ID
pnpm bake --id hero --action 106 --out-suffix confident-walk
pnpm bake --id hero --no-rescale   # Skip metric normalization (diagnostic)
pnpm bake --dry-run              # Validate manifest + print hashes, no API calls
```
