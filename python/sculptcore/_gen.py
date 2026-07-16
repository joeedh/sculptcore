"""Self-hosted .pyi stub generation (python -m sculptcore._gen).

Calls LSTL_GeneratePython on the loaded shared library and writes the
returned path->content set as the `sculptcore.types` stub package, plus
py.typed (PEP 561). Regeneration is deterministic: same registry, same
bytes. Files not produced by the generator are left untouched, so
hand-written stubs can live alongside the generated tree.
"""

from __future__ import annotations

import ctypes
import os
import struct
import sys

from . import _capi


def generate_files(capi: _capi.Capi) -> dict[str, bytes]:
    """Run LSTL_GeneratePython and parse the length-prefixed buffer."""
    lib = capi.lib
    lib.LSTL_GeneratePython.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.LSTL_GeneratePython.restype = ctypes.c_void_p
    lib.LSTL_FreePythonString.argtypes = [ctypes.c_void_p]
    lib.LSTL_FreePythonString.restype = None

    size = ctypes.c_int(0)
    buf_ptr = lib.LSTL_GeneratePython(
        lib.getBindingManager(), ctypes.addressof(size)
    )
    data = _capi.read_bytes(buf_ptr, size.value)
    lib.LSTL_FreePythonString(buf_ptr)

    files: dict[str, bytes] = {}
    pos = 0
    while pos < len(data):
        (name_len,) = struct.unpack_from("<i", data, pos)
        pos += 4
        name = data[pos:pos + name_len].decode("utf-8")
        pos += name_len
        (content_len,) = struct.unpack_from("<i", data, pos)
        pos += 4
        files[name] = data[pos:pos + content_len]
        pos += content_len
    return files


def write_stubs(out_dir: str | None = None) -> list[str]:
    import sculptcore

    mgr = sculptcore.init()
    files = generate_files(mgr.capi)

    pkg_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir = out_dir or os.path.join(pkg_dir, "types")
    os.makedirs(out_dir, exist_ok=True)

    written = []
    for rel_path, content in files.items():
        path = os.path.join(out_dir, rel_path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        prev = None
        if os.path.exists(path):
            with open(path, "rb") as f:
                prev = f.read()
        if prev != content:
            with open(path, "wb") as f:
                f.write(content)
        written.append(path)

    # `sculptcore.types` is stub-only: a py.typed marker plus an __init__.py
    # so the stubs resolve as a real subpackage of the runtime package.
    for name, content in (
        ("py.typed", b""),
        ("__init__.py", b"# Stub-only subpackage; see __init__.pyi (generated).\n"),
    ):
        path = os.path.join(out_dir, name)
        if not os.path.exists(path):
            with open(path, "wb") as f:
                f.write(content)
        written.append(path)

    marker = os.path.join(pkg_dir, "py.typed")
    if not os.path.exists(marker):
        with open(marker, "wb") as f:
            f.write(b"")

    return written


def main(argv: list[str]) -> int:
    out_dir = argv[1] if len(argv) > 1 else None
    written = write_stubs(out_dir)
    print(f"wrote {len(written)} stub files under "
          f"{os.path.dirname(written[0]) if written else '(none)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
