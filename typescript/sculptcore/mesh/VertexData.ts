/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import {BuiltinAttr} from "./BuiltinAttr";
import {float3} from "../../float3";

export interface VertexData {
  capacity_: number
  co: BuiltinAttr<float3, "positions">
  no: BuiltinAttr<float3, "normals">
  e: BuiltinAttr<number, ".vert.e">
}
