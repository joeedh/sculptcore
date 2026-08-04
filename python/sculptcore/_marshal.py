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

import ctypes

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
        isinstance(inner, d.StructType) and inner.is_vector
    ):
        return None
    from . import _bulk

    elem_type = inner.template_params[0][1]
    static_size = inner.template_params[1][1]
    return _bulk.construct_from_items(
        manager, elem_type, value,
        static_size.data if isinstance(static_size, d.NumLitType) else None,
    )


_STEP_STRUCT = 0
_STEP_BOOL = 1
_STEP_NUMBER = 2
_STEP_ENUM = 3
_STEP_PTR = 4
_STEP_REF = 5


class _CallPlan:
    """Reusable scratch and per-parameter writers for one method/constructor.

    Nothing about the thunk ABI's argument block varies between calls except
    the bytes written into it, so a plan holds a single ctypes buffer laid out
    as [ptr_list][slot 0][slot 1]...[return] and reuses it — replacing the
    two-plus native memAlloc/memRelease pairs every call used to make — along
    with the resolved writer for each parameter, so the marshalling loop is a
    dispatch on a small int rather than a chain of isinstance tests.

    Slot addresses are stamped into the args array once, here. References are
    the exception: the thunks take those directly in the array (C++ can't form
    a pointer-to-reference), so their entry is rewritten per call. Struct
    returns are the other: unpack_return hands the buffer to the caller as
    owned memory, so those still come from the native allocator.
    """

    __slots__ = ("buf", "ptr_list", "ret_buf", "ret_type", "steps")

    def __init__(self, arg_types, ret_type):
        resolved = []
        for name, atype in arg_types:
            if isinstance(atype, d.ParentTemplateParamType):
                atype = atype.concrete_type
            resolved.append((name, atype))
        if isinstance(ret_type, d.ParentTemplateParamType):
            ret_type = ret_type.concrete_type

        n = len(resolved)
        size = PTRSIZE * max(n, 1)
        offsets = []
        for _, atype in resolved:
            size = (size + 7) & ~7
            offsets.append(size)
            size += max(8, atype.size)
        own_ret = ret_type is not None and not isinstance(ret_type, d.StructType)
        ret_offset = 0
        if own_ret:
            size = (size + 7) & ~7
            ret_offset = size
            size += max(8, ret_type.size)

        self.buf = ctypes.create_string_buffer(size)
        base = ctypes.addressof(self.buf)
        self.ptr_list = base
        self.ret_type = ret_type
        self.ret_buf = base + ret_offset if own_ret else 0

        steps = []
        for index, (name, atype) in enumerate(resolved):
            slot = base + offsets[index]
            _capi.write_ptr(base + index * PTRSIZE, slot)
            if isinstance(atype, d.StructType):
                ctor = atype.find_copy_constructor()
                if ctor is None:
                    raise InvokeError(f"type {atype.full_name()} has no copy constructor")
                steps.append((_STEP_STRUCT, slot, ctor))
            elif isinstance(atype, d.BooleanType):
                steps.append((_STEP_BOOL, slot, None))
            elif isinstance(atype, d.NumberType):
                steps.append((_STEP_NUMBER, slot, d.number_writer(atype)))
            elif isinstance(atype, d.EnumType):
                steps.append((_STEP_ENUM, slot, _ENUM_WRITERS[atype.base_size]))
            elif isinstance(atype, (d.ArrayType, d.PointerType)):
                steps.append((_STEP_PTR, slot, atype))
            elif isinstance(atype, d.ReferenceType):
                steps.append((_STEP_REF, base + index * PTRSIZE, atype))
            else:
                raise InvokeError(f"cannot marshal argument {name!r} of type {atype.type!r}")
        self.steps = steps


def _acquire_plan(desc, arg_types, ret_type) -> _CallPlan:
    """A free plan for `desc`, pooled rather than singleton: marshalling a
    by-value struct argument re-enters through its copy constructor, and host
    callbacks can re-enter anything."""
    pool = getattr(desc, "_call_plans", None)
    if pool is None:
        pool = desc._call_plans = []
    if pool:
        return pool.pop()
    return _CallPlan(arg_types, ret_type)


def _fill_args(manager, plan: _CallPlan, args, temps) -> None:
    """Write `args` into the plan's slots. Slots are reused across calls, so
    every one is zeroed before a partial write."""
    for (kind, slot, extra), value in zip(plan.steps, args):
        if kind == _STEP_NUMBER:
            _capi.write_u64(slot, 0)
            extra(slot, value)
        elif kind == _STEP_PTR or kind == _STEP_REF:
            if isinstance(value, (list, tuple)):
                vec = _list_as_vector(manager, extra, value)
                if vec is not None:
                    temps.append(vec)
                    _capi.write_ptr(slot, vec.ptr)
                    continue
            _capi.write_ptr(slot, _value_addr(value))
        elif kind == _STEP_BOOL:
            _capi.write_u64(slot, 0)
            _capi.write_u8(slot, 1 if value else 0)
        elif kind == _STEP_ENUM:
            _capi.write_u64(slot, 0)
            extra(slot, int(value))
        else:
            _capi.write_u64(slot, 0)
            construct_to(manager, extra, slot, [_value_addr(value)])


def invoke_method(manager, method: d.MethodType, self_ptr: int, args):
    if len(args) != len(method.params):
        raise InvokeError(
            f"{method.name}: expected {len(method.params)} arguments, got {len(args)}"
        )
    capi = manager.capi
    plan = _acquire_plan(method, method.params, method.return_type)
    temps = []
    try:
        _fill_args(manager, plan, args, temps)
        ret_type = plan.ret_type
        ret_buf = plan.ret_buf
        plan_owns_ret = ret_buf != 0
        if ret_type is not None and not plan_owns_ret:
            ret_buf = capi.mem_alloc("thunk return", max(8, ret_type.size))
        capi.lib.LSTL_Method_Invoke(method.ptr, self_ptr, plan.ptr_list, ret_buf)
        for vec in temps:
            vec.dispose()
        temps = ()
        if ret_type is None:
            return None
        # Before the plan goes back in the pool: a nested call would reuse the
        # buffer this return value still lives in.
        return manager.unpack_return(ret_type, ret_buf, release=not plan_owns_ret)
    finally:
        for vec in temps:
            vec.dispose()
        method._call_plans.append(plan)


def construct_to(manager, ctor: d.ConstructorType, this_ptr: int, args) -> int:
    if len(args) != len(ctor.params):
        raise ConstructError(
            f"{ctor.owner.name}::{ctor.name}: expected {len(ctor.params)} arguments, "
            f"got {len(args)}"
        )
    plan = _acquire_plan(ctor, ctor.params, None)
    temps = []
    try:
        _fill_args(manager, plan, args, temps)
        manager.capi.lib.LSTL_Constructor_Invoke(ctor.ptr, this_ptr, plan.ptr_list)
    finally:
        for vec in temps:
            vec.dispose()
        ctor._call_plans.append(plan)
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
