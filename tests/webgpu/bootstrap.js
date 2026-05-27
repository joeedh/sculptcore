// Thin device/canvas bootstrap for the WebGPU C++ backend.
//
// Everything after device creation lives in C++ (source/webgpu): this module
// only does the async bits that are awkward there — request the adapter/device
// and hand the device to the emdawnwebgpu runtime via the documented
// `preinitializedWebGPUDevice` Module hook. The C++ side adopts it through
// emscripten_webgpu_get_device() and creates the surface from the "#canvas"
// selector itself.

import createModule from '/build/sculptcore-browser.js'

/**
 * Boot the WASM module against a freshly created WebGPU device and render one
 * frame of the fixed demo scene into the page canvas (id="canvas").
 *
 * @param {number} width
 * @param {number} height
 * @returns {Promise<{module: any, ok: boolean}>}
 */
export async function bootAndRender(width, height) {
  if (!navigator.gpu) {
    throw new Error('WebGPU not available (navigator.gpu is undefined)')
  }

  // Force the software (SwiftShader) adapter when offered: it renders
  // deterministically across machines, so the golden PNG is portable. Falls
  // back to whatever adapter exists if no fallback is advertised.
  const adapter =
    (await navigator.gpu.requestAdapter({ forceFallbackAdapter: true })) ||
    (await navigator.gpu.requestAdapter())
  if (!adapter) {
    throw new Error('navigator.gpu.requestAdapter() returned null')
  }
  const device = await adapter.requestDevice()

  // emdawnwebgpu reads this off the Module arg and exposes it to C++ via
  // emscripten_webgpu_get_device().
  const module = await createModule({ preinitializedWebGPUDevice: device })

  const ok = module._webgpuRenderScene(width, height) === 1
  return { module, ok }
}
