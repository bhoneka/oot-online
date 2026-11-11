# Minimal BYML (v2-v7) reader, from TOTK Royale's tools/totkdata.py (our own code).
import struct

class Byml:
    """Minimal BYML (v2-v7) reader. Hash nodes become dicts, arrays lists."""

    def __init__(self, data):
        if data[:2] == b"YB":
            self.e = "<"
        elif data[:2] == b"BY":
            self.e = ">"
        else:
            raise ValueError("not BYML")
        self.d = data
        self.version, key_off, str_off, root_off = struct.unpack_from(self.e + "HIII", data, 2)
        self.keys = self._string_table(key_off) if key_off else []
        self.strings = self._string_table(str_off) if str_off else []
        self.root = self._node_at(root_off) if root_off else None

    def _u32(self, o):
        return struct.unpack_from(self.e + "I", self.d, o)[0]

    def _u24(self, o):
        b = self.d[o:o + 3]
        return int.from_bytes(b, "little" if self.e == "<" else "big")

    def _string_table(self, off):
        assert self.d[off] == 0xC2
        n = self._u24(off + 1)
        offs = [self._u32(off + 4 + 4 * i) for i in range(n + 1)]
        out = []
        for i in range(n):
            s = off + offs[i]
            end = self.d.index(b"\0", s)
            out.append(self.d[s:end].decode("utf-8", "replace"))
        return out

    def _node_at(self, off):
        t = self.d[off]
        return self._container(t, off)

    def _container(self, t, off):
        if t == 0xC0:  # array
            n = self._u24(off + 1)
            types = self.d[off + 4:off + 4 + n]
            vals_off = off + 4 + ((n + 3) & ~3)
            return [self._value(types[i], vals_off + 4 * i) for i in range(n)]
        if t == 0xC1:  # hash with string keys
            n = self._u24(off + 1)
            out = {}
            for i in range(n):
                e = off + 4 + 8 * i
                key_idx = self._u24(e)
                vt = self.d[e + 3]
                out[self.keys[key_idx]] = self._value(vt, e + 4)
            return out
        if t in (0x20, 0x21):  # hash dictionaries keyed by u32/u64 hashes (v7)
            n = self._u24(off + 1)
            wide = t == 0x21
            stride = 12 if wide else 8
            out = {}
            types_off = off + 4 + n * stride
            for i in range(n):
                e = off + 4 + stride * i
                if wide:
                    k = struct.unpack_from(self.e + "Q", self.d, e)[0]
                    vo = e + 8
                else:
                    k = self._u32(e)
                    vo = e + 4
                out[k] = self._value(self.d[types_off + i], vo)
            return out
        if t == 0xC3:  # mono-typed array (v7)
            n = self._u24(off + 1)
            vt = self.d[off + 4]
            vals_off = off + 8
            return [self._value(vt, vals_off + 4 * i) for i in range(n)]
        raise ValueError(f"unknown container type {t:#x} at {off:#x}")

    def _value(self, t, o):
        if t == 0xA0:
            return self.strings[self._u32(o)]
        if t == 0xA1 or t == 0xA2:  # binary / file data
            p = self._u32(o)
            n = self._u32(p)
            return self.d[p + 4:p + 4 + n]
        if t in (0xC0, 0xC1, 0x20, 0x21, 0xC3):
            return self._container(t, self._u32(o))
        if t == 0xD0:
            return self._u32(o) != 0
        if t == 0xD1:
            return struct.unpack_from(self.e + "i", self.d, o)[0]
        if t == 0xD2:
            return struct.unpack_from(self.e + "f", self.d, o)[0]
        if t == 0xD3:
            return self._u32(o)
        if t in (0xD4, 0xD5, 0xD6):
            p = self._u32(o)
            fmt = {0xD4: "q", 0xD5: "Q", 0xD6: "d"}[t]
            return struct.unpack_from(self.e + fmt, self.d, p)[0]
        if t == 0xFF:
            return None
        raise ValueError(f"unknown value type {t:#x}")


