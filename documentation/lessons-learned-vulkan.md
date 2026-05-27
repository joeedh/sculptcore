# Lessons learned (Vulkan) — what carries over to WebGPU

Notes from debugging the native Vulkan brush path that are worth keeping
in mind when the same workloads run on WebGPU. These are about the
*shape* of the GPU↔CPU interaction, not API trivia — that shape is what
bites, and it bites the same way (or worse) on WebGPU.

---

## 1. CPU readback memory is not free, and the trap looks like a stall

The biggest hitch we chased — a ~1s periodic stutter while sculpting —
was per-dab readback of moved vertices into the CPU mesh. The buffers
(`co_`/`no_` in `source/vulkan/vk_compute.cc`) were allocated
`HOST_VISIBLE | HOST_COHERENT` with **no `HOST_CACHED`**. On a discrete
GPU that resolves to *write-combined* memory: fast to write, but CPU
*reads* are uncached and crawl. The scattered `coSrc[v*4]` readback was
~24 ms/dab and grew as the brush touched more verts.

Fix: prefer `HOST_CACHED` for any buffer the CPU reads back, fall back to
uncached if the device has no cached host-visible heap.

**WebGPU translation.** This exact footgun can't happen — WebGPU doesn't
let you map a `STORAGE` buffer or read device memory synchronously at
all — but the underlying cost is still there and the API forces a
*different* discipline:

- A buffer can't be both `STORAGE` and `MAP_READ`. To read compute output
  you must `copyBufferToBuffer` into a dedicated `COPY_DST | MAP_READ`
  staging buffer, then `mapAsync` it.
- The "write-combined vs cached" decision is made *for* you: the mappable
  readback buffer is the cached one, the storage buffer stays
  device-local. So the right structure on Vulkan (separate readback
  buffer, copy only what you need) is *mandatory* on WebGPU. Build it
  that way once and both backends are happy.

---

## 2. Readback is synchronous on Vulkan, asynchronous on WebGPU — design for async

Our Vulkan dab loop is: submit → `vkWaitForFences` → read mapped memory →
update CPU mesh → next dab. That synchronous wait is acceptable natively.

**WebGPU has no synchronous readback.** `mapAsync()` resolves on a future
event-loop tick; there is no `vkWaitForFences` you can block on. A naive
port of "submit, wait, read, repeat" will either deadlock the frame or
collapse to one dab per several frames.

If the same per-dab CPU readback is needed on WebGPU, restructure so the
hot loop never blocks on the GPU:

- **Pipeline it** — consume dab *N*'s readback while dab *N+k* runs, never
  wait on the dab you just submitted.
- **Or eliminate it** — keep the authoritative positions on the GPU for
  the whole stroke and only read back at stroke end (we already defer
  node-bounds regen this way; `node->update(Spatial_RegenBounds)` just
  sets a flag, it doesn't recompute mid-stroke). The per-dab CPU mesh
  write existed for ray-pick / bounds correctness — check whether anything
  *actually* consumes it before the next tree update; often nothing does.

Readback in the hot path is the single thing most likely to need
redesign, not porting, for WebGPU.

---

## 3. Batch work into one submit; minimize round-trips

We collapsed each dab from three-plus queue submits (dab, normals,
scatter) into a single `runOneShot` recording — one queue-wait instead of
several — and made that one-shot reuse a persistent command buffer +
fence instead of allocating/freeing per submit (`vk_context.{h,cc}`).

**WebGPU translation.** You can't reuse a `GPUCommandBuffer` (encoders are
single-use), so the "reuse the command buffer" half doesn't port. But the
"one submit, not several" half matters *more*: each `queue.submit` and
each `mapAsync` has real overhead and an async boundary. Record the dab,
the normal recompute, and the scatter into one `GPUCommandEncoder` and
submit once. Treat submit boundaries the way you'd treat the synchronous
fence waits here — minimize them.

---

## 4. The bug only shows up in the live path — profile there

The stutter never reproduced under the scripted/batch harness, because
that path doesn't do the per-dab live readback. It only appeared
interactively. When chasing a perf regression:

- Reproduce in the path that actually has the problem (here:
  `--interactive`, a setup-only script, real hand input).
- Add temporary, `--profile`-gated phase counters / spike logs to localize
  the cost to cpu / gpu / readback, *then* fix, *then* rip the scaffolding
  out. (See the "Profiling a periodic hitch" note in `CLAUDE.md`.)

This carries over verbatim: a WebGPU live-path hitch won't show in a
headless replay either, and the async timing makes wall-clock phase
splits even more valuable. Use `timestamp-query` for GPU time and
remember your wall-clock numbers include the async map latency.

---

## TL;DR for the WebGPU port

- Never read a storage buffer directly; copy to a `MAP_READ` staging
  buffer and read only what you need.
- Assume readback is async — never block the hot loop on the GPU;
  pipeline it or defer it to stroke end.
- One encoder, one submit per dab.
- Profile the live path, not the batch replay.
