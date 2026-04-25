/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {VertexData} from "./VertexData";
import type {EdgeData} from "./EdgeData";

export interface Mesh {
  v: VertexData
  e: EdgeData
  new(): Mesh
}
