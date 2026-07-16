"""Blender-layout bulk conversion c-api (Mesh_fromArrays / Mesh_toArrays) —
the entry points the addon's enter/flush/exit conversion uses (P3)."""

import ctypes

import numpy as np
import pytest

import sculptcore


@pytest.fixture(scope="session")
def mgr():
    return sculptcore.init()


@pytest.fixture(scope="session")
def capi(mgr):
    lib = mgr.capi.lib
    f32p = np.ctypeslib.ndpointer(dtype=np.float32, flags="C_CONTIGUOUS")
    i32p = np.ctypeslib.ndpointer(dtype=np.int32, flags="C_CONTIGUOUS")
    lib.Mesh_fromArrays.argtypes = [f32p, ctypes.c_int, i32p, ctypes.c_int, i32p, ctypes.c_int]
    lib.Mesh_fromArrays.restype = ctypes.c_void_p
    lib.Mesh_arraySizes.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_int)] * 4
    lib.Mesh_arraySizes.restype = None
    lib.Mesh_toArrays.argtypes = [ctypes.c_void_p, f32p, i32p, i32p, i32p]
    lib.Mesh_toArrays.restype = ctypes.c_int
    lib.freeMesh.argtypes = [ctypes.c_void_p]
    lib.freeMesh.restype = None
    return lib


def test_round_trip(capi):
    positions = np.array(
        [[0, 0, 0], [1, 0, 0], [2, 0, 0], [0, 1, 0], [1, 1, 0], [9, 9, 9]],
        dtype=np.float32,
    )
    corner_verts = np.array([0, 1, 4, 3, 1, 2, 4], dtype=np.int32)  # quad + tri
    face_offsets = np.array([0, 4, 7], dtype=np.int32)

    mesh = capi.Mesh_fromArrays(
        positions.ravel(), 6, corner_verts, 7, face_offsets, 2
    )
    assert mesh

    nv, nc, nf, cap = (ctypes.c_int(0) for _ in range(4))
    capi.Mesh_arraySizes(
        mesh, ctypes.byref(nv), ctypes.byref(nc), ctypes.byref(nf), ctypes.byref(cap)
    )
    assert (nv.value, nc.value, nf.value) == (6, 7, 2)

    out_pos = np.zeros(nv.value * 3, dtype=np.float32)
    out_corners = np.zeros(nc.value, dtype=np.int32)
    out_offsets = np.zeros(nf.value + 1, dtype=np.int32)
    vert_map = np.zeros(cap.value, dtype=np.int32)
    remapped = capi.Mesh_toArrays(mesh, out_pos, out_corners, out_offsets, vert_map)

    assert remapped == 0
    assert np.array_equal(out_pos, positions.ravel())
    assert np.array_equal(out_corners, corner_verts)
    assert np.array_equal(out_offsets, np.array([0, 4, 7], dtype=np.int32))
    # The map spans the whole (page-rounded) vert index space; live slots are
    # identity here, dead tail slots are ELEM_NONE (-1).
    assert np.array_equal(vert_map[:6], np.arange(6, dtype=np.int32))
    assert np.all(vert_map[6:] == -1)

    capi.freeMesh(mesh)
