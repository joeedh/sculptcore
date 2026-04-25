/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {Mesh} from './sculptcore/mesh/Mesh'
import type {BuiltinAttr} from './sculptcore/mesh/BuiltinAttr'
import type {VertexData} from './sculptcore/mesh/VertexData'
import type {int4} from './int4'
import type {EdgeData} from './sculptcore/mesh/EdgeData'
import type {float3} from './float3'
import type {int2} from './int2'

export type {Mesh} from './sculptcore/mesh/Mesh'
export type {BuiltinAttr} from './sculptcore/mesh/BuiltinAttr'
export type {VertexData} from './sculptcore/mesh/VertexData'
export type {int4} from './int4'
export type {EdgeData} from './sculptcore/mesh/EdgeData'
export type {float3} from './float3'
export type {int2} from './int2'

/** Note: Does not include templates */
export type AllBoundTypes = {
  'sculptcore::mesh::EdgeData': EdgeData
  'sculptcore::mesh::BuiltinAttr<number,".edge.c">': BuiltinAttr<number, '.edge.c'>
  'sculptcore::mesh::BuiltinAttr<int2,".edge.vs">': BuiltinAttr<int2, '.edge.vs'>
  'sculptcore::mesh::BuiltinAttr<boolean,"select">': BuiltinAttr<boolean, 'select'>
  'sculptcore::mesh::BuiltinAttr<int4,".edge.vs.disk">': BuiltinAttr<int4, '.edge.vs.disk'>
  int4: int4
  float3: float3
  'sculptcore::mesh::VertexData': VertexData
  'sculptcore::mesh::BuiltinAttr<float3,"positions">': BuiltinAttr<float3, 'positions'>
  'sculptcore::mesh::BuiltinAttr<float3,"normals">': BuiltinAttr<float3, 'normals'>
  'sculptcore::mesh::BuiltinAttr<number,".vert.e">': BuiltinAttr<number, '.vert.e'>
  int2: int2
  'sculptcore::mesh::Mesh': Mesh
}

