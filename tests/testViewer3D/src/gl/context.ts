export const compileShader = (gl: WebGL2RenderingContext, type: number, src: string): WebGLShader => {
  const sh = gl.createShader(type)
  if (!sh) {
    throw new Error('createShader failed')
  }
  gl.shaderSource(sh, src)
  gl.compileShader(sh)
  if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(sh) ?? ''
    gl.deleteShader(sh)
    throw new Error('shader compile error: ' + log + '\n' + src)
  }
  return sh
}

export const linkProgram = (gl: WebGL2RenderingContext, vsSrc: string, fsSrc: string): WebGLProgram => {
  const vs = compileShader(gl, gl.VERTEX_SHADER, vsSrc)
  const fs = compileShader(gl, gl.FRAGMENT_SHADER, fsSrc)
  const p = gl.createProgram()
  if (!p) {
    throw new Error('createProgram failed')
  }
  gl.attachShader(p, vs)
  gl.attachShader(p, fs)
  gl.linkProgram(p)
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(p) ?? ''
    throw new Error('program link error: ' + log)
  }
  return p
}

export const initGL = (canvas: HTMLCanvasElement): WebGL2RenderingContext => {
  const gl = canvas.getContext('webgl2', {antialias: true, preserveDrawingBuffer: false})
  if (!gl) {
    throw new Error('webgl2 not available')
  }
  return gl
}

export const resizeCanvas = (canvas: HTMLCanvasElement): boolean => {
  const w = Math.max(1, Math.floor(canvas.clientWidth * window.devicePixelRatio))
  const h = Math.max(1, Math.floor(canvas.clientHeight * window.devicePixelRatio))
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w
    canvas.height = h
    return true
  }
  return false
}
