"""Descriptor readers — Python port of typescriptRuntime/binding.ts.

Reads litestl::binding descriptor structs out of native memory at the offsets
published by LSTL_GetBindingInfo (see _capi). Field widths follow the C++
declarations (e.g. Vector::size_ is size_t, string::size_ is int32,
EnumItem::value is int32 regardless of the enum's baseSize).
"""

from __future__ import annotations

import enum
from typing import Optional

from . import _capi
from ._capi import Capi, PTRSIZE


class BindingType(enum.IntFlag):
    Boolean = 1 << 0
    Number = 1 << 1
    Pointer = 1 << 2
    Reference = 1 << 3
    Struct = 1 << 4
    Array = 1 << 5
    Method = 1 << 6
    Literal = 1 << 7
    Constructor = 1 << 8
    Enum = 1 << 9
    Union = 1 << 10
    ParentTemplateParam = 1 << 11


class NumberSubtype(enum.IntFlag):
    Int8 = 1 << 0
    Int16 = 1 << 1
    Int32 = 1 << 2
    Int64 = 1 << 3
    Float32 = 1 << 4
    Float64 = 1 << 5


class NumberFlags(enum.IntFlag):
    NoneFlag = 0
    Unsigned = 1 << 0


class LitType(enum.IntEnum):
    Bool = 0
    Number = 1
    String = 2


def read_litestl_string(addr: int) -> str:
    """Read a litestl::util::string at `addr`: {Char *data_; int32 size_;}."""
    data = _capi.read_ptr(addr)
    size = _capi.read_i32(addr + PTRSIZE)
    if not data or size <= 0:
        return ""
    return _capi.read_bytes(data, size).decode("utf-8", errors="replace")


class DescVector:
    """Element-pointer iteration over a litestl::util::Vector at `addr`.

    Native layout: {T *data_; size_t size_; size_t capacity_; inline storage}.
    Yields the address of each element (elem_size comes from the Sizes table).
    """

    def __init__(self, addr: int, elem_size: int):
        self.addr = addr
        self.elem_size = elem_size

    def __len__(self) -> int:
        return _capi.read_size_t(self.addr + PTRSIZE)

    def __iter__(self):
        data = _capi.read_ptr(self.addr)
        elem_size = self.elem_size
        for i in range(len(self)):
            yield data + i * elem_size


class BindingBase:
    """Base descriptor: name, type tag, and native size."""

    def __init__(self, capi: Capi, ptr: int):
        capi.binding_cache[ptr] = self
        self.capi = capi
        self.ptr = ptr
        o = capi.info.Offsets.Base
        self.type = BindingType(_capi.read_i32(ptr + o.type))
        self.name = read_litestl_string(ptr + o.name)
        self.size = capi.lib.LSTL_GetBindTypeSize(ptr)

    def full_name(self) -> str:
        return _capi.read_cstring(self.capi.lib.LSTL_Binding_GetFullName(self.ptr))

    def __repr__(self):
        return f"<{type(self).__name__} {self.name!r} @0x{self.ptr:x}>"


class BooleanType(BindingBase):
    pass


class NumberType(BindingBase):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        o = capi.info.Offsets.Number
        self.subtype = NumberSubtype(_capi.read_i32(ptr + o.subtype))
        self.flags = NumberFlags(_capi.read_i32(ptr + o.flags))

    @property
    def unsigned(self) -> bool:
        return bool(self.flags & NumberFlags.Unsigned)


class ArrayType(BindingBase):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        o = capi.info.Offsets.Array
        elem_ptr = _capi.read_ptr(ptr + o.arrayType)
        self.elem_type = get_binding(capi, elem_ptr) if elem_ptr else None
        self.array_size = _capi.read_size_t(ptr + o.arraySize)


class PointerType(BindingBase):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        o = capi.info.Offsets.Pointer
        self.ptr_type = get_binding(capi, _capi.read_ptr(ptr + o.ptrType))
        self.is_non_null = bool(_capi.read_u8(ptr + o.isNonNull))


class ReferenceType(BindingBase):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        o = capi.info.Offsets.Reference
        self.ptr_type = get_binding(capi, _capi.read_ptr(ptr + o.refType))


class EnumType(BindingBase):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        o = capi.info.Offsets.Enum
        self.base_size = _capi.read_i32(ptr + o.baseSize)
        self.is_bit_mask = bool(_capi.read_u8(ptr + o.isBitMask))
        self.items: dict[str, int] = {}
        item_o = capi.info.Offsets.EnumItem
        for item_ptr in DescVector(ptr + o.items, capi.info.Sizes.Enum.EnumItem):
            item_name = read_litestl_string(item_ptr + item_o.name)
            self.items[item_name] = _capi.read_i32(item_ptr + item_o.value)


class ParentTemplateParamType(BindingBase):
    """Template parameter of a nested struct inherited from a parent."""

    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        o = capi.info.Offsets.ParentTemplateParam
        self.templ_param_name = read_litestl_string(ptr + o.templParamName)
        self.parent_depth = _capi.read_i32(ptr + o.parentDepth)
        self.concrete_type = get_binding(capi, _capi.read_ptr(ptr + o.concreteType))


class MethodType(BindingBase):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        o = capi.info.Offsets
        ret_ptr = _capi.read_ptr(ptr + o.Method.returnType)
        self.return_type = get_binding(capi, ret_ptr) if ret_ptr else None
        self.is_const = bool(_capi.read_u8(ptr + o.Method.isConst))
        self.is_static = bool(_capi.read_u8(ptr + o.Method.isStatic))
        self.params: list[tuple[str, BindingBase]] = []
        for p_ptr in DescVector(ptr + o.Method.params, capi.info.Sizes.Method.MethodParam):
            p_name = read_litestl_string(p_ptr + o.MethodParam.name)
            p_type = get_binding(capi, _capi.read_ptr(p_ptr + o.MethodParam.type))
            self.params.append((p_name, p_type))


class ConstructorType(BindingBase):
    def __init__(self, capi: Capi, ptr: int, owner: "StructType"):
        super().__init__(capi, ptr)
        self.owner = owner
        o = capi.info.Offsets
        self.params: list[tuple[str, BindingBase]] = []
        for p_ptr in DescVector(ptr + o.Constructor.params, capi.info.Sizes.Constructor.ConstructorParam):
            p_name = read_litestl_string(p_ptr + o.ConstructorParam.name)
            p_type = get_binding(capi, _capi.read_ptr(p_ptr + o.ConstructorParam.type))
            self.params.append((p_name, p_type))


class StructType(BindingBase):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        o = capi.info.Offsets
        sz = capi.info.Sizes
        self.struct_size = _capi.read_size_t(ptr + o.Struct.structSize)

        self.constructors: list[ConstructorType] = []
        for slot in DescVector(ptr + o.Struct.constructors, PTRSIZE):
            self.constructors.append(ConstructorType(capi, _capi.read_ptr(slot), self))

        self.members: list[tuple[str, BindingBase, int]] = []
        for m_ptr in DescVector(ptr + o.Struct.members, sz.Struct.StructMember):
            m_name = read_litestl_string(m_ptr + o.StructMember.name)
            m_offset = _capi.read_i32(m_ptr + o.StructMember.offset)
            m_type = get_binding(capi, _capi.read_ptr(m_ptr + o.StructMember.type))
            self.members.append((m_name, m_type, m_offset))

        self.template_params: list[tuple[str, BindingBase]] = []
        for t_ptr in DescVector(ptr + o.Struct.templateParams, sz.Struct.TemplateParam):
            t_name = read_litestl_string(t_ptr + o.TemplateParam.name)
            t_type = get_binding(capi, _capi.read_ptr(t_ptr + o.TemplateParam.type))
            self.template_params.append((t_name, t_type))

        self.methods: list[MethodType] = []
        for slot in DescVector(ptr + o.Struct.methods, PTRSIZE):
            m = get_binding(capi, _capi.read_ptr(slot))
            assert isinstance(m, MethodType)
            self.methods.append(m)

    @property
    def is_vector(self) -> bool:
        return self.name == "litestl::util::Vector"

    def find_constructor(self, name: str) -> Optional[ConstructorType]:
        for c in self.constructors:
            if c.name == name:
                return c
        return None

    def find_default_constructor(self) -> Optional[ConstructorType]:
        for c in self.constructors:
            if not c.params:
                return c
        return None

    def find_copy_constructor(self) -> Optional[ConstructorType]:
        for c in self.constructors:
            if len(c.params) != 1:
                continue
            p_type = c.params[0][1]
            if (
                isinstance(p_type, ReferenceType)
                and p_type.ptr_type.full_name() == self.full_name()
            ):
                return c
        return None


class LiteralType(BindingBase):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        o = capi.info.Offsets.Literal
        self.lit_type = LitType(_capi.read_i32(ptr + o.litType))
        bind_ptr = _capi.read_ptr(ptr + o.litBind)
        self.lit_bind = get_binding(capi, bind_ptr) if bind_ptr else None


class NumLitType(LiteralType):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        self.data = _capi.read_i32(ptr + capi.info.Offsets.NumLit.data)


class BoolLitType(LiteralType):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        self.data = _capi.read_i32(ptr + capi.info.Offsets.BoolLit.data) != 0


class StrLitType(LiteralType):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        self.data = read_litestl_string(ptr + capi.info.Offsets.StrLit.data)


class UnionType(BindingBase):
    def __init__(self, capi: Capi, ptr: int):
        super().__init__(capi, ptr)
        o = capi.info.Offsets.Union
        pair_o = capi.info.Offsets.UnionPair
        self.dis_prop_name = read_litestl_string(ptr + o.disPropName)
        self.dis_prop_type = get_binding(capi, _capi.read_ptr(ptr + o.disPropType))
        # (name, struct, type_value) per alternative; type_value decodes the
        # UnionPair's char[8] payload with the disambiguation property's type.
        self.structs: list[tuple[str, BindingBase, object]] = []
        for s_ptr in DescVector(ptr + o.structs, capi.info.Sizes.Union.UnionPair):
            s_name = read_litestl_string(s_ptr + pair_o.name)
            s_struct = get_binding(capi, _capi.read_ptr(s_ptr + pair_o.type))
            value_addr = s_ptr + pair_o.typeValue
            self.structs.append((s_name, s_struct, self._read_type_value(value_addr)))

    def _read_type_value(self, addr: int):
        t = self.dis_prop_type
        if isinstance(t, BooleanType):
            return bool(_capi.read_u8(addr))
        if isinstance(t, NumberType):
            return read_number(t, addr)
        if isinstance(t, EnumType):
            return {1: _capi.read_i8, 2: _capi.read_i16, 4: _capi.read_i32, 8: _capi.read_i64}[
                t.base_size
            ](addr)
        if isinstance(t, StructType) and t.name.startswith("litestl::util::string"):
            return read_litestl_string(addr)
        raise ValueError(f"cannot decode union type value of type {t.type!r}")

    @property
    def has_dis_prop_func(self) -> bool:
        return bool(self.capi.lib.LSTL_Union_HasDisPropFunc(self.ptr))

    def get_dis_prop(self, this_ptr: int) -> int:
        return self.capi.lib.LSTL_Union_RunDisPropFunc(self.ptr, this_ptr)


_NUM_READERS = {
    (NumberSubtype.Int8, False): _capi.read_i8,
    (NumberSubtype.Int8, True): _capi.read_u8,
    (NumberSubtype.Int16, False): _capi.read_i16,
    (NumberSubtype.Int16, True): _capi.read_u16,
    (NumberSubtype.Int32, False): _capi.read_i32,
    (NumberSubtype.Int32, True): _capi.read_u32,
    (NumberSubtype.Int64, False): _capi.read_i64,
    (NumberSubtype.Int64, True): _capi.read_u64,
    (NumberSubtype.Float32, False): _capi.read_f32,
    (NumberSubtype.Float32, True): _capi.read_f32,
    (NumberSubtype.Float64, False): _capi.read_f64,
    (NumberSubtype.Float64, True): _capi.read_f64,
}

_NUM_WRITERS = {
    (NumberSubtype.Int8, False): _capi.write_i8,
    (NumberSubtype.Int8, True): _capi.write_u8,
    (NumberSubtype.Int16, False): _capi.write_i16,
    (NumberSubtype.Int16, True): _capi.write_u16,
    (NumberSubtype.Int32, False): _capi.write_i32,
    (NumberSubtype.Int32, True): _capi.write_u32,
    (NumberSubtype.Int64, False): _capi.write_i64,
    (NumberSubtype.Int64, True): _capi.write_u64,
    (NumberSubtype.Float32, False): _capi.write_f32,
    (NumberSubtype.Float32, True): _capi.write_f32,
    (NumberSubtype.Float64, False): _capi.write_f64,
    (NumberSubtype.Float64, True): _capi.write_f64,
}


def read_number(num: NumberType, addr: int):
    return _NUM_READERS[(num.subtype, num.unsigned)](addr)


def write_number(num: NumberType, addr: int, value) -> None:
    _NUM_WRITERS[(num.subtype, num.unsigned)](addr, value)


def number_writer(num: NumberType):
    """The `(addr, value)` writer for `num`, for callers hoisting the lookup
    out of a hot path."""
    return _NUM_WRITERS[(num.subtype, num.unsigned)]


def get_binding(capi: Capi, ptr: int) -> BindingBase:
    """Wrap the descriptor at `ptr`, reusing capi.binding_cache (descriptor
    graphs are cyclic; constructors self-register before recursing)."""
    cached = capi.binding_cache.get(ptr)
    if cached is not None:
        return cached  # type: ignore[return-value]

    o = capi.info.Offsets
    btype = BindingType(_capi.read_i32(ptr + o.Base.type))

    if btype == BindingType.Boolean:
        return BooleanType(capi, ptr)
    if btype == BindingType.Number:
        return NumberType(capi, ptr)
    if btype == BindingType.Array:
        return ArrayType(capi, ptr)
    if btype == BindingType.Struct:
        return StructType(capi, ptr)
    if btype == BindingType.Literal:
        lit_type = LitType(_capi.read_i32(ptr + o.Literal.litType))
        if lit_type == LitType.Bool:
            return BoolLitType(capi, ptr)
        if lit_type == LitType.Number:
            return NumLitType(capi, ptr)
        if lit_type == LitType.String:
            return StrLitType(capi, ptr)
        return LiteralType(capi, ptr)
    if btype == BindingType.Enum:
        return EnumType(capi, ptr)
    if btype == BindingType.Method:
        return MethodType(capi, ptr)
    if btype == BindingType.Union:
        return UnionType(capi, ptr)
    if btype == BindingType.Pointer:
        return PointerType(capi, ptr)
    if btype == BindingType.Reference:
        return ReferenceType(capi, ptr)
    if btype == BindingType.ParentTemplateParam:
        return ParentTemplateParamType(capi, ptr)
    return BindingBase(capi, ptr)
