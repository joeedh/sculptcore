/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {Mesh} from '../Mesh'
import type {DrawBatch} from '../../gpu/DrawBatch'
import type {GPUManager} from '../../gpu/GPUManager'

/** Auto-generated file */
/* eslint-disable @typescript-eslint/no-misused-new */
/* eslint-disable @typescript-eslint/no-unused-vars */

type pointer<T = any> = number
type int8 = number
type uint8 = number
type int16 = number
type uint16 = number
type int32 = number
type uint32 = number
type int64 = number
type uint64 = number
type float = number
type double = number

export interface MeshBatchManager {
  [Symbol.dispose](): void
  m: Mesh
  createMeshBatch(gpuManager: GPUManager): DrawBatch | undefined
  new (arg0: Mesh): MeshBatchManager
}
