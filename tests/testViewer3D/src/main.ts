import {JsonFileSource, pickLogDir} from './data/JsonFileSource'
import type {ElemKind, MeshLog, MeshSnapshot} from './data/MeshSource'
import {OrbitCamera} from './gl/camera'
import {initGL, resizeCanvas} from './gl/context'
import {MeshDraw} from './draw/MeshDraw'
import {TopoOverlay} from './draw/TopoOverlay'
import {pickElement} from './draw/Picking'
import {Timeline} from './ui/Timeline'
import {Inspector} from './ui/Inspector'

interface Selection {
  kind: ElemKind
  id: number
}

const main = async (): Promise<void> => {
  const canvas = document.getElementById('view') as HTMLCanvasElement
  const gl = initGL(canvas)
  const cam = new OrbitCamera()
  cam.attach(canvas)

  const meshDraw = new MeshDraw(gl)
  const overlay = new TopoOverlay(gl)
  const inspector = new Inspector(document.getElementById('inspector')!, (sel) => {
    selection = sel
    refreshSelection()
  })

  let source: JsonFileSource | null = null
  let log: MeshLog | null = null
  let snap: MeshSnapshot | null = null
  let selection: Selection | null = null

  const opts = {faces: true, edges: true, verts: true, overlay: true}
  const bind = (id: string, key: keyof typeof opts): void => {
    const el = document.getElementById(id) as HTMLInputElement
    el.addEventListener('change', () => {
      opts[key] = el.checked
    })
  }
  bind('tg-faces', 'faces')
  bind('tg-edges', 'edges')
  bind('tg-verts', 'verts')
  bind('tg-overlay', 'overlay')

  const refreshSelection = (): void => {
    if (snap) {
      overlay.build(snap, selection)
      inspector.render(snap, selection)
    }
  }

  const setSnapshot = (s: MeshSnapshot): void => {
    snap = s
    meshDraw.upload(s)
    fitCamera(s)
    refreshSelection()
  }

  const fitCamera = (s: MeshSnapshot): void => {
    let minX = Infinity
    let minY = Infinity
    let minZ = Infinity
    let maxX = -Infinity
    let maxY = -Infinity
    let maxZ = -Infinity
    for (let i = 0; i < s.caps.v; i++) {
      if (!s.v.alive[i]) {
        continue
      }
      const x = s.v.co[i * 3]
      const y = s.v.co[i * 3 + 1]
      const z = s.v.co[i * 3 + 2]
      if (x < minX) minX = x
      if (y < minY) minY = y
      if (z < minZ) minZ = z
      if (x > maxX) maxX = x
      if (y > maxY) maxY = y
      if (z > maxZ) maxZ = z
    }
    if (!isFinite(minX)) {
      return
    }
    cam.target[0] = (minX + maxX) * 0.5
    cam.target[1] = (minY + maxY) * 0.5
    cam.target[2] = (minZ + maxZ) * 0.5
    const r = Math.max(maxX - minX, maxY - minY, maxZ - minZ)
    cam.dist = Math.max(0.5, r * 1.8)
  }

  const timeline = new Timeline((step) => {
    if (!log) {
      return
    }
    const s = step === 0 ? log.initial : log.steps[step - 1].snapshot
    meshDraw.highlight = step > 0 ? log.steps[step - 1].highlight ?? null : null
    /* on first load fit camera; on scrub keep camera */
    if (snap === null) {
      setSnapshot(s)
    } else {
      snap = s
      meshDraw.upload(s)
      refreshSelection()
    }
  })

  const logSelect = document.getElementById('log-select') as HTMLSelectElement
  logSelect.addEventListener('change', async () => {
    if (!source) {
      return
    }
    if (!logSelect.value) {
      return
    }
    log = await source.loadLog(logSelect.value)
    snap = null
    timeline.setLog(log)
  })

  document.getElementById('btn-open')!.addEventListener('click', async () => {
    const dir = await pickLogDir()
    if (!dir) {
      return
    }
    source = new JsonFileSource(dir)
    const names = await source.listLogs()
    logSelect.innerHTML = ''
    for (const n of names) {
      const opt = document.createElement('option')
      opt.value = n
      opt.textContent = n
      logSelect.appendChild(opt)
    }
    if (names.length > 0) {
      logSelect.value = names[0]
      log = await source.loadLog(names[0])
      snap = null
      timeline.setLog(log)
    }
  })

  canvas.addEventListener('click', (e) => {
    if (!snap) {
      return
    }
    const rect = canvas.getBoundingClientRect()
    const ndcX = ((e.clientX - rect.left) / rect.width) * 2 - 1
    const ndcY = 1 - ((e.clientY - rect.top) / rect.height) * 2
    const tol = 12 / Math.min(rect.width, rect.height)
    const hit = pickElement(snap, cam.vp, ndcX, ndcY, tol)
    selection = hit
    refreshSelection()
  })

  const render = (): void => {
    resizeCanvas(canvas)
    const w = canvas.width
    const h = canvas.height
    gl.viewport(0, 0, w, h)
    gl.clearColor(0.11, 0.12, 0.14, 1)
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT)
    cam.update(w / h)
    if (snap) {
      meshDraw.draw(cam.vp, opts, selection)
      if (opts.overlay) {
        overlay.draw(cam.vp)
      }
    }
    requestAnimationFrame(render)
  }
  render()
}

main().catch((err) => {
  console.error(err)
  document.body.innerText = 'error: ' + (err as Error).stack
})
