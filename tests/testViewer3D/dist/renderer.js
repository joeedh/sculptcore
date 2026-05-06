"use strict";

// src/data/JsonFileSource.ts
var import_electron = require("electron");

// src/data/decode.ts
var decodeBase64 = (s) => {
  const bin = Buffer.from(s, "base64");
  const out = new Uint8Array(bin.length);
  out.set(bin);
  return out;
};
var decodeF32 = (s) => {
  const u8 = decodeBase64(s);
  return new Float32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4);
};
var decodeI32 = (s) => {
  const u8 = decodeBase64(s);
  return new Int32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4);
};
var decodeI16 = (s) => {
  const u8 = decodeBase64(s);
  return new Int16Array(u8.buffer, u8.byteOffset, u8.byteLength / 2);
};
var decodeU8 = (s) => decodeBase64(s);

// src/data/JsonFileSource.ts
var decodeSnap = (s) => ({
  caps: s.caps,
  counts: s.counts,
  v: {
    co: decodeF32(s.v.co),
    no: decodeF32(s.v.no),
    e: decodeI32(s.v.e),
    alive: decodeU8(s.v.alive)
  },
  e: {
    vs: decodeI32(s.e.vs),
    c: decodeI32(s.e.c),
    disk: decodeI32(s.e.disk),
    alive: decodeU8(s.e.alive)
  },
  c: {
    v: decodeI32(s.c.v),
    e: decodeI32(s.c.e),
    l: decodeI32(s.c.l),
    next: decodeI32(s.c.next),
    prev: decodeI32(s.c.prev),
    radial_next: decodeI32(s.c.radial_next),
    radial_prev: decodeI32(s.c.radial_prev),
    alive: decodeU8(s.c.alive)
  },
  l: {
    c: decodeI32(s.l.c),
    f: decodeI32(s.l.f),
    next: decodeI32(s.l.next),
    size: decodeI32(s.l.size),
    alive: decodeU8(s.l.alive)
  },
  f: {
    l: decodeI32(s.f.l),
    no: decodeF32(s.f.no),
    list_count: decodeI16(s.f.list_count),
    alive: decodeU8(s.f.alive)
  }
});
var JsonFileSource = class {
  constructor(dir) {
    this.dir = dir;
  }
  async listLogs() {
    return await import_electron.ipcRenderer.invoke("list-logs", this.dir);
  }
  async loadLog(name) {
    const text = await import_electron.ipcRenderer.invoke("read-log", this.dir, name);
    const raw = JSON.parse(text);
    return {
      version: raw.version,
      tag: raw.tag,
      initial: decodeSnap(raw.initial),
      steps: raw.steps.map((s) => ({
        op: s.op,
        highlight: s.highlight,
        note: s.note,
        snapshot: decodeSnap(s.snapshot)
      }))
    };
  }
};
var pickLogDir = async () => {
  return await import_electron.ipcRenderer.invoke("pick-log-dir");
};

// src/gl/mat4.ts
var mat4Create = () => {
  const m = new Float32Array(16);
  m[0] = m[5] = m[10] = m[15] = 1;
  return m;
};
var mat4Perspective = (out, fovy, aspect, near, far) => {
  const f = 1 / Math.tan(fovy / 2);
  out.fill(0);
  out[0] = f / aspect;
  out[5] = f;
  out[10] = (far + near) / (near - far);
  out[11] = -1;
  out[14] = 2 * far * near / (near - far);
};
var vec3Sub = (a, b, out) => {
  out[0] = a[0] - b[0];
  out[1] = a[1] - b[1];
  out[2] = a[2] - b[2];
};
var vec3Cross = (a, b, out) => {
  const ax = a[0];
  const ay = a[1];
  const az = a[2];
  const bx = b[0];
  const by = b[1];
  const bz = b[2];
  out[0] = ay * bz - az * by;
  out[1] = az * bx - ax * bz;
  out[2] = ax * by - ay * bx;
};
var vec3Norm = (v) => {
  const l = Math.hypot(v[0], v[1], v[2]);
  if (l > 0) {
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
  }
};
var mat4LookAt = (out, eye, center, up) => {
  const f = new Float32Array(3);
  const s = new Float32Array(3);
  const u = new Float32Array(3);
  vec3Sub(center, eye, f);
  vec3Norm(f);
  const upf = new Float32Array([up[0], up[1], up[2]]);
  vec3Cross(f, upf, s);
  vec3Norm(s);
  vec3Cross(s, f, u);
  out[0] = s[0];
  out[4] = s[1];
  out[8] = s[2];
  out[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
  out[1] = u[0];
  out[5] = u[1];
  out[9] = u[2];
  out[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
  out[2] = -f[0];
  out[6] = -f[1];
  out[10] = -f[2];
  out[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
  out[3] = 0;
  out[7] = 0;
  out[11] = 0;
  out[15] = 1;
};
var mat4Multiply = (out, a, b) => {
  const tmp = new Float32Array(16);
  for (let i = 0; i < 4; i++) {
    for (let j = 0; j < 4; j++) {
      let v = 0;
      for (let k = 0; k < 4; k++) {
        v += a[k * 4 + j] * b[i * 4 + k];
      }
      tmp[i * 4 + j] = v;
    }
  }
  out.set(tmp);
};

// src/gl/camera.ts
var OrbitCamera = class {
  yaw = 0.6;
  pitch = 0.4;
  dist = 4;
  target = new Float32Array([0, 0, 0]);
  fov = 45 * Math.PI / 180;
  near = 0.01;
  far = 100;
  proj = mat4Create();
  view = mat4Create();
  vp = mat4Create();
  update(aspect) {
    const cy = Math.cos(this.yaw);
    const sy = Math.sin(this.yaw);
    const cp = Math.cos(this.pitch);
    const sp = Math.sin(this.pitch);
    const eye = new Float32Array([
      this.target[0] + this.dist * cp * sy,
      this.target[1] + this.dist * sp,
      this.target[2] + this.dist * cp * cy
    ]);
    mat4Perspective(this.proj, this.fov, aspect, this.near, this.far);
    mat4LookAt(this.view, eye, this.target, [0, 1, 0]);
    mat4Multiply(this.vp, this.proj, this.view);
  }
  attach(canvas) {
    let dragging = false;
    let panning = false;
    let lx = 0;
    let ly = 0;
    canvas.addEventListener("mousedown", (e) => {
      dragging = true;
      panning = e.shiftKey || e.button === 1;
      lx = e.clientX;
      ly = e.clientY;
      e.preventDefault();
    });
    window.addEventListener("mouseup", () => {
      dragging = false;
    });
    window.addEventListener("mousemove", (e) => {
      if (!dragging) {
        return;
      }
      const dx = e.clientX - lx;
      const dy = e.clientY - ly;
      lx = e.clientX;
      ly = e.clientY;
      if (panning) {
        const speed = 25e-4 * this.dist;
        const cy = Math.cos(this.yaw);
        const sy = Math.sin(this.yaw);
        this.target[0] -= cy * dx * speed;
        this.target[2] += sy * dx * speed;
        this.target[1] += dy * speed;
      } else {
        this.yaw -= dx * 5e-3;
        this.pitch += dy * 5e-3;
        const lim = Math.PI / 2 - 0.01;
        if (this.pitch > lim) {
          this.pitch = lim;
        }
        if (this.pitch < -lim) {
          this.pitch = -lim;
        }
      }
    });
    canvas.addEventListener(
      "wheel",
      (e) => {
        const f = Math.exp(e.deltaY * 1e-3);
        this.dist *= f;
        if (this.dist < 0.05) {
          this.dist = 0.05;
        }
        e.preventDefault();
      },
      { passive: false }
    );
  }
};

// src/gl/context.ts
var compileShader = (gl, type, src) => {
  const sh = gl.createShader(type);
  if (!sh) {
    throw new Error("createShader failed");
  }
  gl.shaderSource(sh, src);
  gl.compileShader(sh);
  if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(sh) ?? "";
    gl.deleteShader(sh);
    throw new Error("shader compile error: " + log + "\n" + src);
  }
  return sh;
};
var linkProgram = (gl, vsSrc, fsSrc) => {
  const vs = compileShader(gl, gl.VERTEX_SHADER, vsSrc);
  const fs = compileShader(gl, gl.FRAGMENT_SHADER, fsSrc);
  const p = gl.createProgram();
  if (!p) {
    throw new Error("createProgram failed");
  }
  gl.attachShader(p, vs);
  gl.attachShader(p, fs);
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(p) ?? "";
    throw new Error("program link error: " + log);
  }
  return p;
};
var initGL = (canvas) => {
  const gl = canvas.getContext("webgl2", { antialias: true, preserveDrawingBuffer: false });
  if (!gl) {
    throw new Error("webgl2 not available");
  }
  return gl;
};
var resizeCanvas = (canvas) => {
  const w = Math.max(1, Math.floor(canvas.clientWidth * window.devicePixelRatio));
  const h = Math.max(1, Math.floor(canvas.clientHeight * window.devicePixelRatio));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
    return true;
  }
  return false;
};

// src/draw/MeshDraw.ts
var VS_SHADED = `//glsl
#version 300 es
in vec3 a_pos;
in vec3 a_no;
in float a_id;
uniform mat4 u_vp;
flat out float v_id;
out vec3 v_no;
void main() {
  v_no = a_no;
  v_id = a_id;
  gl_Position = u_vp * vec4(a_pos, 1.0);
}`;
var FS_SHADED = `//glsl
#version 300 es
precision highp float;
in vec3 v_no;
flat in float v_id;
uniform vec3 u_color;
uniform float u_highlight_id;
out vec4 fragColor;
void main() {
  vec3 N = normalize(v_no);
  float l = clamp(dot(N, normalize(vec3(0.4, 0.8, 0.6))), 0.0, 1.0) * 0.6 + 0.4;
  vec3 c = u_color * l;
  if (abs(v_id - u_highlight_id) < 0.5) {
    c = mix(c, vec3(1.0, 0.8, 0.2), 0.7);
  }
  fragColor = vec4(c, 1.0);
}`;
var VS_LINE = `//glsl
#version 300 es
in vec3 a_pos;
in float a_id;
uniform mat4 u_vp;
flat out float v_id;
void main() {
  v_id = a_id;
  gl_Position = u_vp * vec4(a_pos, 1.0);
}`;
var FS_LINE = `//glsl
#version 300 es
precision highp float;
flat in float v_id;
uniform vec3 u_color;
uniform float u_highlight_id;
out vec4 fragColor;
void main() {
  vec3 c = u_color;
  if (abs(v_id - u_highlight_id) < 0.5) {
    c = vec3(1.0, 0.8, 0.2);
  }
  fragColor = vec4(c, 1.0);
}`;
var VS_POINT = `//glsl
#version 300 es
in vec3 a_pos;
in float a_id;
uniform mat4 u_vp;
uniform float u_size;
flat out float v_id;
void main() {
  v_id = a_id;
  gl_Position = u_vp * vec4(a_pos, 1.0);
  gl_PointSize = u_size;
}`;
var FS_POINT = `//glsl
#version 300 es
precision highp float;
flat in float v_id;
uniform vec3 u_color;
uniform float u_highlight_id;
out vec4 fragColor;
void main() {
  vec2 d = gl_PointCoord - 0.5;
  if (dot(d, d) > 0.25) {
    discard;
  }
  vec3 c = u_color;
  if (abs(v_id - u_highlight_id) < 0.5) {
    c = vec3(1.0, 0.8, 0.2);
  }
  fragColor = vec4(c, 1.0);
}`;
var MeshDraw = class {
  gl;
  progFace;
  progLine;
  progPoint;
  bufs = null;
  highlight = null;
  constructor(gl) {
    this.gl = gl;
    this.progFace = linkProgram(gl, VS_SHADED, FS_SHADED);
    this.progLine = linkProgram(gl, VS_LINE, FS_LINE);
    this.progPoint = linkProgram(gl, VS_POINT, FS_POINT);
  }
  upload(snap) {
    const gl = this.gl;
    const vCap = snap.caps.v;
    const positions = snap.v.co;
    const normals = snap.v.no;
    const ids = new Float32Array(vCap);
    for (let i = 0; i < vCap; i++) {
      ids[i] = i;
    }
    const triIdx = [];
    const fAlive = snap.f.alive;
    const fl = snap.f.l;
    const lc = snap.l.c;
    const lnext = snap.l.next;
    const cv = snap.c.v;
    const cnext = snap.c.next;
    for (let fi = 0; fi < snap.caps.f; fi++) {
      if (!fAlive[fi]) {
        continue;
      }
      let li = fl[fi];
      while (li !== -1) {
        const c0 = lc[li];
        const v0 = cv[c0];
        let cprev = cnext[c0];
        let vprev = cv[cprev];
        let cc = cnext[cprev];
        while (cc !== c0) {
          const vcur = cv[cc];
          triIdx.push(v0, vprev, vcur);
          vprev = vcur;
          cprev = cc;
          cc = cnext[cc];
        }
        li = lnext[li];
        if (li === -1 || li === void 0) {
          break;
        }
      }
    }
    const edgeIdx = [];
    const eAlive = snap.e.alive;
    const evs = snap.e.vs;
    for (let ei = 0; ei < snap.caps.e; ei++) {
      if (!eAlive[ei]) {
        continue;
      }
      edgeIdx.push(evs[ei * 2], evs[ei * 2 + 1]);
    }
    const pointIdx = [];
    const vAlive = snap.v.alive;
    for (let vi = 0; vi < vCap; vi++) {
      if (vAlive[vi]) {
        pointIdx.push(vi);
      }
    }
    if (this.bufs) {
      this.disposeBufs(this.bufs);
    }
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    const posBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuf);
    gl.bufferData(gl.ARRAY_BUFFER, positions, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    const normBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, normBuf);
    gl.bufferData(gl.ARRAY_BUFFER, normals, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 0, 0);
    const idBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, idBuf);
    gl.bufferData(gl.ARRAY_BUFFER, ids, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(2);
    gl.vertexAttribPointer(2, 1, gl.FLOAT, false, 0, 0);
    const triIdxBuf = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, triIdxBuf);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(triIdx), gl.STATIC_DRAW);
    const edgeIdxBuf = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, edgeIdxBuf);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(edgeIdx), gl.STATIC_DRAW);
    const pointIdxBuf = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, pointIdxBuf);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(pointIdx), gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    this.bufs = {
      vao,
      posBuf,
      normBuf,
      idBuf,
      triIdx: triIdxBuf,
      edgeIdx: edgeIdxBuf,
      pointIdx: pointIdxBuf,
      triCount: triIdx.length,
      edgeCount: edgeIdx.length,
      pointCount: pointIdx.length,
      posSize: vCap
    };
  }
  disposeBufs(b) {
    const gl = this.gl;
    gl.deleteVertexArray(b.vao);
    gl.deleteBuffer(b.posBuf);
    gl.deleteBuffer(b.normBuf);
    gl.deleteBuffer(b.idBuf);
    gl.deleteBuffer(b.triIdx);
    gl.deleteBuffer(b.edgeIdx);
    gl.deleteBuffer(b.pointIdx);
  }
  draw(vp, opts, selection) {
    const gl = this.gl;
    const b = this.bufs;
    if (!b) {
      return;
    }
    const stepHl = this.highlight;
    const vHl = selection?.kind === "v" ? selection.id : -1;
    const eHl = selection?.kind === "e" ? selection.id : stepHl?.kind === "e" ? stepHl.ids[0] ?? -1 : -1;
    const fHl = selection?.kind === "f" ? selection.id : -1;
    gl.bindVertexArray(b.vao);
    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.POLYGON_OFFSET_FILL);
    gl.polygonOffset(1, 1);
    if (opts.faces) {
      gl.useProgram(this.progFace);
      gl.uniformMatrix4fv(gl.getUniformLocation(this.progFace, "u_vp"), false, vp);
      gl.uniform3f(gl.getUniformLocation(this.progFace, "u_color"), 0.55, 0.6, 0.7);
      gl.uniform1f(gl.getUniformLocation(this.progFace, "u_highlight_id"), -1);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, b.triIdx);
      gl.drawElements(gl.TRIANGLES, b.triCount, gl.UNSIGNED_INT, 0);
    }
    gl.disable(gl.POLYGON_OFFSET_FILL);
    if (opts.edges) {
      gl.useProgram(this.progLine);
      gl.uniformMatrix4fv(gl.getUniformLocation(this.progLine, "u_vp"), false, vp);
      gl.uniform3f(gl.getUniformLocation(this.progLine, "u_color"), 0.1, 0.1, 0.12);
      gl.uniform1f(gl.getUniformLocation(this.progLine, "u_highlight_id"), -1);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, b.edgeIdx);
      gl.drawElements(gl.LINES, b.edgeCount, gl.UNSIGNED_INT, 0);
    }
    if (opts.verts) {
      gl.useProgram(this.progPoint);
      gl.uniformMatrix4fv(gl.getUniformLocation(this.progPoint, "u_vp"), false, vp);
      gl.uniform3f(gl.getUniformLocation(this.progPoint, "u_color"), 0.9, 0.9, 1);
      gl.uniform1f(gl.getUniformLocation(this.progPoint, "u_highlight_id"), vHl);
      gl.uniform1f(gl.getUniformLocation(this.progPoint, "u_size"), 6);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, b.pointIdx);
      gl.drawElements(gl.POINTS, b.pointCount, gl.UNSIGNED_INT, 0);
    }
    gl.bindVertexArray(null);
  }
};

// src/draw/TopoOverlay.ts
var VS = `//glsl
#version 300 es
in vec3 a_pos;
in vec3 a_color;
uniform mat4 u_vp;
out vec3 v_color;
void main() {
  v_color = a_color;
  gl_Position = u_vp * vec4(a_pos, 1.0);
  gl_PointSize = 8.0;
}`;
var FS = `//glsl
#version 300 es
precision highp float;
in vec3 v_color;
out vec4 fragColor;
void main() {
  fragColor = vec4(v_color, 1.0);
}`;
var ELEM_NONE = -1;
var faceCenter = (snap, fi, out) => {
  const li = snap.f.l[fi];
  const c0 = snap.l.c[li];
  let cc = c0;
  let n = 0;
  out[0] = out[1] = out[2] = 0;
  do {
    const v = snap.c.v[cc];
    out[0] += snap.v.co[v * 3];
    out[1] += snap.v.co[v * 3 + 1];
    out[2] += snap.v.co[v * 3 + 2];
    n++;
    cc = snap.c.next[cc];
  } while (cc !== c0 && n < 1024);
  if (n > 0) {
    out[0] /= n;
    out[1] /= n;
    out[2] /= n;
  }
};
var TopoOverlay = class {
  gl;
  prog;
  vao;
  lineCount = 0;
  pointCount = 0;
  linePosBuf;
  linePosBufColor;
  constructor(gl) {
    this.gl = gl;
    this.prog = linkProgram(gl, VS, FS);
    this.vao = gl.createVertexArray();
    this.linePosBuf = gl.createBuffer();
    this.linePosBufColor = gl.createBuffer();
  }
  build(snap, sel) {
    const linePos = [];
    const lineCol = [];
    const pointPos = [];
    const pointCol = [];
    if (sel) {
      if (sel.kind === "v") {
        this.diskFan(snap, sel.id, linePos, lineCol, pointPos, pointCol);
      } else if (sel.kind === "e") {
        this.radialFan(snap, sel.id, linePos, lineCol, pointPos, pointCol);
      } else if (sel.kind === "f") {
        this.faceCorners(snap, sel.id, linePos, lineCol, pointPos, pointCol);
      } else {
        this.cornerHL(snap, sel.id, linePos, lineCol, pointPos, pointCol);
      }
    }
    const gl = this.gl;
    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.linePosBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([...linePos, ...pointPos]), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.linePosBufColor);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([...lineCol, ...pointCol]), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);
    this.lineCount = linePos.length / 3;
    this.pointCount = pointPos.length / 3;
  }
  pushLine(a, b, color, pos, col) {
    pos.push(a[0], a[1], a[2], b[0], b[1], b[2]);
    col.push(...color, ...color);
  }
  vCo(snap, vi) {
    return new Float32Array([snap.v.co[vi * 3], snap.v.co[vi * 3 + 1], snap.v.co[vi * 3 + 2]]);
  }
  diskFan(snap, vi, linePos, lineCol, pointPos, pointCol) {
    const e0 = snap.v.e[vi];
    if (e0 === ELEM_NONE) {
      return;
    }
    const a = this.vCo(snap, vi);
    pointPos.push(a[0], a[1], a[2]);
    pointCol.push(1, 0.4, 0.2);
    let ec = e0;
    let safety = 0;
    do {
      const v0 = snap.e.vs[ec * 2];
      const v1 = snap.e.vs[ec * 2 + 1];
      const side = v0 === vi ? 0 : 1;
      const other = side === 0 ? v1 : v0;
      const b = this.vCo(snap, other);
      this.pushLine(a, b, [1, 0.5, 0.1], linePos, lineCol);
      ec = snap.e.disk[ec * 4 + side * 2 + 1];
      if (++safety > 4096) {
        break;
      }
    } while (ec !== e0);
  }
  radialFan(snap, ei, linePos, lineCol, pointPos, pointCol) {
    const v0 = snap.e.vs[ei * 2];
    const v1 = snap.e.vs[ei * 2 + 1];
    const a = this.vCo(snap, v0);
    const b = this.vCo(snap, v1);
    this.pushLine(a, b, [1, 0.85, 0.1], linePos, lineCol);
    const mid = new Float32Array([(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5]);
    const c0 = snap.e.c[ei];
    if (c0 === ELEM_NONE) {
      return;
    }
    let cc = c0;
    let safety = 0;
    do {
      const li = snap.c.l[cc];
      const fi = snap.l.f[li];
      const fc = new Float32Array(3);
      faceCenter(snap, fi, fc);
      this.pushLine(mid, fc, [0.2, 0.8, 1], linePos, lineCol);
      pointPos.push(fc[0], fc[1], fc[2]);
      pointCol.push(0.2, 0.8, 1);
      cc = snap.c.radial_next[cc];
      if (++safety > 4096) {
        break;
      }
    } while (cc !== c0);
  }
  faceCorners(snap, fi, linePos, lineCol, pointPos, pointCol) {
    const li = snap.f.l[fi];
    const c0 = snap.l.c[li];
    let cc = c0;
    let safety = 0;
    do {
      const v0 = snap.c.v[cc];
      const v1 = snap.c.v[snap.c.next[cc]];
      const a = this.vCo(snap, v0);
      const b = this.vCo(snap, v1);
      const a2 = new Float32Array([a[0] * 0.85 + b[0] * 0.15, a[1] * 0.85 + b[1] * 0.15, a[2] * 0.85 + b[2] * 0.15]);
      const b2 = new Float32Array([a[0] * 0.15 + b[0] * 0.85, a[1] * 0.15 + b[1] * 0.85, a[2] * 0.15 + b[2] * 0.85]);
      this.pushLine(a2, b2, [0.2, 1, 0.4], linePos, lineCol);
      pointPos.push(b2[0], b2[1], b2[2]);
      pointCol.push(0.2, 1, 0.4);
      cc = snap.c.next[cc];
      if (++safety > 4096) {
        break;
      }
    } while (cc !== c0);
  }
  cornerHL(snap, ci, linePos, lineCol, pointPos, pointCol) {
    const v = snap.c.v[ci];
    const a = this.vCo(snap, v);
    pointPos.push(a[0], a[1], a[2]);
    pointCol.push(1, 1, 1);
  }
  draw(vp) {
    const gl = this.gl;
    if (this.lineCount === 0 && this.pointCount === 0) {
      return;
    }
    gl.useProgram(this.prog);
    gl.bindVertexArray(this.vao);
    gl.uniformMatrix4fv(gl.getUniformLocation(this.prog, "u_vp"), false, vp);
    gl.disable(gl.DEPTH_TEST);
    if (this.lineCount > 0) {
      gl.drawArrays(gl.LINES, 0, this.lineCount);
    }
    if (this.pointCount > 0) {
      gl.drawArrays(gl.POINTS, this.lineCount, this.pointCount);
    }
    gl.enable(gl.DEPTH_TEST);
    gl.bindVertexArray(null);
  }
};

// src/draw/Picking.ts
var project = (vp, x, y, z) => {
  const cx = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
  const cy = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
  const cz = vp[2] * x + vp[6] * y + vp[10] * z + vp[14];
  const cw = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
  if (cw <= 0) {
    return [Number.NaN, Number.NaN, 1];
  }
  return [cx / cw, cy / cw, cz / cw];
};
var pickElement = (snap, vp, ndcX, ndcY, pixelToleranceNdc) => {
  let best = null;
  const vAlive = snap.v.alive;
  const co = snap.v.co;
  const vProj = new Array(snap.caps.v).fill(null);
  const vertTol = pixelToleranceNdc * 1.5;
  for (let vi = 0; vi < snap.caps.v; vi++) {
    if (!vAlive[vi]) {
      continue;
    }
    const p = project(vp, co[vi * 3], co[vi * 3 + 1], co[vi * 3 + 2]);
    vProj[vi] = p;
    const dx = p[0] - ndcX;
    const dy = p[1] - ndcY;
    const d = Math.hypot(dx, dy);
    if (d < vertTol && (best === null || d < best.dist - 5e-3 || d < best.dist && p[2] < best.depth)) {
      best = { kind: "v", id: vi, dist: d, depth: p[2] };
    }
  }
  const eAlive = snap.e.alive;
  const evs = snap.e.vs;
  for (let ei = 0; ei < snap.caps.e; ei++) {
    if (!eAlive[ei]) {
      continue;
    }
    const v0 = evs[ei * 2];
    const v1 = evs[ei * 2 + 1];
    const p0 = vProj[v0];
    const p1 = vProj[v1];
    if (!p0 || !p1) {
      continue;
    }
    const ax = p0[0];
    const ay = p0[1];
    const bx = p1[0];
    const by = p1[1];
    const dx = bx - ax;
    const dy = by - ay;
    const len2 = dx * dx + dy * dy;
    if (len2 < 1e-12) {
      continue;
    }
    let t = ((ndcX - ax) * dx + (ndcY - ay) * dy) / len2;
    if (t < 0)
      t = 0;
    if (t > 1)
      t = 1;
    const cx = ax + dx * t;
    const cy = ay + dy * t;
    const d = Math.hypot(ndcX - cx, ndcY - cy);
    const depth = p0[2] + (p1[2] - p0[2]) * t;
    if (d < pixelToleranceNdc && (best === null || d < best.dist - 5e-3 || d < best.dist && depth < best.depth)) {
      best = { kind: "e", id: ei, dist: d, depth };
    }
  }
  if (!best) {
    return null;
  }
  return { kind: best.kind, id: best.id };
};

// src/ui/Timeline.ts
var Timeline = class {
  slider;
  label;
  opLabel;
  onStep;
  log = null;
  step = 0;
  constructor(onStep) {
    this.slider = document.getElementById("step-slider");
    this.label = document.getElementById("step-label");
    this.opLabel = document.getElementById("op-label");
    this.onStep = onStep;
    this.slider.addEventListener("input", () => {
      this.setStep(parseInt(this.slider.value, 10));
    });
    document.getElementById("step-first").addEventListener("click", () => this.setStep(0));
    document.getElementById("step-last").addEventListener("click", () => {
      this.setStep(this.maxStep());
    });
    document.getElementById("step-prev").addEventListener("click", () => {
      this.setStep(this.step - 1);
    });
    document.getElementById("step-next").addEventListener("click", () => {
      this.setStep(this.step + 1);
    });
  }
  maxStep() {
    return this.log ? this.log.steps.length : 0;
  }
  setLog(log) {
    this.log = log;
    this.step = 0;
    this.slider.min = "0";
    this.slider.max = String(this.maxStep());
    this.slider.value = "0";
    this.refreshLabel();
    this.onStep(0);
  }
  setStep(s) {
    if (s < 0)
      s = 0;
    if (s > this.maxStep())
      s = this.maxStep();
    this.step = s;
    this.slider.value = String(s);
    this.refreshLabel();
    this.onStep(s);
  }
  refreshLabel() {
    const n = this.maxStep();
    this.label.textContent = `${this.step} / ${n}`;
    if (!this.log || this.step === 0) {
      this.opLabel.textContent = this.log ? `[${this.log.tag}] initial` : "";
    } else {
      const st = this.log.steps[this.step - 1];
      const hl = st.highlight ? ` ${st.highlight.kind}=[${st.highlight.ids.join(",")}]` : "";
      this.opLabel.textContent = `[${this.log.tag}] ${st.op}${hl}`;
    }
  }
};

// src/ui/Inspector.ts
var ELEM_NONE2 = -1;
var Inspector = class {
  root;
  onSelect;
  constructor(root, onSelect) {
    this.root = root;
    this.onSelect = onSelect;
  }
  render(snap, sel) {
    this.root.innerHTML = "";
    const summary = document.createElement("div");
    summary.innerHTML = `<h3>Mesh</h3><div class='row'><span>verts</span><span>${snap.counts.v} / ${snap.caps.v}</span></div><div class='row'><span>edges</span><span>${snap.counts.e} / ${snap.caps.e}</span></div><div class='row'><span>corners</span><span>${snap.counts.c} / ${snap.caps.c}</span></div><div class='row'><span>faces</span><span>${snap.counts.f} / ${snap.caps.f}</span></div>`;
    this.root.appendChild(summary);
    if (!sel) {
      const hint = document.createElement("div");
      hint.style.color = "#666";
      hint.style.marginTop = "8px";
      hint.textContent = "Click a vertex or edge to inspect";
      this.root.appendChild(hint);
      return;
    }
    const block = document.createElement("div");
    if (sel.kind === "v") {
      const vi = sel.id;
      const co = snap.v.co;
      block.innerHTML = `<h3>vert ${vi}</h3>` + this.row("co", `${co[vi * 3].toFixed(3)}, ${co[vi * 3 + 1].toFixed(3)}, ${co[vi * 3 + 2].toFixed(3)}`) + this.rowLink("e (disk start)", "e", snap.v.e[vi]);
      const neighbors = this.diskNeighbors(snap, vi);
      block.innerHTML += `<h3>disk (${neighbors.length})</h3>`;
      for (const n of neighbors) {
        block.innerHTML += this.rowLink(`e${n.e}`, "v", n.other);
      }
    } else if (sel.kind === "e") {
      const ei = sel.id;
      const v0 = snap.e.vs[ei * 2];
      const v1 = snap.e.vs[ei * 2 + 1];
      block.innerHTML = `<h3>edge ${ei}</h3>` + this.rowLink("v0", "v", v0) + this.rowLink("v1", "v", v1) + this.rowLink("c (radial start)", "c", snap.e.c[ei]);
      const radial = this.radialFaces(snap, ei);
      block.innerHTML += `<h3>radial (${radial.length})</h3>`;
      for (const r of radial) {
        block.innerHTML += this.rowLink(`c${r.c} \u2192 f${r.f} (l${r.l})`, "f", r.f);
      }
    } else if (sel.kind === "f") {
      const fi = sel.id;
      block.innerHTML = `<h3>face ${fi}</h3>` + this.rowLink("l", "c", snap.f.l[fi]) + this.row("list_count", String(snap.f.list_count[fi]));
      const corners = this.faceCorners(snap, fi);
      block.innerHTML += `<h3>corners (${corners.length})</h3>`;
      for (const c of corners) {
        block.innerHTML += this.rowLink(`c${c.c}`, "v", c.v);
      }
    } else {
      const ci = sel.id;
      block.innerHTML = `<h3>corner ${ci}</h3>` + this.rowLink("v", "v", snap.c.v[ci]) + this.rowLink("e", "e", snap.c.e[ci]) + this.rowLink("l", "c", snap.c.l[ci]) + this.rowLink("next", "c", snap.c.next[ci]) + this.rowLink("prev", "c", snap.c.prev[ci]) + this.rowLink("radial_next", "c", snap.c.radial_next[ci]) + this.rowLink("radial_prev", "c", snap.c.radial_prev[ci]);
    }
    this.root.appendChild(block);
    block.querySelectorAll("a.elref").forEach((a) => {
      a.addEventListener("click", () => {
        const k = a.getAttribute("data-kind");
        const id = parseInt(a.getAttribute("data-id") ?? "-1", 10);
        if (id !== ELEM_NONE2 && id >= 0) {
          this.onSelect({ kind: k, id });
        }
      });
    });
  }
  row(k, v) {
    return `<div class='row'><span>${k}</span><span>${v}</span></div>`;
  }
  rowLink(k, kind, id) {
    if (id === ELEM_NONE2) {
      return this.row(k, "\u2014");
    }
    return `<div class='row'><span>${k}</span><a class='elref' data-kind='${kind}' data-id='${id}'>${kind}${id}</a></div>`;
  }
  diskNeighbors(snap, vi) {
    const out = [];
    const e0 = snap.v.e[vi];
    if (e0 === ELEM_NONE2) {
      return out;
    }
    let ec = e0;
    let safety = 0;
    do {
      const v0 = snap.e.vs[ec * 2];
      const v1 = snap.e.vs[ec * 2 + 1];
      const side = v0 === vi ? 0 : 1;
      out.push({ e: ec, other: side === 0 ? v1 : v0 });
      ec = snap.e.disk[ec * 4 + side * 2 + 1];
      if (++safety > 4096) {
        break;
      }
    } while (ec !== e0);
    return out;
  }
  radialFaces(snap, ei) {
    const out = [];
    const c0 = snap.e.c[ei];
    if (c0 === ELEM_NONE2) {
      return out;
    }
    let cc = c0;
    let safety = 0;
    do {
      const li = snap.c.l[cc];
      out.push({ c: cc, l: li, f: snap.l.f[li] });
      cc = snap.c.radial_next[cc];
      if (++safety > 4096) {
        break;
      }
    } while (cc !== c0);
    return out;
  }
  faceCorners(snap, fi) {
    const out = [];
    const li = snap.f.l[fi];
    if (li === ELEM_NONE2) {
      return out;
    }
    const c0 = snap.l.c[li];
    let cc = c0;
    let safety = 0;
    do {
      out.push({ c: cc, v: snap.c.v[cc] });
      cc = snap.c.next[cc];
      if (++safety > 4096) {
        break;
      }
    } while (cc !== c0);
    return out;
  }
};

// src/main.ts
var main = async () => {
  const canvas = document.getElementById("view");
  const gl = initGL(canvas);
  const cam = new OrbitCamera();
  cam.attach(canvas);
  const meshDraw = new MeshDraw(gl);
  const overlay = new TopoOverlay(gl);
  const inspector = new Inspector(document.getElementById("inspector"), (sel) => {
    selection = sel;
    refreshSelection();
  });
  let source = null;
  let log = null;
  let snap = null;
  let selection = null;
  const opts = { faces: true, edges: true, verts: true, overlay: true };
  const bind = (id, key) => {
    const el = document.getElementById(id);
    el.addEventListener("change", () => {
      opts[key] = el.checked;
    });
  };
  bind("tg-faces", "faces");
  bind("tg-edges", "edges");
  bind("tg-verts", "verts");
  bind("tg-overlay", "overlay");
  const refreshSelection = () => {
    if (snap) {
      overlay.build(snap, selection);
      inspector.render(snap, selection);
    }
  };
  const setSnapshot = (s) => {
    snap = s;
    meshDraw.upload(s);
    fitCamera(s);
    refreshSelection();
  };
  const fitCamera = (s) => {
    let minX = Infinity;
    let minY = Infinity;
    let minZ = Infinity;
    let maxX = -Infinity;
    let maxY = -Infinity;
    let maxZ = -Infinity;
    for (let i = 0; i < s.caps.v; i++) {
      if (!s.v.alive[i]) {
        continue;
      }
      const x = s.v.co[i * 3];
      const y = s.v.co[i * 3 + 1];
      const z = s.v.co[i * 3 + 2];
      if (x < minX)
        minX = x;
      if (y < minY)
        minY = y;
      if (z < minZ)
        minZ = z;
      if (x > maxX)
        maxX = x;
      if (y > maxY)
        maxY = y;
      if (z > maxZ)
        maxZ = z;
    }
    if (!isFinite(minX)) {
      return;
    }
    cam.target[0] = (minX + maxX) * 0.5;
    cam.target[1] = (minY + maxY) * 0.5;
    cam.target[2] = (minZ + maxZ) * 0.5;
    const r = Math.max(maxX - minX, maxY - minY, maxZ - minZ);
    cam.dist = Math.max(0.5, r * 1.8);
  };
  const timeline = new Timeline((step) => {
    if (!log) {
      return;
    }
    const s = step === 0 ? log.initial : log.steps[step - 1].snapshot;
    meshDraw.highlight = step > 0 ? log.steps[step - 1].highlight ?? null : null;
    if (snap === null) {
      setSnapshot(s);
    } else {
      snap = s;
      meshDraw.upload(s);
      refreshSelection();
    }
  });
  const logSelect = document.getElementById("log-select");
  logSelect.addEventListener("change", async () => {
    if (!source) {
      return;
    }
    if (!logSelect.value) {
      return;
    }
    log = await source.loadLog(logSelect.value);
    snap = null;
    timeline.setLog(log);
  });
  document.getElementById("btn-open").addEventListener("click", async () => {
    const dir = await pickLogDir();
    if (!dir) {
      return;
    }
    source = new JsonFileSource(dir);
    const names = await source.listLogs();
    logSelect.innerHTML = "";
    for (const n of names) {
      const opt = document.createElement("option");
      opt.value = n;
      opt.textContent = n;
      logSelect.appendChild(opt);
    }
    if (names.length > 0) {
      logSelect.value = names[0];
      log = await source.loadLog(names[0]);
      snap = null;
      timeline.setLog(log);
    }
  });
  canvas.addEventListener("click", (e) => {
    if (!snap) {
      return;
    }
    const rect = canvas.getBoundingClientRect();
    const ndcX = (e.clientX - rect.left) / rect.width * 2 - 1;
    const ndcY = 1 - (e.clientY - rect.top) / rect.height * 2;
    const tol = 12 / Math.min(rect.width, rect.height);
    const hit = pickElement(snap, cam.vp, ndcX, ndcY, tol);
    selection = hit;
    refreshSelection();
  });
  const render = () => {
    resizeCanvas(canvas);
    const w = canvas.width;
    const h = canvas.height;
    gl.viewport(0, 0, w, h);
    gl.clearColor(0.11, 0.12, 0.14, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    cam.update(w / h);
    if (snap) {
      meshDraw.draw(cam.vp, opts, selection);
      if (opts.overlay) {
        overlay.draw(cam.vp);
      }
    }
    requestAnimationFrame(render);
  };
  render();
};
main().catch((err) => {
  console.error(err);
  document.body.innerText = "error: " + err.stack;
});
//# sourceMappingURL=renderer.js.map
