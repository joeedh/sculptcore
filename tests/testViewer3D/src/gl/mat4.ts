export type Mat4 = Float32Array
export type Vec3 = Float32Array | [number, number, number]

export const mat4Create = (): Mat4 => {
  const m = new Float32Array(16)
  m[0] = m[5] = m[10] = m[15] = 1
  return m
}

export const mat4Identity = (out: Mat4): void => {
  out.fill(0)
  out[0] = out[5] = out[10] = out[15] = 1
}

export const mat4Perspective = (out: Mat4, fovy: number, aspect: number, near: number, far: number): void => {
  const f = 1 / Math.tan(fovy / 2)
  out.fill(0)
  out[0] = f / aspect
  out[5] = f
  out[10] = (far + near) / (near - far)
  out[11] = -1
  out[14] = (2 * far * near) / (near - far)
}

export const vec3Sub = (a: Vec3, b: Vec3, out: Float32Array): void => {
  out[0] = a[0] - b[0]
  out[1] = a[1] - b[1]
  out[2] = a[2] - b[2]
}

export const vec3Cross = (a: Float32Array, b: Float32Array, out: Float32Array): void => {
  const ax = a[0]
  const ay = a[1]
  const az = a[2]
  const bx = b[0]
  const by = b[1]
  const bz = b[2]
  out[0] = ay * bz - az * by
  out[1] = az * bx - ax * bz
  out[2] = ax * by - ay * bx
}

export const vec3Norm = (v: Float32Array): void => {
  const l = Math.hypot(v[0], v[1], v[2])
  if (l > 0) {
    v[0] /= l
    v[1] /= l
    v[2] /= l
  }
}

export const mat4LookAt = (out: Mat4, eye: Vec3, center: Vec3, up: Vec3): void => {
  const f = new Float32Array(3)
  const s = new Float32Array(3)
  const u = new Float32Array(3)
  vec3Sub(center, eye, f)
  vec3Norm(f)
  const upf = new Float32Array([up[0], up[1], up[2]])
  vec3Cross(f, upf, s)
  vec3Norm(s)
  vec3Cross(s, f, u)
  out[0] = s[0]
  out[4] = s[1]
  out[8] = s[2]
  out[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2])
  out[1] = u[0]
  out[5] = u[1]
  out[9] = u[2]
  out[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2])
  out[2] = -f[0]
  out[6] = -f[1]
  out[10] = -f[2]
  out[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]
  out[3] = 0
  out[7] = 0
  out[11] = 0
  out[15] = 1
}

export const mat4Multiply = (out: Mat4, a: Mat4, b: Mat4): void => {
  const tmp = new Float32Array(16)
  for (let i = 0; i < 4; i++) {
    for (let j = 0; j < 4; j++) {
      let v = 0
      for (let k = 0; k < 4; k++) {
        v += a[k * 4 + j] * b[i * 4 + k]
      }
      tmp[i * 4 + j] = v
    }
  }
  out.set(tmp)
}
