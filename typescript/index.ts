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

import type {BuiltinAttr} from "./sculptcore/mesh/BuiltinAttr";
import type {VertexData} from "./sculptcore/mesh/VertexData";
import type {Buffer} from "./sculptcore/gpu/Buffer";
import type {AttrGroup} from "./sculptcore/mesh/AttrGroup";
import type {EdgeData} from "./sculptcore/mesh/EdgeData";
import type {GPUManager} from "./sculptcore/gpu/GPUManager";
import type {int2} from "./litestl/math/int2";
import type {int4} from "./litestl/math/int4";
import type {Mesh} from "./sculptcore/mesh/Mesh";
import type {AttrRef} from "./sculptcore/mesh/AttrRef";
import type {float3} from "./litestl/math/float3";

export type {BuiltinAttr} from "./sculptcore/mesh/BuiltinAttr";
export type {VertexData} from "./sculptcore/mesh/VertexData";
export type {Buffer} from "./sculptcore/gpu/Buffer";
export type {AttrGroup} from "./sculptcore/mesh/AttrGroup";
export type {EdgeData} from "./sculptcore/mesh/EdgeData";
export type {GPUManager} from "./sculptcore/gpu/GPUManager";
export type {int2} from "./litestl/math/int2";
export type {int4} from "./litestl/math/int4";
export type {Mesh} from "./sculptcore/mesh/Mesh";
export type {AttrRef} from "./sculptcore/mesh/AttrRef";
export type {float3} from "./litestl/math/float3";

/** Note: Does not include templates */
export type AllBoundTypes = {
  "litestl::math::float3": float3,
  "sculptcore::mesh::Mesh": Mesh,
  "sculptcore::mesh::VertexData": VertexData,
  "sculptcore::mesh::EdgeData": EdgeData,
  "sculptcore::mesh::AttrGroup": AttrGroup,
  "litestl::util::Vector": AttrRef[],
  "litestl::math::int4": int4,
  "sculptcore::mesh::BuiltinAttr<float3,\"positions\">": BuiltinAttr<float3,"positions">,
  "sculptcore::mesh::BuiltinAttr<float3,\"normals\">": BuiltinAttr<float3,"normals">,
  "sculptcore::mesh::BuiltinAttr<int,\".vert.e\">": BuiltinAttr<int,".vert.e">,
  "sculptcore::gpu::Buffer": Buffer,
  "litestl::math::int2": int2,
  "sculptcore::gpu::GPUManager": GPUManager,
  "sculptcore::mesh::AttrRef": AttrRef,
  "sculptcore::mesh::BuiltinAttr<int,\".edge.c\">": BuiltinAttr<int,".edge.c">,
  "sculptcore::mesh::BuiltinAttr<int2,\".edge.vs\">": BuiltinAttr<int2,".edge.vs">,
  "sculptcore::mesh::BuiltinAttr<boolean,\"select\">": BuiltinAttr<boolean,"select">,
  "sculptcore::mesh::BuiltinAttr<int4,\".edge.vs.disk\">": BuiltinAttr<int4,".edge.vs.disk">,
};
