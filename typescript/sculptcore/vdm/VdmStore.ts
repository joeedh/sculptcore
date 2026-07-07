/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */

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

export interface VdmStore {
  [Symbol.dispose](): void
  tileCount(): int32
  contentRev(): int32
  gpuLayoutOut(out: int32[]): int32
  gpuPageTableOut(out: int32[]): void
  gpuPtexTableOut(out: int32[]): void
  configurePtex(gridCount: int32, defaultRes: int32, links: int32[]): void
  gpuAtlasPixelsOut(out: float[]): void
  gpuTilePixelsOut(slot: int32, out: float[]): int32
  gpuTakeDirtyOut(outSlots: int32[]): int32
}
