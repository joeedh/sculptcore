import type {IWasmInterface} from './wasm'
import type {Buffer, DrawBatch, DrawCommand} from '../index'
import {GPUType} from '../sculptcore/gpu/GPUType'
import {GPUBufferType} from '../sculptcore/gpu/GPUBufferType'
import {GPUCmdType} from '../sculptcore/gpu/GPUCmdType'

interface BoundLike {
  ptr: number
}

interface CachedBuffer {
  glBuf: WebGLBuffer
  uploadedSize: number
  uploadedDataPtr: number
}

const VS_LINES = `#version 300 es
precision highp float;
layout(location = 0) in vec3 a_position;
uniform mat4 u_mvp;
void main() {
  gl_Position = u_mvp * vec4(a_position, 1.0);
}
`

const FS_LINES = `#version 300 es
precision highp float;
uniform vec4 u_color;
out vec4 fragColor;
void main() {
  fragColor = u_color;
}
`

function gpuTypeBytes(t: GPUType): number {
  switch (t) {
    case GPUType.FLOAT16:
    case GPUType.INT16:
    case GPUType.UINT16:
      return 2
    case GPUType.FLOAT32:
    case GPUType.INT32:
    case GPUType.UINT32:
      return 4
    case GPUType.FLOAT64:
      return 8
    case GPUType.INT8:
    case GPUType.UINT8:
      return 1
    default:
      return 4
  }
}

function gpuTypeGL(gl: WebGL2RenderingContext, t: GPUType): GLenum {
  switch (t) {
    case GPUType.FLOAT32:
      return gl.FLOAT
    case GPUType.FLOAT16:
      return gl.HALF_FLOAT
    case GPUType.INT32:
      return gl.INT
    case GPUType.INT16:
      return gl.SHORT
    case GPUType.INT8:
      return gl.BYTE
    case GPUType.UINT32:
      return gl.UNSIGNED_INT
    case GPUType.UINT16:
      return gl.UNSIGNED_SHORT
    case GPUType.UINT8:
      return gl.UNSIGNED_BYTE
    default:
      return gl.FLOAT
  }
}

function bufferTargetGL(gl: WebGL2RenderingContext, t: GPUBufferType): GLenum {
  return t === GPUBufferType.BUFFER_INDEX ? gl.ELEMENT_ARRAY_BUFFER : gl.ARRAY_BUFFER
}

function cmdTypeGL(gl: WebGL2RenderingContext, t: GPUCmdType): GLenum {
  switch (t) {
    case GPUCmdType.DRAW_TRIS:
      return gl.TRIANGLES
    case GPUCmdType.DRAW_TRI_STRIP:
      return gl.TRIANGLE_STRIP
    case GPUCmdType.DRAW_LINES:
      return gl.LINES
    case GPUCmdType.DRAW_POINTS:
      return gl.POINTS
    default:
      return gl.TRIANGLES
  }
}

function compile(gl: WebGL2RenderingContext, src: string, type: GLenum): WebGLShader {
  const sh = gl.createShader(type)!
  gl.shaderSource(sh, src)
  gl.compileShader(sh)
  if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(sh)
    throw new Error('shader compile failed: ' + log)
  }
  return sh
}

function link(gl: WebGL2RenderingContext, vs: WebGLShader, fs: WebGLShader): WebGLProgram {
  const p = gl.createProgram()!
  gl.attachShader(p, vs)
  gl.attachShader(p, fs)
  gl.linkProgram(p)
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(p)
    throw new Error('program link failed: ' + log)
  }
  return p
}

export class WebGLBatchExecutor {
  gl: WebGL2RenderingContext
  wasm: IWasmInterface
  private bufferCache = new Map<number, CachedBuffer>()
  private vao: WebGLVertexArrayObject
  private linesProgram: WebGLProgram
  private uMVP: WebGLUniformLocation
  private uColor: WebGLUniformLocation

  constructor(gl: WebGL2RenderingContext, wasm: IWasmInterface) {
    this.gl = gl
    this.wasm = wasm
    const vs = compile(gl, VS_LINES, gl.VERTEX_SHADER)
    const fs = compile(gl, FS_LINES, gl.FRAGMENT_SHADER)
    this.linesProgram = link(gl, vs, fs)
    this.uMVP = gl.getUniformLocation(this.linesProgram, 'u_mvp')!
    this.uColor = gl.getUniformLocation(this.linesProgram, 'u_color')!
    this.vao = gl.createVertexArray()!
  }

  private uploadBuffer(buf: Buffer): WebGLBuffer {
    const gl = this.gl
    const ptr = (buf as unknown as BoundLike).ptr
    const dataPtr = buf.data
    const size = buf.size
    const elemsize = buf.elemsize
    const bytes = size * elemsize * gpuTypeBytes(buf.type)

    let cached = this.bufferCache.get(ptr)
    if (cached === undefined) {
      cached = {glBuf: gl.createBuffer()!, uploadedSize: -1, uploadedDataPtr: -1}
      this.bufferCache.set(ptr, cached)
    }

    if (cached.uploadedSize !== bytes || cached.uploadedDataPtr !== dataPtr || buf.update_buffer) {
      const view = new Uint8Array(this.wasm.HEAPU8.buffer, dataPtr, bytes)
      const target = bufferTargetGL(gl, buf.target)
      gl.bindBuffer(target, cached.glBuf)
      gl.bufferData(target, view, gl.STATIC_DRAW)
      cached.uploadedSize = bytes
      cached.uploadedDataPtr = dataPtr
      buf.update_buffer = false
    }

    return cached.glBuf
  }

  releaseBuffer(buf: Buffer) {
    const ptr = (buf as unknown as BoundLike).ptr
    const cached = this.bufferCache.get(ptr)
    if (cached) {
      this.gl.deleteBuffer(cached.glBuf)
      this.bufferCache.delete(ptr)
    }
  }

  dispatch(batch: DrawBatch, mvp: Float32Array, color: Float32Array = new Float32Array([1, 1, 1, 1])) {
    const gl = this.gl
    const commands = batch.commands
    if (commands.length === 0) {
      return
    }

    gl.useProgram(this.linesProgram)
    gl.uniformMatrix4fv(this.uMVP, false, mvp)
    gl.uniform4fv(this.uColor, color)
    gl.bindVertexArray(this.vao)

    for (let i = 0; i < commands.length; i++) {
      const cmd = commands[i] as unknown as DrawCommand
      const attrs = cmd.attrs
      for (let a = 0; a < attrs.length; a++) {
        const attr = attrs[a] as unknown as Buffer | undefined
        if (attr === undefined) continue
        const glBuf = this.uploadBuffer(attr)
        gl.bindBuffer(gl.ARRAY_BUFFER, glBuf)
        gl.enableVertexAttribArray(a)
        const components = attr.elemsize
        gl.vertexAttribPointer(a, components, gpuTypeGL(gl, attr.type), false, 0, 0)
      }

      const count = cmd.end - cmd.start
      gl.drawArrays(cmdTypeGL(gl, cmd.type), cmd.start, count)
    }

    gl.bindVertexArray(null)
  }

  dispose() {
    const gl = this.gl
    for (const cached of this.bufferCache.values()) {
      gl.deleteBuffer(cached.glBuf)
    }
    this.bufferCache.clear()
    gl.deleteProgram(this.linesProgram)
    gl.deleteVertexArray(this.vao)
  }
}
