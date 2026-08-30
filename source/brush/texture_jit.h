#pragma once

namespace sculptcore::brush {

/** Whether runtime CPU compilation of texture scripts (the embedded tinycc
 * JIT) works in this process. The first call JITs and runs a trivial
 * function and caches the verdict — this is where a hardened-runtime macOS
 * host without the allow-jit entitlement (MAP_JIT) shows up as a clean
 * `false` instead of a crash. False on WASM builds, which have no tcc and
 * use precompiled textures only. Hosts should degrade to the precompiled
 * registry when this is false; documentation/plans/texture-scripts.md (T3). */
bool textureScriptCpuAvailable();

} // namespace sculptcore::brush
