#!/usr/bin/env python3
# Puts OoT Online's game-side module into a local nxnative checkout (default ~/nxnative): copies the client sources into
# dm/host and makes four small edits, each checked against the exact text it replaces (a changed upstream file stops
# the script instead of being patched blindly). Run it again after pulling nxnative: it is idempotent.
#   python3 -I integrate_nxnative.py [nxnative dir]        --check: only report what would change
import pathlib, shutil, sys

HERE = pathlib.Path(__file__).resolve().parent
CLIENT = HERE.parent / 'client'
args = [a for a in sys.argv[1:] if not a.startswith('--')]
NX = pathlib.Path(args[0] if args else pathlib.Path.home() / 'nxnative').expanduser()
CHECK = '--check' in sys.argv
HOST = NX / 'dm' / 'host'

# client file -> its name in dm/host (hle_*.c are picked up by the Makefile's wildcard)
COPIES = {'hle_mp.c': 'hle_mp.c', 'mp_net.c': 'hle_mp_net.c', 'mp_net.h': 'mp_net.h', 'mp_ui.h': 'mp_ui.h', 'mp_game.h': 'mp_game.h',
          'mp_overlay_mac.m': 'mp_overlay_mac.m', 'hle_mp_host.c': 'hle_mp_host.c'}
for g in sorted((CLIENT / 'games').glob('*.c')) if (CLIENT / 'games').exists() else []:
    COPIES[f'games/{g.name}'] = f'hle_mp_game_{g.stem}.c'

EDITS = [
    (NX / 'dm' / 'games' / '0400486024DEC000' / 'patches.py',
     "    dynamic_resolution(ctx)\n",
     "    dynamic_resolution(ctx)\n    oot_online_frame_call(ctx)   # OoT Online's call of its host module once a frame (the end of this file)\n"),
    (HOST / 'Makefile',
     'SRC=main.c loader.c hle.c crash.c probe.c thread.c save_sync.c memstat.m haptics_apple.m pad_switch2_apple.m $(sort $(wildcard hle_*.c)) front_mac.m web_apple.m entry.S',
     'SRC=main.c loader.c hle.c crash.c probe.c thread.c save_sync.c memstat.m haptics_apple.m pad_switch2_apple.m $(sort $(wildcard hle_*.c)) front_mac.m mp_overlay_mac.m web_apple.m entry.S'),
    (HOST / 'Makefile',
     'home_quit.h home_quit_mac.h ctrl_prompt.h front.h $(GFX_H)',
     'home_quit.h home_quit_mac.h ctrl_prompt.h front.h mp_net.h mp_ui.h mp_game.h $(GFX_H)'),
    (HOST / 'main.c',
     '    dm_cheats_init();   // the Atmosphère cheat files for this build',
     '    { void dm_mp_init(void); dm_mp_init(); }   // OoT Online (hle_mp.c): the network client when DM_MP_SERVER is set\n    dm_cheats_init();   // the Atmosphère cheat files for this build'),
    (NX / 'dm' / 'gfx' / 'nvn' / 'hle_nvn.c',
     '    dm_cheats_present();   // Atmosphère cheat files (hle_cheats.c): enabled cheats apply once per presented frame; the ~60 Hz idle thread only covers gaps',
     '    dm_cheats_present();   // Atmosphère cheat files (hle_cheats.c): enabled cheats apply once per presented frame; the ~60 Hz idle thread only covers gaps\n'
     '    { extern void dm_mp_present(void) __attribute__((weak)); if (dm_mp_present) dm_mp_present(); }   // OoT Online (hle_mp.c), when the host links it'),
    (HOST / 'front_mac.m',
     '#import <QuartzCore/CAMetalLayer.h>',
     '#import <QuartzCore/CAMetalLayer.h>\n#include "mp_ui.h"   // OoT Online\'s overlay (mp_overlay_mac.m): chat box, kill feed, name tags'),
    (HOST / 'front_mac.m',
     '            if (e.window != win) return e;   // Only swallow keys for the game window; other windows such as the software keyboard input box receive typing as usual',
     '            if (e.window != win) return e;   // Only swallow keys for the game window; other windows such as the software keyboard input box receive typing as usual\n'
     '            if (mp_ui_editing()) return e;   // OoT Online\'s chat box has the keyboard (the game\'s buttons were let go when it opened)\n'
     '            if (e.type == NSEventTypeKeyDown && getenv("DM_MP_SERVER") && *getenv("DM_MP_SERVER") && mp_ui_key(e)) return nil;   // T or / opens it'),
    (HOST / 'front_mac.m',
     '        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *e) {',
     '        if (getenv("DM_MP_SERVER") && *getenv("DM_MP_SERVER")) mp_ui_attach(win, input_release, input_resume);   // OoT Online\'s overlay over the game\n'
     '        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *e) {'),
    (HOST / 'hle_hid.c',
     '    if (atomic_load(&g_hold)) { b = 0; memset(a, 0, sizeof a); }   // an applet is in front: the game reads nothing (the latches were taken all the same)',
     '    if (!pl) { extern void dm_mp_input_filter(uint64_t *b, int32_t a[4]) __attribute__((weak)); if (dm_mp_input_filter) dm_mp_input_filter(&b, a); }   // OoT Online (hle_mp.c): the lobby page and the chat box hold player 1\n'
     '    if (atomic_load(&g_hold)) { b = 0; memset(a, 0, sizeof a); }   // an applet is in front: the game reads nothing (the latches were taken all the same)'),
    (HOST / 'dm.h',
     '       OBS_MAX = 27 };',
     '       OBS_MP_TICK = 27, OBS_MP_EVENT = 28, OBS_MP_GAMEOVER = 29, OBS_MP_SHOOT = 30,   // OoT Online (hle_mp.c): the frame tick, event requests, the game-over check, shots\n'
     '       OBS_MAX = 31 };'),
]

PATCHES = NX / 'dm' / 'games' / '0400486024DEC000' / 'patches.py'
OBS_MARK = '# ---- OoT Online: observation gates (tools/integrate_nxnative.py) ----'
OBS_BLOCK = OBS_MARK + '''
# Three gates for OoT Online's host module (dm/host/hle_mp.c); without its callbacks attached (no DM_MP_SERVER) each goes in and out once.
# Sites from ~/oot-online/re/REPORT.md, original words read from v131072's main.
OBS = {'e0f3ccfaf50065ae4ea01eb5aeb13b1a': [
    (0xd7a820, 28, 'b940b808', 'OoT Online event filter: EventMgr::requestEvent entry (ldr w8, [x0, #0xb8]); a refused event gets a stand-in manager whose flag bit 11 ends the request'),
    (0x4ae664, 29, '38686a68', 'OoT Online game over: PlayerComponent::calc loads the flag byte P+0x1c34 (ldrb w8, [x19, x8]); bit 6 = the game-over event requested'),
    (0x2eb2a44, 30, 'a9bd7bfd', 'OoT Online shots: ShooterComponent::shoot, second instruction (stp x29, x30, [sp, #-0x30]!); x0 the shooter, x1 the ShootInfo'),
], '33fa6c2c23848282a77e8b88a0a93f48': [   # v65536, the base game (~/oot-online/re/PORT_v65536.md: the same sites, the same instructions)
    (0xd3d2b0, 28, 'b940b808', 'OoT Online event filter: EventMgr::requestEvent entry (ldr w8, [x0, #0xb8])'),
    (0x569ca4, 29, '38686a68', 'OoT Online game over: PlayerComponent::calc loads the flag byte P+0x1c34 (ldrb w8, [x19, x8])'),
    (0x2e5d604, 30, 'a9bd7bfd', 'OoT Online shots: ShooterComponent::shoot, second instruction (stp x29, x30, [sp, #-0x30]!)'),
]}

# OoT Online's frame call (frame_step above calls this): once a frame, on the game's main thread and its own stack, a call of the host
# function whose address hle_mp.c puts at frame step state page +0x800 (0 = no call). A gate cannot be used for it: a gate handler runs
# on nxnative's gate stack, where game code that makes a system call (any lock) is fatal, and the multiplayer calls the game's actor
# creation; player 1's input read, which runs on the game's stack, is on another thread during play. The call goes at the frame-ratio
# function's second instruction (str d8, [sp, #-0x40]!, after pacibsp), with x0..x17 and q0..q7 kept (the arguments; the rest are the
# callee's or dead at a function's entry; the host function keeps x19..x29 and d8..d15 itself). Found by the frame-ratio function's
# fingerprint, as the frame step stub is, so it follows that function into other builds.
OOT_ONLINE_SAVE = [0xa9ae7bfd, 0xa90107e0, 0xa9020fe2, 0xa90317e4, 0xa9041fe6, 0xa90527e8, 0xa9062fea, 0xa90737ec, 0xa9083fee, 0xa90947f0,
                   0xad0507e0, 0xad060fe2, 0xad0717e4, 0xad081fe6]   # stp x29,x30,[sp,#-0x120]!; stp x0..x17; stp q0..q7 (clang)
OOT_ONLINE_CALL = [0xf9440210, 0xb4000050, 0xd63f0200,   # ldr x16,[x16,#0x800]; cbz x16,+8; blr x16
                   0xad481fe6, 0xad4717e4, 0xad460fe2, 0xad4507e0, 0xa94947f0, 0xa9483fee, 0xa94737ec, 0xa9462fea, 0xa94527e8, 0xa9441fe6,
                   0xa94317e4, 0xa9420fe2, 0xa94107e0, 0xa8d27bfd]   # ldp back, ldp x29,x30,[sp],#0x120 (clang)
def oot_online_frame_call(ctx, orig=0xfc1c0fe8):   # orig: the function's second instruction (v131072 str d8, [sp, #-0x40]!; v65536 #-0x50)
    here, emit, site_at, put, state = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    site = ctx.base + 4
    site_at(site, orig.to_bytes(4, 'little').hex())
    start = here()
    for w in OOT_ONLINE_SAVE: emit(w)
    emit(ADRP(16, here(), state))
    for w in OOT_ONLINE_CALL: emit(w)
    emit(orig)                  # the original str d8
    emit(B(here(), site + 4))
    put(site, [B(site, start)], 'OoT Online: a call of the host module once a frame (frame step state page +0x800; 0 = none), then the original str d8')

def frame_step_v65536(ctx):   # OoT Online: upstream frame_step (above) at v65536's division, main+0x6b5900 + 0x108 (re/PORT_v65536.md)
    here, emit, fs_site, fs_put, FS_STATE_OFF, base = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state, ctx.base
    site = base + 0x108
    fs_site(site, '2818201e')   # fdiv s8,s1,s0
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211, 0x340001b1, 0xb9402211, 0x34000171, 0x1e230220, 0xf9401a10, 0x8b110210): emit(w)
    emit(ADRP(17, here(), FS_STATE_OFF))
    for w in (0xf9001a30, 0x1e210808, 0x52a84e10, 0x1e270200, 0x1e201908): emit(w)
    emit(B(here(), site + 4))
    emit(0x1e201828); emit(B(here(), site + 4))
    fs_put(site, [B(site, start)], 'frame ratio (v65536): the host step x base rate / 60 instead of base rate / the game frame rate')
    oot_online_frame_call(ctx, 0xfc1b0fe8)

MATCH_FRAME_STEP['main'].append({'name': MATCH_FRAME_STEP['main'][0]['name'], 'size': 0x9a8,   # one patch, so the same name (nso_match.locate_stub)
    'fingerprints': ['777177e79e876b03bb01e602619c9fbde6b80741d9a256fca0dddb3cabb760d8'], 'stub': frame_step_v65536})   # v65536 main+0x6b5900
'''

def main():
    if not HOST.is_dir(): sys.exit(f'not an nxnative checkout: {NX}')
    changed = 0
    for src, dst in COPIES.items():
        a, b = CLIENT / src, HOST / dst
        if not b.exists() or a.read_bytes() != b.read_bytes():
            print(('would copy ' if CHECK else 'copy ') + f'{src} -> dm/host/{dst}'); changed += 1
            if not CHECK: shutil.copyfile(a, b)
    for path, old, new in EDITS:
        t = path.read_text()
        if new in t: continue
        n = t.count(old)
        if n != 1: sys.exit(f'[RED] {path.relative_to(NX)}: expected the original text once, found it {n} times: nxnative changed here; edit by hand')
        print(('would edit ' if CHECK else 'edit ') + str(path.relative_to(NX))); changed += 1
        if not CHECK: path.write_text(t.replace(old, new))
    pt = PATCHES.read_text()
    if OBS_MARK not in pt:
        if '\nOBS = ' in pt or '\nOBS=' in pt: sys.exit(f'[RED] {PATCHES.relative_to(NX)} already has an OBS table: merge OoT Online\'s gates by hand')
        print(('would add ' if CHECK else 'add ') + 'the OoT Online gates and frame call to ' + str(PATCHES.relative_to(NX))); changed += 1
        if not CHECK: PATCHES.write_text(pt.rstrip('\n') + '\n\n' + OBS_BLOCK)
    elif pt[pt.index(OBS_MARK):].rstrip('\n') != OBS_BLOCK.rstrip('\n'):   # ours, from an earlier version of this script: replaced (it is the file's last block)
        print(('would replace ' if CHECK else 'replace ') + 'the OoT Online block of ' + str(PATCHES.relative_to(NX))); changed += 1
        if not CHECK: PATCHES.write_text(pt[:pt.index(OBS_MARK)].rstrip('\n') + '\n\n' + OBS_BLOCK)
    print('nothing to do' if not changed else ('would change' if CHECK else 'changed') + f' {changed} things in {NX}')

main()
