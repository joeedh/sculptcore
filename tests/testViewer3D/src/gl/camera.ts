import {mat4Create, mat4LookAt, mat4Multiply, mat4Perspective, type Mat4} from './mat4'

export class OrbitCamera {
  yaw = 0.6
  pitch = 0.4
  dist = 4
  target: Float32Array = new Float32Array([0, 0, 0])
  fov = (45 * Math.PI) / 180
  near = 0.01
  far = 100

  proj: Mat4 = mat4Create()
  view: Mat4 = mat4Create()
  vp: Mat4 = mat4Create()

  update(aspect: number): void {
    const cy = Math.cos(this.yaw)
    const sy = Math.sin(this.yaw)
    const cp = Math.cos(this.pitch)
    const sp = Math.sin(this.pitch)
    const eye = new Float32Array([
      this.target[0] + this.dist * cp * sy,
      this.target[1] + this.dist * sp,
      this.target[2] + this.dist * cp * cy,
    ])
    mat4Perspective(this.proj, this.fov, aspect, this.near, this.far)
    mat4LookAt(this.view, eye, this.target, [0, 1, 0])
    mat4Multiply(this.vp, this.proj, this.view)
  }

  attach(canvas: HTMLCanvasElement): void {
    let dragging = false
    let panning = false
    let lx = 0
    let ly = 0
    canvas.addEventListener('mousedown', (e) => {
      dragging = true
      panning = e.shiftKey || e.button === 1
      lx = e.clientX
      ly = e.clientY
      e.preventDefault()
    })
    window.addEventListener('mouseup', () => {
      dragging = false
    })
    window.addEventListener('mousemove', (e) => {
      if (!dragging) {
        return
      }
      const dx = e.clientX - lx
      const dy = e.clientY - ly
      lx = e.clientX
      ly = e.clientY
      if (panning) {
        const speed = 0.0025 * this.dist
        const cy = Math.cos(this.yaw)
        const sy = Math.sin(this.yaw)
        this.target[0] -= cy * dx * speed
        this.target[2] += sy * dx * speed
        this.target[1] += dy * speed
      } else {
        this.yaw -= dx * 0.005
        this.pitch += dy * 0.005
        const lim = Math.PI / 2 - 0.01
        if (this.pitch > lim) {
          this.pitch = lim
        }
        if (this.pitch < -lim) {
          this.pitch = -lim
        }
      }
    })
    canvas.addEventListener(
      'wheel',
      (e) => {
        const f = Math.exp(e.deltaY * 0.001)
        this.dist *= f
        if (this.dist < 0.05) {
          this.dist = 0.05
        }
        e.preventDefault()
      },
      {passive: false}
    )
  }
}
