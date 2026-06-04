import {test, expect} from '@playwright/test'

// Renders the fixed demo scene (a spherified cube) through the C++ WebGPU
// backend in a real browser and gates it against a committed golden PNG.
// The fixture sets window.__renderReady once webgpuRenderScene() succeeds, or
// window.__renderError with the failure.
test('webgpu backend renders the demo scene', async ({page}) => {
  const consoleErrors: string[] = []
  page.on('console', (msg) => {
    if (msg.type() === 'error') consoleErrors.push(msg.text())
  })
  page.on('pageerror', (err) => consoleErrors.push(String(err)))

  await page.goto('/tests/webgpu/fixture.html')

  await page.waitForFunction(() => (window as any).__renderReady === true || (window as any).__renderError !== null)

  const renderError = await page.evaluate(() => (window as any).__renderError)
  expect(renderError, `render failed: ${renderError}\nconsole: ${consoleErrors.join('\n')}`).toBeNull()

  const canvas = page.locator('#canvas')
  await expect(canvas).toHaveScreenshot('demo-scene.png', {maxDiffPixelRatio: 0.02})
})
