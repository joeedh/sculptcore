"""Vector wrappers + zero-copy numpy views (_bulk)."""

import pytest

import sculptcore


@pytest.fixture(scope="session")
def mgr():
    return sculptcore.init()


def test_construct_from_items(mgr):
    elem = mgr.get("float")
    with sculptcore.construct_from_items(mgr, elem, [1.0, 2.5, -3.0]) as vec:
        assert len(vec) == 3
        assert list(vec) == [1.0, 2.5, -3.0]
        vec[1] = 7.0
        assert vec[1] == 7.0


def test_numpy_view_is_zero_copy(mgr):
    elem = mgr.get("int32")
    with sculptcore.construct_from_items(mgr, elem, list(range(16))) as vec:
        view = vec.numpy()
        assert view.dtype.name == "int32"
        assert list(view) == list(range(16))
        # Same storage, not a copy: the numpy buffer sits at the engine's
        # data pointer, and writes through either side are visible in both.
        from sculptcore import _capi

        assert view.ctypes.data == _capi.read_ptr(vec.ptr)
        view[3] = 999
        assert vec[3] == 999
        vec[4] = -5
        assert view[4] == -5


def test_vector_leaks(mgr):
    base = mgr.mem_size()
    for _ in range(16):
        with sculptcore.construct_from_items(mgr, mgr.get("float"), [0.0] * 100):
            pass
    assert mgr.mem_size() == base
