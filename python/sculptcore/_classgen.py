"""Dynamic bound classes + the binding manager.

Python port of typescriptRuntime/bind.ts + manager.ts: one Python class per
reflected struct, with a property per member (typed reads/writes at
this.ptr + offset) and a method per reflected method (dispatch through
LSTL_Method_Invoke via _marshal). Classes are built with type(), not source
generation — the generated .pyi stubs (Workstream C) provide the static view.
"""

from __future__ import annotations

import re

from . import _capi
from ._capi import Capi, PTRSIZE
from . import _descriptors as d
from . import _marshal


class UnknownTypeError(KeyError):
    pass


class NotStructError(TypeError):
    pass


class UnknownConstructorError(LookupError):
    pass


class BoundObject:
    """A wrapped native struct instance.

    `owning` wrappers destruct + free the native object on dispose(); use them
    as context managers for deterministic teardown. Disposal is explicit —
    there is no GC finalizer, since the engine may be torn down first.
    """

    __slots__ = ("ptr", "manager", "owning", "_disposed")

    # Set per generated subclass.
    bind_type: d.StructType = None  # type: ignore[assignment]

    def __init__(self, manager: "Manager", ptr: int, owning: bool = False):
        self.ptr = ptr
        self.manager = manager
        self.owning = owning
        self._disposed = False

    def dispose(self) -> None:
        if self._disposed:
            raise RuntimeError(f"{type(self).__name__} instance disposed twice")
        self._disposed = True
        if self.owning:
            self.manager.destroy_instance(self.bind_type, self.ptr)
        self.ptr = 0

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        if not self._disposed:
            self.dispose()
        return False

    def __repr__(self):
        state = " disposed" if self._disposed else ""
        return f"<{type(self).__name__} @0x{self.ptr:x}{state}>"


class BoundArray:
    """A fixed-size inline array member (e.g. `float vec[3]`)."""

    __slots__ = ("manager", "addr", "elem_type", "length", "elem_size")

    def __init__(self, manager: "Manager", addr: int, atype: d.ArrayType):
        self.manager = manager
        self.addr = addr
        self.elem_type = atype.elem_type
        self.length = atype.array_size
        self.elem_size = atype.elem_type.size if atype.elem_type else 0

    def _elem_addr(self, i: int) -> int:
        if not 0 <= i < self.length:
            raise IndexError(i)
        return self.addr + i * self.elem_size

    def __len__(self):
        return self.length

    def __getitem__(self, i: int):
        return self.manager.get_bound_pointer(self.elem_type, self._elem_addr(i))

    def __setitem__(self, i: int, value):
        self.manager.set_scalar(self.elem_type, self._elem_addr(i), value)

    def __iter__(self):
        for i in range(self.length):
            yield self[i]

    def __repr__(self):
        return f"<BoundArray[{self.length}] @0x{self.addr:x}>"


def _make_member_property(mtype: d.BindingBase, offset: int):
    """A property reading/writing a struct member at self.ptr + offset,
    mirroring createBoundCode in bind.ts."""
    if mtype.type == d.BindingType.ParentTemplateParam:
        return _make_member_property(mtype.concrete_type, offset)

    if mtype.type == d.BindingType.Boolean:
        def get_bool(self):
            return _capi.read_u8(self.ptr + offset) != 0

        def set_bool(self, value):
            _capi.write_u8(self.ptr + offset, 1 if value else 0)

        return property(get_bool, set_bool)

    if mtype.type == d.BindingType.Number:
        def get_num(self):
            return d.read_number(mtype, self.ptr + offset)

        def set_num(self, value):
            d.write_number(mtype, self.ptr + offset, value)

        return property(get_num, set_num)

    if mtype.type == d.BindingType.Enum:
        def get_enum(self):
            return _marshal.read_enum(mtype, self.ptr + offset)

        def set_enum(self, value):
            _marshal.write_enum(mtype, self.ptr + offset, value)

        return property(get_enum, set_enum)

    if mtype.type == d.BindingType.Struct:
        def get_struct(self):
            return self.manager.get_bound_pointer(mtype, self.ptr + offset)

        def set_struct(self, value):
            raise AttributeError("setting embedded struct members is not supported")

        return property(get_struct, set_struct)

    if mtype.type == d.BindingType.Array:
        def get_array(self):
            return BoundArray(self.manager, self.ptr + offset, mtype)

        return property(get_array)

    if mtype.type in (d.BindingType.Pointer, d.BindingType.Reference):
        inner = mtype.ptr_type
        if inner is not None and inner.type == d.BindingType.Struct:
            def get_ptr(self):
                addr = _capi.read_ptr(self.ptr + offset)
                if not addr:
                    return None
                return self.manager.get_bound_pointer(inner, addr, deref=False)

            def set_ptr(self, value):
                _capi.write_ptr(self.ptr + offset, _marshal._value_addr(value))

            return property(get_ptr, set_ptr)

        def get_deref(self):
            addr = _capi.read_ptr(self.ptr + offset)
            if not addr:
                return None
            return self.manager.get_bound_pointer(inner, addr, deref=False)

        return property(get_deref)

    return None


def _make_method(manager: "Manager", method: d.MethodType):
    def call(self, *args):
        return _marshal.invoke_method(self.manager, method, self.ptr, args)

    call.__name__ = method.name
    return call


def _class_name(struct_name: str) -> str:
    return re.sub(r"\W+", "_", struct_name).strip("_")


class Manager:
    """Python-side view of the engine's BindingManager singleton."""

    def __init__(self, capi: Capi):
        self.capi = capi
        self.ptr = capi.lib.getBindingManager()
        self.types: dict[str, d.BindingBase] = {}
        self.bound_classes: dict[str, type] = {}
        # element-type full name -> Vector struct descriptors (per static size).
        self.type_vec_map: dict[str, list[d.StructType]] = {}
        self._load()

    def _load(self) -> None:
        lib = self.capi.lib
        keys_ptr = lib.LSTL_Binding_GetKeys(self.ptr)
        keys = _capi.read_cstring(keys_ptr)
        lib.LSTL_Binding_FreeKeys(keys_ptr)

        for key in filter(None, keys.split("\\")):
            desc_ptr = lib.LSTL_Binding_Get(self.ptr, key.encode("utf-8"))
            if not desc_ptr:
                raise UnknownTypeError(f"binding {key!r} not found")
            self.types[key] = d.get_binding(self.capi, desc_ptr)

        for key, btype in self.types.items():
            if key.startswith("litestl::util::Vector<") and isinstance(btype, d.StructType):
                elem_key = btype.template_params[0][1].full_name()
                self.type_vec_map.setdefault(elem_key, []).append(btype)

    # --- lookup -------------------------------------------------------------

    def get(self, type_name: str) -> d.BindingBase:
        try:
            return self.types[type_name]
        except KeyError:
            raise UnknownTypeError(f"unknown type {type_name!r}") from None

    def get_struct(self, type_name: str) -> d.StructType:
        btype = self.get(type_name)
        if not isinstance(btype, d.StructType):
            raise NotStructError(f"type {type_name!r} is not a struct")
        return btype

    def find_vector_class(self, elem_full_name: str,
                          static_size: int | None = None) -> d.StructType | None:
        """The Vector<elem, static_size> struct descriptor, if registered."""
        if static_size is None:
            static_size = self.capi.info.Sizes.VectorDefaultStaticSize
        for st in self.type_vec_map.get(elem_full_name, ()):
            size_lit = st.template_params[1][1]
            if isinstance(size_lit, d.NumLitType) and size_lit.data == static_size:
                return st
        return None

    # --- class generation -----------------------------------------------------

    def get_bound_class(self, struct: d.StructType) -> type:
        cls = self.bound_classes.get(struct.name)
        if cls is not None:
            return cls

        ns: dict[str, object] = {"bind_type": struct, "__slots__": ()}
        # Methods first, then member properties — later names win, matching
        # the TS class-body order in bind.ts. Overloads: last registration
        # wins (TODO: signature dispatch once .pyi overloads exist).
        for method in struct.methods:
            ns[method.name] = _make_method(self, method)
        for name, mtype, offset in struct.members:
            prop = _make_member_property(mtype, offset)
            if prop is not None:
                ns[name] = prop

        cls = type(_class_name(struct.name), (BoundObject,), ns)
        self.bound_classes[struct.name] = cls
        return cls

    # --- value <-> native ------------------------------------------------------

    def get_bound_pointer(self, btype: d.BindingBase, addr: int, *,
                          deref: bool = True, owning: bool = False):
        """Wrap the value of descriptor `btype` living at `addr` (manager.ts
        getBoundPointer). With deref=False, `addr` is already the object
        address for struct wrapping rather than the address of a pointer."""
        if btype.type == d.BindingType.ParentTemplateParam:
            btype = btype.concrete_type

        if btype.type == d.BindingType.Number:
            return d.read_number(btype, addr)
        if btype.type == d.BindingType.Boolean:
            return _capi.read_u8(addr) != 0
        if btype.type == d.BindingType.Enum:
            return _marshal.read_enum(btype, addr)
        if btype.type == d.BindingType.Array:
            return BoundArray(self, addr, btype)
        if btype.type == d.BindingType.Struct:
            if btype.is_vector:
                from . import _bulk

                return _bulk.BoundVector(self, addr, btype, owning=owning)
            cls = self.get_bound_class(btype)
            return cls(self, addr, owning=owning)
        if btype.type in (d.BindingType.Pointer, d.BindingType.Reference):
            indirect = _capi.read_ptr(addr) if deref else addr
            if not indirect:
                return None
            inner = btype.ptr_type
            if inner is None:
                return indirect
            return self.get_bound_pointer(inner, indirect, deref=False)
        raise UnknownTypeError(f"cannot wrap binding type {btype.type!r}")

    def set_scalar(self, btype: d.BindingBase, addr: int, value) -> None:
        if btype.type == d.BindingType.Number:
            d.write_number(btype, addr, value)
        elif btype.type == d.BindingType.Boolean:
            _capi.write_u8(addr, 1 if value else 0)
        elif btype.type == d.BindingType.Enum:
            _marshal.write_enum(btype, addr, value)
        elif btype.type in (d.BindingType.Pointer, d.BindingType.Reference):
            _capi.write_ptr(addr, _marshal._value_addr(value))
        else:
            raise UnknownTypeError(f"cannot assign binding type {btype.type!r}")

    def unpack_return(self, ret_type: d.BindingBase, ret_buf: int):
        """Unpack a thunk return buffer. Struct-by-value returns keep the
        buffer and own it; everything else is read out and the buffer freed."""
        if ret_type.type == d.BindingType.ParentTemplateParam:
            ret_type = ret_type.concrete_type
        if ret_type.type == d.BindingType.Struct:
            return self.get_bound_pointer(ret_type, ret_buf, deref=False, owning=True)
        try:
            return self.get_bound_pointer(ret_type, ret_buf)
        finally:
            self.capi.mem_release(ret_buf)

    # --- construction -----------------------------------------------------------

    def construct(self, type_name: str, *args):
        struct = self.get_struct(type_name)
        if args:
            for ctor in struct.constructors:
                if len(ctor.params) == len(args) and ctor.name != "copy":
                    return self.construct_with(ctor, *args)
            raise UnknownConstructorError(
                f"no {len(args)}-argument constructor for {type_name!r}"
            )
        ctor = struct.find_default_constructor()
        if ctor is None:
            raise UnknownConstructorError(f"no default constructor for {type_name!r}")
        return self.construct_with(ctor)

    def construct_with(self, ctor: d.ConstructorType, *args):
        cls = self.get_bound_class(ctor.owner)
        ptr = _marshal.construct(self, ctor, args)
        return cls(self, ptr, owning=True)

    def destroy_instance(self, struct: d.StructType, ptr: int) -> None:
        self.capi.lib.LSTL_Destructor_Invoke(struct.ptr, ptr)
        self.capi.mem_release(ptr)

    # --- introspection -----------------------------------------------------------

    def mem_size(self, include_permanent: bool = False) -> int:
        return self.capi.lib.LSTL_GetMemSize(include_permanent)
