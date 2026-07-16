"""Vector wrappers + zero-copy numpy views.

Python port of typescriptRuntime/boundVector.ts (+ the setValue.ts write
half). BoundVector wraps a native litestl::util::Vector: indexing reads the
data pointer on every access (resize moves it), struct elements wrap
non-owning, and scalar element types expose a zero-copy numpy view over the
engine's storage. numpy(): the view aliases native memory — it is
invalidated by any resize and by disposal of the owner.
"""

from __future__ import annotations

from . import _capi
from ._capi import PTRSIZE
from . import _descriptors as d
from . import _marshal

_NUMPY_DTYPES = {
    (d.NumberSubtype.Int8, False): "int8",
    (d.NumberSubtype.Int8, True): "uint8",
    (d.NumberSubtype.Int16, False): "int16",
    (d.NumberSubtype.Int16, True): "uint16",
    (d.NumberSubtype.Int32, False): "int32",
    (d.NumberSubtype.Int32, True): "uint32",
    (d.NumberSubtype.Int64, False): "int64",
    (d.NumberSubtype.Int64, True): "uint64",
    (d.NumberSubtype.Float32, False): "float32",
    (d.NumberSubtype.Float32, True): "float32",
    (d.NumberSubtype.Float64, False): "float64",
    (d.NumberSubtype.Float64, True): "float64",
}


def _resolve(btype: d.BindingBase) -> d.BindingBase:
    while isinstance(btype, d.ParentTemplateParamType):
        btype = btype.concrete_type
    return btype


class BoundVector:
    """A litestl::util::Vector<T> at a native address.

    Non-owning by default (the engine owns most vectors it hands out);
    owning instances (construct_from_items, by-value returns) destruct +
    free on dispose().
    """

    __slots__ = ("manager", "ptr", "vec_type", "elem_type", "elem_size",
                 "owning", "_disposed")

    def __init__(self, manager, ptr: int, vec_type: d.StructType,
                 owning: bool = False):
        self.manager = manager
        self.ptr = ptr
        self.vec_type = vec_type
        self.elem_type = _resolve(vec_type.template_params[0][1])
        self.elem_size = self.elem_type.size
        self.owning = owning
        self._disposed = False

    # Vector layout: {T *data_; size_t size_; size_t capacity_; inline...}.
    def _data(self) -> int:
        return _capi.read_ptr(self.ptr)

    def __len__(self) -> int:
        return _capi.read_size_t(self.ptr + PTRSIZE)

    def _elem_addr(self, i: int) -> int:
        n = len(self)
        if i < 0:
            i += n
        if not 0 <= i < n:
            raise IndexError(i)
        return self._data() + i * self.elem_size

    def __getitem__(self, i: int):
        addr = self._elem_addr(i)
        if isinstance(self.elem_type, d.StructType):
            return self.manager.get_bound_pointer(self.elem_type, addr, deref=False)
        return self.manager.get_bound_pointer(self.elem_type, addr)

    def __setitem__(self, i: int, value) -> None:
        self._set(self._elem_addr(i), value, destruct=True)

    def set_uninitialized(self, i: int, value) -> None:
        """Assign into an element that was never constructed (fresh resize) —
        struct elements are copy-constructed without a prior destructor run."""
        self._set(self._elem_addr(i), value, destruct=False)

    def _set(self, addr: int, value, *, destruct: bool) -> None:
        etype = self.elem_type
        if isinstance(etype, d.StructType):
            ctor = etype.find_copy_constructor()
            if ctor is None:
                raise _marshal.InvokeError(
                    f"no copy constructor for {etype.full_name()}")
            if destruct:
                self.manager.capi.lib.LSTL_Destructor_Invoke(etype.ptr, addr)
            _marshal.construct_to(self.manager, ctor, addr,
                                  [_marshal._value_addr(value)])
        else:
            self.manager.set_scalar(etype, addr, value)

    def __iter__(self):
        for i in range(len(self)):
            yield self[i]

    def numpy(self):
        """Zero-copy numpy view over the vector's current storage."""
        import ctypes

        import numpy as np

        etype = self.elem_type
        if isinstance(etype, d.BooleanType):
            dtype = "uint8"
        elif isinstance(etype, d.NumberType):
            dtype = _NUMPY_DTYPES[(etype.subtype, etype.unsigned)]
        else:
            raise TypeError(
                f"no numpy view for element type {etype.full_name()}")
        n = len(self)
        buf = (ctypes.c_uint8 * (n * self.elem_size)).from_address(self._data())
        return np.frombuffer(buf, dtype=dtype)

    def resize(self, n: int) -> None:
        for m in self.vec_type.methods:
            if m.name == "resize":
                _marshal.invoke_method(self.manager, m, self.ptr, (n,))
                return
        raise _marshal.InvokeError(f"{self.vec_type.name} has no resize method")

    def dispose(self) -> None:
        if self._disposed:
            raise RuntimeError("BoundVector disposed twice")
        self._disposed = True
        if self.owning:
            self.manager.destroy_instance(self.vec_type, self.ptr)
        self.ptr = 0

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        if not self._disposed:
            self.dispose()
        return False

    def __repr__(self):
        state = " disposed" if self._disposed else f" len={len(self)}"
        return f"<BoundVector[{self.elem_type.name}] @0x{self.ptr:x}{state}>"


def construct_from_items(manager, elem_type: d.BindingBase, items,
                         static_size: int | None = None) -> BoundVector:
    """Build a fresh owning Vector<elem_type> holding `items` (the TS
    runtime's BoundVector.constructFromItems — used to pass Python lists as
    Vector arguments)."""
    vec_type = manager.find_vector_class(elem_type.full_name(), static_size)
    if vec_type is None:
        raise _marshal.InvokeError(
            f"missing litestl::util::Vector binding for {elem_type.full_name()}")

    ctor = vec_type.find_default_constructor()
    if ctor is None:
        raise _marshal.ConstructError(f"no default constructor for {vec_type.name}")
    ptr = _marshal.construct(manager, ctor, ())
    vec = BoundVector(manager, ptr, vec_type, owning=True)
    vec.resize(len(items))
    for i, item in enumerate(items):
        vec.set_uninitialized(i, item)
    return vec
