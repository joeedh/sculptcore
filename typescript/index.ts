/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
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

import type {DrawBatch} from "./sculptcore/gpu/DrawBatch";
import type {BuiltinAttr} from "./sculptcore/mesh/BuiltinAttr";
import type {VertexData} from "./sculptcore/mesh/VertexData";
import type {DrawCommand} from "./sculptcore/gpu/DrawCommand";
import type {ShaderDef} from "./sculptcore/gpu/ShaderDef";
import type {AttrData} from "./sculptcore/mesh/AttrData";
import type {Buffer} from "./sculptcore/gpu/Buffer";
import type {AttrGroup} from "./sculptcore/mesh/AttrGroup";
import type {EdgeData} from "./sculptcore/mesh/EdgeData";
import type {SpatialNode} from "./sculptcore/spatial/SpatialNode";
import type {SpatialTree} from "./sculptcore/spatial/SpatialTree";
import type {GPUManager} from "./sculptcore/gpu/GPUManager";
import type {int2} from "./litestl/math/int2";
import type {int4} from "./litestl/math/int4";
import type {Mesh} from "./sculptcore/mesh/Mesh";
import type {MeshBatchManager} from "./sculptcore/mesh/gpu/MeshBatchManager";
import type {AttrRef} from "./sculptcore/mesh/AttrRef";
import type {float3} from "./litestl/math/float3";
import type {AttrPage} from "./sculptcore/mesh/AttrPage";

export type {DrawBatch} from "./sculptcore/gpu/DrawBatch";
export type {BuiltinAttr} from "./sculptcore/mesh/BuiltinAttr";
export type {VertexData} from "./sculptcore/mesh/VertexData";
export type {DrawCommand} from "./sculptcore/gpu/DrawCommand";
export type {ShaderDef} from "./sculptcore/gpu/ShaderDef";
export type {AttrData} from "./sculptcore/mesh/AttrData";
export type {Buffer} from "./sculptcore/gpu/Buffer";
export type {AttrGroup} from "./sculptcore/mesh/AttrGroup";
export type {EdgeData} from "./sculptcore/mesh/EdgeData";
export type {SpatialNode} from "./sculptcore/spatial/SpatialNode";
export type {SpatialTree} from "./sculptcore/spatial/SpatialTree";
export type {GPUManager} from "./sculptcore/gpu/GPUManager";
export type {int2} from "./litestl/math/int2";
export type {int4} from "./litestl/math/int4";
export type {Mesh} from "./sculptcore/mesh/Mesh";
export type {MeshBatchManager} from "./sculptcore/mesh/gpu/MeshBatchManager";
export type {AttrRef} from "./sculptcore/mesh/AttrRef";
export type {float3} from "./litestl/math/float3";
export type {AttrPage} from "./sculptcore/mesh/AttrPage";

/** Note: Does not include templates */
export type AllBoundTypes = {
  "litestl::math::float3": float3,
  "sculptcore::gpu::DrawCommand": DrawCommand,
  "sculptcore::mesh::Mesh": Mesh,
  "sculptcore::mesh::VertexData": VertexData,
  "sculptcore::mesh::EdgeData": EdgeData,
  "sculptcore::spatial::SpatialNode": SpatialNode,
  "litestl::math::int2": int2,
  "sculptcore::mesh::AttrGroup": AttrGroup,
  "litestl::math::int4": int4,
  "sculptcore::mesh::BuiltinAttr<litestl::math::float3,positions>": BuiltinAttr<float3,"positions">,
  "sculptcore::mesh::BuiltinAttr<litestl::math::float3,normals>": BuiltinAttr<float3,"normals">,
  "sculptcore::mesh::BuiltinAttr<int,.vert.e>": BuiltinAttr<int,".vert.e">,
  "sculptcore::gpu::GPUManager": GPUManager,
  "sculptcore::mesh::BuiltinAttr<int,.edge.c>": BuiltinAttr<int,".edge.c">,
  "sculptcore::mesh::BuiltinAttr<litestl::math::int2,.edge.vs>": BuiltinAttr<int2,".edge.vs">,
  "sculptcore::mesh::BuiltinAttr<boolean,select>": BuiltinAttr<boolean,"select">,
  "sculptcore::mesh::BuiltinAttr<litestl::math::int4,.edge.vs.disk>": BuiltinAttr<int4,".edge.vs.disk">,
  "sculptcore::gpu::DrawBatch": DrawBatch,
  "sculptcore::gpu::Buffer": Buffer,
  "sculptcore::mesh::AttrRef": AttrRef,
  "sculptcore::gpu::ShaderDef": ShaderDef,
  "sculptcore::mesh::gpu::MeshBatchManager": MeshBatchManager,
  "sculptcore::spatial::SpatialTree": SpatialTree,
};
