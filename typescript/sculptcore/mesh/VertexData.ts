/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */

import type {AttrGroup} from "./AttrGroup";
import type {BuiltinAttr} from "./BuiltinAttr";
import type {float3} from "../../float3";

export interface VertexData {
  attrs: AttrGroup
  capacity_: number
  co: BuiltinAttr<float3,"positions">
  no: BuiltinAttr<float3,"normals">
  e: BuiltinAttr<number,".vert.e">
  alloc(arg0: number, arg1: boolean): void
  alloc(): number
}
