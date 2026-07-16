"""ctypes loader for the sculptcore_capi shared library.

Python counterpart of typescriptRuntime/wasmInterface.ts: loads the native
aggregate lib (build/python/sculptcore_capi.*), declares the LSTL_* dispatch
ABI, and reads the BindingInfo offset/size table so every descriptor read is
data-driven — Python never hardcodes a C++ struct layout.

Unlike the WASM runtime (32-bit heap views), this reads native memory through
ctypes.from_address at real 64-bit addresses.
"""

from __future__ import annotations

import ctypes
import os
import sys
from types import SimpleNamespace

# Must match LSTL_AbiVersion() in litestl/binding/binding.cc.
ABI_VERSION = 1

PTRSIZE = ctypes.sizeof(ctypes.c_void_p)


class CapiError(RuntimeError):
    pass


def _lib_basename() -> str:
    if sys.platform == "win32":
        return "sculptcore_capi.dll"
    if sys.platform == "darwin":
        return "libsculptcore_capi.dylib"
    return "libsculptcore_capi.so"


def _candidate_paths():
    env = os.environ.get("SCULPTCORE_CAPI_PATH")
    if env:
        yield env
    pkg_dir = os.path.dirname(os.path.abspath(__file__))
    # Deployed package: lib ships beside the Python sources.
    yield os.path.join(pkg_dir, _lib_basename())
    # Source checkout: <repo>/python/sculptcore/ -> <repo>/build/python/.
    yield os.path.normpath(os.path.join(pkg_dir, "..", "..", "build", "python", _lib_basename()))


# --- raw memory accessors ---------------------------------------------------

def read_i8(addr: int) -> int:
    return ctypes.c_int8.from_address(addr).value


def read_u8(addr: int) -> int:
    return ctypes.c_uint8.from_address(addr).value


def read_i16(addr: int) -> int:
    return ctypes.c_int16.from_address(addr).value


def read_u16(addr: int) -> int:
    return ctypes.c_uint16.from_address(addr).value


def read_i32(addr: int) -> int:
    return ctypes.c_int32.from_address(addr).value


def read_u32(addr: int) -> int:
    return ctypes.c_uint32.from_address(addr).value


def read_i64(addr: int) -> int:
    return ctypes.c_int64.from_address(addr).value


def read_u64(addr: int) -> int:
    return ctypes.c_uint64.from_address(addr).value


def read_f32(addr: int) -> float:
    return ctypes.c_float.from_address(addr).value


def read_f64(addr: int) -> float:
    return ctypes.c_double.from_address(addr).value


def read_ptr(addr: int) -> int:
    return ctypes.c_size_t.from_address(addr).value


read_size_t = read_ptr


def write_i8(addr: int, v: int) -> None:
    ctypes.c_int8.from_address(addr).value = v


def write_u8(addr: int, v: int) -> None:
    ctypes.c_uint8.from_address(addr).value = v


def write_i16(addr: int, v: int) -> None:
    ctypes.c_int16.from_address(addr).value = v


def write_u16(addr: int, v: int) -> None:
    ctypes.c_uint16.from_address(addr).value = v


def write_i32(addr: int, v: int) -> None:
    ctypes.c_int32.from_address(addr).value = v


def write_u32(addr: int, v: int) -> None:
    ctypes.c_uint32.from_address(addr).value = v


def write_i64(addr: int, v: int) -> None:
    ctypes.c_int64.from_address(addr).value = v


def write_u64(addr: int, v: int) -> None:
    ctypes.c_uint64.from_address(addr).value = v


def write_f32(addr: int, v: float) -> None:
    ctypes.c_float.from_address(addr).value = v


def write_f64(addr: int, v: float) -> None:
    ctypes.c_double.from_address(addr).value = v


def write_ptr(addr: int, v: int) -> None:
    ctypes.c_size_t.from_address(addr).value = v


def read_bytes(addr: int, size: int) -> bytes:
    return ctypes.string_at(addr, size)


def read_cstring(addr: int) -> str:
    if not addr:
        return ""
    return ctypes.string_at(addr).decode("utf-8", errors="replace")


# --- the ABI ----------------------------------------------------------------

_P = ctypes.c_void_p
_SZ = ctypes.c_size_t

# name -> (argtypes, restype); every function is extern "C".
_DECLS = {
    "LSTL_AbiVersion": ([], ctypes.c_int),
    "initBindings": ([], None),
    "getBindingManager": ([], _P),
    "LSTL_GetBindingInfo": ([], _P),
    "LSTL_FreeBindingInfo": ([_P], None),
    "LSTL_Binding_GetKeys": ([_P], _P),
    "LSTL_Binding_FreeKeys": ([_P], None),
    "LSTL_Binding_Get": ([_P, ctypes.c_char_p], _P),
    "LSTL_Binding_GetFullName": ([_P], _P),
    "LSTL_Struct_GetMethodCount": ([_P], _SZ),
    "LSTL_Struct_GetMethod": ([_P, _SZ], _P),
    "LSTL_Method_GetParamCount": ([_P], _SZ),
    "LSTL_Method_GetParam": ([_P, _SZ, _P], _P),
    "LSTL_Method_GetReturn": ([_P], _P),
    "LSTL_Method_GetName": ([_P], _P),
    "LSTL_Method_IsConst": ([_P], ctypes.c_int),
    "LSTL_Method_Invoke": ([_P, _P, _P, _P], None),
    "LSTL_Struct_GetConstructorCount": ([_P], _SZ),
    "LSTL_Struct_GetConstructor": ([_P, _SZ], _P),
    "LSTL_Constructor_GetName": ([_P], _P),
    "LSTL_Constructor_GetOwner": ([_P], _P),
    "LSTL_Constructor_GetParamCount": ([_P], _SZ),
    "LSTL_Constructor_GetParam": ([_P, _SZ, _P], _P),
    "LSTL_Constructor_Invoke": ([_P, _P, _P], None),
    "LSTL_Destructor_Invoke": ([_P, _P], None),
    "LSTL_GetBindTypeSize": ([_P], ctypes.c_int),
    "LSTL_Union_HasDisPropFunc": ([_P], ctypes.c_bool),
    "LSTL_Union_RunDisPropFunc": ([_P, _P], ctypes.c_int32),
    "LSTL_GetMemSize": ([ctypes.c_bool], _SZ),
    "LSTL_PrintAllocBlocks": ([ctypes.c_bool], None),
    "LSTL_FormatBlocks": ([ctypes.c_bool], _P),
    "LSTL_FormatBlock": ([_P], _P),
    "LSTL_FreeFormatBlocks": ([_P], None),
    "memAlloc": ([ctypes.c_char_p, _SZ], _P),
    "memRelease": ([_P], None),
    "_rawAlloc": ([ctypes.c_int], _P),
    "_rawRelease": ([_P], None),
    "_rawGetAllocSize": ([], ctypes.c_int),
}

# Sequential int32 field order of BindingInfo (binding.cc WasmOffsets +
# TypeSizes, both [[gnu::packed]]). Mirrors createWasmHelpers in
# wasmInterface.ts; extend here AND there when binding.cc grows a field.
_OFFSETS_LAYOUT = (
    ("Base", ("name", "type")),
    ("Struct", ("members", "methods", "constructors", "templateParams", "structSize")),
    ("Constructor", ("ownerType", "params")),
    ("ConstructorParam", ("name", "type")),
    ("StructMember", ("name", "offset", "type")),
    ("Number", ("subtype", "flags")),
    ("Array", ("arrayType", "arraySize")),
    ("Literal", ("litType", "litBind")),
    ("NumLit", ("data",)),
    ("BoolLit", ("data",)),
    ("StrLit", ("data",)),
    ("Pointer", ("ptrType", "isNonNull")),
    ("Reference", ("refType",)),
    ("EnumItem", ("name", "value")),
    ("Enum", ("items", "baseSize", "isBitMask")),
    ("TemplateParam", ("name", "type")),
    ("Method", ("returnType", "params", "isConst", "isStatic")),
    ("MethodParam", ("name", "type")),
    ("Union", ("structs", "disPropName", "disPropType")),
    ("UnionPair", ("name", "type", "typeValue")),
    ("ParentTemplateParam", ("templParamName", "parentDepth", "concreteType")),
)

_SIZES_LAYOUT = (
    ("Struct", ("StructMember", "StructBase", "TemplateParam", "StructMethod", "StructConstructor")),
    ("Types", ("Boolean", "NumLit", "BoolLit", "StrLit", "Pointer", "Reference")),
    ("Constructor", ("ConstructorParam",)),
    ("Enum", ("EnumItem", "Enum")),
    ("Method", ("Method", "MethodParam")),
    ("Union", ("Union", "UnionPair", "typeValue")),
    ("ParentTemplateParam", ("ParentTemplateParam",)),
)


def _read_binding_info(lib) -> SimpleNamespace:
    info_ptr = lib.LSTL_GetBindingInfo()
    addr = info_ptr

    def read_groups(layout):
        nonlocal addr
        ns = SimpleNamespace()
        for group, fields in layout:
            gns = SimpleNamespace()
            for field in fields:
                setattr(gns, field, read_i32(addr))
                addr += 4
            setattr(ns, group, gns)
        return ns

    offsets = read_groups(_OFFSETS_LAYOUT)
    sizes = read_groups(_SIZES_LAYOUT)
    sizes.VectorDefaultStaticSize = read_i32(addr)

    lib.LSTL_FreeBindingInfo(info_ptr)
    return SimpleNamespace(Offsets=offsets, Sizes=sizes)


class Capi:
    """The loaded shared library plus the BindingInfo layout table."""

    def __init__(self, lib_path: str):
        if sys.platform == "win32":
            # Resolve wgpu_native.dll (and other staged deps) beside the lib.
            self._dll_dir = os.add_dll_directory(os.path.dirname(lib_path))
        self.lib = ctypes.CDLL(lib_path)
        self.lib_path = lib_path

        for name, (argtypes, restype) in _DECLS.items():
            fn = getattr(self.lib, name)
            fn.argtypes = argtypes
            fn.restype = restype

        abi = self.lib.LSTL_AbiVersion()
        if abi != ABI_VERSION:
            raise CapiError(
                f"ABI mismatch: {lib_path} reports version {abi}, "
                f"this package expects {ABI_VERSION}"
            )

        self.info = _read_binding_info(self.lib)
        # litestl stores allocation-tag pointers, so tag bytes must stay
        # alive for the process lifetime (the TS runtime's cstring pool).
        self._tag_pool: dict[str, bytes] = {}
        # Descriptor wrapper cache, keyed by descriptor address
        # (_descriptors.get_binding).
        self.binding_cache: dict[int, object] = {}

    def tag(self, s: str) -> bytes:
        b = self._tag_pool.get(s)
        if b is None:
            b = s.encode("utf-8")
            self._tag_pool[s] = b
        return b

    def mem_alloc(self, tag: str, size: int) -> int:
        return self.lib.memAlloc(self.tag(tag), max(size, 1))

    def mem_release(self, ptr: int) -> None:
        self.lib.memRelease(ptr)


def load(path: str | None = None) -> Capi:
    if path is not None:
        return Capi(path)
    tried = []
    for candidate in _candidate_paths():
        if os.path.exists(candidate):
            return Capi(candidate)
        tried.append(candidate)
    raise CapiError(
        "sculptcore_capi library not found; build it with `node make.mjs build python` "
        "or set SCULPTCORE_CAPI_PATH. Tried:\n  " + "\n  ".join(tried)
    )
