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

import type {DrawCommand} from './sculptcore/gpu/DrawCommand'
import type {AttrData} from './sculptcore/mesh/AttrData'
import type {SpatialTree} from './sculptcore/spatial/SpatialTree'
import type {DynTopoParams} from './sculptcore/dyntopo/DynTopoParams'
import type {BrushProgram} from './sculptcore/brush/BrushProgram'
import type {CastRayIsect} from './sculptcore/spatial/CastRayIsect'
import type {AttrGroup} from './sculptcore/mesh/AttrGroup'
import type {AttrRef} from './sculptcore/mesh/AttrRef'
import type {Buffer} from './sculptcore/gpu/Buffer'
import type {Mesh} from './sculptcore/mesh/Mesh'
import type {FaceData} from './sculptcore/mesh/FaceData'
import type {AABB} from './litestl/math/AABB'
import type {Brush} from './sculptcore/brush/Brush'
import type {EdgeData} from './sculptcore/mesh/EdgeData'
import type {float3} from './litestl/math/float3'
import type {DrawBatch} from './sculptcore/gpu/DrawBatch'
import type {DynTopoStats} from './sculptcore/dyntopo/DynTopoStats'
import type {AttrDef} from './sculptcore/gpu/AttrDef'
import type {DrawPipeline} from './sculptcore/gpu/DrawPipeline'
import type {ShaderDef} from './sculptcore/gpu/ShaderDef'
import type {VertexData} from './sculptcore/mesh/VertexData'
import type {AttrPage} from './sculptcore/mesh/AttrPage'
import type {StructProp} from './sculptcore/props/StructProp'
import type {UniformDef} from './sculptcore/gpu/UniformDef'
import type {SpatialNode} from './sculptcore/spatial/SpatialNode'
import type {BrushUniformManifestEntry} from './sculptcore/brush/BrushUniformManifestEntry'
import type {int4} from './litestl/math/int4'
import type {float4} from './litestl/math/float4'
import type {UniformBlockDef} from './sculptcore/gpu/UniformBlockDef'
import type {float2} from './litestl/math/float2'
import type {MeshBatchManager} from './sculptcore/mesh/gpu/MeshBatchManager'
import type {RemeshParams} from './sculptcore/remesh/RemeshParams'
import type {SpatialShaders} from './sculptcore/spatial/SpatialShaders'
import type {CommandExecutor} from './sculptcore/brush/CommandExecutor'
import type {MeshLog} from './sculptcore/meshlog/MeshLog'
import type {GPUManager} from './sculptcore/gpu/GPUManager'
import type {CornerData} from './sculptcore/mesh/CornerData'
import type {int2} from './litestl/math/int2'
import type {UniformBlockInstance} from './sculptcore/gpu/UniformBlockInstance'
import type {BuiltinAttr} from './sculptcore/mesh/BuiltinAttr'

export type {DrawCommand} from './sculptcore/gpu/DrawCommand'
export type {AttrData} from './sculptcore/mesh/AttrData'
export type {SpatialTree} from './sculptcore/spatial/SpatialTree'
export type {DynTopoParams} from './sculptcore/dyntopo/DynTopoParams'
export type {BrushProgram} from './sculptcore/brush/BrushProgram'
export type {CastRayIsect} from './sculptcore/spatial/CastRayIsect'
export type {AttrGroup} from './sculptcore/mesh/AttrGroup'
export type {AttrRef} from './sculptcore/mesh/AttrRef'
export type {Buffer} from './sculptcore/gpu/Buffer'
export type {Mesh} from './sculptcore/mesh/Mesh'
export type {FaceData} from './sculptcore/mesh/FaceData'
export type {AABB} from './litestl/math/AABB'
export type {Brush} from './sculptcore/brush/Brush'
export type {EdgeData} from './sculptcore/mesh/EdgeData'
export type {float3} from './litestl/math/float3'
export type {DrawBatch} from './sculptcore/gpu/DrawBatch'
export type {DynTopoStats} from './sculptcore/dyntopo/DynTopoStats'
export type {AttrDef} from './sculptcore/gpu/AttrDef'
export type {DrawPipeline} from './sculptcore/gpu/DrawPipeline'
export type {ShaderDef} from './sculptcore/gpu/ShaderDef'
export type {VertexData} from './sculptcore/mesh/VertexData'
export type {AttrPage} from './sculptcore/mesh/AttrPage'
export type {StructProp} from './sculptcore/props/StructProp'
export type {UniformDef} from './sculptcore/gpu/UniformDef'
export type {SpatialNode} from './sculptcore/spatial/SpatialNode'
export type {BrushUniformManifestEntry} from './sculptcore/brush/BrushUniformManifestEntry'
export type {int4} from './litestl/math/int4'
export type {float4} from './litestl/math/float4'
export type {UniformBlockDef} from './sculptcore/gpu/UniformBlockDef'
export type {float2} from './litestl/math/float2'
export type {MeshBatchManager} from './sculptcore/mesh/gpu/MeshBatchManager'
export type {RemeshParams} from './sculptcore/remesh/RemeshParams'
export type {SpatialShaders} from './sculptcore/spatial/SpatialShaders'
export type {CommandExecutor} from './sculptcore/brush/CommandExecutor'
export type {MeshLog} from './sculptcore/meshlog/MeshLog'
export type {GPUManager} from './sculptcore/gpu/GPUManager'
export type {CornerData} from './sculptcore/mesh/CornerData'
export type {int2} from './litestl/math/int2'
export type {UniformBlockInstance} from './sculptcore/gpu/UniformBlockInstance'
export type {BuiltinAttr} from './sculptcore/mesh/BuiltinAttr'

/** Note: Does not include templates */
export type AllBoundTypes = {
  'sculptcore::mesh::BuiltinAttr<boolean,select>': BuiltinAttr<
    boolean,
    'select'
  >
  'sculptcore::gpu::GPUManager': GPUManager
  'sculptcore::spatial::SpatialNode': SpatialNode
  'sculptcore::mesh::BuiltinAttr<int16,.face.list_count>': BuiltinAttr<
    int16,
    '.face.list_count'
  >
  'sculptcore::mesh::AttrGroup': AttrGroup
  'sculptcore::mesh::BuiltinAttr<int32,.corner.prev>': BuiltinAttr<
    int32,
    '.corner.prev'
  >
  'sculptcore::gpu::UniformBlockDef': UniformBlockDef
  'sculptcore::gpu::DrawBatch': DrawBatch
  'sculptcore::mesh::BuiltinAttr<litestl::math::int2,.edge.vs>': BuiltinAttr<
    int2,
    '.edge.vs'
  >
  'sculptcore::brush::BrushUniformManifestEntry': BrushUniformManifestEntry
  'sculptcore::meshlog::MeshLog': MeshLog
  'sculptcore::spatial::SpatialShaders': SpatialShaders
  'sculptcore::remesh::RemeshParams': RemeshParams
  'sculptcore::mesh::BuiltinAttr<int32,.face.list>': BuiltinAttr<
    int32,
    '.face.list'
  >
  'sculptcore::dyntopo::DynTopoParams': DynTopoParams
  'litestl::math::float2': float2
  'sculptcore::mesh::BuiltinAttr<litestl::math::float3,.face.normal>': BuiltinAttr<
    float3,
    '.face.normal'
  >
  'sculptcore::mesh::BuiltinAttr<int32,.vert.e>': BuiltinAttr<int32, '.vert.e'>
  'sculptcore::gpu::Buffer': Buffer
  'litestl::math::int2': int2
  'sculptcore::spatial::SpatialTree': SpatialTree
  'sculptcore::brush::CommandExecutor': CommandExecutor
  'sculptcore::gpu::DrawPipeline': DrawPipeline
  'sculptcore::mesh::gpu::MeshBatchManager': MeshBatchManager
  'sculptcore::mesh::AttrRef': AttrRef
  'sculptcore::dyntopo::DynTopoStats': DynTopoStats
  'sculptcore::mesh::BuiltinAttr<int32,.corner.radial_prev>': BuiltinAttr<
    int32,
    '.corner.radial_prev'
  >
  'sculptcore::mesh::BuiltinAttr<int32,.corner.next>': BuiltinAttr<
    int32,
    '.corner.next'
  >
  'sculptcore::spatial::CastRayIsect': CastRayIsect
  'sculptcore::brush::Brush': Brush
  'sculptcore::props::StructProp': StructProp
  'sculptcore::brush::BrushProgram': BrushProgram
  'sculptcore::gpu::ShaderDef': ShaderDef
  'litestl::math::float3': float3
  'litestl::math::AABB<litestl::math::float3>': AABB<float3>
  'sculptcore::mesh::BuiltinAttr<int32,.corner.v>': BuiltinAttr<
    int32,
    '.corner.v'
  >
  'sculptcore::mesh::Mesh': Mesh
  'litestl::math::int4': int4
  'sculptcore::mesh::BuiltinAttr<int32,.corner.radial_next>': BuiltinAttr<
    int32,
    '.corner.radial_next'
  >
  'sculptcore::mesh::CornerData': CornerData
  'sculptcore::gpu::DrawCommand': DrawCommand
  'sculptcore::mesh::BuiltinAttr<int32,.corner.l>': BuiltinAttr<
    int32,
    '.corner.l'
  >
  'sculptcore::mesh::BuiltinAttr<int32,.corner.e>': BuiltinAttr<
    int32,
    '.corner.e'
  >
  'sculptcore::mesh::BuiltinAttr<litestl::math::float3,positions>': BuiltinAttr<
    float3,
    'positions'
  >
  'sculptcore::gpu::AttrDef': AttrDef
  'sculptcore::gpu::UniformBlockInstance': UniformBlockInstance
  'sculptcore::mesh::BuiltinAttr<litestl::math::int4,.edge.vs.disk>': BuiltinAttr<
    int4,
    '.edge.vs.disk'
  >
  'sculptcore::mesh::VertexData': VertexData
  'litestl::math::float4': float4
  'sculptcore::mesh::BuiltinAttr<litestl::math::float3,normals>': BuiltinAttr<
    float3,
    'normals'
  >
  'sculptcore::mesh::BuiltinAttr<int32,.edge.c>': BuiltinAttr<int32, '.edge.c'>
  'sculptcore::mesh::FaceData': FaceData
  'sculptcore::mesh::EdgeData': EdgeData
}
