"""sculptcore — Python ctypes runtime over the engine's LSTL_* C ABI.

Mirrors the TypeScript/WASM runtime (litestl/binding/typescriptRuntime/):
the same reflection registry drives dynamically built classes, so the two
runtimes expose the same surface. Static types come from generated .pyi
stubs (python -m sculptcore._gen).

Usage:
    import sculptcore
    mgr = sculptcore.init()
    with mgr.construct("sculptcore::mesh::Mesh") as mesh:
        ...
"""

from __future__ import annotations

from ._capi import ABI_VERSION, Capi, CapiError, load
from ._bulk import BoundVector, construct_from_items
from ._classgen import (
    BoundArray,
    BoundObject,
    Manager,
    NotStructError,
    UnknownConstructorError,
    UnknownTypeError,
)

_manager: Manager | None = None


def init(lib_path: str | None = None) -> Manager:
    """Load the shared library, run initBindings(), and build the manager.

    Idempotent: subsequent calls return the same manager (lib_path must not
    change — the engine registry is process-global).
    """
    global _manager
    if _manager is not None:
        return _manager
    capi = load(lib_path)
    capi.lib.initBindings()
    _manager = Manager(capi)
    return _manager


def manager() -> Manager:
    if _manager is None:
        raise CapiError("sculptcore.init() has not been called")
    return _manager
