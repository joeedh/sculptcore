"""ABI smoke — drives the engine through pure ctypes; no C module built.

Python analogue of source/napi/napi_smoke.cjs. The sculpt-dab half of that
smoke lands with the bulk/vector slice (Workstream B complete).
"""

import pytest

import sculptcore
from sculptcore import _descriptors as d


@pytest.fixture(scope="session")
def mgr():
    return sculptcore.init()


def test_abi_and_enumeration(mgr):
    assert len(mgr.types) >= 100
    for key in (
        "sculptcore::mesh::Mesh",
        "sculptcore::brush::Brush",
        "litestl::math::float3",
    ):
        assert isinstance(mgr.get(key), d.StructType)


def test_array_member_get_set(mgr):
    with mgr.construct("litestl::math::float3") as f3:
        f3.vec[0] = 1.5
        f3.vec[1] = 2.0
        f3.vec[2] = -3.0
        assert list(f3.vec) == [1.5, 2.0, -3.0]
        with pytest.raises(IndexError):
            f3.vec[3]


def test_scalar_member_get_set(mgr):
    with mgr.construct("sculptcore::brush::Brush") as brush:
        brush.strength = 0.75
        assert brush.strength == pytest.approx(0.75)
        brush.radius = 55.0
        assert brush.radius == pytest.approx(55.0)


def test_method_invoke(mgr):
    with mgr.construct("sculptcore::mesh::Mesh") as mesh:
        assert mesh.ngonFaceCount() == 0
        assert mesh.repairMesh() == 0


def test_dispose_guards(mgr):
    obj = mgr.construct("litestl::math::float3")
    obj.dispose()
    with pytest.raises(RuntimeError):
        obj.dispose()


def test_no_leaks(mgr):
    base = mgr.mem_size()
    for _ in range(32):
        with mgr.construct("sculptcore::mesh::Mesh") as mesh:
            mesh.ngonFaceCount()
    assert mgr.mem_size() == base
