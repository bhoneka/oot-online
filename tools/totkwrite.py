"""Writers for the Royale data mod: typed BYML (v7, little endian) and SARC.

totkdata.Byml reads BYML into plain Python values and loses the exact node types; to edit a file and write it back
we need them, so this module has its own typed reader:
    str, bool, int (s32), float (f32), dict (string keys), list (array), None, bytes (binary)
    and wrappers for the rest: U32, I64, U64, F64, HashDict (0x20/0x21), MonoList (0xC3).

    root = read_byml(data); root["Components"]["LifeRef"] = "?Component/..."; data = write_byml(root)
    files = read_sarc(data) (dict name -> bytes, in file order); data = write_sarc(files)
"""

import struct


class U32(int):
    pass


class I64(int):
    pass


class U64(int):
    pass


class F64(float):
    pass


class HashDict(dict):
    """v7 hash-keyed dictionary (keys are u32 or u64 hashes)."""

    def __init__(self, *a, wide=False, **k):
        super().__init__(*a, **k)
        self.wide = wide


class MonoList(list):
    """v7 mono-typed array: every element has the node type `vtype`."""

    def __init__(self, *a, vtype=0xD1, **k):
        super().__init__(*a, **k)
        self.vtype = vtype


# ---------------------------------------------------------------- BYML read (typed)
def read_byml(data):
    if data[:2] != b"YB":
        raise ValueError("only little-endian BYML (YB) is supported")
    version, key_off, str_off, root_off = struct.unpack_from("<HIII", data, 2)

    def u24(o):
        return int.from_bytes(data[o:o + 3], "little")

    def u32(o):
        return struct.unpack_from("<I", data, o)[0]

    def table(off):
        if not off:
            return []
        assert data[off] == 0xC2
        n = u24(off + 1)
        offs = [u32(off + 4 + 4 * i) for i in range(n + 1)]
        return [data[off + offs[i]:data.index(b"\0", off + offs[i])].decode("utf-8") for i in range(n)]

    keys, strings = table(key_off), table(str_off)

    def container(t, off):
        n = u24(off + 1)
        if t == 0xC0:
            types = data[off + 4:off + 4 + n]
            vals = off + 4 + ((n + 3) & ~3)
            return [value(types[i], vals + 4 * i) for i in range(n)]
        if t == 0xC1:
            out = {}
            for i in range(n):
                e = off + 4 + 8 * i
                out[keys[u24(e)]] = value(data[e + 3], e + 4)
            return out
        if t in (0x20, 0x21):
            wide = t == 0x21
            stride = 12 if wide else 8
            types_off = off + 4 + n * stride
            out = HashDict(wide=wide)
            for i in range(n):
                e = off + 4 + stride * i
                k = struct.unpack_from("<Q", data, e)[0] if wide else u32(e)
                out[k] = value(data[types_off + i], e + (8 if wide else 4))
            return out
        if t == 0xC3:
            vt = data[off + 4]
            return MonoList([value(vt, off + 8 + 4 * i) for i in range(n)], vtype=vt)
        raise ValueError(f"unknown container {t:#x}")

    def value(t, o):
        if t == 0xA0:
            return strings[u32(o)]
        if t in (0xA1, 0xA2):
            p = u32(o)
            return bytes(data[p + 4:p + 4 + u32(p)])
        if t in (0xC0, 0xC1, 0x20, 0x21, 0xC3):
            return container(t, u32(o))
        if t == 0xD0:
            return u32(o) != 0
        if t == 0xD1:
            return struct.unpack_from("<i", data, o)[0]
        if t == 0xD2:
            return struct.unpack_from("<f", data, o)[0]
        if t == 0xD3:
            return U32(u32(o))
        if t == 0xD4:
            return I64(struct.unpack_from("<q", data, u32(o))[0])
        if t == 0xD5:
            return U64(struct.unpack_from("<Q", data, u32(o))[0])
        if t == 0xD6:
            return F64(struct.unpack_from("<d", data, u32(o))[0])
        if t == 0xFF:
            return None
        raise ValueError(f"unknown value type {t:#x}")

    root = container(data[root_off], root_off) if root_off else None
    return root


# ---------------------------------------------------------------- BYML write
def _type_of(v):
    if isinstance(v, str):
        return 0xA0
    if isinstance(v, bool):
        return 0xD0
    if isinstance(v, HashDict):
        return 0x21 if v.wide else 0x20
    if isinstance(v, dict):
        return 0xC1
    if isinstance(v, MonoList):
        return 0xC3
    if isinstance(v, list):
        return 0xC0
    if isinstance(v, (bytes, bytearray)):
        return 0xA1
    if isinstance(v, U32):
        return 0xD3
    if isinstance(v, I64):
        return 0xD4
    if isinstance(v, U64):
        return 0xD5
    if isinstance(v, F64):
        return 0xD6
    if isinstance(v, int):
        return 0xD1
    if isinstance(v, float):
        return 0xD2
    if v is None:
        return 0xFF
    raise TypeError(f"no BYML type for {type(v)}")


def write_byml(root, version=7):
    keys, strings = set(), set()

    def collect(v):
        if isinstance(v, dict):
            if not isinstance(v, HashDict):
                keys.update(v.keys())
            for x in v.values():
                collect(x)
        elif isinstance(v, list):
            for x in v:
                collect(x)
        elif isinstance(v, str):
            strings.add(v)

    collect(root)
    key_list = sorted(keys, key=lambda s: s.encode("utf-8"))
    str_list = sorted(strings, key=lambda s: s.encode("utf-8"))
    key_idx = {k: i for i, k in enumerate(key_list)}
    str_idx = {s: i for i, s in enumerate(str_list)}

    out = bytearray(16)

    def align(n):
        while len(out) % n:
            out.append(0)

    def write_table(items):
        nonlocal out
        if not items:
            return 0
        align(4)
        start = len(out)
        enc = [s.encode("utf-8") + b"\0" for s in items]
        head = 4 + 4 * (len(items) + 1)
        offs, pos = [], head
        for e in enc:
            offs.append(pos)
            pos += len(e)
        offs.append(pos)
        out += bytes([0xC2]) + len(items).to_bytes(3, "little")
        for o in offs:
            out += struct.pack("<I", o)
        for e in enc:
            out += e
        return start

    key_off = write_table(key_list)
    str_off = write_table(str_list)

    def scalar_bits(v, t):
        if t == 0xA0:
            return str_idx[v]
        if t == 0xD0:
            return 1 if v else 0
        if t == 0xD1:
            return struct.unpack("<I", struct.pack("<i", v))[0]
        if t == 0xD2:
            return struct.unpack("<I", struct.pack("<f", v))[0]
        if t == 0xD3:
            return int(v) & 0xFFFFFFFF
        if t == 0xFF:
            return 0
        raise ValueError(t)

    def write_node(v):
        """Write a container (or out-of-line value) and return its offset."""
        nonlocal out
        t = _type_of(v)
        if t in (0xD4, 0xD5, 0xD6):
            align(8)
            o = len(out)
            out += struct.pack({0xD4: "<q", 0xD5: "<Q", 0xD6: "<d"}[t], v)
            return o
        if t == 0xA1:
            align(4)
            o = len(out)
            out += struct.pack("<I", len(v)) + bytes(v)
            return o
        align(4)
        o = len(out)
        if t == 0xC1:
            items = sorted(v.items(), key=lambda kv: key_idx[kv[0]])
            out += bytes([0xC1]) + len(items).to_bytes(3, "little")
            slots = []
            for k, x in items:
                xt = _type_of(x)
                out += key_idx[k].to_bytes(3, "little") + bytes([xt])
                slots.append((len(out), x, xt))
                out += b"\0\0\0\0"
            for pos, x, xt in slots:
                bits = write_node(x) if xt in (0xC0, 0xC1, 0x20, 0x21, 0xC3, 0xA1, 0xD4, 0xD5, 0xD6) else scalar_bits(x, xt)
                struct.pack_into("<I", out, pos, bits)
            return o
        if t in (0x20, 0x21):
            items = sorted(v.items())
            out += bytes([t]) + len(items).to_bytes(3, "little")
            slots = []
            for k, x in items:
                out += struct.pack("<Q" if v.wide else "<I", k)
                slots.append((len(out), x, _type_of(x)))
                out += b"\0\0\0\0"
            out += bytes(xt for _, _, xt in slots)
            align(4)
            for pos, x, xt in slots:
                bits = write_node(x) if xt in (0xC0, 0xC1, 0x20, 0x21, 0xC3, 0xA1, 0xD4, 0xD5, 0xD6) else scalar_bits(x, xt)
                struct.pack_into("<I", out, pos, bits)
            return o
        if t == 0xC3:
            out += bytes([0xC3]) + len(v).to_bytes(3, "little") + bytes([v.vtype, 0, 0, 0])
            slots = []
            for x in v:
                slots.append(len(out))
                out += b"\0\0\0\0"
            for pos, x in zip(slots, v):
                bits = write_node(x) if v.vtype in (0xC0, 0xC1, 0x20, 0x21, 0xC3, 0xA1, 0xD4, 0xD5, 0xD6) else scalar_bits(x, v.vtype)
                struct.pack_into("<I", out, pos, bits)
            return o
        if t == 0xC0:
            out += bytes([0xC0]) + len(v).to_bytes(3, "little")
            types = [_type_of(x) for x in v]
            out += bytes(types)
            align(4)
            slots = []
            for x in v:
                slots.append(len(out))
                out += b"\0\0\0\0"
            for pos, x, xt in zip(slots, v, types):
                bits = write_node(x) if xt in (0xC0, 0xC1, 0x20, 0x21, 0xC3, 0xA1, 0xD4, 0xD5, 0xD6) else scalar_bits(x, xt)
                struct.pack_into("<I", out, pos, bits)
            return o
        raise ValueError(f"root/child of type {t:#x} is not a container")

    root_off = write_node(root) if root is not None else 0
    struct.pack_into("<2sHIII", out, 0, b"YB", version, key_off, str_off, root_off)
    align(4)
    return bytes(out)


# ---------------------------------------------------------------- SARC
def _sarc_hash(name, key=0x65):
    h = 0
    for b in name.encode("utf-8"):
        h = (h * key + (b if b < 0x80 else b - 0x100)) & 0xFFFFFFFF
    return h


def read_sarc(data):
    assert data[:4] == b"SARC"
    doff = struct.unpack_from("<I", data, 0x0C)[0]
    n = struct.unpack_from("<H", data, 0x14 + 6)[0]
    names_off = 0x20 + 16 * n + 8
    files = {}
    entries = []
    for i in range(n):
        h, attr, s, e = struct.unpack_from("<IIII", data, 0x20 + 16 * i)
        no = names_off + (attr & 0xFFFF) * 4
        name = data[no:data.index(b"\0", no)].decode("utf-8")
        entries.append((s, name, bytes(data[doff + s:doff + e])))
    for s, name, blob in sorted(entries):
        files[name] = blob
    return files


def _file_align(name):
    """the alignment the game's own archives keep per file type (measured on Pack/Actor and UI/LayoutArchive:
    GPU data read in place needs it; a texture bundle at an 8-byte offset crashed the game)"""
    if name.endswith((".bntx", ".bnsh", ".bfres", ".bfsha")):
        return 0x1000
    if name.endswith(".bushvt"):
        return 0x80
    if name.endswith(".bphhb"):
        return 0x20
    if name.endswith((".bphcl", ".bphsh", ".bphsc", ".bphnm", ".bflyt", ".bflan", ".glsl")):
        return 0x10
    return 8


def write_sarc(files):
    """files: dict name -> bytes. Entries sorted by name hash; data in the given order."""
    names = list(files.keys())
    by_hash = sorted(names, key=_sarc_hash)
    sfnt = bytearray()
    name_off = {}
    for nm in names:
        name_off[nm] = len(sfnt) // 4
        sfnt += nm.encode("utf-8") + b"\0"
        while len(sfnt) % 4:
            sfnt.append(0)
    head = 0x14 + 0x0C + 16 * len(names) + 8 + len(sfnt)
    top = max(_file_align(nm) for nm in names) if names else 8
    pad = max(top, 0x80) - 1
    doff = (head + pad) & ~pad
    data = bytearray()
    ranges = {}
    for nm in names:
        a = _file_align(nm)
        while (doff + len(data)) % a:
            data.append(0)
        s = len(data)
        data += files[nm]
        ranges[nm] = (s, len(data))
    out = bytearray()
    out += struct.pack("<4sHHIIHH", b"SARC", 0x14, 0xFEFF, doff + len(data), doff, 0x0100, 0)
    out += struct.pack("<4sHHI", b"SFAT", 0x0C, len(names), 0x65)
    for nm in by_hash:
        s, e = ranges[nm]
        out += struct.pack("<IIII", _sarc_hash(nm), 0x01000000 | name_off[nm], s, e)
    out += struct.pack("<4sHH", b"SFNT", 8, 0) + sfnt
    while len(out) < doff:
        out.append(0)
    out += data
    return bytes(out)


# ---- Ocarina of Time (Switch 2) archives (OoT Online): the same SARC, laid out a little differently. The data section starts at the
# header's end rounded to 8 (not 0x80), and each file at its own type's boundary: the GPU and Havok files as TOTK's,
# everything else 8. Checked byte for byte against the game's own actor packs (tools/make_online_mod.py).
def _file_align_oot(name):
    return _file_align(name)   # .ainb and .bgyml at 8 like everything else (an .ainb at 16 in some packs is a coincidence of their sizes)


def write_sarc_oot(files):
    names = list(files.keys())
    by_hash = sorted(names, key=_sarc_hash)
    sfnt = bytearray()
    name_off = {}
    for nm in names:
        name_off[nm] = len(sfnt) // 4
        sfnt += nm.encode("utf-8") + b"\0"
        while len(sfnt) % 4:
            sfnt.append(0)
    head = 0x14 + 0x0C + 16 * len(names) + 8 + len(sfnt)
    doff = (head + 7) & ~7
    data = bytearray()
    ranges = {}
    for nm in names:
        a = _file_align_oot(nm)
        while (doff + len(data)) % a:
            data.append(0)
        s = len(data)
        data += files[nm]
        ranges[nm] = (s, len(data))
    out = bytearray()
    out += struct.pack("<4sHHIIHH", b"SARC", 0x14, 0xFEFF, doff + len(data), doff, 0x0100, 0)
    out += struct.pack("<4sHHI", b"SFAT", 0x0C, len(names), 0x65)
    for nm in by_hash:
        s, e = ranges[nm]
        out += struct.pack("<IIII", _sarc_hash(nm), 0x01000000 | name_off[nm], s, e)
    out += struct.pack("<4sHH", b"SFNT", 8, 0) + sfnt
    while len(out) < doff:
        out.append(0)
    out += data
    return bytes(out)
