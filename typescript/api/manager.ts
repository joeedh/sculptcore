import {INeededWasm, BindingManager as WasmBindingManager} from '@litestl/typescript-runtime'
import type {AllBoundTypes} from '../index'

export type BindingManager = WasmBindingManager<INeededWasm, AllBoundTypes>
export const BindingManager = WasmBindingManager
