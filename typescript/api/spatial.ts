import type {IWasmInterface} from './wasm'
import type {Mesh, SpatialNode, SpatialTree} from '../index'

export function buildSpatialTree(wasm: IWasmInterface, mesh: Mesh, leafLimit = 512): SpatialTree {
  const tree = wasm.Mesh_buildSpatialTree(mesh, leafLimit)
  tree.leaf_limit = leafLimit
  return tree
}

export interface LeafBounds {
  min: Float32Array
  max: Float32Array
}

export function getLeafBounds(tree: SpatialTree): LeafBounds[] {
  const result: LeafBounds[] = []
  const leaves = tree.leaves()
  const len = (leaves as unknown as {length: number}).length
  for (let i = 0; i < len; i++) {
    const node = (leaves as unknown as SpatialNode[])[i]
    result.push({
      min: new Float32Array(node.min.vec as unknown as ArrayLike<number>),
      max: new Float32Array(node.max.vec as unknown as ArrayLike<number>),
    })
  }
  return result
}
