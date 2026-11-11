#!/usr/bin/env python3
# The game's own fonts out of a player's own RomFS image, as plain OpenType/TrueType files for OoT Online's screens.
# Font2/Scalable/*.bfotf|bfttf (Switch 2): an 8-byte header (magic 0x1A879BD9, then the length) and the font, both
# XORed with the repeating bytes a6 01 85 02 (found from the file: the first word decodes to 'OTTO' / 0x00010000,
# the length word to the file size - 8). Output stays on the player's own Mac (derived from their dump).
#   game_fonts.py <RomFS.img> <out dir> [name substring ...]
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import romfs_read
KEY = bytes.fromhex('a6018502')

def unwrap(b):
    if b[:4] != bytes.fromhex('d99b871a'): raise ValueError('not a scrambled Switch font')
    body = bytes(x ^ KEY[i & 3] for i, x in enumerate(b[4:]))
    n = int.from_bytes(body[:4], 'big')
    font = body[4:4 + n]
    if font[:4] not in (b'OTTO', b'\x00\x01\x00\x00', b'true', b'ttcf'): raise ValueError('key did not fit: ' + font[:4].hex())
    return font

if __name__ == '__main__':
    img, out = sys.argv[1], pathlib.Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
    want = sys.argv[3:]
    sys.path.insert(0, str(pathlib.Path.home() / 'nxnative' / 'dm' / 'tools')); import romfs
    for e in sorted(romfs.scandir(img + '/Font2/Scalable'), key=lambda e: e.name):
        if want and not any(w.lower() in e.name.lower() for w in want): continue
        font = unwrap(romfs_read.read(img, 'Font2/Scalable/' + e.name))
        ext = '.otf' if font[:4] == b'OTTO' else '.ttf'
        dst = out / (e.name.split('.')[0] + ext); dst.write_bytes(font)
        print(f'{len(font):>9}  {dst.name}')
