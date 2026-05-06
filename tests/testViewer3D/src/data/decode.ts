export const decodeBase64 = (s: string): Uint8Array => {
  const bin = Buffer.from(s, 'base64')
  const out = new Uint8Array(bin.length)
  out.set(bin)
  return out
}

export const decodeF32 = (s: string): Float32Array => {
  const u8 = decodeBase64(s)
  return new Float32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4)
}

export const decodeI32 = (s: string): Int32Array => {
  const u8 = decodeBase64(s)
  return new Int32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4)
}

export const decodeI16 = (s: string): Int16Array => {
  const u8 = decodeBase64(s)
  return new Int16Array(u8.buffer, u8.byteOffset, u8.byteLength / 2)
}

export const decodeU8 = (s: string): Uint8Array => decodeBase64(s)
