"""End-to-end sculpt dab through pure ctypes (the napi_smoke stroke scenario).

Builds a cube + spatial tree via the Mesh_* c-api seams, then drives a DRAW
dab through the reflected CommandExecutor. Geometry change is verified by
diffing serializeMeshRaw snapshots.
"""

import ctypes

import pytest

import sculptcore
from sculptcore import BoundVector, _capi


@pytest.fixture(scope="session")
def mgr():
    return sculptcore.init()


def _declare_capi(lib):
    lib.Mesh_createCube.argtypes = [ctypes.c_int, ctypes.c_float, ctypes.c_float]
    lib.Mesh_createCube.restype = ctypes.c_void_p
    lib.Mesh_buildSpatialTree.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 3
    lib.Mesh_buildSpatialTree.restype = ctypes.c_void_p
    lib.serializeMeshRaw.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.serializeMeshRaw.restype = ctypes.c_void_p
    lib.freeMeshBuffer.argtypes = [ctypes.c_void_p]
    lib.freeMeshBuffer.restype = None
    lib.freeMesh.argtypes = [ctypes.c_void_p]
    lib.freeMesh.restype = None
    lib.SpatialTree_free.argtypes = [ctypes.c_void_p]
    lib.SpatialTree_free.restype = None


def _snapshot(lib, mesh_ptr) -> bytes:
    size = ctypes.c_int(0)
    buf = lib.serializeMeshRaw(mesh_ptr, ctypes.addressof(size))
    assert buf and size.value > 0
    data = _capi.read_bytes(buf, size.value)
    lib.freeMeshBuffer(buf)
    return data


def _float3(mgr, x, y, z):
    v = mgr.construct("litestl::math::float3")
    v.vec[0] = x
    v.vec[1] = y
    v.vec[2] = z
    return v


def test_sculpt_dab(mgr):
    lib = mgr.capi.lib
    _declare_capi(lib)

    mesh_ptr = lib.Mesh_createCube(8, 1.0, 1.0)
    assert mesh_ptr
    tree_ptr = lib.Mesh_buildSpatialTree(mesh_ptr, 0, 0, 0)
    assert tree_ptr

    mesh = mgr.get_bound_pointer(mgr.get("sculptcore::mesh::Mesh"), mesh_ptr, deref=False)
    tree = mgr.get_bound_pointer(
        mgr.get("sculptcore::spatial::SpatialTree"), tree_ptr, deref=False
    )

    brush = mgr.construct("sculptcore::brush::Brush")
    brush.strength = 1.0
    brush.radius = 2.0
    brush.invert = False
    brush.writeProps()

    ctor = mgr.get_struct("sculptcore::brush::CommandExecutor").find_constructor("main")
    assert ctor is not None
    executor = mgr.construct_with(ctor, tree, brush)

    center = _float3(mgr, 0.5, 0.5, 0.5)
    normal = _float3(mgr, 0.577, 0.577, 0.577)
    nodes = mgr.construct("litestl::util::Vector<sculptcore::spatial::SpatialNode*,4>")

    assert tree.filterNodes(center, 2.0, nodes)
    node_count = len(BoundVector(mgr, nodes.ptr, nodes.bind_type))
    assert node_count > 0

    before = _snapshot(lib, mesh_ptr)
    executor.execBrush(mesh, 0, nodes, center, normal)  # 0 == SculptBrushes.DRAW
    mesh.recalc_normals()
    after = _snapshot(lib, mesh_ptr)

    assert before != after, "DRAW dab did not change the serialized geometry"

    for obj in (executor, brush, center, normal, nodes):
        obj.dispose()
    lib.SpatialTree_free(tree_ptr)
    lib.freeMesh(mesh_ptr)
