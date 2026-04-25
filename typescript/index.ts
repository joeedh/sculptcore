/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {BuiltinAttr} from "./sculptcore/mesh/BuiltinAttr";
import type {VertexData} from "./sculptcore/mesh/VertexData";
import type {EdgeData} from "./sculptcore/mesh/EdgeData";
import type {Vector} from "./litestl/util/Vector";
import type {AttrGroup} from "./sculptcore/mesh/AttrGroup";
import type {int4} from "./int4";
import type {int2} from "./int2";
import type {Mesh} from "./sculptcore/mesh/Mesh";
import type {AttrRef} from "./sculptcore/mesh/AttrRef";
import type {String} from "./litestl/util/String";
import type {float3} from "./float3";

export type {Mesh} from "./sculptcore/mesh/Mesh";
export type {BuiltinAttr} from "./sculptcore/mesh/BuiltinAttr";
export type {int4} from "./int4";
export type {VertexData} from "./sculptcore/mesh/VertexData";
export type {AttrRef} from "./sculptcore/mesh/AttrRef";
export type {String} from "./litestl/util/String";
export type {EdgeData} from "./sculptcore/mesh/EdgeData";
export type {AttrGroup} from "./sculptcore/mesh/AttrGroup";
export type {float3} from "./float3";
export type {int2} from "./int2";
export type {Vector} from "./litestl/util/Vector";

/** Note: Does not include templates */
export type AllBoundTypes = {
  "sculptcore::mesh::Mesh": Mesh,
  "sculptcore::mesh::VertexData": VertexData,
  "sculptcore::mesh::EdgeData": EdgeData,
  "sculptcore::mesh::AttrRef": AttrRef,
  "litestl::util::String": String,
  "sculptcore::mesh::AttrGroup": AttrGroup,
  "litestl::util::Vector<AttrRef,4>": Vector<AttrRef,4>,
  "int4": int4,
  "int2": int2,
  "sculptcore::mesh::BuiltinAttr<float3,\"positions\">": BuiltinAttr<float3,"positions">,
  "sculptcore::mesh::BuiltinAttr<float3,\"normals\">": BuiltinAttr<float3,"normals">,
  "sculptcore::mesh::BuiltinAttr<number,\".vert.e\">": BuiltinAttr<number,".vert.e">,
  "sculptcore::mesh::BuiltinAttr<number,\".edge.c\">": BuiltinAttr<number,".edge.c">,
  "sculptcore::mesh::BuiltinAttr<int2,\".edge.vs\">": BuiltinAttr<int2,".edge.vs">,
  "sculptcore::mesh::BuiltinAttr<boolean,\"select\">": BuiltinAttr<boolean,"select">,
  "sculptcore::mesh::BuiltinAttr<int4,\".edge.vs.disk\">": BuiltinAttr<int4,".edge.vs.disk">,
  "float3": float3,
};
