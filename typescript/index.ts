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

import type {DrawBatch} from './sculptcore/gpu/DrawBatch'
import type {SpatialShaders} from './sculptcore/spatial/SpatialShaders'
import type {BuiltinAttr} from './sculptcore/mesh/BuiltinAttr'
import type {VertexData} from './sculptcore/mesh/VertexData'
import type {DrawCommand} from './sculptcore/gpu/DrawCommand'
import type {Brush} from './sculptcore/brush/Brush'
import type {float2} from './litestl/math/float2'
import type {int2} from './litestl/math/int2'
import type {MeshLog} from './sculptcore/meshlog/MeshLog'
import type {AttrData} from './sculptcore/mesh/AttrData'
import type {Buffer} from './sculptcore/gpu/Buffer'
import type {AttrGroup} from './sculptcore/mesh/AttrGroup'
import type {AttrRef} from './sculptcore/mesh/AttrRef'
import type {SpatialNode} from './sculptcore/spatial/SpatialNode'
import type {AttrDef} from './sculptcore/gpu/AttrDef'
import type {CommandExecutor} from './sculptcore/brush/CommandExecutor'
import type {AABB} from './litestl/math/AABB'
import type {ShaderDef} from './sculptcore/gpu/ShaderDef'
import type {StructProp} from './sculptcore/props/StructProp'
import type {CastRayIsect} from './sculptcore/spatial/CastRayIsect'
import type {GPUManager} from './sculptcore/gpu/GPUManager'
import type {SpatialTree} from './sculptcore/spatial/SpatialTree'
import type {int4} from './litestl/math/int4'
import type {Mesh} from './sculptcore/mesh/Mesh'
import type {MeshBatchManager} from './sculptcore/mesh/gpu/MeshBatchManager'
import type {EdgeData} from './sculptcore/mesh/EdgeData'
import type {float3} from './litestl/math/float3'
import type {AttrPage} from './sculptcore/mesh/AttrPage'

export type {DrawBatch} from './sculptcore/gpu/DrawBatch'
export type {SpatialShaders} from './sculptcore/spatial/SpatialShaders'
export type {BuiltinAttr} from './sculptcore/mesh/BuiltinAttr'
export type {VertexData} from './sculptcore/mesh/VertexData'
export type {DrawCommand} from './sculptcore/gpu/DrawCommand'
export type {Brush} from './sculptcore/brush/Brush'
export type {float2} from './litestl/math/float2'
export type {int2} from './litestl/math/int2'
export type {MeshLog} from './sculptcore/meshlog/MeshLog'
export type {AttrData} from './sculptcore/mesh/AttrData'
export type {Buffer} from './sculptcore/gpu/Buffer'
export type {AttrGroup} from './sculptcore/mesh/AttrGroup'
export type {AttrRef} from './sculptcore/mesh/AttrRef'
export type {SpatialNode} from './sculptcore/spatial/SpatialNode'
export type {AttrDef} from './sculptcore/gpu/AttrDef'
export type {CommandExecutor} from './sculptcore/brush/CommandExecutor'
export type {AABB} from './litestl/math/AABB'
export type {ShaderDef} from './sculptcore/gpu/ShaderDef'
export type {StructProp} from './sculptcore/props/StructProp'
export type {CastRayIsect} from './sculptcore/spatial/CastRayIsect'
export type {GPUManager} from './sculptcore/gpu/GPUManager'
export type {SpatialTree} from './sculptcore/spatial/SpatialTree'
export type {int4} from './litestl/math/int4'
export type {Mesh} from './sculptcore/mesh/Mesh'
export type {MeshBatchManager} from './sculptcore/mesh/gpu/MeshBatchManager'
export type {EdgeData} from './sculptcore/mesh/EdgeData'
export type {float3} from './litestl/math/float3'
export type {AttrPage} from './sculptcore/mesh/AttrPage'

/** Note: Does not include templates */
export type AllBoundTypes = {
  'litestl::math::float3': float3
  'litestl::math::float2': float2
  'sculptcore::gpu::DrawCommand': DrawCommand
  'sculptcore::mesh::Mesh': Mesh
  'sculptcore::mesh::BuiltinAttr<int32,.vert.e>': BuiltinAttr<int32, '.vert.e'>
  'sculptcore::spatial::SpatialNode': SpatialNode
  'sculptcore::mesh::BuiltinAttr<boolean,select>': BuiltinAttr<
    boolean,
    'select'
  >
  'sculptcore::meshlog::MeshLog': MeshLog
  'sculptcore::mesh::AttrGroup': AttrGroup
  'litestl::math::int4': int4
  'sculptcore::mesh::AttrRef': AttrRef
  'sculptcore::gpu::DrawBatch': DrawBatch
  'sculptcore::gpu::GPUManager': GPUManager
  'sculptcore::spatial::SpatialTree': SpatialTree
  'sculptcore::gpu::ShaderDef': ShaderDef
  'sculptcore::props::StructProp': StructProp
  'litestl::math::int2': int2
  'sculptcore::brush::CommandExecutor': CommandExecutor
  'sculptcore::brush::Brush': Brush
  'sculptcore::gpu::AttrDef': AttrDef
  'sculptcore::mesh::BuiltinAttr<litestl::math::int2,.edge.vs>': BuiltinAttr<
    int2,
    '.edge.vs'
  >
  'sculptcore::gpu::Buffer': Buffer
  'sculptcore::spatial::CastRayIsect': CastRayIsect
  'sculptcore::mesh::VertexData': VertexData
  'sculptcore::mesh::BuiltinAttr<litestl::math::float3,normals>': BuiltinAttr<
    float3,
    'normals'
  >
  'sculptcore::mesh::BuiltinAttr<litestl::math::int4,.edge.vs.disk>': BuiltinAttr<
    int4,
    '.edge.vs.disk'
  >
  'sculptcore::mesh::BuiltinAttr<litestl::math::float3,positions>': BuiltinAttr<
    float3,
    'positions'
  >
  'sculptcore::mesh::EdgeData': EdgeData
  'sculptcore::spatial::SpatialShaders': SpatialShaders
  'sculptcore::mesh::BuiltinAttr<int32,.edge.c>': BuiltinAttr<int32, '.edge.c'>
  'litestl::math::AABB<litestl::math::float3>': AABB<float3>
  'sculptcore::mesh::gpu::MeshBatchManager': MeshBatchManager
}
