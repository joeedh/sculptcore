"""Argument/return marshalling for the LSTL_* invoke thunks.

Python port of the buildArgs/invoke halves of typescriptRuntime/binding.ts.
The thunk ABI is void(*)(void *self, void **args, void *ret): every argument
gets an 8-byte-min slot buffer; scalars are written into the buffer, pointers
point at the pointee, by-value structs are copy-constructed into the buffer,
and references place the referee's address directly in the args array.

Improvements over the TS original (behavior-compatible additions): enum
arguments are written by baseSize (TS leaves them zeroed), and scalar/pointer
return buffers are freed after unpacking (TS leaks them).
"""

from __future__ import annotations

from . import _capi
from ._capi import PTRSIZE
from . import _descriptors as d


class InvokeError(RuntimeError):
    pass


class ConstructError(RuntimeError):
    pass


def _value_addr(value) -> int:
    """The native address of a bound object / raw int address / None."""
    if value is None:
        return 0
    ptr = getattr(value, "ptr", value)
    if not isinstance(ptr, int):
        raise InvokeError(f"cannot pass {value!r} as a native address")
    return ptr


_ENUM_WRITERS = {1: _capi.write_u8, 2: _capi.write_u16, 4: _capi.write_u32, 8: _capi.write_u64}
_ENUM_READERS = {1: _capi.read_u8, 2: _capi.read_u16, 4: _capi.read_u32, 8: _capi.read_u64}


def _list_as_vector(manager, ptype, value):
    """A Python list/tuple passed where Vector<T>*/& is expected becomes a
    temporary owning vector (disposed after the call), or None if `ptype`
    isn't a vector pointer/reference."""
    inner = getattr(ptype, "ptr_type", None)
    if inner is None or not (
        inner.type == d.BindingType.Struct and inner.is_vector
    ):
        return None
    from . import _bulk

    elem_type = inner.template_params[0][1]
    static_size = inner.template_params[1][1]
    return _bulk.construct_from_items(
        manager, elem_type, value,
        static_size.data if isinstance(static_size, d.NumLitType) else None,
    )


def build_args(manager, arg_types, args):
    """Pack `args` for the thunk ABI.

    Returns (ptr_list, owned, temps): `owned` lists every allocation to
    release after the call (per-arg slot buffers, then ptr_list itself);
    `temps` holds temporary vectors built from Python lists, disposed after
    the call.
    """
    capi = manager.capi
    n = len(arg_types)
    ptr_list = capi.mem_alloc("thunk args", PTRSIZE * max(n, 1))
    owned = []
    temps = []

    def addr_of(atype, value):
        if isinstance(value, (list, tuple)):
            vec = _list_as_vector(manager, atype, value)
            if vec is not None:
                temps.append(vec)
                return vec.ptr
        return _value_addr(value)

    for index, ((name, atype), value) in enumerate(zip(arg_types, args)):
        if atype.type == d.BindingType.ParentTemplateParam:
            atype = atype.concrete_type

        arg_ptr = capi.mem_alloc("thunk arg", max(8, atype.size))
        owned.append(arg_ptr)
        _capi.write_u64(arg_ptr, 0)
        _capi.write_ptr(ptr_list + index * PTRSIZE, arg_ptr)

        if atype.type == d.BindingType.Struct:
            ctor = atype.find_copy_constructor()
            if ctor is None:
                raise InvokeError(f"type {atype.full_name()} has no copy constructor")
            construct_to(manager, ctor, arg_ptr, [_value_addr(value)])
        elif atype.type == d.BindingType.Boolean:
            _capi.write_u8(arg_ptr, 1 if value else 0)
        elif atype.type == d.BindingType.Number:
            d.write_number(atype, arg_ptr, value)
        elif atype.type == d.BindingType.Enum:
            _ENUM_WRITERS[atype.base_size](arg_ptr, int(value))
        elif atype.type in (d.BindingType.Array, d.BindingType.Pointer):
            _capi.write_ptr(arg_ptr, addr_of(atype, value))
        elif atype.type == d.BindingType.Reference:
            # The thunks take reference parameters directly in the args
            # array (C++ can't form a pointer-to-reference); see the TS
            # runtime's buildArgs and invokeImpl in binding_method.h.
            _capi.write_ptr(ptr_list + index * PTRSIZE, addr_of(atype, value))
        else:
            raise InvokeError(f"cannot marshal argument {name!r} of type {atype.type!r}")

    owned.append(ptr_list)
    return ptr_list, owned, temps


def invoke_method(manager, method: d.MethodType, self_ptr: int, args):
    if len(args) != len(method.params):
        raise InvokeError(
            f"{method.name}: expected {len(method.params)} arguments, got {len(args)}"
        )
    capi = manager.capi
    ptr_list, owned, temps = build_args(manager, method.params, args)

    ret_type = method.return_type
    ret_buf = 0
    if ret_type is not None:
        ret_buf = capi.mem_alloc("thunk return", max(8, ret_type.size))

    capi.lib.LSTL_Method_Invoke(method.ptr, self_ptr, ptr_list, ret_buf)

    for vec in temps:
        vec.dispose()
    for ptr in owned:
        capi.mem_release(ptr)

    if ret_type is None:
        return None
    return manager.unpack_return(ret_type, ret_buf)


def construct_to(manager, ctor: d.ConstructorType, this_ptr: int, args) -> int:
    if len(args) != len(ctor.params):
        raise ConstructError(
            f"{ctor.owner.name}::{ctor.name}: expected {len(ctor.params)} arguments, "
            f"got {len(args)}"
        )
    capi = manager.capi
    ptr_list, owned, temps = build_args(manager, ctor.params, args)
    capi.lib.LSTL_Constructor_Invoke(ctor.ptr, this_ptr, ptr_list)
    for vec in temps:
        vec.dispose()
    for ptr in owned:
        capi.mem_release(ptr)
    return this_ptr


def construct(manager, ctor: d.ConstructorType, args) -> int:
    """Allocate storage for ctor's owner struct and construct into it."""
    capi = manager.capi
    ptr = capi.mem_alloc(ctor.owner.name, ctor.owner.struct_size)
    construct_to(manager, ctor, ptr, args)
    return ptr


def read_enum(etype: d.EnumType, addr: int) -> int:
    return _ENUM_READERS[etype.base_size](addr)


def write_enum(etype: d.EnumType, addr: int, value: int) -> None:
    _ENUM_WRITERS[etype.base_size](addr, int(value))
