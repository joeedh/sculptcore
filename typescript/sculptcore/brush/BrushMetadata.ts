/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {BrushAttrManifestEntry} from './BrushAttrManifestEntry'
import type {BrushDefFlags} from './BrushDefFlags'

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

export interface BrushMetadata {
  [Symbol.dispose](): void
  queryAttrManifest(brushType: int32): int32
  queriedAttrEntry(idx: int32): BrushAttrManifestEntry | undefined
  queryBrushFlags(brushType: int32): BrushDefFlags | undefined
  new (): BrushMetadata
}
