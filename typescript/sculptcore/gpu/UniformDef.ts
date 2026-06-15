/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {UniformBindTypeMap} from './UniformBindTypeMap'
import {getTypeSymbol} from '@litestl/typescript-runtime'
import type {GPUType} from './GPUType'

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

export interface UniformDef<T, K extends keyof UniformBindTypeMap> {
  [Symbol.dispose](): void
  readonly [getTypeSymbol]: K
  name: string
  type: GPUType
  elemSize: int32
  defaultValue: UniformBindTypeMap[K]
}
