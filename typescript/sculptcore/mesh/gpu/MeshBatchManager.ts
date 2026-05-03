/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {Mesh} from "../Mesh";
import type {DrawBatch} from "../../gpu/DrawBatch";
import type {GPUManager} from "../../gpu/GPUManager";

/** Auto-generated file */
/* eslint-disable @typescript-eslint/no-misused-new */
/* eslint-disable @typescript-eslint/no-unused-vars */

type float = number;
type pointer<T=any> = number;
type int = number;
type uint = number;
type double = number;
type short = number;
type ushort = number;
type char = number;
type uchar = number;
export interface MeshBatchManager {
  [Symbol.dispose](): void;
  m: Mesh
  createMeshBatch(gpuManager: GPUManager): DrawBatch | undefined
  new(arg0: Mesh): MeshBatchManager
}
