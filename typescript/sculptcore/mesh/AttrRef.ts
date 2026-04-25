/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
export enum AttrFlag {
  None = 0,
  Topo = 1,
  Temp = 2,
  NoCopy = 4,
  NoInterp = 8,
}

export enum AttrType {
  Float = 0,
  Int = 32,
  Vec2 = 1,
  Float2 = 2,
  Float3 = 4,
  Float4 = 8,
  Bool = 16,
  Byte = 512,
  Short = 1024,
  Int2 = 64,
  Int3 = 128,
  Int4 = 256,
}

import type {String} from '../../litestl/util/String'

export interface AttrRef {
  name: String
  type: AttrType
  flag: AttrFlag
}
