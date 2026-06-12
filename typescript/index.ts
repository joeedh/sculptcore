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

import type {DynTopoParams} from './sculptcore/dyntopo/DynTopoParams'
import type {DrawBatch} from './sculptcore/gpu/DrawBatch'
import type {SpatialShaders} from './sculptcore/spatial/SpatialShaders'
import type {CornerData} from './sculptcore/mesh/CornerData'
import type {BuiltinAttr} from './sculptcore/mesh/BuiltinAttr'
import type {UniformBlockInstance} from './sculptcore/gpu/UniformBlockInstance'
import type {VertexData} from './sculptcore/mesh/VertexData'
import type {BrushUniformManifestEntry} from './sculptcore/brush/BrushUniformManifestEntry'
import type {DrawCommand} from './sculptcore/gpu/DrawCommand'
import type {Brush} from './sculptcore/brush/Brush'
import type {float2} from './litestl/math/float2'
import type {ShaderDef} from './sculptcore/gpu/ShaderDef'
import type {MeshLog} from './sculptcore/meshlog/MeshLog'
import type {UniformDef} from './sculptcore/gpu/UniformDef'
import type {AttrData} from './sculptcore/mesh/AttrData'
import type {Buffer} from './sculptcore/gpu/Buffer'
import type {AttrGroup} from './sculptcore/mesh/AttrGroup'
import type {AttrRef} from './sculptcore/mesh/AttrRef'
import type {EdgeData} from './sculptcore/mesh/EdgeData'
import type {SpatialNode} from './sculptcore/spatial/SpatialNode'
import type {DynTopoStats} from './sculptcore/dyntopo/DynTopoStats'
import type {AttrDef} from './sculptcore/gpu/AttrDef'
import type {UniformBlockDef} from './sculptcore/gpu/UniformBlockDef'
import type {BrushProgram} from './sculptcore/brush/BrushProgram'
import type {DrawPipeline} from './sculptcore/gpu/DrawPipeline'
import type {StructProp} from './sculptcore/props/StructProp'
import type {CastRayIsect} from './sculptcore/spatial/CastRayIsect'
import type {GPUManager} from './sculptcore/gpu/GPUManager'
import type {SpatialTree} from './sculptcore/spatial/SpatialTree'
import type {CommandExecutor} from './sculptcore/brush/CommandExecutor'
import type {int4} from './litestl/math/int4'
import type {int2} from './litestl/math/int2'
import type {RemeshParams} from './sculptcore/remesh/RemeshParams'
import type {Mesh} from './sculptcore/mesh/Mesh'
import type {MeshBatchManager} from './sculptcore/mesh/gpu/MeshBatchManager'
import type {AABB} from './litestl/math/AABB'
import type {float3} from './litestl/math/float3'
import type {AttrPage} from './sculptcore/mesh/AttrPage'
import type {FaceData} from './sculptcore/mesh/FaceData'

export type {DynTopoParams} from './sculptcore/dyntopo/DynTopoParams'
export type {DrawBatch} from './sculptcore/gpu/DrawBatch'
export type {SpatialShaders} from './sculptcore/spatial/SpatialShaders'
export type {CornerData} from './sculptcore/mesh/CornerData'
export type {BuiltinAttr} from './sculptcore/mesh/BuiltinAttr'
export type {UniformBlockInstance} from './sculptcore/gpu/UniformBlockInstance'
export type {VertexData} from './sculptcore/mesh/VertexData'
export type {BrushUniformManifestEntry} from './sculptcore/brush/BrushUniformManifestEntry'
export type {DrawCommand} from './sculptcore/gpu/DrawCommand'
export type {Brush} from './sculptcore/brush/Brush'
export type {float2} from './litestl/math/float2'
export type {ShaderDef} from './sculptcore/gpu/ShaderDef'
export type {MeshLog} from './sculptcore/meshlog/MeshLog'
export type {UniformDef} from './sculptcore/gpu/UniformDef'
export type {AttrData} from './sculptcore/mesh/AttrData'
export type {Buffer} from './sculptcore/gpu/Buffer'
export type {AttrGroup} from './sculptcore/mesh/AttrGroup'
export type {AttrRef} from './sculptcore/mesh/AttrRef'
export type {EdgeData} from './sculptcore/mesh/EdgeData'
export type {SpatialNode} from './sculptcore/spatial/SpatialNode'
export type {DynTopoStats} from './sculptcore/dyntopo/DynTopoStats'
export type {AttrDef} from './sculptcore/gpu/AttrDef'
export type {UniformBlockDef} from './sculptcore/gpu/UniformBlockDef'
export type {BrushProgram} from './sculptcore/brush/BrushProgram'
export type {DrawPipeline} from './sculptcore/gpu/DrawPipeline'
export type {StructProp} from './sculptcore/props/StructProp'
export type {CastRayIsect} from './sculptcore/spatial/CastRayIsect'
export type {GPUManager} from './sculptcore/gpu/GPUManager'
export type {SpatialTree} from './sculptcore/spatial/SpatialTree'
export type {CommandExecutor} from './sculptcore/brush/CommandExecutor'
export type {int4} from './litestl/math/int4'
export type {int2} from './litestl/math/int2'
export type {RemeshParams} from './sculptcore/remesh/RemeshParams'
export type {Mesh} from './sculptcore/mesh/Mesh'
export type {MeshBatchManager} from './sculptcore/mesh/gpu/MeshBatchManager'
export type {AABB} from './litestl/math/AABB'
export type {float3} from './litestl/math/float3'
export type {AttrPage} from './sculptcore/mesh/AttrPage'
export type {FaceData} from './sculptcore/mesh/FaceData'

/** Note: Does not include templates */
export type AllBoundTypes = {
  'sculptcore::dyntopo::DynTopoStats': DynTopoStats
  'sculptcore::mesh::BuiltinAttr<int32,.corner.next>': BuiltinAttr<
    int32,
    '.corner.next'
  >
  'sculptcore::mesh::BuiltinAttr<int32,.corner.radial_prev>': BuiltinAttr<
    int32,
    '.corner.radial_prev'
  >
  'sculptcore::gpu::AttrDef': AttrDef
  'sculptcore::dyntopo::DynTopoParams': DynTopoParams
  'sculptcore::mesh::FaceData': FaceData
  'sculptcore::props::StructProp': StructProp
  'sculptcore::gpu::UniformBlockInstance': UniformBlockInstance
  'sculptcore::mesh::BuiltinAttr<litestl::math::float3,positions>': BuiltinAttr<
    float3,
    'positions'
  >
  'sculptcore::gpu::DrawBatch': DrawBatch
  'litestl::math::float2': float2
  'sculptcore::mesh::BuiltinAttr<int16,.face.list_count>': BuiltinAttr<
    int16,
    '.face.list_count'
  >
  'sculptcore::mesh::BuiltinAttr<litestl::math::float3,.face.normal>': BuiltinAttr<
    float3,
    '.face.normal'
  >
  'litestl::math::AABB<litestl::math::float3>': AABB<float3>
  'sculptcore::mesh::BuiltinAttr<int32,.face.list>': BuiltinAttr<
    int32,
    '.face.list'
  >
  'sculptcore::mesh::CornerData': CornerData
  'sculptcore::remesh::RemeshParams': RemeshParams
  'sculptcore::mesh::VertexData': VertexData
  'sculptcore::spatial::SpatialNode': SpatialNode
  'sculptcore::brush::BrushUniformManifestEntry': BrushUniformManifestEntry
  'sculptcore::gpu::GPUManager': GPUManager
  'litestl::math::int2': int2
  'sculptcore::mesh::BuiltinAttr<boolean,select>': BuiltinAttr<
    boolean,
    'select'
  >
  'sculptcore::spatial::CastRayIsect': CastRayIsect
  'sculptcore::mesh::gpu::MeshBatchManager': MeshBatchManager
  'litestl::math::int4': int4
  'sculptcore::brush::CommandExecutor': CommandExecutor
  'sculptcore::mesh::BuiltinAttr<int32,.corner.radial_next>': BuiltinAttr<
    int32,
    '.corner.radial_next'
  >
  'litestl::math::float3': float3
  'sculptcore::gpu::ShaderDef': ShaderDef
  'sculptcore::gpu::DrawPipeline': DrawPipeline
  'sculptcore::mesh::BuiltinAttr<int32,.corner.prev>': BuiltinAttr<
    int32,
    '.corner.prev'
  >
  'sculptcore::mesh::BuiltinAttr<litestl::math::int4,.edge.vs.disk>': BuiltinAttr<
    int4,
    '.edge.vs.disk'
  >
  'sculptcore::mesh::BuiltinAttr<litestl::math::float3,normals>': BuiltinAttr<
    float3,
    'normals'
  >
  'sculptcore::mesh::AttrRef': AttrRef
  'sculptcore::mesh::BuiltinAttr<int32,.vert.e>': BuiltinAttr<int32, '.vert.e'>
  'sculptcore::brush::Brush': Brush
  'sculptcore::meshlog::MeshLog': MeshLog
  'sculptcore::gpu::Buffer': Buffer
  'sculptcore::mesh::BuiltinAttr<int32,.edge.c>': BuiltinAttr<int32, '.edge.c'>
  'sculptcore::gpu::DrawCommand': DrawCommand
  'sculptcore::mesh::BuiltinAttr<litestl::math::int2,.edge.vs>': BuiltinAttr<
    int2,
    '.edge.vs'
  >
  'sculptcore::mesh::AttrGroup': AttrGroup
  'sculptcore::gpu::UniformBlockDef': UniformBlockDef
  'sculptcore::mesh::EdgeData': EdgeData
  'sculptcore::spatial::SpatialTree': SpatialTree
  'sculptcore::mesh::BuiltinAttr<int32,.corner.v>': BuiltinAttr<
    int32,
    '.corner.v'
  >
  'sculptcore::mesh::Mesh': Mesh
  'sculptcore::brush::BrushProgram': BrushProgram
  'sculptcore::spatial::SpatialShaders': SpatialShaders
  'sculptcore::mesh::BuiltinAttr<int32,.corner.e>': BuiltinAttr<
    int32,
    '.corner.e'
  >
  'sculptcore::mesh::BuiltinAttr<int32,.corner.l>': BuiltinAttr<
    int32,
    '.corner.l'
  >
}
