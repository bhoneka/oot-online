#!/usr/bin/env python3
# Reads one file out of a player's own RomFS image (nxnative's RomFS.img), undoing the game's zstd (with its ZsDic
# dictionaries) when the name ends in .zs. Used to take the game's own fonts and UI art for OoT Online's screens.
#   romfs_read.py <RomFS.img> <path in the RomFS> [out]      (no out: prints the size and the first bytes)
import sys, struct, pathlib
from compression import zstd
sys.path.insert(0, str(pathlib.Path.home() / 'nxnative' / 'dm' / 'tools'))
import romfs

def sarc_files(b):
    assert b[:4] == b'SARC', b[:4]
    bom = b[6:8]; e = '<' if bom == b'\xff\xfe' else '>'
    data_off = struct.unpack_from(e + 'I', b, 0xc)[0]
    sfat = 0x14; n = struct.unpack_from(e + 'H', b, sfat + 6)[0]
    nodes = [struct.unpack_from(e + 'IIII', b, sfat + 0xc + 16 * i) for i in range(n)]
    sfnt = sfat + 0xc + 16 * n; names_at = sfnt + 8
    out = {}
    for h, attr, s, t in nodes:
        name = None
        if attr & 0x01000000:
            o = names_at + (attr & 0xffff) * 4; name = b[o:b.index(b'\0', o)].decode()
        out[name or f'{h:08x}'] = b[data_off + s:data_off + t]
    return out

_dicts = None
def dicts(img):
    global _dicts
    if _dicts is None:
        with romfs.open(img + '/Pack/ZsDic.pack.zs', 'rb') as f: pack = zstd.decompress(f.read())
        _dicts = {k: zstd.ZstdDict(v) for k, v in sarc_files(pack).items()}
    return _dicts

def read(img, path):
    with romfs.open(img + '/' + path.lstrip('/'), 'rb') as f: raw = f.read()
    if not path.endswith('.zs'): return raw
    name = path.rsplit('/', 1)[-1]
    order = ['bcett.byml.zsdic'] if name.endswith('.bcett.byml.zs') else ['pack.zsdic'] if name.endswith('.pack.zs') else []
    for d in order + ['zs.zsdic', None]:
        try:
            return zstd.decompress(raw) if d is None else zstd.decompress(raw, zstd_dict=dicts(img)[d])
        except Exception: continue
    raise SystemExit(f'cannot decompress {path}')

if __name__ == '__main__':
    img, path = sys.argv[1], sys.argv[2]
    b = read(img, path)
    if len(sys.argv) > 3: pathlib.Path(sys.argv[3]).write_bytes(b); print(f'{len(b)} bytes -> {sys.argv[3]}')
    else: print(len(b), 'bytes'); print(b[:64].hex(' '))
