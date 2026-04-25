/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {BuiltinAttr} from "./BuiltinAttr";
import type {int2} from "../../int2";
import type {int4} from "../../int4";

export interface EdgeData {
  capacity_: number
  c: BuiltinAttr<number,".edge.c">
  vs: BuiltinAttr<int2,".edge.vs">
  select: BuiltinAttr<boolean,"select">
  disk: BuiltinAttr<int4,".edge.vs.disk">
}
