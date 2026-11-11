#!/usr/bin/env python3
"""OoT Online's data mod, made from the player's own RomFS image: the other players' stand-in actors, OnlinePlayer_Adult and
OnlinePlayer_Child, and the resource size table they need. Ported from TOTK Royale's tools/make_royale_standin.py and
tools/restbl.py (the same engine family, the same lessons; see that project's MODLOG "The stand-ins get their own actor").

    make_online_mod.py <RomFS.img> <out dir>        then run the game with DM_ROMFS_OVERLAY=<out dir>

Why a new actor: the game's own PouchPlayer_<age> is the inventory screen's Link; the game recreates it after warps with a small
memory budget (RSDB/ActorInfo InstanceHeapSize 1,234,248 adult / 1,112,560 child) and its own poses, and stand-ins made of it
fight that. OnlinePlayer_<age> is PouchPlayer_<age>'s pack with:
  - ASRef -> ?AS/Player_<age>.root.asb (Link's own 471 animation commands; a file the game already has),
  - ASInfoRef -> Link's ASInfo (copied in): his sequence needs its bigger node and slot buffers,
  - ELinkRef / SLinkRef -> Link's (copied in), with Link's ELink / SLink user names in the ActorInfo row: footsteps, swings,
  - AIInfoRef -> "": the inventory Link's AI follows the local pouch (in TOTK it made every stand-in wear what WE picked up);
and rows in RSDB/ActorInfo (PouchPlayer's, with Link's InstanceHeapSize: 10,245,064 adult / 5,692,448 child) and
RSDB/GameActorInfo, inserted in name order (both tables are sorted by __RowId). The size table gets entries for the new pack,
the files new or changed inside it (5000 + 3x size + 1 KiB: a too-small entry crashed TOTK when the actor was created), and
the two bigger tables. Everything written is derived from the player's game: it stays on their Mac.

Quick start (no cutscene on the way in; TOTK Royale did the same with its own new game): a new game in the OoT Online app does not
sit through the opening. The game's startup scripts (Sequence/*.ainb) get three same-length text swaps, the way ~/oot-openworld's
test start proved works (tools/make_overlay.py --test-start): the first area (FirstBancParam) becomes HyruleField_Child instead of
KokiriForest_Child; the opening's 8-minute movie event, Opening_LoadingKokiriForest, becomes DefaultParentEvent (the game's own
30-frame do-nothing event; the opening script only waits for it to start); and the scene reserved for after the first load,
Opening_Introduction (Navi and the Deku Tree in Link's house), is blanked so the Field's own arrival runs. A loaded save is not
affected: those scripts run for a new game only. Only the OoT Online app loads this overlay; online play keeps to a copy of the save.
"""
import bisect, copy, os, pathlib, re, struct, sys, zlib
import zstandard
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(pathlib.Path.home() / 'nxnative/dm/tools'))
import romfs, romfs_read
from totkwrite import read_byml, read_sarc, write_byml, write_sarc_oot as write_sarc   # OoT's archive layout (byte-exact on the game's own packs)

ACTOR_INFO = 'RSDB/ActorInfo.Product.111.rstbl.byml.zs'
GAME_ACTOR_INFO = 'RSDB/GameActorInfo.Product.111.rstbl.byml.zs'
RESTBL = 'System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs'
MARGIN = 1024


def pick(img, folder, rx):   # the one file of a table in this game version's RomFS (the update names them Product.111; another version may not)
    names = [e.name for e in romfs.scandir(img + '/' + folder) if re.fullmatch(rx, e.name)]
    assert len(names) == 1, f'{folder}: {len(names)} files match {rx}: {names[:4]}'
    return f'{folder}/{names[0]}'


def main():
    global ACTOR_INFO, GAME_ACTOR_INFO, RESTBL
    img, out = sys.argv[1], pathlib.Path(sys.argv[2]).expanduser()
    ACTOR_INFO = pick(img, 'RSDB', r'ActorInfo\.Product\.\d+\.rstbl\.byml\.zs')
    GAME_ACTOR_INFO = pick(img, 'RSDB', r'GameActorInfo\.Product\.\d+\.rstbl\.byml\.zs')
    RESTBL = pick(img, 'System/Resource', r'ResourceSizeTable\.Product\.\d+\.Nin_Ounce_NVN2\.rsizetable\.zs')
    with romfs.open(img + '/Pack/ZsDic.pack.zs', 'rb') as f: zsdic = romfs_read.sarc_files(zstandard.ZstdDecompressor().decompress(f.read(), max_output_size=1 << 24))
    D = {k: zstandard.ZstdCompressionDict(v) for k, v in zsdic.items()}
    written = {}   # rel path -> decompressed bytes we wrote (for the size table)

    def put(rel, data, dic):
        comp = zstandard.ZstdCompressor(level=19, dict_data=D[dic] if dic else None).compress(data)
        assert zstandard.ZstdDecompressor(**({'dict_data': D[dic]} if dic else {})).decompress(comp, max_output_size=len(data) + 1) == data
        p = out / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(comp); written[rel] = data

    actor_rows = read_byml(romfs_read.read(img, ACTOR_INFO))
    game_rows = read_byml(romfs_read.read(img, GAME_ACTOR_INFO))
    for age in ('Adult', 'Child'):
        name, pouch, link = f'OnlinePlayer_{age}', f'PouchPlayer_{age}', f'Player_{age}'
        pack = read_sarc(romfs_read.read(img, f'Pack/Actor/{pouch}.pack.zs'))
        donor = read_sarc(romfs_read.read(img, f'Pack/Actor/{link}.pack.zs'))
        donor_ap = read_byml(donor[f'Actor/{link}.engine__actor__ActorParam.bgyml'])['Components']
        ap = read_byml(pack.pop(f'Actor/{pouch}.engine__actor__ActorParam.bgyml'))
        comps = ap['Components']
        comps['ASRef'] = f'?AS/{link}.root.asb'
        as_info = 'Component/ASInfo/Player.engine__component__ASInfo.bgyml'
        pack[as_info] = donor[as_info]; comps['ASInfoRef'] = '?' + as_info
        for key in ('ELinkRef', 'SLinkRef'):
            ref = donor_ap.get(key)
            if ref: pack[ref.lstrip('?')] = donor[ref.lstrip('?')]; comps[key] = ref
        comps['AIInfoRef'] = ''
        pack[f'Actor/{name}.engine__actor__ActorParam.bgyml'] = write_byml(ap)
        put(f'Pack/Actor/{name}.pack.zs', write_sarc(pack), 'pack.zsdic')
        print(f'Pack/Actor/{name}.pack.zs: {pouch} with {link}\'s animation set, ASInfo and links, no AI ({len(pack)} files)')
        lrow = next(r for r in actor_rows if r['__RowId'] == link)

        def add(rows, edit):
            rows[:] = [r for r in rows if r.get('__RowId') != name]
            src = next(r for r in rows if r.get('__RowId') == pouch)
            row = copy.deepcopy(src); row['__RowId'] = name; row.pop('ActorName', None); edit(row)
            keys = [r['__RowId'] for r in rows]; assert keys == sorted(keys), 'table not sorted by name'
            rows.insert(bisect.bisect_left(keys, name), row)

        def actor_edit(row):
            row['InstanceHeapSize'] = max(row.get('InstanceHeapSize', 0), lrow['InstanceHeapSize'])
            for k in ('ELinkUserName', 'SLinkUserName'):
                if k in lrow: row[k] = lrow[k]
        add(actor_rows, actor_edit); add(game_rows, lambda row: None)

    # ---- OnlineNavi: each stand-in's own fairy (the client keeps her at its shoulder). The cutscene Navi (Dm_Npc_Navi: her model, glow and
    # sounds; she shares the screen with the real Navi in cutscenes) with the gameplay Navi's animation sequence (Npc_Navi: Wait, Fly, Turn...,
    # the cutscene one has none: its events animate her) and no AI, so she never goes after our own Link
    navi, dm, gp = 'OnlineNavi', 'Dm_Npc_Navi', 'Npc_Navi'
    pack = read_sarc(romfs_read.read(img, f'Pack/Actor/{dm}.pack.zs'))
    donor = read_sarc(romfs_read.read(img, f'Pack/Actor/{gp}.pack.zs'))
    donor_ap = read_byml(donor[f'Actor/{gp}.engine__actor__ActorParam.bgyml'])['Components']
    ap = read_byml(pack.pop(f'Actor/{dm}.engine__actor__ActorParam.bgyml')); comps = ap.setdefault('Components', {})
    for key in ('ASRef', 'ASInfoRef', 'AnimationRef'):
        ref = donor_ap.get(key)
        if not ref: continue
        comps[key] = ref; inner = ref.lstrip('?')
        if inner in donor and inner not in pack: pack[inner] = donor[inner]
    comps['AIInfoRef'] = ''
    # no physics either: the client places her every frame, and with the cutscene Navi's character controller one stayed caught on the
    # drawbridge's pillar over the moat while her Link walked off (runs/vid13)
    comps['PhysicsRef'] = ''
    pack[f'Actor/{navi}.engine__actor__ActorParam.bgyml'] = write_byml(ap)
    put(f'Pack/Actor/{navi}.pack.zs', write_sarc(pack), 'pack.zsdic')
    print(f'Pack/Actor/{navi}.pack.zs: {dm} with {gp}\'s animation sequence, no AI, no physics ({len(pack)} files)')
    nrow = next(r for r in actor_rows if r['__RowId'] == gp)
    for rows, heap in ((actor_rows, True), (game_rows, False)):
        rows[:] = [r for r in rows if r.get('__RowId') != navi]
        row = copy.deepcopy(next(r for r in rows if r.get('__RowId') == dm)); row['__RowId'] = navi; row.pop('ActorName', None)
        if heap: row['InstanceHeapSize'] = max(row.get('InstanceHeapSize', 0), nrow.get('InstanceHeapSize', 0))
        keys = [r['__RowId'] for r in rows]; assert keys == sorted(keys), 'table not sorted by name'
        rows.insert(bisect.bisect_left(keys, navi), row)
    for rel, rows in ((ACTOR_INFO, actor_rows), (GAME_ACTOR_INFO, game_rows)):
        data = write_byml(rows); assert read_byml(data) == rows
        put(rel, data, 'zs.zsdic'); print(f'{rel}: OnlinePlayer_Adult, _Child and OnlineNavi rows ({len(rows)} rows)')

    # ---- the resource size table (RESTBL v1: "RESTBL", u32 version 1, u32 name length 160, u32 crc count, u32 name count;
    # sorted (crc32(path), size) pairs; then (char[160] path, u32 size) for colliding paths). The path is the RomFS path without .zs
    table = bytearray(romfs_read.read(img, RESTBL))
    assert table[:6] == b'RESTBL'
    version, name_len, n_crc, n_name = struct.unpack_from('<IIII', table, 6); assert version == 1 and name_len == 160
    crc_slot = {struct.unpack_from('<I', table, 22 + 8 * i)[0]: 22 + 8 * i + 4 for i in range(n_crc)}
    names_at = 22 + 8 * n_crc; name_slot = {}
    for i in range(n_name):
        at = names_at + i * (name_len + 4); name_slot[table[at:at + name_len].split(b'\0')[0].decode()] = at + name_len
    added, changed = {}, []
    def need_entry(key, need):
        slot = name_slot.get(key) or crc_slot.get(zlib.crc32(key.encode()))
        if slot is None:
            c = zlib.crc32(key.encode()); assert c not in added or added[c][1] == key, f'crc collision: {key}'
            added[c] = (max(need, added[c][0]) if c in added else need, key)
        elif struct.unpack_from('<I', table, slot)[0] < need:
            changed.append((key, struct.unpack_from('<I', table, slot)[0], need)); struct.pack_into('<I', table, slot, need)
    for rel, data in written.items():
        key = rel[:-3] if rel.endswith('.zs') else rel
        if key.startswith('Pack/Actor/') and key.endswith('.pack'):
            stock_rel = rel.replace('OnlinePlayer_', 'PouchPlayer_').replace('OnlineNavi', 'Dm_Npc_Navi')
            stock = read_sarc(romfs_read.read(img, stock_rel))
            for inner, body in read_sarc(data).items():
                if stock.get(inner) != body: need_entry(inner, 5000 + 3 * len(body) + MARGIN)
        need_entry(key, len(data) + MARGIN)
    if added:
        pairs = [struct.unpack_from('<II', table, 22 + 8 * i) for i in range(n_crc)] + [(c, size) for c, (size, _) in added.items()]
        pairs.sort(); rest = table[22 + 8 * n_crc:]
        table = bytearray(table[:22]) + b''.join(struct.pack('<II', c, s) for c, s in pairs) + rest
        struct.pack_into('<I', table, 14, len(pairs))
    for key, old, new in changed: print(f'  size table: {key} {old} -> {new}')
    for _, (size, key) in sorted(added.items(), key=lambda kv: kv[1][1]): print(f'  size table: {key} new, {size}')
    comp = zstandard.ZstdCompressor(level=19).compress(bytes(table))
    p = out / RESTBL; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(comp)
    print(f'{RESTBL}: {len(changed)} raised, {len(added)} added -> {out}')
    quick_start(img, out)


def quick_start(img, out):   # the new game's startup scripts: same-length swaps, padded with zeros (string pools keep their offsets)
    swaps = [(b'Work/Banc/GameBancParam/KokiriForest_Child.game__banc__GameBancParam.gyml', b'Work/Banc/GameBancParam/HyruleField_Child.game__banc__GameBancParam.gyml'),
             (b'Opening_LoadingKokiriForest', b'DefaultParentEvent'),
             (b'Opening_Introduction', b'')]
    # Title.module stays as it is: with its first area swapped too the title screen lost its 3D background (runs/r18, black behind the logo)
    for path in ('Sequence/FormalSequence.root.ainb', 'Sequence/NormalTitle.module.ainb', 'Sequence/Opening.module.ainb'):
        b = romfs_read.read(img, path); done = []
        for old, new in swaps:
            k = b.count(old + b'\0')
            if not k: continue
            b = b.replace(old + b'\0', new.ljust(len(old), b'\0') + b'\0'); done.append(f'{old.decode().split("/")[-1].split(".")[0]} x{k}')
        assert done, f'{path}: none of the startup strings found (another game version?)'
        p = out / path; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b)
        print(f'{path}: quick start ({", ".join(done)})')


if __name__ == '__main__':
    main()
