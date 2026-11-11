#!/usr/bin/env python3
# The server's respawn points for an area: the game's own StartPos markers there (where it puts Link when he enters),
# leaving out the ones with a special state (swimming, climbing). Read from the player's own RomFS image; written to
# the server's data/spawns.json, keyed by the area hash the game side reports: murmur3_32 (seed 0) of the area's name without the
# age (HyruleField for HyruleField_Child and _Adult: the network treats the two ages' versions as one place), and, for older
# clients, of each Banc name with its age.
#   make_spawns.py <RomFS.img> <spawns.json> [Area ...]      (default: HyruleField)
import sys, json, math, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent)); sys.path.insert(0, str(pathlib.Path.home() / 'nxnative/dm/tools'))
import romfs_read, byml

def mm3(s, seed=0):
    d = s.encode(); h = seed; c1, c2 = 0xcc9e2d51, 0x1b873593; n = len(d) & ~3
    for i in range(0, n, 4):
        k = int.from_bytes(d[i:i + 4], 'little'); k = (k * c1) & 0xffffffff; k = ((k << 15) | (k >> 17)) & 0xffffffff; k = (k * c2) & 0xffffffff
        h ^= k; h = ((h << 13) | (h >> 19)) & 0xffffffff; h = (h * 5 + 0xe6546b64) & 0xffffffff
    k = 0; t = d[n:]
    if len(t) >= 3: k ^= t[2] << 16
    if len(t) >= 2: k ^= t[1] << 8
    if t: k ^= t[0]; k = (k * c1) & 0xffffffff; k = ((k << 15) | (k >> 17)) & 0xffffffff; k = (k * c2) & 0xffffffff; h ^= k
    h ^= len(d); h ^= h >> 16; h = (h * 0x85ebca6b) & 0xffffffff; h ^= h >> 13; h = (h * 0xc2b2ae35) & 0xffffffff; h ^= h >> 16
    return h

def yaw16(rot):   # the forward axis of an XYZ Euler rotation (radians), on the ground plane -> 0..65535 as the game side sends it
    x, y, z = rot; cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    # R = Rz * Ry * Rx; forward = R * (0, 0, 1)
    fx = cz * sy * cx + sz * sx; fz = cy * cx
    a = math.atan2(fx, fz) % (2 * math.pi)
    return int(round(a / (2 * math.pi) * 65536)) & 0xffff

if __name__ == '__main__':
    img, out = sys.argv[1], pathlib.Path(sys.argv[2]); areas = sys.argv[3:] or ['HyruleField']
    spawns = json.loads(out.read_text()) if out.exists() else {}
    for area in areas:
        both = []
        for age in ('Adult', 'Child'):
            pts = []
            for part in ('Static', 'Dynamic'):
                try: doc = byml.Byml(romfs_read.read(img, f'Banc/{area}/{area}_{age}_{part}.bcett.byml.zs')).root
                except SystemExit: continue
                except Exception: continue
                for a in doc.get('Actors', []):
                    if a.get('Gyaml') != 'StartPos': continue
                    dyn = a.get('Dynamic') or {}
                    if dyn.get('PlayerState'): continue        # swimming / climbing starts
                    if any(w in a.get('Name', '') for w in ('MiniGame', 'Bowling', 'Treasure', 'Shooting', 'Fishing')): continue   # inside a minigame
                    t = a['Translate']; pts.append([round(t[0], 2), round(t[1], 2), round(t[2], 2), yaw16(a.get('Rotate', [0, 0, 0])), a.get('Name', '')])
            if pts:
                scene = f'{area}_{age}'
                spawns[str(mm3(scene))] = [p[:4] for p in pts]
                print(f'{scene} ({mm3(scene):#010x}): {len(pts)} spawn points, e.g. ' + ', '.join(p[4] for p in pts[:4]))
                both += [p[:4] for p in pts if p[:4] not in both]
        if both:   # the shared place: both ages' points, once each
            spawns[str(mm3(area))] = both
            print(f'{area} ({mm3(area):#010x}, both ages): {len(both)} spawn points')
    out.parent.mkdir(parents=True, exist_ok=True); out.write_text(json.dumps(spawns, indent=1)); print('->', out)
