import {linkProgram} from '../gl/context'
import type {MeshSnapshot, ElemKind} from '../data/MeshSource'
import type {Mat4} from '../gl/mat4'

const VS = `//glsl
#version 300 es
in vec3 a_pos;
in vec3 a_color;
uniform mat4 u_vp;
out vec3 v_color;
void main() {
  v_color = a_color;
  gl_Position = u_vp * vec4(a_pos, 1.0);
  gl_PointSize = 8.0;
}`

const FS = `//glsl
#version 300 es
precision highp float;
in vec3 v_color;
out vec4 fragColor;
void main() {
  fragColor = vec4(v_color, 1.0);
}`

const ELEM_NONE = -1

const faceCenter = (snap: MeshSnapshot, fi: number, out: Float32Array): void => {
  const li = snap.f.l[fi]
  const c0 = snap.l.c[li]
  let cc = c0
  let n = 0
  out[0] = out[1] = out[2] = 0
  do {
    const v = snap.c.v[cc]
    out[0] += snap.v.co[v * 3]
    out[1] += snap.v.co[v * 3 + 1]
    out[2] += snap.v.co[v * 3 + 2]
    n++
    cc = snap.c.next[cc]
  } while (cc !== c0 && n < 1024)
  if (n > 0) {
    out[0] /= n
    out[1] /= n
    out[2] /= n
  }
}

export class TopoOverlay {
  private gl: WebGL2RenderingContext
  private prog: WebGLProgram
  private vao: WebGLVertexArrayObject
  private lineCount = 0
  private pointCount = 0
  private linePosBuf: WebGLBuffer
  private linePosBufColor: WebGLBuffer

  constructor(gl: WebGL2RenderingContext) {
    this.gl = gl
    this.prog = linkProgram(gl, VS, FS)
    this.vao = gl.createVertexArray()!
    this.linePosBuf = gl.createBuffer()!
    this.linePosBufColor = gl.createBuffer()!
  }

  build(snap: MeshSnapshot, sel: {kind: ElemKind; id: number} | null): void {
    const linePos: number[] = []
    const lineCol: number[] = []
    const pointPos: number[] = []
    const pointCol: number[] = []
    if (sel) {
      if (sel.kind === 'v') {
        this.diskFan(snap, sel.id, linePos, lineCol, pointPos, pointCol)
      } else if (sel.kind === 'e') {
        this.radialFan(snap, sel.id, linePos, lineCol, pointPos, pointCol)
      } else if (sel.kind === 'f') {
        this.faceCorners(snap, sel.id, linePos, lineCol, pointPos, pointCol)
      } else {
        this.cornerHL(snap, sel.id, linePos, lineCol, pointPos, pointCol)
      }
    }
    const gl = this.gl
    gl.bindVertexArray(this.vao)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.linePosBuf)
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([...linePos, ...pointPos]), gl.DYNAMIC_DRAW)
    gl.enableVertexAttribArray(0)
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.linePosBufColor)
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([...lineCol, ...pointCol]), gl.DYNAMIC_DRAW)
    gl.enableVertexAttribArray(1)
    gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 0, 0)
    gl.bindVertexArray(null)
    this.lineCount = linePos.length / 3
    this.pointCount = pointPos.length / 3
  }

  private pushLine(
    a: Float32Array | number[],
    b: Float32Array | number[],
    color: [number, number, number],
    pos: number[],
    col: number[]
  ): void {
    pos.push(a[0], a[1], a[2], b[0], b[1], b[2])
    col.push(...color, ...color)
  }

  private vCo(snap: MeshSnapshot, vi: number): Float32Array {
    return new Float32Array([snap.v.co[vi * 3], snap.v.co[vi * 3 + 1], snap.v.co[vi * 3 + 2]])
  }

  private diskFan(
    snap: MeshSnapshot,
    vi: number,
    linePos: number[],
    lineCol: number[],
    pointPos: number[],
    pointCol: number[]
  ): void {
    const e0 = snap.v.e[vi]
    if (e0 === ELEM_NONE) {
      return
    }
    const a = this.vCo(snap, vi)
    pointPos.push(a[0], a[1], a[2])
    pointCol.push(1, 0.4, 0.2)
    let ec = e0
    let safety = 0
    do {
      const v0 = snap.e.vs[ec * 2]
      const v1 = snap.e.vs[ec * 2 + 1]
      const side = v0 === vi ? 0 : 1
      const other = side === 0 ? v1 : v0
      const b = this.vCo(snap, other)
      this.pushLine(a, b, [1.0, 0.5, 0.1], linePos, lineCol)
      ec = snap.e.disk[ec * 4 + side * 2 + 1]
      if (++safety > 4096) {
        break
      }
    } while (ec !== e0)
  }

  private radialFan(
    snap: MeshSnapshot,
    ei: number,
    linePos: number[],
    lineCol: number[],
    pointPos: number[],
    pointCol: number[]
  ): void {
    const v0 = snap.e.vs[ei * 2]
    const v1 = snap.e.vs[ei * 2 + 1]
    const a = this.vCo(snap, v0)
    const b = this.vCo(snap, v1)
    this.pushLine(a, b, [1.0, 0.85, 0.1], linePos, lineCol)
    const mid = new Float32Array([(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5])
    const c0 = snap.e.c[ei]
    if (c0 === ELEM_NONE) {
      return
    }
    let cc = c0
    let safety = 0
    do {
      const li = snap.c.l[cc]
      const fi = snap.l.f[li]
      const fc = new Float32Array(3)
      faceCenter(snap, fi, fc)
      this.pushLine(mid, fc, [0.2, 0.8, 1.0], linePos, lineCol)
      pointPos.push(fc[0], fc[1], fc[2])
      pointCol.push(0.2, 0.8, 1.0)
      cc = snap.c.radial_next[cc]
      if (++safety > 4096) {
        break
      }
    } while (cc !== c0)
  }

  private faceCorners(
    snap: MeshSnapshot,
    fi: number,
    linePos: number[],
    lineCol: number[],
    pointPos: number[],
    pointCol: number[]
  ): void {
    const li = snap.f.l[fi]
    const c0 = snap.l.c[li]
    let cc = c0
    let safety = 0
    do {
      const v0 = snap.c.v[cc]
      const v1 = snap.c.v[snap.c.next[cc]]
      const a = this.vCo(snap, v0)
      const b = this.vCo(snap, v1)
      const a2 = new Float32Array([a[0] * 0.85 + b[0] * 0.15, a[1] * 0.85 + b[1] * 0.15, a[2] * 0.85 + b[2] * 0.15])
      const b2 = new Float32Array([a[0] * 0.15 + b[0] * 0.85, a[1] * 0.15 + b[1] * 0.85, a[2] * 0.15 + b[2] * 0.85])
      this.pushLine(a2, b2, [0.2, 1.0, 0.4], linePos, lineCol)
      pointPos.push(b2[0], b2[1], b2[2])
      pointCol.push(0.2, 1.0, 0.4)
      cc = snap.c.next[cc]
      if (++safety > 4096) {
        break
      }
    } while (cc !== c0)
  }

  private cornerHL(
    snap: MeshSnapshot,
    ci: number,
    linePos: number[],
    lineCol: number[],
    pointPos: number[],
    pointCol: number[]
  ): void {
    const v = snap.c.v[ci]
    const a = this.vCo(snap, v)
    pointPos.push(a[0], a[1], a[2])
    pointCol.push(1, 1, 1)
    void linePos
    void lineCol
  }

  draw(vp: Mat4): void {
    const gl = this.gl
    if (this.lineCount === 0 && this.pointCount === 0) {
      return
    }
    gl.useProgram(this.prog)
    gl.bindVertexArray(this.vao)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.prog, 'u_vp'), false, vp)
    gl.disable(gl.DEPTH_TEST)
    if (this.lineCount > 0) {
      gl.drawArrays(gl.LINES, 0, this.lineCount)
    }
    if (this.pointCount > 0) {
      gl.drawArrays(gl.POINTS, this.lineCount, this.pointCount)
    }
    gl.enable(gl.DEPTH_TEST)
    gl.bindVertexArray(null)
  }
}
