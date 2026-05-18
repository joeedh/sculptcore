"""GDB pretty-printers for sculptcore types.

Load via gdb:
    source tools/gdb/sculptcore.py
or pass --command=tools/gdb/run-gdb.sh, which sources both this file and
helpers.gdb.

The printers favor giving you orientation rather than exhaustive dumps: a
container shows size + first few elements, a Mesh shows counts + AABB, a
SpatialNode shows the leaf flag + bounds + tri count. Use the helper
commands in helpers.gdb to walk full trees.
"""

import gdb
import re


def _safe_int(val):
    try:
        return int(val)
    except (gdb.error, ValueError):
        return -1


class VectorPrinter:
    """litestl::util::Vector<T> — prints size + first few elements."""

    PREVIEW = 4

    def __init__(self, val):
        self.val = val

    def to_string(self):
        size = _safe_int(self.val["size_"])
        cap = _safe_int(self.val["capacity_"])
        return f"Vector size={size} capacity={cap}"

    def children(self):
        size = _safe_int(self.val["size_"])
        if size <= 0:
            return
        data = self.val["data_"]
        n = min(size, self.PREVIEW)
        for i in range(n):
            try:
                yield (f"[{i}]", (data + i).dereference())
            except gdb.error:
                yield (f"[{i}]", "<unreadable>")
        if size > self.PREVIEW:
            yield ("...", f"+{size - self.PREVIEW} more")


class Float3Printer:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            d = self.val["data_"]
            return f"({float(d[0]):.4g}, {float(d[1]):.4g}, {float(d[2]):.4g})"
        except gdb.error:
            return "float3<unreadable>"


class AABBPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            mn = self.val["min"]
            mx = self.val["max"]
            return f"AABB min={mn} max={mx}"
        except gdb.error:
            return "AABB<unreadable>"


class MeshPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            v = _safe_int(self.val["v"]["count"])
            e = _safe_int(self.val["e"]["count"])
            c = _safe_int(self.val["c"]["count"])
            f = _safe_int(self.val["f"]["count"])
            return f"Mesh v={v} e={e} c={c} f={f}"
        except gdb.error:
            return "Mesh<unreadable>"


class SpatialNodePrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            nid = _safe_int(self.val["id"])
            depth = _safe_int(self.val["depth"])
            flag = _safe_int(self.val["flag"])
            is_leaf = bool(flag & 1)  # Spatial_Leaf
            tri_count = -1
            data = self.val["data"]
            if int(data) != 0:
                tri_count = _safe_int(data.dereference()["tris"]["size_"])
            return (f"SpatialNode id={nid} depth={depth} "
                    f"{'leaf' if is_leaf else 'inner'} tris={tri_count}")
        except gdb.error:
            return "SpatialNode<unreadable>"


class BufferPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            t = self.val["type"]
            elem = _safe_int(self.val["elemSize"])
            cnt = _safe_int(self.val["elemCount"])
            return f"Buffer type={t} elemSize={elem} elemCount={cnt}"
        except gdb.error:
            return "Buffer<unreadable>"


_PRINTERS = [
    (re.compile(r"^litestl::util::Vector<"),       VectorPrinter),
    (re.compile(r"^litestl::math::Vec<.*,\s*3"),   Float3Printer),
    (re.compile(r"^litestl::math::AABB<"),         AABBPrinter),
    (re.compile(r"^sculptcore::mesh::Mesh"),       MeshPrinter),
    (re.compile(r"^sculptcore::spatial::SpatialNode"), SpatialNodePrinter),
    (re.compile(r"^sculptcore::gpu::Buffer"),      BufferPrinter),
]


def _lookup(val):
    tag = val.type.strip_typedefs().tag
    if not tag:
        return None
    for rx, cls in _PRINTERS:
        if rx.match(tag):
            return cls(val)
    return None


def register():
    gdb.printing.register_pretty_printer(None, _lookup, replace=True)


register()
print("[sculptcore.py] pretty-printers registered")
