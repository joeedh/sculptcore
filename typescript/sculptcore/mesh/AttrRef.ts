/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {AttrType} from "./AttrType";
import type {AttrFlag} from "./AttrFlag";

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
export interface AttrRef {
  [Symbol.dispose](): void;
  name: string
  type: AttrType
  flag: AttrFlag
  data: AttrData<float>|AttrData<int>|AttrData<unsigned char>|AttrData<short>
}
