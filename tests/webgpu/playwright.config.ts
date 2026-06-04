import {defineConfig} from '@playwright/test'
import path from 'node:path'
import {fileURLToPath} from 'node:url'

// serv.mjs serves files relative to its cwd and sets the COOP/COEP headers the
// pthread/SharedArrayBuffer WASM build needs. Launch it from the sculptcore
// root so /build/* and /tests/* resolve.
const repoRoot = path.resolve(fileURLToPath(new URL('.', import.meta.url)), '../..')
const PORT = 5004

export default defineConfig({
  testDir             : '.',
  // The render is deterministic; a single worker keeps the shared dev server
  // and golden snapshots stable.
  workers             : 1,
  fullyParallel       : false,
  // Software WebGPU warms up slowly on first pipeline compile.
  timeout             : 60_000,
  expect              : {timeout: 30_000},
  snapshotPathTemplate: '{testDir}/golden/{arg}{ext}',
  use: {
    baseURL      : `http://localhost:${PORT}`,
    browserName  : 'chromium',
    // The default Playwright "chromium" is the headless *shell*, which ships
    // without the Dawn/WebGPU backend — navigator.gpu.requestAdapter() returns
    // null. The full Chrome-for-Testing build (channel: 'chromium') has WebGPU
    // and runs new-headless with software rendering.
    channel      : 'chromium',
    launchOptions: {
      // Software WebGPU: Dawn on SwiftShader Vulkan, no real GPU required
      // (matches the software path the Dawn replay harness uses in CI). The
      // bootstrap forces the fallback (SwiftShader) adapter for a portable
      // golden. Do NOT add --use-angle=swiftshader: it makes requestAdapter()
      // return null here.
      args: ['--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--enable-features=Vulkan'],
    },
  },
  webServer: {
    command            : 'node serv.mjs',
    cwd                : repoRoot,
    url                : `http://localhost:${PORT}/tests/webgpu/fixture.html`,
    reuseExistingServer: true,
    timeout            : 30_000,
  },
})
