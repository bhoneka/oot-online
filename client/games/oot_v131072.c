// OoT Online's knowledge of The Legend of Zelda: Ocarina of Time (Switch 2), update v131072, main build e0f3ccfa...
// Addresses are main-relative, from the static map in re/REPORT.md. Nothing here runs on another build of the game.
// Two threads come in (mp_game.h): the present thread only READS game memory, through safe reads that cannot fault;
// the game's own thread (the tick from player 1's input read, hle_mp.c) may CALL the game's functions.
//
// Switches (environment): DM_MP_PROBE=1 logs Link's state once a second.
//   DM_MP_TEST=gamedata | warp | spawn | tunic | hp   one dynamic check of the map (re/REPORT.md, "Recommended order"):
//     gamedata: GameData getBool(Sequence_IsChildhood) from the game thread, logged
//     warp:     20 s in, Link 5 m up through warpToMatrix (watch him fall)
//     spawn:    20 s in, PouchPlayer_<age> 2 m in front of Link; it circles him for 15 s, then is deleted
//     tunic:    20 s in, adult Link's tunic set to the Goron Tunic (GameData + equipFromGameData)
//     hp:       20 s in, one heart of damage through LifeComponent's pending counter
//     project:  every 2 s, Link's head through the game's world-to-screen on the "Main" and "3D" layers, logged
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <time.h>
#include "mp_net.h"
#include "mp_game.h"
uint64_t dm_guest_frame(void);   // the host's presented-frame count (hle_os.c)

#ifndef MP_GAME_LOG
#define MP_GAME_LOG(...) do { fprintf(stderr, "[dm] mp-oot: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

// ---- the host's module table (dm.h); declared here so this file needs no host headers
typedef struct module_s module_t;
const char *dm_mp_main_build(void);      // hle_mp_host.c: main's build ID as hex ("" unknown)
uint8_t *dm_mp_main_base(void);          // hle_mp_host.c: main's image base in the host address space

// The game's code and data addresses, per build of main (re/REPORT.md, re/ui/CODE.md, re/AREA_CHANGE.md, re/ACTOR_CREATE.md for the
// update v131072; re/PORT_v65536.md and re/port_v65536.tsv for the base game v65536: every function by its fingerprint or identical body,
// every global from the matched code; no struct offset differs between the two). The build is picked at start by main's build ID
typedef struct {
    const char *build; uint32_t build4;   // main's build ID (hex), and its first 4 bytes (sent to the server)
    uint64_t g_scene_module, g_actor_mgr, g_gamedata, g_banc_mgr, g_ui_mgr, g_ui, g_event_mgr, g_gfx_mgr, v_message_request, v_actorlink_empty;
    uint64_t f_getbool, f_setstring64, f_equip_from_gd, f_create_actor, f_force_set_matrix, f_warp_to_matrix, f_request_delete, f_as_play, f_bb_add_string,
             f_load_screen, f_unload_screen, f_is_open, f_is_closed, f_opening, f_closing, f_set_message, f_anim_player, f_anim_pose, f_focus, f_control_by_name,
             f_decided, f_queue_request, f_arg_ctor, f_comp_request, f_link_reset, f_cur_event, f_getstring64, f_setint, f_layer, f_w2s,
             f_watch_take, f_watch_free_failed, f_watch_cancel;
    uint64_t g_pouch, g_playing_info,   // the ranged weapon (re/ITEMS.md): game::PouchMgr, PlayingInfo
             f_pouch_push, f_pouch_actor_info, f_pouch_category, f_get_assigned, f_set_assigned, f_setbool, f_getint,
             f_req_equip, f_armor_default;   // a stand-in's tunic (the game's equipFromGameData path for slot 8)
} oot_build;
static const oot_build BUILDS[] = {
    { "e0f3ccfaf50065ae4ea01eb5aeb13b1a", 0xe0f3ccfau,   // v131072 (the update)
      0x4e97648, 0x4e91888, 0x4e842a8, 0x4e919d0, 0x4e84518, 0x4e982e0, 0x4e94840, 0x4e950a8, 0x4c220e0, 0x4974b20,
      0x20d9c0, 0xd24e50, 0x2df6c80, 0x372a70, 0xc6fd60, 0xc6fb30, 0xa6490, 0x2022a0, 0xfe3fe0,
      0x3bc7230, 0x3bc7240, 0xa687f0, 0xbad3c0, 0x377af70, 0x377afb0, 0xa67d30, 0x43d870, 0x2d2d90, 0x86ffc0, 0xa76360,
      0x3773090, 0xfe6110, 0xd79f10, 0xf61460, 0x20b6f0, 0xbcc0b0, 0xcb0ea0, 0x163b70, 0x17ce50, 0x29d40d0,
      0x36fbd0, 0x36fde0, 0xb2fa30,
      0x4e78e28, 0x4e7cfa8, 0x110d570, 0x58ed10, 0x13fdd0, 0x17b6e0, 0x1211c00, 0x682b20, 0x837070,
      0x1013f20, 0x10aceb0 },
    { "33fa6c2c23848282a77e8b88a0a93f48", 0x33fa6c2cu,   // v65536 (the base game, no update)
      0x4e271e8, 0x4e21428, 0x4e13e48, 0x4e21570, 0x4e140b8, 0x4e27e80, 0x4e243e0, 0x4e24c48, 0x4bbc4d8, 0x49101f8,
      0x27e8e0, 0xd01360, 0x2da26f0, 0x97feb0, 0xc35730, 0xc35500, 0xc4fa0, 0x2b63a0, 0xb4ac90,
      0x3b69e30, 0x3b69e40, 0x3719220, 0xb02d80, 0x3719240, 0x3719280, 0x4c82d0, 0x4c6360, 0x2f7640, 0x876b90, 0xfe8090,
      0x3710670, 0xfbdc90, 0xd3c9a0, 0xf32150, 0x27c630, 0xbf8ee0, 0xdb5670, 0x4211f0, 0x2fd4a0, 0x2981b80,
      0x97d000, 0x97d210, 0xa1f060,
      // the item functions, mapped 2026-11-11 with re/tools/port_v65536.py's matchers: push 0x11378f0 by 9 calls from 3 matched callers
      // (its body changed), the others by body or callers; PouchMgr's +0x3bb0 / +0x3bc0 checked in its init and readers
      0x4e08a30, 0x4e0cb60, 0x11378f0, 0x473490, 0x47aaf0, 0x59be30, 0xa01f40, 0x691500, 0xba7d00,
      0xb15580, 0x107d9a0 },   // (the two equip calls: by their callers, 746/747 and 408/422 words equal)
};
static const oot_build *B = &BUILDS[0];
// the names the code below uses, read from the running build's table
#define G_SCENE_MODULE (B->g_scene_module)
#define G_ACTOR_MGR (B->g_actor_mgr)
#define G_GAMEDATA (B->g_gamedata)
#define G_BANC_MGR (B->g_banc_mgr)
#define G_UI_MGR (B->g_ui_mgr)
#define G_UI (B->g_ui)
#define G_EVENT_MGR (B->g_event_mgr)
#define V_MESSAGE_REQUEST (B->v_message_request)
#define V_ACTORLINK_EMPTY (B->v_actorlink_empty)
#define F_GETBOOL (B->f_getbool)
#define F_SETSTRING64 (B->f_setstring64)
#define F_EQUIP_FROM_GD (B->f_equip_from_gd)
#define F_CREATE_ACTOR (B->f_create_actor)
#define F_FORCE_SET_MATRIX (B->f_force_set_matrix)
#define F_WARP_TO_MATRIX (B->f_warp_to_matrix)
#define F_REQUEST_DELETE (B->f_request_delete)
#define F_AS_PLAY (B->f_as_play)
#define F_BB_ADD_STRING (B->f_bb_add_string)
#define F_LOAD_SCREEN (B->f_load_screen)
#define F_IS_OPEN (B->f_is_open)
#define F_IS_CLOSED (B->f_is_closed)
#define F_SET_MESSAGE (B->f_set_message)
#define F_ANIM_PLAYER (B->f_anim_player)
#define F_ANIM_POSE (B->f_anim_pose)
#define F_FOCUS (B->f_focus)
#define F_CONTROL_BY_NAME (B->f_control_by_name)
#define F_DECIDED (B->f_decided)
#define F_QUEUE_REQUEST (B->f_queue_request)
#define F_ARG_CTOR (B->f_arg_ctor)
#define F_COMP_REQUEST (B->f_comp_request)
#define F_LINK_RESET (B->f_link_reset)
#define F_CUR_EVENT (B->f_cur_event)
#define F_GETSTRING64 (B->f_getstring64)
#define F_SETINT (B->f_setint)
#define F_WATCH_TAKE (B->f_watch_take)
#define F_WATCH_FREE_FAILED (B->f_watch_free_failed)
#define G_POUCH (B->g_pouch)
#define G_PLAYING_INFO (B->g_playing_info)
#define F_POUCH_PUSH (B->f_pouch_push)
#define F_POUCH_ACTOR_INFO (B->f_pouch_actor_info)
#define F_POUCH_CATEGORY (B->f_pouch_category)
#define F_GET_ASSIGNED (B->f_get_assigned)
#define F_SET_ASSIGNED (B->f_set_assigned)
#define F_SETBOOL (B->f_setbool)
#define F_GETINT (B->f_getint)
#define F_REQ_EQUIP (B->f_req_equip)
#define F_ARMOR_DEFAULT (B->f_armor_default)
#define F_WATCH_CANCEL (B->f_watch_cancel)   // the running build's addresses (ready() picks it; until then the update's, never used)
static int g_ok = -1;                    // 1: a build the map knows
static double now_s(void);
// A switch read again about once a second, so a test run can change it while the game runs (tools/ctl.sh <run> "env NAME=value")
typedef struct { const char *name; double read_at; char value[32]; } env_switch;
static const char *env_now(env_switch *s) {
    double t = now_s();
    if (s->read_at <= 0 || t - s->read_at >= 1) { s->read_at = t; const char *e = getenv(s->name); snprintf(s->value, sizeof s->value, "%s", e ? e : ""); }
    return s->value;
}
// The stand-in actor: PouchPlayer (the game's own inventory Link) or OnlinePlayer (OoT Online's, Link's animation set; needs the data
// mod of tools/make_online_mod.py in DM_ROMFS_OVERLAY). DM_MP_STANDIN=OnlinePlayer picks it
static int standin_online(void) { static env_switch s = { .name = "DM_MP_STANDIN" }; return !strcmp(env_now(&s), "OnlinePlayer"); }
static const char *standin_name(int child) {
    return standin_online() ? (child ? "OnlinePlayer_Child" : "OnlinePlayer_Adult") : (child ? "PouchPlayer_Child" : "PouchPlayer_Adult");
}
static uint8_t *M;                       // main's base
static int ready(void) {
    if (g_ok < 0) {
        const char *b = dm_mp_main_build(); M = dm_mp_main_base(); g_ok = 0;
        for (size_t i = 0; b && M && i < sizeof BUILDS / sizeof *BUILDS; i++) if (!strncasecmp(b, BUILDS[i].build, 32)) { B = &BUILDS[i]; g_ok = 1; }
        MP_GAME_LOG("%s (main build %s)", g_ok ? (B == &BUILDS[0] ? "OoT v131072: game-side features on" : "OoT v65536 (no update): game-side features on")
                                                : "[RED] not a build OoT Online knows: game-side features off", b ? b : "?");
    }
    return g_ok;
}

// ---- safe reads (present thread): a fault-free copy, so a stale pointer reads as "not there" instead of crashing
static int rd(uint64_t addr, void *out, size_t n) {
    if (addr < 0x10000) return 0;
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)addr, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n;
}
static uint64_t rp(uint64_t addr) { uint64_t v = 0; return rd(addr, &v, 8) ? v : 0; }
static uint32_t r32(uint64_t addr) { uint32_t v = 0; rd(addr, &v, 4); return v; }

// ---- murmur3_32, seed 0 (the game's name hash, main+0x1739e0)
static uint32_t mm3(const char *s) {
    const uint8_t *d = (const uint8_t *)s; size_t n = strlen(s); uint32_t h = 0, k;
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        memcpy(&k, d + i, 4); k *= c1; k = (k << 15) | (k >> 17); k *= c2;
        h ^= k; h = (h << 13) | (h >> 19); h = h * 5 + 0xe6546b64;
    }
    k = 0;
    switch (n & 3) { case 3: k ^= d[i + 2] << 16; /* fallthrough */ case 2: k ^= d[i + 1] << 8; /* fallthrough */ case 1: k ^= d[i]; k *= c1; k = (k << 15) | (k >> 17); k *= c2; h ^= k; }
    h ^= (uint32_t)n; h ^= h >> 16; h *= 0x85ebca6b; h ^= h >> 13; h *= 0xc2b2ae35; h ^= h >> 16;
    return h;
}

// ---- the map (re/REPORT.md)
enum {
    A_STATE = 0x2c, A_COMP = 0x270, A_POS = 0x31c, A_ROT = 0x328, A_VEL = 0x3a0,
    C_AS = 8, C_EQUIP = 64, C_LIFE = 87, C_PLAYER = 107,
};
// The player actor, or 0 (present thread: safe reads; the game thread may read it the same way)
static uint64_t player_actor(void) {
    uint64_t sm = rp((uint64_t)M + G_SCENE_MODULE); if (!sm) return 0;
    uint64_t scene = rp(sm + 0x1f0); if (!scene) return 0;
    if (r32(scene + 0x50) < 16) return 0;
    uint64_t ram = rp(rp(scene + 0x58) + 15 * 8); if (!ram || !rp(ram + 0x30)) return 0;
    for (int i = 0; i < 2; i++) {   // getPlayerLink (main+0x37cca0): the child list's link first, then the adult's
        uint64_t link = rp(ram + (i ? 0x50 : 0x40)); if (!link) continue;
        uint32_t id = r32(link + 0x10); if (id == 0xffffffffu) continue;
        uint64_t ld = rp(link + 8); if (!ld || r32(ld + 0x48) != id) continue;
        uint64_t actor = rp(ld + 0x40); if (!actor) continue;
        uint32_t st = r32(actor + A_STATE);
        if (st == 4) return actor;
    }
    return 0;
}
static uint64_t comp(uint64_t actor, int i) { return rp(rp(actor + A_COMP) + 8ull * i); }

// The animation the network carries: an index into this list (both games share it). Link's own command names map onto it
static const char *ANIM[] = { "Wait", "Move", "Jump", "Fall", "Land", "Rolling", "BowCharge", "BowShoot", "CutS1", "CutS2", "CutS3",
    "DamageS", "DamageM", "DamageL", "Dead", "ClimbWait", "ClimbMove", "SwimWait", "SwimMove", "LockOnWait", "LockOnMove", "Crouch",
    "HorseWait", "HorseMove", "LadderWait", "LadderUp", "Push", "Pull", "BackFlip", "SideStep", "Turn", "Brake" };
enum { N_ANIM = sizeof ANIM / sizeof *ANIM };
static uint16_t anim_index(const char *cmd) {
    if (!cmd || !*cmd) return 0;
    for (int i = 0; i < N_ANIM; i++) if (!strcmp(cmd, ANIM[i])) return (uint16_t)i;
    if (!strncmp(cmd, "Swim", 4)) return 18;
    if (!strncmp(cmd, "Climb", 5) || !strncmp(cmd, "CliffHold", 9)) return 16;
    if (!strncmp(cmd, "Horse", 5) || !strncmp(cmd, "Rideon", 6)) return 23;
    if (!strncmp(cmd, "Bow", 3)) return 6;
    if (!strncmp(cmd, "Cut", 3)) return 8;
    if (!strncmp(cmd, "Damage", 6)) return 11;
    if (!strncmp(cmd, "LockOn", 6)) return 20;
    if (!strncmp(cmd, "Ladder", 6)) return 24;
    if (strstr(cmd, "Dead")) return 14;
    return 0;
}
static int slot_command(uint64_t actor, int si, char *out, size_t cap) {   // AS slot si's command name (QueryASGetCurrentCommand, main+0xcb06a0, reads slot 0)
    out[0] = 0;
    uint64_t as = comp(actor, C_AS); if (!as) return 0;
    uint64_t ctrl = rp(rp(as + 0x18) + 0x18); if (!ctrl || (int32_t)r32(ctrl + 0x18) <= si) return 0;
    uint64_t slot = rp(rp(ctrl + 0x20) + 0x48ull * si); if (!slot) return 0;
    uint64_t name = rp(slot + 0x10); if (!name) return 0;
    if (!rd(name, out, cap - 1)) { size_t i = 0; while (i < cap - 1 && rd(name + i, out + i, 1) && out[i]) i++; out[i] = 0; }
    out[cap - 1] = 0; out[strnlen(out, cap - 1)] = 0;
    return out[0] != 0;
}
static int current_command(uint64_t actor, char *out, size_t cap) { return slot_command(actor, 0, out, cap); }   // Link's own: slot 0
static int banc_name(char *out, size_t cap) {   // getCurrentNormalizedBancName (main+0x58aac0): the basename of the banc path
    out[0] = 0;
    uint64_t bm = rp((uint64_t)M + G_BANC_MGR); if (!bm) return 0;
    uint64_t p = rp(bm + 0x40); if (!p) return 0;
    char path[160]; size_t i = 0;
    for (; i < sizeof path - 1 && rd(p + i, path + i, 1) && path[i]; i++);
    path[i] = 0; if (!i) return 0;
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    snprintf(out, cap, "%s", b); char *dot = strchr(out, '.'); if (dot) *dot = 0;
    return out[0] != 0;
}
static int life(uint64_t actor, int *hp, int *max) {
    uint64_t L = comp(actor, C_LIFE); if (!L) return 0;
    *hp = (int32_t)r32(rp(L + 0x1c10) + 8); *max = (int32_t)r32(rp(L + 0x1c18) + 8);
    return *max > 0 && *max < 1000;
}

uint32_t mp_game_build(void) { ready(); return B->build4; }
// The area as the network knows it: young and adult Link play in two versions of each area (HyruleField_Child, HyruleField_Adult)
// with the same coordinates; the network treats them as one place so the two ages meet (each stand-in shows its own player's age)
static uint32_t shared_scene(const char *banc) {
    char s[64]; snprintf(s, sizeof s, "%s", banc); size_t n = strlen(s);
    if (n > 6 && !strcmp(s + n - 6, "_Child")) s[n - 6] = 0; else if (n > 6 && !strcmp(s + n - 6, "_Adult")) s[n - 6] = 0;
    return mm3(s);
}
int mp_game_ready(void) { return ready(); }   // the server compares it as the first 8 hex digits of the build ID

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
// Seconds of game time in a frame-exact recording (DM_FSTEP_ONE: frames / 60, however slowly a run that saves every frame goes), else
// real time: what arrows in flight and Navi's bobbing move by, so they keep pace with the game's own animation in footage
static double game_now(void) { static int one = -1; if (one < 0) one = getenv("DM_FSTEP_ONE") && atoi(getenv("DM_FSTEP_ONE")); return one ? (double)dm_guest_frame() / 60.0 : now_s(); }
static int g_probe = -1;
static char g_scene_name[64];

int mp_game_read_local(mp_local_state *st) {
    if (!ready()) return 0;
    uint64_t a = player_actor(); if (!a) return 0;
    float pos[3], rot[9], vel[3];
    if (!rd(a + A_POS, pos, sizeof pos) || !rd(a + A_ROT, rot, sizeof rot)) return 0;
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) return 0;
    rd(a + A_VEL, vel, sizeof vel);
    char cmd[64]; current_command(a, cmd, sizeof cmd);
    char bn[64]; if (!banc_name(bn, sizeof bn)) return 0;
    int hp = 0, max = 0; life(a, &hp, &max);
    memset(st, 0, sizeof *st);
    st->scene = shared_scene(bn);
    st->x = pos[0]; st->y = pos[1]; st->z = pos[2];
    // the facing: the rotation's forward axis (local +z), row-major
    float fx = rot[2], fz = rot[8];
    double yaw = atan2(fx, fz); if (yaw < 0) yaw += 2 * M_PI;
    st->yaw = (uint16_t)lrint(yaw / (2 * M_PI) * 65536.0);
    st->anim = anim_index(cmd);
    if (!strncmp(cmd, "Swim", 4)) st->flags |= MP_PF_SWIM;
    if (!strncmp(cmd, "Climb", 5) || !strncmp(cmd, "Ladder", 6)) st->flags |= MP_PF_CLIMB;
    if (!strncmp(cmd, "Bow", 3)) st->flags |= MP_PF_AIM;
    if (!strcmp(cmd, "Fall") || !strcmp(cmd, "Jump")) st->flags |= MP_PF_AIR;
    st->hp = (uint8_t)(hp < 0 ? 0 : hp > 255 ? 255 : hp); st->max_hp = (uint8_t)(max > 255 ? 255 : max);
    if ((r32(comp(a, C_PLAYER) + 0x1c5d) & 0x101) == 0x101) st->equip |= MP_EQ_CHILD;   // young Link: the others' stand-in of him is the child one
    snprintf(g_scene_name, sizeof g_scene_name, "%s", bn);
    if (g_probe < 0) g_probe = getenv("DM_MP_PROBE") && atoi(getenv("DM_MP_PROBE"));
    if (g_probe) {
        static double last; double t = now_s();
        if (t - last >= 1) {
            last = t;
            MP_GAME_LOG("probe: actor %#llx in %s (%#x) at %.2f %.2f %.2f yaw %u vel %.2f %.2f %.2f hp %d/%d anim '%s' -> %u",
                        (unsigned long long)a, bn, st->scene, pos[0], pos[1], pos[2], st->yaw, vel[0], vel[1], vel[2], hp, max, cmd, st->anim);
        }
    }
    return 1;
}

// ---------------------------------------------------------------- the game thread: calls into the game
typedef bool (*f_getbool)(void *mgr, bool *out, uint32_t hash);
typedef bool (*f_setstring64)(void *mgr, const char **safestr, uint32_t hash);
typedef void (*f_equip_from_gd)(void *eu, uint32_t slot);
typedef bool (*f_create)(void *mgr, const char **name, void *arg, void *watcher, int prio, void *pre, void *file, void *fn, uint64_t b, uint32_t *result, void **preout);
typedef void (*f_force_set_matrix)(void *actor, const float *m34, bool b);
typedef void (*f_warp_to_matrix)(void *actor, const float *m34, bool direct);
typedef bool (*f_request_delete)(void *proc, int why);
static void *fn(uint64_t off) { return (void *)(M + off); }
// An actor created with a watcher waits at state 3 until its creator takes it (re/ACTOR_CREATE.md; runs/seventh, eighth: every actor the
// mod created sat at 3, invisible, for minutes): take(w, wake 1) starts it (state 4 on the next pass) and hands the watcher back to the
// engine, which reuses it, so it is never read again. A failed request (status 4) is freed; one given up is cancelled
typedef void *(*f_watch_take)(void *w, bool wake, void *info);
typedef bool (*f_watch_free)(void *w);
typedef bool (*f_watch_cancel)(uint64_t *slot, bool b);
static uint64_t watch_take(uint64_t *slot) {   // the actor once created (status 3), its slot cleared; 0 = not yet; a failed one is freed (*slot = 0, returns 0)
    if (!*slot) return 0;
    uint32_t status = r32(*slot) & 0xffff;
    if (status == 3) { uint64_t a = (uint64_t)((f_watch_take)fn(F_WATCH_TAKE))((void *)*slot, true, NULL); *slot = 0; return a; }
    if (status == 4) { ((f_watch_free)fn(F_WATCH_FREE_FAILED))((void *)*slot); *slot = 0; }
    return 0;
}
static void watch_cancel(uint64_t *slot) { if (*slot) { ((f_watch_cancel)fn(F_WATCH_CANCEL))(slot, true); *slot = 0; } }

static double g_t0, g_test_wait, g_test_last;
static char g_test[32];
static int g_test_done;
// The stand-in of the "spawn" test
static uint64_t g_watch;                 // CreateWatcher* the game fills
static uint64_t g_stand; static double g_stand_t;
static void apply_damage(int quarter_hearts, uint16_t attacker);
static void respawn_at(uint32_t scene, float x, float y, float z, uint16_t yaw, int own);
static uint16_t g_last_attacker = MP_NOBODY;
static int g_down;   // our Link "fell" to another player: waiting for the server's respawn

// ---------------------------------------------------------------- travel: Link to another area and arrival point
// re/AREA_CHANGE.md recipe B (static analysis; the first dynamic test is runs/seventh's "travel"): the area change the game's own exits
// end in is made by Sequence/EventToMain.module once an event ends while GameData Sequence_ReservedNextBanc is set. So: request the
// game's do-nothing event DefaultParentEvent through Link's EventPerformer (Link the starter, as for his own events), and in the same
// frame reserve the area and arrival point; the event ends after 30 frames, the game fades, loads and puts Link there. World tick only
// (the request takes the EventMgr's lock). A failed or dropped request clears the reservation, or the game's next event would warp Link
enum { H_RESERVED_BANC = 0xee27bf71u, H_RESERVED_POS = 0x0b570ab2u, H_RESERVED_BOOT = 0xa242ccc2u, H_RESERVED_STARTER = 0x3ad1fd51u,
       H_RESERVED_ROOM = 0x050249d2u, H_LOADING = 0xef2b60c2u, H_SCENE_PAUSE = 0x9a9506e7u, H_PAUSE = 0x604aebe1u, H_GAMEOVER_TRIG = 0x2fd0bc6cu,
       H_EVENT_BANC_PAUSE = 0xf96989b6u, H_CURRENT_BANC = 0x1d6189dau, H_CHILDHOOD = 0xcea220d3u };
typedef bool (*f_getstring64)(void *mgr, const char **out, uint32_t hash);
typedef void (*f_setint)(void *mgr, int32_t v, uint32_t hash);
static _Alignas(16) uint8_t g_ev_arg[0x1b20];   // EventRequestArg: built in place (it points into itself), never copied
static struct { int st; int t; char target[64], carrier[32]; } g_tr;   // st: 0 idle, 1 waiting for the event, 2 in it, 3 loading, 4 settling, 5 done, -1 failed
static int gd_bool(uint32_t h) { bool b = 0; return ((f_getbool)fn(F_GETBOOL))((void *)rp((uint64_t)M + G_GAMEDATA), &b, h) && b; }
static const char *event_now(void) {   // the playing event's name, "" when none
    uint64_t em = rp((uint64_t)M + G_EVENT_MGR); if (!em) return "";
    typedef const char **(*f_cur)(void *em); const char **ss = ((f_cur)fn(F_CUR_EVENT))((void *)em);
    return ss && *ss ? *ss : "";
}
static int can_travel(uint64_t link) {   // re/AREA_CHANGE.md 4.1
    uint64_t em = rp((uint64_t)M + G_EVENT_MGR);
    if (!em || !link || r32(link + A_STATE) != 4) return 0;
    if (rp(em + 0x68) || event_now()[0]) return 0;                    // an event plays
    uint64_t pend = rp(em + 0xc0); char c = 0; if (pend) rd(pend, &c, 1); if (c) return 0;   // a main event pending
    if (r32(em + 0xb8) & (0x1 | 0x8 | 0x800 | 0x4000)) return 0;   // requests off / refused / one queued
    return !gd_bool(H_LOADING) && !gd_bool(H_SCENE_PAUSE) && !gd_bool(H_PAUSE) && !gd_bool(H_GAMEOVER_TRIG) && !gd_bool(H_EVENT_BANC_PAUSE);
}
static void reserve(const char *banc, const char *pos) {
    void *gd = (void *)rp((uint64_t)M + G_GAMEDATA); const char *v;
    v = banc; ((f_setstring64)fn(F_SETSTRING64))(gd, &v, H_RESERVED_BANC);
    v = pos;  ((f_setstring64)fn(F_SETSTRING64))(gd, &v, H_RESERVED_POS);
    v = "";   ((f_setstring64)fn(F_SETSTRING64))(gd, &v, H_RESERVED_BOOT);
    v = "";   ((f_setstring64)fn(F_SETSTRING64))(gd, &v, H_RESERVED_STARTER);
    ((f_setint)fn(F_SETINT))(gd, -1, H_RESERVED_ROOM);
}
static void unreserve(void) { void *gd = (void *)rp((uint64_t)M + G_GAMEDATA); const char *v = ""; ((f_setstring64)fn(F_SETSTRING64))(gd, &v, H_RESERVED_BANC); }
// Start a trip to banc / arrival point (world tick). 1 = under way (poll with travel_tick), 0 = not now (can_travel said no, or refused)
static int travel(uint64_t link, const char *banc, const char *pos) {
    if (g_tr.st > 0 && g_tr.st < 5) return 0;
    if (!can_travel(link)) return 0;
    typedef void (*f_ctor)(void *arg); typedef bool (*f_req)(void *comp, void *arg, bool no_starter); typedef void (*f_reset)(void *link);
    ((f_ctor)fn(F_ARG_CTOR))(g_ev_arg);
    char *name = (char *)rp((uint64_t)g_ev_arg + 0x1a90); if (!name) return 0;
    snprintf(name, 0x80, "%s", "DefaultParentEvent");
    uint64_t perf = rp(rp(link + A_COMP) + 0x90);   // component 18: Link's EventPerformer
    bool ok = perf && ((f_req)fn(F_COMP_REQUEST))((void *)perf, g_ev_arg, false);
    for (int k = 0; k < 2; k++) { uint8_t *l = g_ev_arg + (k ? 0x1950 : 0x1968); *(uint64_t *)l = (uint64_t)M + V_ACTORLINK_EMPTY; ((f_reset)fn(F_LINK_RESET))(l); }
    MP_GAME_LOG("travel: DefaultParentEvent request %s (to %s at '%s')", ok ? "accepted" : "refused", banc, pos);
    if (!ok) return 0;
    reserve(banc, pos);   // the same frame: the event cannot end before these queued writes land
    g_tr.st = 1; g_tr.t = 0; snprintf(g_tr.target, sizeof g_tr.target, "%s", banc); snprintf(g_tr.carrier, sizeof g_tr.carrier, "DefaultParentEvent");
    return 1;
}
static void travel_tick(void) {   // world tick, once a frame (re/AREA_CHANGE.md 4.4)
    if (g_tr.st <= 0 || g_tr.st >= 5) return;
    uint64_t em = rp((uint64_t)M + G_EVENT_MGR); int prev = g_tr.st; g_tr.t++;
    switch (g_tr.st) {
    case 1: if (em && rp(em + 0x68) && !strcmp(event_now(), g_tr.carrier)) { g_tr.st = 2; g_tr.t = 0; } else if (g_tr.t > 120) { unreserve(); g_tr.st = -1; } break;
    case 2: if (gd_bool(H_LOADING)) { g_tr.st = 3; g_tr.t = 0; } else if (em && !rp(em + 0x68) && g_tr.t > 600) { unreserve(); g_tr.st = -1; } break;
    case 3: if (!gd_bool(H_LOADING)) { const char *cur = NULL; ((f_getstring64)fn(F_GETSTRING64))((void *)rp((uint64_t)M + G_GAMEDATA), &cur, H_CURRENT_BANC);
                char now[64] = ""; if (cur) { size_t i = 0; while (i < sizeof now - 1 && rd((uint64_t)cur + i, now + i, 1) && now[i]) i++; now[i] = 0; }
                g_tr.st = !strcmp(now, g_tr.target) ? 4 : -1; if (g_tr.st < 0) MP_GAME_LOG("travel: arrived in '%s', not %s", now, g_tr.target); } break;
    case 4: if (em && !rp(em + 0x68) && !(r32(em + 0xb8) & 0x4000)) g_tr.st = 5; break;
    }
    if (g_tr.st != prev) MP_GAME_LOG("travel: %s", g_tr.st == 2 ? "the carrier event plays" : g_tr.st == 3 ? "loading the area" : g_tr.st == 4 ? "arrived, settling" : g_tr.st == 5 ? "done" : "failed (reservation cleared)");
}
// DM_MP_FIELD_AGE=adult / child (tests only, on a test run's own save): that age's Field, which makes Link that age when he arrives
// (re/AREA_CHANGE.md: the banc's IsChildWhenChangeBanc switches him); unset: his own age's
static const char *field_banc(void) {
    static env_switch b = { .name = "DM_MP_FIELD_BANC" }; const char *eb = env_now(&b);
    if (eb[0]) return eb;   // (tests: another area for footage, e.g. KokiriForest_Child with DM_MP_FIELD_POS=ReturnFromDreamWorld)
    static env_switch s = { .name = "DM_MP_FIELD_AGE" }; const char *e = env_now(&s);
    if (!strcmp(e, "adult")) return "HyruleField_Adult";
    if (!strcmp(e, "child")) return "HyruleField_Child";
    return gd_bool(H_CHILDHOOD) ? "HyruleField_Child" : "HyruleField_Adult";
}
// The shared place's arrival point: in the open Field in front of the Castle Town drawbridge (both ages have it). runs/eighth:
// ReturnFromDreamWorld, the "central" one, is inside the Lon Lon Ranch house (the ranch is part of the Field's map). DM_MP_FIELD_POS overrides
static const char *field_pos(void) { static env_switch s = { .name = "DM_MP_FIELD_POS" }; const char *e = env_now(&s); return e[0] ? e : "HyruleField_FrontOfMarket"; }
int mp_game_go_to_field(void) {   // world tick: 1 = under way, or already in the Field; 0 = not now (an event, a menu, a load: ask again)
    if (!ready()) return 0;
    uint64_t a = player_actor(); if (!a) return 0;
    char bn[64]; if (!banc_name(bn, sizeof bn)) return 0;
    if (g_tr.st > 0 && g_tr.st < 5) return 1;
    if (!strncmp(bn, "HyruleField", 11) && !strcmp(bn, field_banc())) {   // already in the Field (this age's): near the hub (in front of the drawbridge, -0.04 150.66 32.16) stays,
        float q[3]; rd(a + A_POS, q, sizeof q);   // farther (a new game starts by the Kokiri Forest bridge, 340 m off) goes there
        float dx = q[0] + 0.04f, dz = q[2] - 32.16f;
        if (dx * dx + dz * dz < 150.f * 150.f || strcmp(field_pos(), "HyruleField_FrontOfMarket")) return 1;
    }
    return travel(a, field_banc(), field_pos());
}

// ---- the game's screens (re/ui/CODE.md): the screen manager, a screen by name, load / open / close, texts, buttons
typedef struct { const char *text; uint32_t bytes, pad; int64_t id; } ui_message;   // 0x18, as the game's (init 0xa66f70: {0, 0, -1})
typedef uint32_t (*f_set_message)(void *layout, const char *path, const ui_message *m, uint32_t one, void *zero);
typedef void (*f_screen_mode)(void *screen, int32_t mode);
typedef bool (*f_screen_bool)(void *screen);
typedef void *(*f_load_screen)(void *ui, uint32_t hash);
// The screen object of a row of RSDB/UIScreen (0 while not loaded); *index = its row, -1 = no such row (the game itself would use row 0)
static uint64_t screen_of(const char *name, int32_t *index) {
    *index = -1;
    uint64_t mgr = rp((uint64_t)M + G_UI_MGR), tbl = mgr ? rp(mgr + 0x48) : 0, vt = tbl ? rp(tbl) : 0; if (!vt) return 0;
    typedef int32_t (*f_index)(void *tbl, uint32_t hash);
    int32_t i = ((f_index)rp(vt + 0x50))((void *)tbl, mm3(name)); *index = i;
    if (i < 0 || (uint32_t)i >= r32(mgr + 8)) return 0;
    return rp(rp(mgr + 0x10) + 8ull * (uint32_t)i);
}
static void screen_call(uint64_t s, int slot, int32_t mode) { ((f_screen_mode)rp(rp(s) + slot))((void *)s, mode); }   // slot 0x28 open, 0x30 close
static uint32_t screen_text(uint64_t s, const char *path, const char *text) {   // only while the screen is closed (the game's own SystemWindow preset does it so)
    ui_message m = { text, (uint32_t)strlen(text), 0, -1 };
    return ((f_set_message)fn(F_SET_MESSAGE))((void *)rp(s + 0x28), path, &m, 1, NULL);
}
static const char *control_name(uint64_t ctl) { static char n[48]; n[0] = 0; uint64_t p = ctl ? rp(ctl + 0x18) : 0; for (size_t i = 0; p && i < sizeof n - 1 && rd(p + i, n + i, 1) && n[i]; i++) n[i + 1] = 0; return n; }
static double g_ui_t;
static atomic_int g_load_lobby;   // the UI tick wants SystemBG_00 / SystemWindow_00 loaded: the world tick does it (loading takes a lock)
static atomic_int g_unload_window; // the UI tick wants SystemWindow_00 unloaded (a fresh one after each choice): the world tick does it
// A text change on an open screen: the game's MessageRequest (vtable main+0x4c220e0, applied by the screen's own update with
// setMessage), queued with 0xfe6110(screen+0xb8, &req). One request object per text box; it may be queued again once +0x10 is 0
typedef struct { uint64_t vtable, state, queue, mgr; int32_t index; uint16_t flags; uint8_t pad[2]; const char *path; ui_message msg; } ui_msg_req;   // 0x48
_Static_assert(sizeof(ui_msg_req) == 0x48, "MessageRequest is 0x48 bytes");
static int screen_text_open(uint64_t s, ui_msg_req *rq, const char *path, const char *text) {   // 1 queued, 0 the last one is still pending
    if (rq->queue) return 0;
    memset(rq, 0, sizeof *rq);
    rq->vtable = (uint64_t)M + V_MESSAGE_REQUEST; rq->state = 1; rq->mgr = rp((uint64_t)M + G_UI_MGR); rq->index = -1;
    rq->path = path; rq->msg = (ui_message){ text, (uint32_t)strlen(text), 0, -1 };
    typedef void (*f_queue)(void *queue, void *req); ((f_queue)fn(F_QUEUE_REQUEST))((void *)(s + 0xb8), rq);
    return 1;
}

// DM_MP_TEST, read again about once a second: a run goes through every check without a restart (tools/ctl.sh <run> "env DM_MP_TEST=spawn").
// A test given at the start waits 20 s for the game to settle, one switched to in play 2 s
static void test_switch(void) {
    static env_switch s = { .name = "DM_MP_TEST" }; static int started;
    const char *v = env_now(&s);
    if (started && !strcmp(v, g_test)) return;
    if (g_stand && r32(g_stand + A_STATE) >= 3 && r32(g_stand + A_STATE) <= 6) ((f_request_delete)fn(F_REQUEST_DELETE))((void *)g_stand, 0);   // the last test's stand-in
    watch_cancel(&g_watch); g_stand = 0; g_test_done = 0; g_test_last = -1e9; g_t0 = now_s(); g_test_wait = started ? 2 : 20;
    if (started) MP_GAME_LOG("test switched to '%s'", v);
    started = 1; snprintf(g_test, sizeof g_test, "%s", v);
}
static void test_tick(int ui_ctx) {   // ui_ctx: the UI tick (the frame gate: no system calls) or the world tick (player 1's input read)
    test_switch();
    if (!*g_test || g_test_done == 2) return;
    if ((g_test[0] == 'u' && g_test[1] == 'i') != ui_ctx) return;   // ui*: UI tests run in the UI tick, everything else in the world tick
    double t = now_s() - g_t0;
    uint64_t a = player_actor();
    if (!strcmp(g_test, "gamedata")) {
        if (t - g_test_last < 2) return; g_test_last = t;
        bool child = 0; bool ok = ((f_getbool)fn(F_GETBOOL))((void *)rp((uint64_t)M + G_GAMEDATA), &child, 0xcea220d3u);   // Sequence_IsChildhood
        char tn[64] = ""; pthread_getname_np(pthread_self(), tn, sizeof tn);
        MP_GAME_LOG("test gamedata: getBool(Sequence_IsChildhood) -> ok %d value %d (thread '%s')", ok, child, tn);
        return;
    }
    if (!strcmp(g_test, "project") && a) {   // where the game's world-to-screen puts Link's head, on both camera layers
        if (t - g_test_last < 2) return; g_test_last = t;
        typedef void *(*f_layer)(void *list, uint32_t hash);
        typedef bool (*f_w2s)(void *layer, float *out, float x, float y, float z);
        uint64_t gm = rp((uint64_t)M + B->g_gfx_mgr); void *list = gm ? (void *)rp(gm + 0x38) : NULL;
        float p[3]; rd(a + A_POS, p, sizeof p);
        static const uint32_t L[2] = { 0x1b6ce343u, 0x62a726dbu };   // "Main", "3D"
        for (int i = 0; i < 2 && list; i++) {
            void *layer = ((f_layer)fn(B->f_layer))(list, L[i]);
            float out[2] = { -1, -1 }; bool ok = layer ? ((f_w2s)fn(B->f_w2s))(layer, out, p[0], p[1] + 1.8f, p[2]) : 0;
            float vp[8] = { 0 }; if (layer) rd((uint64_t)layer + 0x28, vp, sizeof vp);   // what the projection is given with it: the layer's viewport (w2s passes layer+0x28)
            MP_GAME_LOG("test project: layer %s %p -> %d (%.1f, %.1f); layer+0x28: %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f", i ? "3D" : "Main", layer, ok, out[0], out[1],
                        vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7]);
        }
        return;
    }
    if (t < g_test_wait || !a) return;
    if (!strcmp(g_test, "warp") && !g_test_done) {
        float r[9]; rd(a + A_ROT, r, sizeof r); float p[3]; rd(a + A_POS, p, sizeof p);
        float m[12] = { r[0], r[1], r[2], p[0], r[3], r[4], r[5], p[1] + 5.0f, r[6], r[7], r[8], p[2] };
        ((f_warp_to_matrix)fn(F_WARP_TO_MATRIX))((void *)a, m, false);
        MP_GAME_LOG("test warp: Link from %.2f %.2f %.2f to 5 m above", p[0], p[1], p[2]);
        g_test_done = 2;
    }
    if (!strcmp(g_test, "tunic") && !g_test_done) {
        const char *armor = "Armor_Adult_Goron";
        bool ok = ((f_setstring64)fn(F_SETSTRING64))((void *)rp((uint64_t)M + G_GAMEDATA), &armor, 0x008d7f0cu);   // PlayerStatus.EquipArmor
        uint64_t eu = comp(a, C_EQUIP);
        if (eu) ((f_equip_from_gd)fn(F_EQUIP_FROM_GD))((void *)eu, 8);
        MP_GAME_LOG("test tunic: setString64 -> %d, equipFromGameData(eu %#llx, 8)", ok, (unsigned long long)eu);
        g_test_done = 2;
    }
    if (!strcmp(g_test, "hp") && !g_test_done) {
        uint64_t L = comp(a, C_LIFE); int32_t pend = 0;
        if (L && rd(L + 0x228, &pend, 4)) { pend += 4; *(volatile int32_t *)(L + 0x228) = pend; }   // one heart, consumed by LifeComponent::update
        MP_GAME_LOG("test hp: +4 quarter hearts pending at %#llx", (unsigned long long)(L + 0x228));
        g_test_done = 2;
    }
    if (!strcmp(g_test, "travel") && !g_test_done) {   // to the shared place in Hyrule Field (field_pos: DM_MP_FIELD_POS or in front of Castle Town)
        if (travel(a, field_banc(), field_pos())) g_test_done = 2;
        else if (t - g_test_last > 2) { g_test_last = t; MP_GAME_LOG("test travel: not now (event %s'%s', em flags %#x)", "", event_now(), r32(rp((uint64_t)M + G_EVENT_MGR) + 0xb8)); }
    }
    if (!strcmp(g_test, "hit") && !g_test_done) {   // one heart through the path a player's arrow takes
        int hp = 0, max = 0; life(a, &hp, &max); apply_damage(4, MP_NOBODY);
        MP_GAME_LOG("test hit: 4 quarter hearts at %d/%d", hp, max); g_test_done = 2;
    }
    if (!strcmp(g_test, "down") && !g_test_done) {   // a hit that would end Link: one quarter heart left, DIED to the server, its RESPAWN heals him
        int hp = 0, max = 0; life(a, &hp, &max); apply_damage(hp + 4, MP_NOBODY);
        MP_GAME_LOG("test down: %d quarter hearts at %d/%d (down %d)", hp + 4, hp, max, g_down); g_test_done = 2;
    }
    if (!strcmp(g_test, "respawn") && !g_test_done) {   // heal and warp 3 m ahead, as the server's respawn does
        float p[3], r[9]; rd(a + A_POS, p, sizeof p); rd(a + A_ROT, r, sizeof r);
        double yaw = atan2(r[2], r[8]); if (yaw < 0) yaw += 2 * M_PI;
        respawn_at(shared_scene(g_scene_name), p[0] + 3 * r[2], p[1], p[2] + 3 * r[8], (uint16_t)lrint(yaw / (2 * M_PI) * 65536.0), 0);
        MP_GAME_LOG("test respawn: from %.2f %.2f %.2f to 3 m ahead in %s", p[0], p[1], p[2], g_scene_name); g_test_done = 2;
    }
    if (!strcmp(g_test, "ui1") && !g_test_done) {   // read only: the screen manager and three of the game's screens
        uint64_t mgr = rp((uint64_t)M + G_UI_MGR);
        MP_GAME_LOG("test ui1: manager %#llx, %u screens, ui %#llx", (unsigned long long)mgr, mgr ? r32(mgr + 8) : 0, (unsigned long long)rp((uint64_t)M + G_UI));
        static const char *names[] = { "SystemWindow_00", "SystemBG_00", "SimpleInfo_00", "NoSuchScreen_00" };
        for (int k = 0; k < 4; k++) {
            int32_t i; uint64_t sc = screen_of(names[k], &i); uint64_t vt = sc ? rp(sc) : 0;
            MP_GAME_LOG("test ui1: %s row %d screen %#llx vtable main+%#llx request %d state %u visible %u", names[k], i, (unsigned long long)sc,
                        (unsigned long long)(vt ? vt - (uint64_t)M : 0), sc ? (int8_t)(r32(sc + 0x1a4) >> 8) : 0, sc ? (r32(sc + 0x1a4) >> 16) & 0xff : 0,
                        sc ? r32(sc + 0x1a8) & 0xff : 0);
        }
        g_test_done = 2;
    }
    if (!strcmp(g_test, "ui2")) {   // the game's own info bar (SimpleInfo_00) with our sentence, 8 s, then closed
        static const char line[] = "OoT Online: hello from the game's own info bar";
        int32_t i; uint64_t sc = screen_of("SimpleInfo_00", &i);
        if (!sc) { if (t - g_test_last > 1) { g_test_last = t; MP_GAME_LOG("test ui2: SimpleInfo_00 not loaded (row %d)", i); } return; }
        if (!g_test_done && ((f_screen_bool)fn(F_IS_CLOSED))((void *)sc)) {
            uint32_t n = screen_text(sc, "T_Text_00", line); screen_call(sc, 0x28, 1);
            MP_GAME_LOG("test ui2: text set on %u boxes, open requested", n); g_test_done = 1; g_ui_t = t;
        } else if (g_test_done == 1) {
            if (t - g_test_last > 1) { g_test_last = t; MP_GAME_LOG("test ui2: open %d state %u", ((f_screen_bool)fn(F_IS_OPEN))((void *)sc), (r32(sc + 0x1a4) >> 16) & 0xff); }
            if (t - g_ui_t > 8) { screen_call(sc, 0x30, -1); MP_GAME_LOG("test ui2: close requested"); g_test_done = 2; }
        }
        return;
    }
    if (!strcmp(g_test, "ui0") && !g_test_done) {   // close the system window and its background (after ui3 / ui4)
        static const char *names[2] = { "SystemWindow_00", "SystemBG_00" };
        for (int k = 0; k < 2; k++) { int32_t i; uint64_t sc = screen_of(names[k], &i); if (sc && !((f_screen_bool)fn(F_IS_CLOSED))((void *)sc)) { screen_call(sc, 0x30, -1); MP_GAME_LOG("test ui0: %s close requested", names[k]); } }
        g_test_done = 2;
    }
    if (!strcmp(g_test, "ui4")) {   // the open system window's first label changed every 2 s through the game's request queue
        static ui_msg_req rq; static const char *T[3] = { "Tunic: Kokiri", "Tunic: Goron", "Tunic: Zora" }; static int k;
        int32_t i; uint64_t win = screen_of("SystemWindow_00", &i);
        if (!win || !((f_screen_bool)fn(F_IS_OPEN))((void *)win)) { if (t - g_test_last > 2) { g_test_last = t; MP_GAME_LOG("test ui4: needs the window of ui3 open (row %d, screen %#llx)", i, (unsigned long long)win); } return; }
        if (t - g_test_last > 2) { g_test_last = t; k = (k + 1) % 3; int q = screen_text_open(win, &rq, "Pa_Button_00/T_Btn_00", T[k]);
            MP_GAME_LOG("test ui4: '%s' %s (request +0x10 %#llx)", T[k], q ? "queued" : "still pending", (unsigned long long)rq.queue); }
        return;
    }
    if (!strcmp(g_test, "ui3")) {   // the game's own system window over its own dimmed background, our texts and three buttons (the lobby's look)
        static const char *names[2] = { "SystemBG_00", "SystemWindow_00" };
        uint64_t sc[2]; int32_t ix[2];
        for (int k = 0; k < 2; k++) {
            sc[k] = screen_of(names[k], &ix[k]);
            if (!sc[k]) { if (ix[k] >= 0 && t - g_test_last > 1) { g_test_last = t; atomic_store(&g_load_lobby, 1); MP_GAME_LOG("test ui3: %s to be loaded by the world tick (row %d)", names[k], ix[k]); } return; }
        }
        uint64_t win = sc[1], cm = rp(win + 0x38);
        if (!g_test_done) {
            if (!((f_screen_bool)fn(F_IS_CLOSED))((void *)win)) return;
            uint32_t n = screen_text(win, "T_Text_00", "Welcome to OoT Online. Pick your tunic, then Play.");
            n += screen_text(win, "Pa_Button_00/T_Btn_00", "Tunic: Kokiri");
            n += screen_text(win, "Pa_Button_01/T_Btn_00", "Name: Tester");
            n += screen_text(win, "Pa_Button_02/T_Btn_00", "Play");
            typedef void *(*f_anim)(void *layout, const char *name); typedef void (*f_pose)(void *p, float frame);
            void *bn = ((f_anim)fn(F_ANIM_PLAYER))((void *)rp(win + 0x28), "BtnNum"); if (bn) ((f_pose)fn(F_ANIM_POSE))(bn, 3.0f);
            typedef void *(*f_ctl)(void *cm, const char *name); typedef void (*f_focus)(void *s, void *ctl, int z);
            void *play = cm ? ((f_ctl)fn(F_CONTROL_BY_NAME))((void *)cm, "Pa_Button_02") : NULL; if (play) ((f_focus)fn(F_FOCUS))((void *)win, play, 0);
            screen_call(sc[0], 0x28, 1); screen_call(win, 0x28, 1);
            MP_GAME_LOG("test ui3: %u text boxes set, BtnNum %p, focus on %p, both open requested", n, bn, play);
            g_test_done = 1; g_ui_t = t; return;
        }
        if (t - g_test_last > 1) {   // each button's state byte (+0x38) and flags (+0x40): which value is "focused", which "decided"
            g_test_last = t; typedef void *(*f_dec)(void *cm); uint64_t d = cm ? (uint64_t)((f_dec)fn(F_DECIDED))((void *)cm) : 0;
            typedef void *(*f_ctl)(void *cm, const char *name); uint32_t st[3] = { 0 }, fl[3] = { 0 };
            static const char *btn[3] = { "Pa_Button_00", "Pa_Button_01", "Pa_Button_02" };
            for (int k = 0; k < 3 && cm; k++) { uint64_t c = (uint64_t)((f_ctl)fn(F_CONTROL_BY_NAME))((void *)cm, btn[k]); if (c) { st[k] = r32(c + 0x38); fl[k] = r32(c + 0x40); } }
            MP_GAME_LOG("test ui3: open %d state %u cm+0x28 %#llx decided '%s' | 00 %08x/%08x 01 %08x/%08x 02 %08x/%08x", ((f_screen_bool)fn(F_IS_OPEN))((void *)win),
                        (r32(win + 0x1a4) >> 16) & 0xff, (unsigned long long)(cm ? rp(cm + 0x28) : 0), control_name(d), st[0], fl[0], st[1], fl[1], st[2], fl[2]);
        }
        if (t - g_ui_t > 90) { screen_call(win, 0x30, -1); screen_call(sc[0], 0x30, -1); MP_GAME_LOG("test ui3: both close requested"); g_test_done = 2; }
        return;
    }
    if (!strncmp(g_test, "spawn", 5)) {   // spawn: a stand-in of Link's own age; spawn_adult / spawn_child: that age
        if (!g_test_done) {
            static uint8_t arg[0xa0] __attribute__((aligned(16)));
            memset(arg, 0, sizeof arg);
            float p[3], r[9]; rd(a + A_POS, p, sizeof p); rd(a + A_ROT, r, sizeof r);
            float fwd[3] = { r[2], r[5], r[8] };
            float pos[3] = { p[0] + 2 * fwd[0], p[1], p[2] + 2 * fwd[2] };
            float rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, sc[3] = { 1, 1, 1 };
            memcpy(arg + 0x00, pos, 12); memcpy(arg + 0x0c, rot, 36); memcpy(arg + 0x30, sc, 12);
            static const char *empty = ""; memcpy(arg + 0x40, &empty, 8);
            arg[0x4a] = 1; *(uint32_t *)(arg + 0x98) = 1 | 2 | 4;
            static const char *name; char bn[64]; banc_name(bn, sizeof bn);
            int child = !strcmp(g_test, "spawn_child") ? 1 : !strcmp(g_test, "spawn_adult") ? 0 : (r32(comp(a, C_PLAYER) + 0x1c5d) & 0x101) == 0x101;
            static env_switch any = { .name = "DM_MP_SPAWN_ACTOR" }; static char any_name[64];   // a test of any actor by its name
            const char *e = env_now(&any); if (e[0]) { snprintf(any_name, sizeof any_name, "%s", e); name = any_name; } else
            name = standin_name(child);
            g_watch = 0; uint32_t result = 0;
            bool ok = ((f_create)fn(F_CREATE_ACTOR))((void *)rp((uint64_t)M + G_ACTOR_MGR), &name, arg, &g_watch, 1, NULL, NULL, NULL, 0, &result, NULL);
            MP_GAME_LOG("test spawn: requestCreateActorAsync(%s) -> %d result %u watcher %#llx", name, ok, result, (unsigned long long)g_watch);
            g_test_done = 1; g_stand_t = t;
            return;
        }
        if (!g_stand && g_watch) {
            uint32_t status = r32(g_watch) & 0xffff;
            if (t - g_test_last > 1) { g_test_last = t; MP_GAME_LOG("test spawn: watcher status %u", status); }
            uint64_t actor = watch_take(&g_watch);
            if (actor) { g_stand = actor; MP_GAME_LOG("test spawn: taken: %#llx (state %u)", (unsigned long long)actor, r32(actor + A_STATE)); }
            else if (!g_watch) { MP_GAME_LOG("test spawn: the request failed (status %u)", status); g_test_done = 2; }
        }
        if (g_stand && t - g_test_last > 1) { g_test_last = t; float q[3] = { 0 }; rd(g_stand + A_POS, q, sizeof q);
            MP_GAME_LOG("test spawn: state %u at %.2f %.2f %.2f", r32(g_stand + A_STATE), q[0], q[1], q[2]); }
        if (g_stand) {
            float p[3]; rd(a + A_POS, p, sizeof p);
            double ang = (t - g_stand_t) * 0.8;
            float m[12] = { (float)cos(ang), 0, (float)sin(ang), p[0] + 3 * (float)cos(ang), 0, 1, 0, p[1], -(float)sin(ang), 0, (float)cos(ang), p[2] + 3 * (float)sin(ang) };
            ((f_force_set_matrix)fn(F_FORCE_SET_MATRIX))((void *)g_stand, m, false);
            if (t - g_stand_t > 15) {
                bool ok = ((f_request_delete)fn(F_REQUEST_DELETE))((void *)g_stand, 0);
                MP_GAME_LOG("test spawn: requestDelete -> %d", ok); g_stand = 0; g_test_done = 2;
            }
        }
    }
}

// ---------------------------------------------------------------- the live multiplayer (DM_MP_LIVE=1, once the checks pass)
// Stand-ins: one PouchPlayer per nearby player (the game's own Link model; dressed through the blackboard key the game's
// EquipmentUser reads), placed every frame with forceSetMatrix. The game deletes every actor on an area change, so a new
// area forgets them without deleting. Damage goes into LifeComponent's pending counter; a hit that would end Link is a
// heal + warp to the server's spawn instead of the game's Game Over (deaths from the world still take the game's own path).
#define MAX_PUPPETS 16
typedef struct { uint16_t id; uint64_t watch; uint64_t actor; int outfit, child; double asked; int anim; char name[32]; int dressed; double taken_at;
                 float lx, lz, spd; uint64_t lf; double lt; int moving, bb_said;   // puppet_speed: the last position, when, the speed it gives (m/s, smoothed)
                 double check_t; int replays;   // puppet_check: when its sequence was last checked, how often a command had to be played again
                 uint64_t navi_watch, navi; int navi_anim; uint8_t navi_arg[0xa0] __attribute__((aligned(16)));   // navi_tick: its own fairy
                 int weapon_on;   // its bow / slingshot made (puppet_weapon)
                 double down_t;   // when its player went down (it falls and lies there DOWN_HOLD s, then goes)
                 float aim_turn; double aim_t;   // its turn while it aims (AIM_TURN: its bow arm along its player's aim), and when last turned
                 uint8_t arg[0xa0] __attribute__((aligned(16)));
                 uint8_t bb[0x1918] __attribute__((aligned(16))); } Puppet;
static Puppet g_pup[MAX_PUPPETS];
static uint32_t g_pup_scene;
static int g_live = -1;
typedef bool (*f_bb_add_string)(void *info, const char **key, const char **value);
static const char *ARMOR[] = { "Armor_Adult_Normal", "Armor_Adult_Goron", "Armor_Adult_Zora" };

static int live(void) { static env_switch s = { .name = "DM_MP_LIVE" }; int on = atoi(env_now(&s)); if (on != g_live && g_live >= 0) MP_GAME_LOG("live %s", on ? "on" : "off"); g_live = on; return on; }
static int link_is_child(uint64_t a) { return (r32(comp(a, C_PLAYER) + 0x1c5d) & 0x101) == 0x101; }
static void yaw_matrix(float *m, float x, float y, float z, uint16_t yaw) {   // Matrix34f, row-major, turned about +y
    double a = yaw / 65536.0 * 2 * M_PI; float c = (float)cos(a), s = (float)sin(a);
    float r[12] = { c, 0, s, x, 0, 1, 0, y, -s, 0, c, z }; memcpy(m, r, sizeof r);
}
// AS play (main+0x2022a0): an ASPlayRecord {actor, command, slot, rate, -1, -1, ignore same, all slots}; only Link's animation set
// (OnlinePlayer) has his commands, the inventory Link's knows two
typedef bool (*f_as_play)(void *rec);
// An actor's animation controller (the AS component's controller set, its current one: QueryASGetCurrentCommand's chain) and its
// blackboard (+0x40, TOTK's as::Controller layout, the same engine). 0 while a new stand-in's sequence is not bound yet
static uint64_t as_controller(uint64_t actor) { uint64_t as = comp(actor, C_AS); return as ? rp(rp(as + 0x18) + 0x18) : 0; }
// 1 = played. A stand-in's first frames have no controller yet: the play does nothing then, and a command sent once (the network's
// animation changes only when the player's does) left it in its bind pose for good: the T-pose of runs/vid3, arms out
// A stand-in's sequence, played the way it shows: every command on slots 0 and 1 (as_play_rate; DM_MP_AS_SLOTS overrides). Slot 0
// alone moved only the legs and left the rest in the bind pose (arms out, no cap: its visibility is an animation track too); slot 1
// alone posed the whole body but stood still on its first frame (it keeps slot 0's time); on both they stand, walk and run as Link
// does (runs/legs7). as_body_slot is the slot puppet_check reads back (DM_MP_AS_SLOT overrides)
static int as_body_slot(void) { static int slot = -1; if (slot < 0) { const char *e = getenv("DM_MP_AS_SLOT"); slot = e && *e ? atoi(e) : 1; } return slot; }
static int as_play_rate(uint64_t actor, const char *cmd, float rate) {
    if (!standin_online() || !as_controller(actor)) return 0;
    // On every slot of DM_MP_AS_SLOTS (default "0,1"): slot 1 poses the whole body but keeps the time of slot 0, which moves the legs
    // (alone, slot 1 stood still on its first frame and slot 0 left the arms in the bind pose: runs/legs3-5, yaw1)
    static int slots[4], ns = -1;
    if (ns < 0) { const char *e = getenv("DM_MP_AS_SLOTS"); if (!e || !*e) e = "0,1"; ns = 0;
                  for (const char *q = e; *q && ns < 4; ) { slots[ns++] = atoi(q); q = strchr(q, ','); if (!q) break; q++; } }
    int any = 0;
    for (int i = 0; i < ns; i++) {
        struct { uint64_t actor; const char *cmd; int32_t slot; float rate; float a, b; uint8_t ignore_same, all_slots; uint8_t pad[6]; } rec =
            { actor, cmd, slots[i], rate, -1.0f, -1.0f, 1, 0, { 0 } };
        any |= ((f_as_play)fn(F_AS_PLAY))(&rec) ? 1 : 0;
    }
    (void)any; return 1;   // called: done (what the call returns is not whether it played)
}
static void as_play(uint64_t actor, const char *cmd) { as_play_rate(actor, cmd, 1.0f); }
// The command a stand-in plays for the network's Wait / Move (tests: DM_MP_WAIT_CMD, DM_MP_MOVE_CMD)
static const char *standin_cmd(int anim) {
    static env_switch w = { .name = "DM_MP_WAIT_CMD" }, m = { .name = "DM_MP_MOVE_CMD" };
    if (anim == 0) { const char *e = env_now(&w); return e[0] ? e : "Wait"; }
    if (anim == 1) { const char *e = env_now(&m); return e[0] ? e : "Move"; }
    return ANIM[anim];
}
// A command on a given slot (Link's own: 0 the body, 4 the face, 8 the mask): what his player code keeps playing besides the body
static void as_play_slot(uint64_t actor, const char *cmd, int slot) {
    if (!standin_online() || !as_controller(actor)) return;
    struct { uint64_t actor; const char *cmd; int32_t slot; float rate; float a, b; uint8_t ignore_same, all_slots; uint8_t pad[6]; } rec =
        { actor, cmd, slot, 1.0f, -1.0f, -1.0f, 1, 0, { 0 } };
    ((f_as_play)fn(F_AS_PLAY))(&rec);
}
// DM_MP_BBPROBE=1 (tests): Link's animation blackboard twice a second: its five value arrays' heads (TOTK's order: string, int, float, bool,
// vec3f at +0x18..+0x38), then the 100-entry one's words as floats and the 94-entry one's bytes, to find which entries his movement drives
static void bb_probe(uint64_t a) {
    static int on = -1; static double last; if (on < 0) on = getenv("DM_MP_BBPROBE") && atoi(getenv("DM_MP_BBPROBE"));
    if (!on) return;
    double t = now_s(); if (t - last < 0.5) return; last = t;
    uint64_t ctrl = as_controller(a), bb = ctrl ? rp(ctrl + 0x40) : 0; if (!bb) { MP_GAME_LOG("bbprobe: no blackboard (controller %#llx)", (unsigned long long)ctrl); return; }
    char line[2400]; int w = snprintf(line, sizeof line, "bbprobe: frame %llu bb %#llx vt %#llx arrays", (unsigned long long)dm_guest_frame(), (unsigned long long)bb, (unsigned long long)(rp(bb) - (uint64_t)M));
    uint64_t fa = 0, ba = 0;
    for (int k = 0; k < 5; k++) { uint64_t arr = rp(bb + 0x18 + 8ull * k); uint32_t c = arr ? r32(arr) : 0; w += snprintf(line + w, sizeof line - w, " [%d] %#llx n=%u", k, (unsigned long long)arr, c);
        if (c == 100 && !fa) fa = arr; else if (c == 94 && !ba) ba = arr; }
    MP_GAME_LOG("%s", line);
    if (fa) { w = snprintf(line, sizeof line, "bbprobe: f100"); for (int i = 0; i < 104 && w < (int)sizeof line - 16; i++) { float v; if (!rd(fa + 4 + 4ull * i, &v, 4)) break; w += snprintf(line + w, sizeof line - w, " %d:%.3g", i, v); } MP_GAME_LOG("%s", line); }
    if (ba) { w = snprintf(line, sizeof line, "bbprobe: b94"); for (int i = 0; i < 100 && w < (int)sizeof line - 8; i++) { uint8_t v; if (!rd(ba + 4 + i, &v, 1)) break; w += snprintf(line + w, sizeof line - w, "%s%u", i % 10 ? "" : " ", v); } MP_GAME_LOG("%s", line); }
}
// The rate a stand-in plays Move at, by age: DM_MP_MOVE_RATE_ADULT / DM_MP_MOVE_RATE_CHILD (tests: matching the run's stride to the speed)
static float move_rate(int child) {
    static env_switch a = { .name = "DM_MP_MOVE_RATE_ADULT" }, c = { .name = "DM_MP_MOVE_RATE_CHILD" };
    float r = (float)atof(env_now(child ? &c : &a)); return r > 0.05f && r < 4 ? r : 1.0f;
}
static void puppet_request(Puppet *p, const mp_remote *o, int child) {
    memset(p->arg, 0, sizeof p->arg); memset(p->bb, 0, sizeof p->bb);
    float pos[3] = { o->x, o->y, o->z }, rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, sc[3] = { 1, 1, 1 };
    double a = o->yaw / 65536.0 * 2 * M_PI; rot[0] = (float)cos(a); rot[2] = (float)sin(a); rot[6] = -(float)sin(a); rot[8] = (float)cos(a);
    memcpy(p->arg + 0x00, pos, 12); memcpy(p->arg + 0x0c, rot, 36); memcpy(p->arg + 0x30, sc, 12);
    static const char *empty = ""; memcpy(p->arg + 0x40, &empty, 8);
    p->arg[0x4a] = 1; *(uint32_t *)(p->arg + 0x98) = 1 | 2 | 4;
    if (!child) {   // the tunic, through the blackboard override the game's EquipmentUser reads (bb::InitInfo<32>, addString keeps the pointer)
        // bb::InitInfo<32> (re/AREA_CHANGE.md 2.2): header, then each 0xc8-byte entry's key pointer at +0x08 to its own buffer at +0x14 (size 0x80
        // at +0x10), as the game's constructor sets them: addString copies the key there (runs/r16: left zero, the first adult stand-in crashed in it)
        *(uint32_t *)(p->bb + 0x00) = 32; *(uint64_t *)(p->bb + 0x08) = (uint64_t)(p->bb + 0x18); *(uint32_t *)(p->bb + 0x10) = 0;
        for (int k = 0; k < 32; k++) { uint8_t *e = p->bb + 0x18 + 0xc8 * k; *(uint64_t *)(e + 0x08) = (uint64_t)(e + 0x14); *(int32_t *)(e + 0x10) = 0x80; }
        static const char *key = "EquipmentUser_ArmorUpper";
        ((f_bb_add_string)fn(F_BB_ADD_STRING))(p->bb, &key, &ARMOR[o->outfit % 3]);
        *(uint64_t *)(p->arg + 0x60) = (uint64_t)p->bb;
    }
    static const char *name; name = standin_name(child);
    p->watch = 0; uint32_t result = 0;
    bool ok = ((f_create)fn(F_CREATE_ACTOR))((void *)rp((uint64_t)M + G_ACTOR_MGR), &name, p->arg, &p->watch, 1, NULL, NULL, NULL, 0, &result, NULL);
    p->id = o->id; p->actor = 0; p->outfit = o->outfit; p->child = child; p->asked = now_s(); p->anim = -1; snprintf(p->name, sizeof p->name, "%s", o->name);
    if (!ok) { MP_GAME_LOG("stand-in for #%u not requested (result %u)", o->id, result); p->id = 0; }
}
// An adult stand-in's tunic. The stand-in is the inventory screen's Link, which dresses itself from the player's own equipment
// (runs/vid2: every adult stand-in wore the watcher's Kokiri tunic; the blackboard override given at creation is not read for it), so
// it is dressed the way the game's own equipFromGameData does slot 8 (main+0x2df7a4c): the Kokiri tunic (Armor_Adult_Normal)
// through the default-armor call, any other through requestCreateEquipment(eu, 8, &name, 0, 1)
typedef void (*f_req_equip)(void *eu, uint32_t slot, const char **name, int32_t a, bool b);
typedef void (*f_armor_default)(void *eu, bool a, bool b);
static void puppet_dress(Puppet *p) {
    p->dressed = 1;
    if (!F_REQ_EQUIP) return;
    uint64_t eu = comp(p->actor, C_EQUIP); if (!eu) return;
    const char *armor = ARMOR[p->outfit % 3];
    if (p->outfit % 3 == 0) ((f_armor_default)fn(F_ARMOR_DEFAULT))((void *)eu, true, false);
    else ((f_req_equip)fn(F_REQ_EQUIP))((void *)eu, 8, &armor, 0, true);
    MP_GAME_LOG("stand-in for %s dressed: %s", p->name, armor);
}
// The walk / run blend of Link's Move reads his animation blackboard's stick and speed values, which only his player code sets (TOTK
// Royale's stand-ins "hovered" without them): every frame the stand-in's own blackboard gets the values Link's has at its speed, so it
// walks at a walk and runs at a run. Where those values sit: Link's own, read while he runs and stands (DM_MP_BBPROBE, runs/rec2): the
// float array with 100 entries (Player_<age>.root.asb's) at +0x28, the bool array with 94 at +0x30, entries from +4
// Measured (runs/rec2, adult Link: stick 1 at a 5.1-5.4 m/s run, 0.4 at a 1.7 m/s walk, 0 standing; the same names in Player_Child.root.asb):
//   floats 0 LeftStickLength, 1 PrevLeftStickLength, 2 LeftStickLengthClampMin (never under 0.55), 8 LinearVelXZ (m/s), 13 LeftStickY,
//   16 LeftStickDeg; bool 12 IsLeftStickInput (1 exactly while he moves). Young Link runs at 4.5 m/s at a full tilt (runs/rec1)
enum { BBF_STICK = 0, BBF_PREV_STICK = 1, BBF_STICK_MIN = 2, BBF_VEL = 8, BBF_STICK_Y = 13, BBF_STICK_DEG = 16, BBB_INPUT = 12 };
static const float g_stick_full_c = 4.5f, g_stick_full_a = 5.3f;   // m/s at a full tilt, young / adult
// Setting a value as the blackboard's own setters do (its vtable [16] setFloatByIndex main+0x37b9cc0, [17] setBoolByIndex main+0x37ba100):
// the value, then one more on its change count (animation nodes can read a value again only when its count moved). Floats at +0x28
// (u32 count, values), their counts at +0x58 (u32 count, a byte each); bools at +0x30 / +0x60
static void bb_set_f(uint64_t bb, int i, float v) {
    uint64_t a = rp(bb + 0x28), c = rp(bb + 0x58); if (!a || !c || (uint32_t)i >= r32(a) || (uint32_t)i >= r32(c)) return;
    *(volatile float *)(a + 4 + 4ull * i) = v; volatile uint8_t *k = (volatile uint8_t *)(c + 4 + i); *k = (uint8_t)(*k + 1);
}
static void bb_set_b(uint64_t bb, int i, int v) {
    uint64_t a = rp(bb + 0x30), c = rp(bb + 0x60); if (!a || !c || (uint32_t)i >= r32(a) || (uint32_t)i >= r32(c)) return;
    *(volatile uint8_t *)(a + 4 + i) = (uint8_t)(v != 0); volatile uint8_t *k = (volatile uint8_t *)(c + 4 + i); *k = (uint8_t)(*k + 1);
}
static void bb_set_i(uint64_t bb, int i, int32_t v) {   // ints at +0x18, their counts at +0x48
    uint64_t a = rp(bb + 0x18), c = rp(bb + 0x48); if (!a || !c || (uint32_t)i >= r32(a) || (uint32_t)i >= r32(c)) return;
    *(volatile int32_t *)(a + 4 + 4ull * i) = v; volatile uint8_t *k = (volatile uint8_t *)(c + 4 + i); *k = (uint8_t)(*k + 1);
}
// Link's aim reads his blackboard too (Player_Adult.root.asb): BowCharge and BowChargeUpper choose the slingshot's clips or the bow's by
// IsChild (bool 11), then by IsBowCameraInvalidSideOfs (bool 42) and ArrowType (int 5). Clips the actor doesn't have play nothing: an adult
// stand-in whose IsChild said young stood still while its arrow flew (runs/vid19). Its age is set when its blackboard first appears, and
// with IsAiming (bool 9) and BattleMode (bool 6) while it aims. DM_MP_AIMBB=0 / odd (tests: none / only odd player ids)
// Link's own while he aims the slingshot (runs/aimt1): WeaponType (int 1) 4, UsingItem (int 23) the item (the item enum: SlingShot 4,
// Bow 30; -1 none), BattleMode, IsAiming and IsCaution (bool 26) on
enum { BBB_BATTLE = 6, BBB_AIMING = 9, BBB_CHILD = 11, BBB_CAUTION = 26, BBB_BOWCAM = 42, BBI_WEAPON = 1, BBI_POSTURE = 3, BBI_ARROW = 5, BBI_USING = 23 };
static int aim_bb_on(const Puppet *p) {
    static env_switch e = { .name = "DM_MP_AIMBB" }; const char *v = env_now(&e);
    if (!strcmp(v, "0")) return 0;
    if (!strcmp(v, "odd")) return (p->id & 1) != 0;
    return 1;
}
static void puppet_aim_bb(Puppet *p, uint64_t bb, int aiming) {
    if (!bb || !aim_bb_on(p)) return;
    bb_set_b(bb, BBB_CHILD, p->child); bb_set_b(bb, BBB_BOWCAM, 0); bb_set_i(bb, BBI_ARROW, 0); bb_set_i(bb, BBI_POSTURE, 0);
    if (aiming >= 0) {
        bb_set_b(bb, BBB_AIMING, aiming); bb_set_b(bb, BBB_BATTLE, aiming); bb_set_b(bb, BBB_CAUTION, aiming);
        bb_set_i(bb, BBI_WEAPON, aiming ? 4 : 0); bb_set_i(bb, BBI_USING, aiming ? (p->child ? 4 : 30) : -1);
    }
}
static void puppet_speed(Puppet *p, float x, float z) {
    static int one = -1; if (one < 0) one = getenv("DM_FSTEP_ONE") && atoi(getenv("DM_FSTEP_ONE"));
    uint64_t f = dm_guest_frame(); double t = now_s();
    // the time since the last frame: game frames in a frame-exact recording (DM_FSTEP_ONE, which can run far slower than real time), else real time
    float dt = p->lf ? (one ? (float)(f - p->lf) / 60.0f : (float)(t - p->lt)) : 0;
    float jump = p->lf ? sqrtf((x - p->lx) * (x - p->lx) + (z - p->lz) * (z - p->lz)) : 0;
    if (dt > 0 && dt < 0.5f && jump < 3.0f) {
        float v = jump / dt, k = 1.0f - expf(-10.0f * dt); p->spd += (v - p->spd) * k;
    } else if (dt >= 0.5f || jump >= 3.0f) p->spd = 0;   // a long gap, or a jump of metres in a frame (a respawn, a warp): no speed from it
    p->lx = x; p->lz = z; p->lf = f; p->lt = t;
    p->moving = p->spd > (p->moving ? 0.3f : 0.8f);   // a margin either way: one threshold flickered on a slow stroll
    uint64_t ctrl = as_controller(p->actor), bb = ctrl ? rp(ctrl + 0x40) : 0; if (!bb) return;
    uint64_t fa = rp(bb + 0x28), ba = rp(bb + 0x30);
    if (!fa || !ba || r32(fa) != 100 || r32(ba) != 94 || !rp(bb + 0x58) || !rp(bb + 0x60)) {
        if (!p->bb_said) { p->bb_said = 1; MP_GAME_LOG("[RED] stand-in for %s: no animation blackboard of Link's shape (floats %#llx, bools %#llx): its legs won't follow its speed", p->name, (unsigned long long)fa, (unsigned long long)ba); }
        return;
    }
    if (!p->bb_said) { p->bb_said = 1; MP_GAME_LOG("stand-in for %s: its walk / run follows its speed (blackboard %#llx)", p->name, (unsigned long long)bb); puppet_aim_bb(p, bb, -1); }
    float full = p->child ? g_stick_full_c : g_stick_full_a, stick = p->moving ? (p->spd >= full ? 1.0f : p->spd / full) : 0;
    if (p->moving && stick < 0.25f) stick = 0.25f;
    {   // DM_MP_BBCOPY=1 (tests): our own Link's whole blackboard first (ints, bools, floats, each through its change count), so the
        // stand-in's sequence sees the same surroundings Link's does; its own movement values go over it below
        static int copy = -1; if (copy < 0) copy = getenv("DM_MP_BBCOPY") && atoi(getenv("DM_MP_BBCOPY"));
        uint64_t me = player_actor(), mc = me ? as_controller(me) : 0, mb = mc ? rp(mc + 0x40) : 0;
        if (copy && mb) {
            uint64_t mi = rp(mb + 0x18), ii = rp(bb + 0x18), mic = rp(mb + 0x48), iic = rp(bb + 0x48);
            if (mi && ii && iic && r32(mi) == r32(ii)) for (uint32_t i = 0; i < r32(ii); i++) {
                int32_t v; rd(mi + 4 + 4ull * i, &v, 4); if (*(volatile int32_t *)(ii + 4 + 4ull * i) != v) { *(volatile int32_t *)(ii + 4 + 4ull * i) = v; volatile uint8_t *k = (volatile uint8_t *)(iic + 4 + i); *k = (uint8_t)(*k + 1); } }
            uint64_t mf = rp(mb + 0x28), mbo = rp(mb + 0x30);
            if (mf && r32(mf) == 100) for (int i = 0; i < 100; i++) { float v; rd(mf + 4 + 4ull * i, &v, 4); if (*(volatile float *)(fa + 4 + 4ull * i) != v) bb_set_f(bb, i, v); }
            if (mbo && r32(mbo) == 94) for (int i = 0; i < 94; i++) { uint8_t v; rd(mbo + 4 + i, &v, 1); if (*(volatile uint8_t *)(ba + 4 + i) != v) bb_set_b(bb, i, v); }
        }
    }
    float prev = *(volatile float *)(fa + 4 + 4ull * BBF_STICK);
    bb_set_f(bb, BBF_PREV_STICK, prev); bb_set_f(bb, BBF_STICK, stick); bb_set_f(bb, BBF_STICK_MIN, stick > 0.55f ? stick : 0.55f);
    bb_set_f(bb, BBF_VEL, p->moving ? p->spd : 0); bb_set_f(bb, BBF_STICK_Y, stick); bb_set_f(bb, BBF_STICK_DEG, 0);
    bb_set_b(bb, BBB_INPUT, p->moving);
}
// Twice a second: is the stand-in's body slot playing what it was told? Wait and Move only,
// the commands that last (a one-off like DamageS hands over to another by itself). DM_MP_PROBE logs each stand-in's slots every 2 s
static void puppet_check(Puppet *p, double t, float dist) {
    if (p->anim < 0 || t - p->check_t < 0.5) return;
    p->check_t = t;
    char cur[64]; int ok = slot_command(p->actor, as_body_slot(), cur, sizeof cur);
    static int probe = -1; if (probe < 0) probe = getenv("DM_MP_PROBE") && atoi(getenv("DM_MP_PROBE"));
    if (probe && (int)(t * 2) % 4 == 0) {
        uint64_t ctrl = as_controller(p->actor);
        char slots[400]; int w = 0; slots[0] = 0;   // every slot's command: which one is the body
        int ns = ctrl ? (int)r32(ctrl + 0x18) : 0; uint64_t arr = ctrl ? rp(ctrl + 0x20) : 0;
        for (int i = 0; i < ns && i < 12 && arr && w < (int)sizeof slots - 40; i++) {
            uint64_t sl = rp(arr + 0x48ull * i), nm = sl ? rp(sl + 0x10) : 0; char c[32] = ""; if (nm) { rd(nm, c, sizeof c - 1); c[sizeof c - 1] = 0; }
            w += snprintf(slots + w, sizeof slots - w, " %d:%s", i, c[0] ? c : "-");
        }
        MP_GAME_LOG("stand-in %s: %.1f m away, told %s, slots%s (state %u, speed %.2f)", p->name, dist, ANIM[p->anim], slots, r32(p->actor + A_STATE), p->spd);
    }
    if (p->anim > 1 || (ok && !strcmp(cur, standin_cmd(p->anim)))) return;
    int r = as_play_rate(p->actor, standin_cmd(p->anim), p->anim == 1 ? move_rate(p->child) : 1.0f);
    if (p->replays++ < 4) MP_GAME_LOG("stand-in for %s: its sequence plays '%s', not %s: played again (%s)", p->name, ok ? cur : "(nothing)", ANIM[p->anim], r ? "taken" : "refused");
}
// Every stand-in has its own Navi, as Link has: OnlineNavi (tools/make_online_mod.py: the cutscene Navi's model, glow and sounds with the
// gameplay Navi's animation sequence and no AI) made beside it and kept at its shoulder each frame, bobbing and drifting a little the way
// Navi hovers. DM_MP_NAVI=0: none. Deleted with its stand-in
static void navi_drop(Puppet *p) {
    if (p->navi && r32(p->navi + A_STATE) >= 3 && r32(p->navi + A_STATE) <= 6) ((f_request_delete)fn(F_REQUEST_DELETE))((void *)p->navi, 0);
    watch_cancel(&p->navi_watch); p->navi = 0; p->navi_anim = 0;
}
static void navi_spot(const Puppet *p, const mp_remote *o, float *out) {
    double a = o->yaw / 65536.0 * 2 * M_PI, t = game_now() + p->id * 0.37;
    float fx = (float)sin(a), fz = (float)cos(a), rx = -fz, rz = fx;   // its facing, and its right
    float side = 0.42f + 0.10f * (float)sin(t * 1.3), back = 0.20f + 0.08f * (float)sin(t * 0.9 + 1.0);
    float up = (p->child ? 1.02f : 1.48f) + 0.07f * (float)sin(t * 2.4);
    out[0] = o->x + rx * side - fx * back; out[1] = o->y + up; out[2] = o->z + rz * side - fz * back;
}
static void navi_tick(Puppet *p, const mp_remote *o) {
    static env_switch sw = { .name = "DM_MP_NAVI" }; if (!strcmp(env_now(&sw), "0")) return;
    float at[3]; navi_spot(p, o, at);
    if (!p->navi && !p->navi_watch) {
        memset(p->navi_arg, 0, sizeof p->navi_arg);
        float rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, sc[3] = { 1, 1, 1 };
        memcpy(p->navi_arg + 0x00, at, 12); memcpy(p->navi_arg + 0x0c, rot, 36); memcpy(p->navi_arg + 0x30, sc, 12);
        static const char *empty = ""; memcpy(p->navi_arg + 0x40, &empty, 8);
        p->navi_arg[0x4a] = 1; *(uint32_t *)(p->navi_arg + 0x98) = 1 | 2 | 4;
        static const char *name = "OnlineNavi"; uint32_t result = 0;
        bool ok = ((f_create)fn(F_CREATE_ACTOR))((void *)rp((uint64_t)M + G_ACTOR_MGR), &name, p->navi_arg, &p->navi_watch, 1, NULL, NULL, NULL, 0, &result, NULL);
        static int said; if (!ok && said++ < 3) MP_GAME_LOG("Navi for %s not requested (result %u)", p->name, result);
        return;
    }
    if (!p->navi) { if ((p->navi = watch_take(&p->navi_watch))) MP_GAME_LOG("Navi for %s taken", p->name); return; }
    uint32_t st = r32(p->navi + A_STATE); if (st == 3) return;
    if (st != 4) { p->navi = 0; return; }   // gone (the game deleted her): made again next frame
    float m[12]; yaw_matrix(m, at[0], at[1], at[2], o->yaw);
    ((f_force_set_matrix)fn(F_FORCE_SET_MATRIX))((void *)p->navi, m, false);
    if (!p->navi_anim && as_controller(p->navi)) {   // her idle hover (the gameplay Navi's Wait), on each slot it could be
        struct { uint64_t actor; const char *cmd; int32_t slot; float rate; float a, b; uint8_t ignore_same, all_slots; uint8_t pad[6]; } rec =
            { p->navi, "Wait", 0, 1.0f, -1.0f, -1.0f, 1, 1, { 0 } };
        p->navi_anim = ((f_as_play)fn(F_AS_PLAY))(&rec) ? 1 : 0;
        if (p->navi_anim) { char c0[32] = "", c1[32] = ""; slot_command(p->navi, 0, c0, sizeof c0); slot_command(p->navi, 1, c1, sizeof c1);
                            MP_GAME_LOG("Navi for %s hovers (slots 0 '%s', 1 '%s')", p->name, c0, c1); }
    }
}
// DM_MP_BBPROBE=1 also logs the first stand-in's whole blackboard every second (floats and bools, as for Link): what Link's own code
// keeps set that a stand-in lacks
static void bb_dump(const char *who, uint64_t actor) {
    uint64_t ctrl = as_controller(actor), bb = ctrl ? rp(ctrl + 0x40) : 0; if (!bb) return;
    uint64_t fa = rp(bb + 0x28), ba = rp(bb + 0x30), ia = rp(bb + 0x18); if (!fa || !ba) return;
    char line[2600]; int w = snprintf(line, sizeof line, "bbdump %s f", who);
    for (int i = 0; i < 100 && w < (int)sizeof line - 16; i++) { float v; rd(fa + 4 + 4ull * i, &v, 4); w += snprintf(line + w, sizeof line - w, " %d:%.3g", i, v); }
    MP_GAME_LOG("%s", line);
    w = snprintf(line, sizeof line, "bbdump %s b", who);
    for (int i = 0; i < 94 && w < (int)sizeof line - 8; i++) { uint8_t v; rd(ba + 4 + i, &v, 1); w += snprintf(line + w, sizeof line - w, "%s%u", i % 10 ? "" : " ", v); }
    if (ia) { w += snprintf(line + w, sizeof line - w, " i"); for (int i = 0; i < 37 && w < (int)sizeof line - 16; i++) { int32_t v; rd(ia + 4 + 4ull * i, &v, 4); w += snprintf(line + w, sizeof line - w, " %d:%d", i, v); } }
    MP_GAME_LOG("%s", line);
    w = snprintf(line, sizeof line, "bbdump %s slots", who);   // and every slot's command and play rate (slot+0xd4)
    int ns = (int)r32(ctrl + 0x18); uint64_t arr = rp(ctrl + 0x20);
    for (int i = 0; i < ns && i < 12 && arr && w < (int)sizeof line - 60; i++) {
        uint64_t sl = rp(arr + 0x48ull * i), nm = sl ? rp(sl + 0x10) : 0; char c[40] = ""; float rate = 0; if (nm) { rd(nm, c, sizeof c - 1); c[sizeof c - 1] = 0; } if (sl) rd(sl + 0xd4, &rate, 4);
        w += snprintf(line + w, sizeof line - w, " %d:%s@%.2f", i, c[0] ? c : "-", rate);
    }
    MP_GAME_LOG("%s", line);
}
// Its age's ranged weapon in its hands the first time it aims, through requestCreateEquipment as the tunic is made: the slot before
// the shield's (0 the sword, 6 the shield, 7-9 head, tunic, boots: re/REPORT.md; TOTK's bow is the slot before its shield too), with
// the hand's equipment state (1; 2 is the back: TOTK Royale's puppets.cpp Give). Only with DM_MP_BOW_SLOT / DM_MP_BOW_STATE (tests)
static void puppet_weapon(Puppet *p) {
    if (p->weapon_on || !F_REQ_EQUIP) return;
    uint64_t eu = comp(p->actor, C_EQUIP); if (!eu) return;
    const char *w = p->child ? "Weapon_Range_SlingShot" : "Weapon_Range_Bow";
    const char *es = getenv("DM_MP_BOW_SLOT"), *et = getenv("DM_MP_BOW_STATE");
    if (!es || !*es) return;   // not yet for players: slots 1-4 were asked and showed nothing (runs/aim5, aimt2-4), slot 5 in the hand state crashed the game (runs/aim4)
    // (tests: comma lists, one entry per stand-in in the order they first aim, the last repeated: DM_MP_BOW_SLOT=3,3,3,4)
    static int nth; int k = nth++;
    const char *q = es; for (int i = 0; i < k && strchr(q, ','); i++) q = strchr(q, ',') + 1;
    const char *r = et && *et ? et : "1"; for (int i = 0; i < k && strchr(r, ','); i++) r = strchr(r, ',') + 1;
    int slot = atoi(q), state = atoi(r);
    typedef bool (*f_req_equip_ok)(void *eu, uint32_t slot, const char **name, int32_t a, bool b);   // (it says whether it asked)
    bool asked = ((f_req_equip_ok)fn(F_REQ_EQUIP))((void *)eu, (uint32_t)slot, &w, state, true);
    p->weapon_on = 1; MP_GAME_LOG("stand-in for %s: %s out (slot %d, state %d): %s", p->name, w, slot, state, asked ? "asked" : "refused");
}
#define AIM_TURN 8192.0f   // 45 degrees (u16 yaw)
// A player who goes down falls where they stood (Link's DamageL: knocked flat; DM_MP_DOWN_CMD tries another) and lies there DOWN_HOLD
// seconds before the stand-in goes; it went the moment the server said so before, gone in a blink mid-fight (runs/vid23)
#define DOWN_HOLD 2.2
static const char *down_cmd(void) { static env_switch e = { .name = "DM_MP_DOWN_CMD" }; const char *v = env_now(&e); return v[0] ? v : "DamageL"; }
static void puppets_update(uint64_t me, const mp_remote *o, int n, uint32_t scene) {
    if (scene != g_pup_scene) {   // a new area: the game deleted the stand-ins; requests still pending are cancelled (they would leak the engine's watchers)
        for (int i = 0; i < MAX_PUPPETS; i++) { watch_cancel(&g_pup[i].watch); watch_cancel(&g_pup[i].navi_watch); }
        memset(g_pup, 0, sizeof g_pup); g_pup_scene = scene;
    }
    if (n > MAX_PUPPETS) n = MAX_PUPPETS;
    // remove the ones no longer near (or who changed clothes: dressed again)
    for (int i = 0; i < MAX_PUPPETS; i++) {
        Puppet *p = &g_pup[i]; if (!p->id) continue;
        int keep = 0;   // same player, same age, same tunic (the tunic is the adult's only); down: only while it lies there (DOWN_HOLD)
        for (int j = 0; j < n; j++) { int c = (o[j].equip & MP_EQ_CHILD) != 0;
            if (o[j].id != p->id || c != p->child || (!c && o[j].outfit != p->outfit)) continue;
            if (!(o[j].flags & MP_PF_DEAD)) { keep = 1; p->down_t = 0; }
            else { if (!p->down_t) p->down_t = game_now(); keep = p->actor && game_now() - p->down_t < DOWN_HOLD; } }   // (game time: footage too)
        if (!keep) {
            if (p->actor && r32(p->actor + A_STATE) >= 3 && r32(p->actor + A_STATE) <= 6) ((f_request_delete)fn(F_REQUEST_DELETE))((void *)p->actor, 0);
            navi_drop(p);
            watch_cancel(&p->watch); p->id = 0; p->actor = 0;
        }
    }
    double t = now_s();
    for (int j = 0; j < n; j++) {
        Puppet *p = NULL;
        for (int i = 0; i < MAX_PUPPETS; i++) if (g_pup[i].id == o[j].id) p = &g_pup[i];
        if (o[j].flags & MP_PF_DEAD) {   // down: no stand-in made for it; the one there falls where it stood and lies still (then goes, above)
            if (p && p->actor && r32(p->actor + A_STATE) == 4 && p->anim != 14 && as_play_rate(p->actor, down_cmd(), 1.0f)) {
                p->anim = 14; navi_drop(p); MP_GAME_LOG("stand-in for %s: down (%s)", p->name, down_cmd()); }
            continue;
        }
        if (!p) { for (int i = 0; i < MAX_PUPPETS; i++) if (!g_pup[i].id) { p = &g_pup[i]; break; } if (p) puppet_request(p, &o[j], (o[j].equip & MP_EQ_CHILD) != 0); continue; }
        if (!p->actor) {
            if (p->watch) {
                uint64_t a = watch_take(&p->watch);
                if (a) { p->actor = a; p->taken_at = t; p->dressed = 0; p->lf = 0; p->spd = 0; p->moving = 0; p->bb_said = 0; p->check_t = 0; p->replays = 0; p->weapon_on = 0; MP_GAME_LOG("stand-in for %s (#%u) taken", p->name, p->id); }
                else if (!p->watch) p->id = 0;                                   // the game said no (freed): asked again next frame
                else if (t - p->asked > 10) { watch_cancel(&p->watch); p->id = 0; }   // never came: cancelled, asked again
            } else p->id = 0;
            continue;
        }
        uint32_t ast = r32(p->actor + A_STATE);
        if (ast == 3) continue;                               // taken, starting (4 on the engine's next pass)
        if (ast != 4) { p->id = 0; p->actor = 0; continue; }   // gone (deleted by the game)
        {   // aiming, it turns AIM_TURN to its right, eased over a few frames: Link's draw holds the bow arm ~45 degrees left of where his
            // body faces (runs/aimdir: four archers at 0, 90, 180, 270), and his own code turns him to the aim, which a stand-in doesn't run
            double gt = game_now(), dt = p->aim_t > 0 ? gt - p->aim_t : 0; p->aim_t = gt; if (dt > 0.1) dt = 0.1;
            float want = (p->anim == 6 || p->anim == 7) ? AIM_TURN : 0, step = (float)(AIM_TURN * dt / 0.12);
            p->aim_turn += want > p->aim_turn ? fminf(step, want - p->aim_turn) : -fminf(step, p->aim_turn - want);
        }
        float m[12]; yaw_matrix(m, o[j].x, o[j].y, o[j].z, (uint16_t)(o[j].yaw - (int)p->aim_turn));
        ((f_force_set_matrix)fn(F_FORCE_SET_MATRIX))((void *)p->actor, m, false);
        { static int probe = -1; static double last; if (probe < 0) probe = getenv("DM_MP_BBPROBE") ? atoi(getenv("DM_MP_BBPROBE")) : 0;
          if (probe && p == &g_pup[0] && t - last > 1.0) { last = t; bb_dump("Link", me); for (int q = 0; q < MAX_PUPPETS; q++) if (g_pup[q].actor && (probe > 1 || q == 0)) bb_dump(g_pup[q].name, g_pup[q].actor); } }   // (before our writes; DM_MP_BBPROBE=2: every stand-in)
        puppet_speed(p, o[j].x, o[j].z);
        navi_tick(p, &o[j]);
        {   // DM_MP_PUPLOG=<file> (tests): where each stand-in is drawn every frame, to measure how smoothly others move
            static int on = -1; static FILE *lf; if (on < 0) { const char *e = getenv("DM_MP_PUPLOG"); on = e && *e && (lf = fopen(e, "w")) != NULL; if (on) fprintf(lf, "#frame\tt\tid\tx\ty\tz\tyaw\tanim\tspd\n"); }
            if (on) { fprintf(lf, "%llu\t%.4f\t%u\t%.4f\t%.4f\t%.4f\t%u\t%u\t%.3f\n", (unsigned long long)dm_guest_frame(), now_s(), o[j].id, o[j].x, o[j].y, o[j].z, o[j].yaw, o[j].anim, p->spd); fflush(lf); }
        }
        { float lp[3] = { 0, 0, 0 }; rd(me + A_POS, lp, sizeof lp); float dx = o[j].x - lp[0], dz = o[j].z - lp[2]; puppet_check(p, t, sqrtf(dx * dx + dz * dz)); }
        if (p->anim != o[j].anim && (o[j].anim == 6 || o[j].anim == 7) && as_controller(p->actor)) {
            // the bow (young: the slingshot) out and drawn, as Link's own code does it (runs/aim1, his slots while aiming): the command on
            // slot 0, its Upper variant on 1 and 4 (BowCharge / BowChargeUpper, BowShoot / BowShootUpper), with his aim's blackboard values
            // (puppet_aim_bb: without them an adult stand-in stood still while its arrow flew, runs/vid19; with them it draws, runs/aimt1)
            puppet_weapon(p);
            { uint64_t ctl = as_controller(p->actor); puppet_aim_bb(p, ctl ? rp(ctl + 0x40) : 0, 1); }
            const char *c = ANIM[o[j].anim]; char up[32]; snprintf(up, sizeof up, "%sUpper", c);
            as_play_slot(p->actor, c, 0); as_play_slot(p->actor, up, 1); as_play_slot(p->actor, up, 4);
            p->anim = o[j].anim;
        } else if (p->anim != o[j].anim && o[j].anim < N_ANIM && as_play_rate(p->actor, standin_cmd(o[j].anim), o[j].anim == 1 ? move_rate(p->child) : 1.0f)) {   // (not played: again next frame)
            if (p->anim == 6 || p->anim == 7) { uint64_t ctl = as_controller(p->actor); puppet_aim_bb(p, ctl ? rp(ctl + 0x40) : 0, 0); }   // (the aim over)
            p->anim = o[j].anim;
            static int extra = -1; if (extra < 0) { const char *e = getenv("DM_MP_AS_EXTRA"); extra = e && *e ? atoi(e) : 1; }
            if (extra) {   // the face and the mask, as Link's own code plays them (slot 4: FaceLocomotion moving, Wait standing; slot 8: MaskUnequip)
                as_play_slot(p->actor, o[j].anim == 1 ? "FaceLocomotion" : "Wait", 4);
                as_play_slot(p->actor, "MaskUnequip", 8);
            }
        }
        if (!p->child && !p->dressed && t - p->taken_at > 0.3) puppet_dress(p);
    }
}

// Damage and respawn
void mp_game_apply_damage(int quarter_hearts, uint16_t attacker, int kind) {
    (void)kind;
    if (ready() && live()) apply_damage(quarter_hearts, attacker);
}
static void apply_damage(int quarter_hearts, uint16_t attacker) {
    uint64_t a = player_actor(); if (!a) return;
    uint64_t L = comp(a, C_LIFE); if (!L) return;
    int hp = 0, max = 0; if (!life(a, &hp, &max)) return;
    g_last_attacker = attacker;
    if (hp - quarter_hearts <= 0) {   // this hit ends him: no Game Over; one quarter heart left, the server is told, the respawn heals him
        *(volatile int32_t *)(rp(L + 0x1c10) + 8) = 1;
        if (!g_down) { g_down = 1; mp_send_died(attacker, MP_HOW_PLAYER); }
        return;
    }
    // the hearts value itself (runs/eighth: a count added to LifeComponent+0x228, the pending damage of re/REPORT.md, was never taken)
    *(volatile int32_t *)(rp(L + 0x1c10) + 8) = hp - quarter_hearts;
}
static int g_kit_refill_req;   // a respawn: the ammo refilled at once (kit_tick)
void mp_game_respawn(uint32_t scene, float x, float y, float z, uint16_t yaw, int own) {
    if (ready() && live()) { respawn_at(scene, x, y, z, yaw, own); g_kit_refill_req = 1; }
}
static double g_resp_watch;   // until then a respawn is checked for water (resp_tick)
static void respawn_at(uint32_t scene, float x, float y, float z, uint16_t yaw, int own) {
    uint64_t a = player_actor(); if (!a) return;
    char cmd[64]; current_command(a, cmd, sizeof cmd);
    if (strstr(cmd, "Dead")) {   // the game itself ended him (drowned, fell; runs/kit1): its own Game Over runs, which a heal and a warp would only stall
        MP_GAME_LOG("respawn: Link is in the game's own death ('%s'): left to it", cmd); g_down = 0; return;
    }
    uint64_t L = comp(a, C_LIFE); int hp = 0, max = 0;
    if (L && life(a, &hp, &max)) *(volatile int32_t *)(rp(L + 0x1c10) + 8) = max;   // full hearts
    g_down = 0;
    if (own || scene != shared_scene(g_scene_name)) return;   // no spawn for this area: stay where he is, healed
    float m[12]; yaw_matrix(m, x, y, z, yaw);
    ((f_warp_to_matrix)fn(F_WARP_TO_MATRIX))((void *)a, m, false);
    g_resp_watch = now_s() + 3;
}
// Some of the game's own start points the server respawns at are in water (a river's entry: runs/kit1 came up swimming there, and the
// game drowned him). In the 3 s after a respawn, Link swimming is taken to the hub on dry land in front of the drawbridge instead
static void resp_tick(uint64_t a) {
    if (!g_resp_watch || now_s() > g_resp_watch) { g_resp_watch = 0; return; }
    char cmd[64]; current_command(a, cmd, sizeof cmd);
    if (strncmp(cmd, "Swim", 4)) return;
    g_resp_watch = 0;
    char bn[64]; if (!banc_name(bn, sizeof bn) || strncmp(bn, "HyruleField", 11)) { MP_GAME_LOG("respawn: in water in %s (no dry spot known here)", bn); return; }
    float m[12]; yaw_matrix(m, -0.04f, 150.66f, 32.16f, 0);
    ((f_warp_to_matrix)fn(F_WARP_TO_MATRIX))((void *)a, m, false);
    MP_GAME_LOG("respawn: the spawn point was in water ('%s'): taken to the hub in front of the drawbridge", cmd);
}
int mp_game_set_outfit(int outfit) {
    if (!ready() || !live()) return 0;
    uint64_t a = player_actor(); if (!a || link_is_child(a)) return 0;   // the Goron and Zora tunics are adult Link's
    const char *armor = ARMOR[outfit % 3];
    ((f_setstring64)fn(F_SETSTRING64))((void *)rp((uint64_t)M + G_GAMEDATA), &armor, 0x008d7f0cu);
    uint64_t eu = comp(a, C_EQUIP); if (eu) ((f_equip_from_gd)fn(F_EQUIP_FROM_GD))((void *)eu, 8);
    MP_GAME_LOG("tunic: %s", armor);
    return 1;
}

static atomic_int g_gate_shot, g_gate_shoot_seen;
// Our shots: Link's animation turning into BowShoot is a shot; it flies where he faces (level). Hits are tested here against
// the others (a capsule round each), the server checks them again
static char g_prev_cmd[64];
static void local_shots(uint64_t a, const mp_remote *o, int n) {
    char cmd[64]; current_command(a, cmd, sizeof cmd);
    int shot = !strcmp(cmd, "BowShoot") && strcmp(g_prev_cmd, "BowShoot");
    if (atomic_load(&g_gate_shoot_seen)) shot = atomic_exchange(&g_gate_shot, 0);   // the shot gate is in: it says exactly when an arrow leaves
    snprintf(g_prev_cmd, sizeof g_prev_cmd, "%s", cmd);
    if (!shot) return;
    float p[3], r[9]; rd(a + A_POS, p, sizeof p); rd(a + A_ROT, r, sizeof r);
    float d[3] = { r[2], 0, r[8] }; float l = sqrtf(d[0] * d[0] + d[2] * d[2]); if (l < 1e-3f) return; d[0] /= l; d[2] /= l;
    float from[3] = { p[0] + d[0] * 0.5f, p[1] + 1.3f, p[2] + d[2] * 0.5f };
    mp_send_shot(from[0], from[1], from[2], d[0], d[1], d[2], 0);
    int best = -1; float bestt = 1e9f;
    for (int j = 0; j < n; j++) {   // the nearest player whose body (a 0.6 m capsule from their feet to 1.8 m) the line passes
        if (o[j].flags & (MP_PF_DEAD | MP_PF_INVULN)) continue;
        float rel[3] = { o[j].x - from[0], 0, o[j].z - from[2] };
        float tt = rel[0] * d[0] + rel[2] * d[2]; if (tt < 0.5f || tt > 60) continue;
        float cx = from[0] + d[0] * tt - o[j].x, cz = from[2] + d[2] * tt - o[j].z;
        float y = from[1] - o[j].y;
        if (cx * cx + cz * cz < 0.6f * 0.6f && y > -0.3f && y < 2.1f && tt < bestt) { bestt = tt; best = j; }
    }
    if (best >= 0) mp_send_hit(o[best].id, MP_HIT_ARROW);
    MP_GAME_LOG("shot: from %.1f %.1f %.1f toward %.2f %.2f (%s)%s%s", from[0], from[1], from[2], d[0], d[2], atomic_load(&g_gate_shoot_seen) ? "the shot gate" : "the BowShoot command",
                best >= 0 ? ", hit " : ", no hit", best >= 0 ? o[best].name : "");
}

// ---- the ranged weapon (re/ITEMS.md): online, each Link gets his age's weapon once, through the game's own pouch push, the function
// chests, shops and the new-game setup end in (push starts no event: the get-item cutscene is the event flow around it). Adult Link: a
// quiver (arrow cap 30, filled) then the Fairy Bow; young Link: a seed bag then the Fairy Slingshot (the bow is adult-only, the slingshot
// child-only). push puts the weapon on that age's item button when the button is empty; the next frame the button is set if it is not.
// The ammo is refilled after each respawn and, slowly, when it runs low. All of it goes into the online copy of the save (hle_mp.c).
// World tick only (the game's own callers run on its main thread; the pending list is not locked). DM_MP_KIT=0: none of it
typedef uint64_t (*f_pouch_push)(void *mgr, const char **name, uint32_t cat, int32_t num, uint32_t reason, bool from_bundle, bool rental, bool get_event, uint8_t no_tab_hint);
typedef const uint8_t *(*f_pouch_actor_info)(const char **name);
typedef uint64_t (*f_pouch_category)(uint32_t *out, const char *const *s);
typedef int32_t (*f_get_assigned)(void *mgr, bool child, bool latest);
typedef void (*f_set_assigned)(void *mgr, bool child, int32_t item, int32_t mode, bool b);
typedef void (*f_setbool)(void *mgr, bool v, uint32_t hash);
typedef uint64_t (*f_getint)(void *mgr, int32_t *out, uint32_t hash);
typedef struct { const char *bag, *weapon, *ammo, *what; uint32_t h_max, h_ammo, h_weapon, h_guide; int32_t item; } kit_t;
static const kit_t KIT[2] = {   // [child]; hashes: PouchContentMaxNum.<ammo>, PouchData.<ammo / weapon>.StockNum, ActionGuide_IsEnd.<weapon>
    { "Item_Quiver_Small", "Weapon_Range_Bow", "Item_Arrow_Normal", "bow", 0x418db791u, 0x518ec74bu, 0x938d56bfu, 0x4f1b84c3u, 30 },
    { "Item_BulletBag_Small", "Weapon_Range_SlingShot", "Item_DekuSeed", "slingshot", 0xae8cc742u, 0x5fadf7fcu, 0x80d34ce9u, 0x066b5169u, 4 } };
enum { POUCH_REASON_SEQ = 8, H_SEQ_PAUSE = 0x604aebe1u };
static struct { int step, child, tries; double t, refill_t; } g_kit;
static void *gmd(void) { return (void *)rp((uint64_t)M + G_GAMEDATA); }
static int32_t gd_int(uint32_t h) { int32_t v = 0; return (((f_getint)fn(F_GETINT))(gmd(), &v, h) & 0xff) ? v : -1; }
static uint64_t pouch_mgr(void) {   // ready once its GameData struct refs (PouchData, PlayerStatus) are set
    uint64_t m = rp((uint64_t)M + G_POUCH);
    return m && gmd() && rp(m + 0x3bb0) && rp(m + 0x3bc0) ? m : 0;
}
static int kit_idle(uint64_t link) {   // no event (get / chest / talk), no load, no menu
    uint64_t em = rp((uint64_t)M + G_EVENT_MGR);
    return link && r32(link + A_STATE) == 4 && em && !rp(em + 0x68) && !event_now()[0] && !gd_bool(H_LOADING) && !gd_bool(H_SEQ_PAUSE);
}
static int kit_age(int child) {   // push files the button under PlayingInfo's age: it must agree with GameData's
    uint64_t pi = rp((uint64_t)M + G_PLAYING_INFO); uint8_t c = 0xff;
    return pi && rd(pi + 0x5c0, &c, 1) && c == (uint8_t)child && gd_bool(H_CHILDHOOD) == child;
}
static int push_named(uint64_t mgr, const char *actor, int32_t num) {   // as the game's SeqOneShotPushToPouch: the category from the item's row
    const char *n = actor; uint32_t cat = 0;
    const uint8_t *row = ((f_pouch_actor_info)fn(F_POUCH_ACTOR_INFO))(&n);
    if (!row || !(((f_pouch_category)fn(F_POUCH_CATEGORY))(&cat, (const char *const *)(row + 0x70)) & 0xff)) { MP_GAME_LOG("kit: no pouch row for %s", actor); return 0; }
    int ok = (((f_pouch_push)fn(F_POUCH_PUSH))((void *)mgr, &n, cat, num, POUCH_REASON_SEQ, false, false, false, 0) & 0xff) != 0;
    MP_GAME_LOG("kit: %s x%d (category %u) %s", actor, num, cat, ok ? "given" : "[RED] refused");
    return ok;
}
static void kit_refill(int child) {   // the ammo to its cap (PouchContentMaxNum.<ammo>, what getMaxNum returns for these): a queued write
    const kit_t *k = &KIT[child]; int32_t max = gd_int(k->h_max), have = gd_int(k->h_ammo);
    if (max > 0 && have >= 0 && have < max) ((f_setint)fn(F_SETINT))(gmd(), max, k->h_ammo);
}
static void kit_tick(uint64_t link) {
    static env_switch sw = { .name = "DM_MP_KIT" };
    if (!F_POUCH_PUSH || !strcmp(env_now(&sw), "0")) return;
    double t = now_s(); uint64_t mgr = pouch_mgr();
    if (!mgr || !kit_idle(link)) { if (!g_kit.step) g_kit.t = 0; return; }   // the give waits for 2 s of Link idle in one go
    int child = link_is_child(link) ? 1 : 0; const kit_t *k = &KIT[child];
    static int said; if (!said) {   // once: what the give will rely on (re/ITEMS.md section 9, step 1)
        said = 1; uint64_t pi = rp((uint64_t)M + G_PLAYING_INFO); uint8_t c = 0xff; if (pi) rd(pi + 0x5c0, &c, 1);
        MP_GAME_LOG("kit: pouch manager %#llx (PouchData %#llx, PlayerStatus %#llx); age: Link %s, PlayingInfo %d, GameData %s; %s owned %d, cap %d, ammo %d, button %d",
                    (unsigned long long)mgr, (unsigned long long)rp(mgr + 0x3bb0), (unsigned long long)rp(mgr + 0x3bc0), child ? "young" : "adult", c,
                    gd_bool(H_CHILDHOOD) ? "young" : "adult", k->what, gd_int(k->h_weapon), gd_int(k->h_max), gd_int(k->h_ammo),
                    ((f_get_assigned)fn(F_GET_ASSIGNED))((void *)mgr, child, false));
    }
    if (g_kit.step && g_kit.child != child) g_kit.step = 0;   // the other age (a new Link): his own weapon
    switch (g_kit.step) {
    case 0:   // give what is missing; Link must have stood idle a moment (not the frame a load ended)
        if (!g_kit.t) { g_kit.t = t; return; }
        if (t - g_kit.t < 2 || !kit_age(child)) return;
        if (++g_kit.tries > 3) { if (g_kit.tries == 4) MP_GAME_LOG("[RED] kit: the %s could not be given (3 tries)", k->what); return; }
        ((f_setbool)fn(F_SETBOOL))(gmd(), true, k->h_guide);   // no "how to use" card when it lands on the button
        if (gd_int(k->h_max) <= 0 && !push_named(mgr, k->bag, 1)) return;      // cap 30 and the ammo filled
        if (gd_int(k->h_weapon) <= 0 && !push_named(mgr, k->weapon, 1)) return; // owned, on the button if it was empty
        g_kit.child = child; g_kit.step = 1; g_kit.t = t; g_kit.tries = 0; return;
    case 1: {   // a frame later the queued GameData writes are in: the button, the ammo
        int32_t on = ((f_get_assigned)fn(F_GET_ASSIGNED))((void *)mgr, child, false);
        if (on != k->item) ((f_set_assigned)fn(F_SET_ASSIGNED))((void *)mgr, child, k->item, 2, true);
        kit_refill(child); g_kit.step = 2; return; }
    case 2: {   // check and say it once
        int32_t w = gd_int(k->h_weapon), m = gd_int(k->h_max), n = gd_int(k->h_ammo), on = ((f_get_assigned)fn(F_GET_ASSIGNED))((void *)mgr, child, false);
        MP_GAME_LOG("kit: %s Link: %s owned %d, ammo %d of %d, item button %d (%s)", child ? "young" : "adult", k->what, w, n, m, on, on == k->item ? "the weapon" : "[RED] not the weapon");
        g_kit.step = 3; g_kit.refill_t = t; return; }
    default:   // ready: refill after a respawn at once, and when low (under a third) every 20 s
        if (g_kit_refill_req || (t - g_kit.refill_t > 20 && gd_int(k->h_ammo) < gd_int(k->h_max) / 3)) { kit_refill(child); g_kit_refill_req = 0; g_kit.refill_t = t; }
    }
}

// ---- test footage (tools/demo_compose.py). DM_MP_TRACE=<file>: Link's state every frame (frame, position, facing, the animation
// the network carries, age, the game's own command). DM_MP_DEMO=<file> with DM_MP_DEMO_AT=<frame>: the other players come from that
// file from that frame on, one row per player per frame, in place of the server's. With DM_FSTEP_ONE a frame is 1/60 s of game time
// however slowly a run that saves every frame goes, so recorded players move exactly as fast as the game animates them
uint64_t dm_guest_frame(void);
static void trace_tick(uint64_t a) {
    static int on = -1; static FILE *f;
    if (on < 0) { const char *p = getenv("DM_MP_TRACE"); on = p && *p && (f = fopen(p, "w")) != NULL; if (on) fprintf(f, "#frame\tx\ty\tz\tyaw\tanim\tchild\tcmd\n"); }
    if (!on) return;
    mp_local_state st; if (!mp_game_read_local(&st)) return;
    char cmd[64]; if (!current_command(a, cmd, sizeof cmd)) snprintf(cmd, sizeof cmd, "?");
    fprintf(f, "%llu\t%.4f\t%.4f\t%.4f\t%u\t%u\t%d\t%s\n", (unsigned long long)dm_guest_frame(), st.x, st.y, st.z, st.yaw, st.anim, !!(st.equip & MP_EQ_CHILD), cmd);
    fflush(f);   // a run ends by a signal: every line already on disk
}
static void apply_damage(int quarter_hearts, uint16_t attacker);
typedef struct { uint32_t f; mp_remote r; } demo_row;
// Event rows (tools/demo_compose.py): "E frame shot shooter x y z dx dy dz" (an arrow from another player, drawn as theirs are),
// "E frame damage quarter_hearts attacker" (a hit on our Link, as the server's), "E frame notice text" (the game's info bar),
// "E frame warp x y z yaw" (our Link moved there: footage recordings walk him along several paths in one run).
// Input rows: "I frame buttons lx ly rx ry" (hex buttons, sticks -32767..32767): player 1's pad from that frame to the next I row
typedef struct { uint32_t f; char kind; uint16_t who; int qh; float v[6]; char text[96]; } demo_event;
typedef struct { uint32_t f; uint64_t buttons; int32_t axes[4]; } demo_input;
static demo_event *g_dev; static size_t g_ndev, g_dev_at;
static demo_input *g_din; static size_t g_ndin;
static uint64_t g_demo_at; static int g_demo_on = -1;
void mp_game_demo_input(uint64_t *buttons, int32_t axes[4]) {   // hle_mp.c, at player 1's input read: the scripted pad, if any for now
    if (g_demo_on != 1 || !g_ndin) return;
    uint64_t fr = dm_guest_frame(); if (fr < g_demo_at) return;
    uint64_t rel = fr - g_demo_at; const demo_input *cur = NULL;
    for (size_t i = 0; i < g_ndin && g_din[i].f <= rel; i++) cur = &g_din[i];
    if (!cur) return;
    *buttons = cur->buttons; memcpy(axes, cur->axes, sizeof cur->axes);
}
static void demo_events(uint64_t rel) {
    while (g_dev_at < g_ndev && g_dev[g_dev_at].f <= rel) {
        const demo_event *e = &g_dev[g_dev_at++];
        if (e->kind == 's') mp_game_shot_fx(e->who, e->v[0], e->v[1], e->v[2], e->v[3], e->v[4], e->v[5], 0);
        else if (e->kind == 'd') apply_damage(e->qh, e->who);
        else if (e->kind == 'n') mp_game_notice(e->text);
        else if (e->kind == 'w') { uint64_t a = player_actor(); if (a) { float m[12]; yaw_matrix(m, e->v[0], e->v[1], e->v[2], (uint16_t)e->v[3]); ((f_warp_to_matrix)fn(F_WARP_TO_MATRIX))((void *)a, m, false); } }
        MP_GAME_LOG("demo: event %c at %u", e->kind, e->f);
    }
}
static const mp_remote *demo_others(const mp_remote *o, int *n) {
    static int on = -1; static demo_row *rows; static size_t nrows, at_row; static uint64_t at; static mp_remote cur[MAX_PUPPETS]; static int ncur, said;
    if (on < 0) {
        on = 0; g_demo_on = 0; const char *p = getenv("DM_MP_DEMO"), *e = getenv("DM_MP_DEMO_AT");
        FILE *f = p && *p ? fopen(p, "r") : NULL; if (!f) return o;
        char line[256]; size_t cap = 0, ecap = 0, icap = 0;
        while (fgets(line, sizeof line, f)) {   // frame id name outfit child x y z yaw anim [flags], in frame order
            if (line[0] == 'E') {
                demo_event ev; memset(&ev, 0, sizeof ev); char kind[16]; int k = 0; unsigned fr, who;
                if (sscanf(line, "E %u %15s %n", &fr, kind, &k) < 2) continue;
                ev.f = fr; const char *rest = line + k;
                if (!strcmp(kind, "shot") && sscanf(rest, "%u %f %f %f %f %f %f", &who, &ev.v[0], &ev.v[1], &ev.v[2], &ev.v[3], &ev.v[4], &ev.v[5]) == 7) { ev.kind = 's'; ev.who = (uint16_t)who; }
                else if (!strcmp(kind, "damage") && sscanf(rest, "%d %u", &ev.qh, &who) == 2) { ev.kind = 'd'; ev.who = (uint16_t)who; }
                else if (!strcmp(kind, "notice")) { ev.kind = 'n'; snprintf(ev.text, sizeof ev.text, "%s", rest); ev.text[strcspn(ev.text, "\r\n")] = 0; }
                else if (!strcmp(kind, "warp") && sscanf(rest, "%f %f %f %f", &ev.v[0], &ev.v[1], &ev.v[2], &ev.v[3]) == 4) ev.kind = 'w';   // our Link to x y z, facing yaw (0..65535)
                else continue;
                if (g_ndev == ecap) { ecap = ecap ? ecap * 2 : 64; demo_event *ne = realloc(g_dev, ecap * sizeof *g_dev); if (!ne) break; g_dev = ne; }
                g_dev[g_ndev++] = ev; continue;
            }
            if (line[0] == 'I') {
                demo_input in; memset(&in, 0, sizeof in); unsigned fr; unsigned long long b; int a0, a1, a2, a3;
                if (sscanf(line, "I %u %llx %d %d %d %d", &fr, &b, &a0, &a1, &a2, &a3) != 6) continue;
                in.f = fr; in.buttons = b; in.axes[0] = a0; in.axes[1] = a1; in.axes[2] = a2; in.axes[3] = a3;
                if (g_ndin == icap) { icap = icap ? icap * 2 : 64; demo_input *ni = realloc(g_din, icap * sizeof *g_din); if (!ni) break; g_din = ni; }
                g_din[g_ndin++] = in; continue;
            }
            demo_row d; memset(&d, 0, sizeof d); unsigned id, yaw, anim, outfit, child, flags = 0; char name[MP_NAME_MAX];
            if (line[0] == '#' || sscanf(line, "%u %u %23s %u %u %f %f %f %u %u %u", &d.f, &id, name, &outfit, &child, &d.r.x, &d.r.y, &d.r.z, &yaw, &anim, &flags) < 10) continue;
            d.r.id = (uint16_t)id; d.r.yaw = (uint16_t)yaw; d.r.anim = (uint16_t)anim; d.r.outfit = (uint8_t)outfit; d.r.equip = child ? MP_EQ_CHILD : 0; d.r.hp_pct = 100; d.r.flags = (uint8_t)flags;
            snprintf(d.r.name, sizeof d.r.name, "%s", name);
            if (nrows == cap) { cap = cap ? cap * 2 : 8192; demo_row *nr = realloc(rows, cap * sizeof *rows); if (!nr) break; rows = nr; }
            rows[nrows++] = d;
        }
        fclose(f); at = e ? strtoull(e, 0, 0) : 0; on = nrows > 0 || g_ndev > 0 || g_ndin > 0; g_demo_at = at; g_demo_on = on;   // (a recording: events and input only)
        MP_GAME_LOG("demo: %zu rows, %zu events, %zu input rows from %s, playing from frame %llu", nrows, g_ndev, g_ndin, p, (unsigned long long)at);
    }
    if (!on) return o;
    uint64_t fr = dm_guest_frame();
    if (fr < at) { *n = 0; return cur; }   // before it starts: nobody (a demo run shows only its own players)
    uint64_t rel = fr - at; size_t i = at_row;
    demo_events(rel);
    while (i < nrows && rows[i].f < rel) i++;
    if (i < nrows && rows[i].f == rel) { ncur = 0; for (; i < nrows && rows[i].f == rel; i++) if (ncur < MAX_PUPPETS) cur[ncur++] = rows[i].r; }
    at_row = i;   // past the last row the last frame's players stay where they are
    if (!said) { said = 1; MP_GAME_LOG("demo: started at frame %llu with %d players", (unsigned long long)fr, ncur); }
    *n = ncur; return cur;
}

static void notices_tick(void);
static void flights_tick(uint32_t scene);
static void tags_tick(const mp_remote *o, int n);
// ---- the game thread's part (hle_mp.c dm_mp_game_tick)
void mp_game_update_puppets(const mp_remote *others, int n) {
    if (!ready()) return;
    test_tick(0);
    travel_tick();
    if (!live()) return;
    uint64_t a = player_actor(); if (!a) { memset(g_pup, 0, sizeof g_pup); return; }
    char bn[64]; if (!banc_name(bn, sizeof bn)) return;
    trace_tick(a);
    bb_probe(a);
    others = demo_others(others, &n);
    puppets_update(a, others, n, mm3(bn));
    local_shots(a, others, n);
    kit_tick(a);
    resp_tick(a);
    flights_tick(mm3(bn));
    tags_tick(others, n);
}
// ---- the gates
// Story cutscenes (Dm_<Area>_*) are refused while live with DM_MP_CUTSCENES unset or 0; system flows (Dm_SY_*, DmF_SY_*: area changes,
// saves, game over) and everything else go through. A refusal points requestEvent's first load at a stand-in manager whose flag bit 11
// makes it return false before it does anything (main+0xd7a824 -> 0xd7a924). Every name is logged once with DM_MP_EVENTLOG=1
static uint32_t g_refusing_mgr[0x100 / 4];
static int g_cut = -1, g_evlog = -1;
static int seen_before(const char *name) {   // a small set of names already logged
    static uint32_t seen[512]; static int n; uint32_t h = mm3(name);
    for (int i = 0; i < n; i++) if (seen[i] == h) return 1;
    if (n < 512) seen[n++] = h;
    return 0;
}
void mp_game_gate_event(uint64_t *regs) {
    if (!ready()) return;
    char name[96] = ""; uint64_t p = rp(regs[1] + 0x1a90);
    for (size_t i = 0; p && i < sizeof name - 1 && rd(p + i, name + i, 1) && name[i]; i++) name[i + 1] = 0;
    if (g_cut < 0) { const char *e = getenv("DM_MP_CUTSCENES"); g_cut = !(e && atoi(e)); g_evlog = getenv("DM_MP_EVENTLOG") && atoi(getenv("DM_MP_EVENTLOG")); }
    int story = !strncmp(name, "Dm_", 3) && strncmp(name, "Dm_SY_", 6) != 0;
    int refuse = story && g_cut && live();
    if ((g_evlog || refuse) && !seen_before(name)) MP_GAME_LOG("event %s: %s", refuse ? "refused" : "requested", name);
    if (refuse) { g_refusing_mgr[0xb8 / 4] = 1u << 11; regs[0] = (uint64_t)g_refusing_mgr; }
}
void mp_game_gate_gameover(uint64_t *regs) {
    if (!ready()) return;
    uint8_t f = 0; rd(regs[19] + 0x1c34, &f, 1);
    if (f & 0x40) { static double last; double t = now_s(); if (t - last > 5) { last = t; MP_GAME_LOG("game over requested (player component %#llx)", (unsigned long long)regs[19]); } }
}
void mp_game_gate_shoot(uint64_t *regs) {
    if (!ready()) return;
    atomic_store(&g_gate_shoot_seen, 1);
    uint64_t a = player_actor(); if (!a) return;
    if (regs[0] == comp(a, 123)) atomic_store(&g_gate_shot, 1);   // Link's own Shooter component (components.txt: 123)
}

// ---------------------------------------------------------------- the lobby in the game's own system window
// re/ui/CODE.md recipe (b) on the game's SystemWindow_00 over SystemBG_00 (runs/second: our sentence and three buttons in the
// game's style, the D-pad moving the focus through the game's own focus manager, Link still while it is open). Texts are set
// while the window is closed (the game's own SystemWindow preset does the same); a label that changes while it is open goes
// through the window's request queue. A choice closes the window; hle_mp.c asks for it again when the lobby should stay.
static struct { int st, armed, focus, fresh; double t, t_open, link_since; char text[256], lab[3][64]; ui_msg_req rq[3]; uint8_t last[3]; } g_lb;   // st: 0 closed, 1 opening, 2 open, 3 closing
void mp_game_world_prep(void) {
    if (atomic_exchange(&g_unload_window, 0) && ready()) {   // runs/thirteenth, r14: a window reopened after a choice kept its old button states and never took the
        int32_t i; uint64_t w = screen_of("SystemWindow_00", &i);   // focus again; a newly loaded one starts clean, as the first one did
        if (w && ((f_screen_bool)fn(F_IS_CLOSED))((void *)w)) { typedef void (*f_unload)(void *ui, uint32_t hash); ((f_unload)fn(B->f_unload_screen))((void *)rp((uint64_t)M + G_UI), mm3("SystemWindow_00")); MP_GAME_LOG("lobby: window unloaded, a fresh one next"); }
    }
    if (!atomic_exchange(&g_load_lobby, 0) || !ready() || !player_actor()) return;
    void *ui = (void *)rp((uint64_t)M + G_UI); int32_t i;
    if (!screen_of("SystemBG_00", &i) && i >= 0) ((f_load_screen)fn(F_LOAD_SCREEN))(ui, mm3("SystemBG_00"));
    if (!screen_of("SystemWindow_00", &i) && i >= 0) ((f_load_screen)fn(F_LOAD_SCREEN))(ui, mm3("SystemWindow_00"));
}
int mp_game_lobby(int want, const char *text, const char *const labels[3], int *decided) {
    *decided = -1;
    if (!ready() || !player_actor()) { g_lb.st = 0; g_lb.link_since = 0; return 0; }
    // Not in Link's first 1.5 s in the world: the game's menus keep the A that confirmed "Load" through the loading and hand it to the
    // first window that opens after it (runs/tenth: one deliberate press on Load also picked Play)
    if (!g_lb.link_since) g_lb.link_since = now_s();
    if (want && g_lb.st == 0 && now_s() - g_lb.link_since < 1.5) return 0;
    int32_t ib, iw; uint64_t bg = screen_of("SystemBG_00", &ib), win = screen_of("SystemWindow_00", &iw);
    if (ib < 0 || iw < 0) return 0;
    if (!bg || !win) {   // not loaded (SystemWindow_00 only once used; every screen goes on title <-> game): the world tick loads them
        if (!want) { g_lb.st = 0; return 0; }
        atomic_store(&g_load_lobby, 1);
        return 1;
    }
    int closed = ((f_screen_bool)fn(F_IS_CLOSED))((void *)win), open = ((f_screen_bool)fn(F_IS_OPEN))((void *)win);
    if (!want) {
        if (g_lb.st == 1 || g_lb.st == 2) { screen_call(win, 0x30, -1); g_lb.st = 3; }
        if (g_lb.st && !((f_screen_bool)fn(F_IS_CLOSED))((void *)bg) && !((f_screen_bool)fn(B->f_closing))((void *)bg)) screen_call(bg, 0x30, -1);   // and its background (once)
        if (g_lb.st == 3 && closed) g_lb.st = 0;
        return g_lb.st;
    }
    int bg_closed = ((f_screen_bool)fn(F_IS_CLOSED))((void *)bg);
    if (g_lb.st == 3 && closed && bg_closed && g_lb.fresh) { g_lb.fresh = 0; atomic_store(&g_unload_window, 1); return 3; }   // after a choice: a new window
    if (g_lb.st == 0 || (g_lb.st == 3 && closed && bg_closed)) {   // both closed: they open together, as the first time (runs/thirteenth: a window
        if (!closed || !bg_closed) {                              // reopened over the still-open background got no focus and took no input)
            if (g_lb.st == 0 && !bg_closed && !((f_screen_bool)fn(B->f_closing))((void *)bg)) screen_call(bg, 0x30, -1);
            return 3;
        }
        snprintf(g_lb.text, sizeof g_lb.text, "%s", text);
        for (int k = 0; k < 3; k++) snprintf(g_lb.lab[k], sizeof g_lb.lab[k], "%s", labels[k]);
        screen_text(win, "T_Text_00", g_lb.text);
        static const char *path[3] = { "Pa_Button_00/T_Btn_00", "Pa_Button_01/T_Btn_00", "Pa_Button_02/T_Btn_00" };
        for (int k = 0; k < 3; k++) screen_text(win, path[k], g_lb.lab[k]);
        typedef void *(*f_anim)(void *layout, const char *name); typedef void (*f_pose)(void *p, float frame);
        void *bn = ((f_anim)fn(F_ANIM_PLAYER))((void *)rp(win + 0x28), "BtnNum"); if (bn) ((f_pose)fn(F_ANIM_POSE))(bn, 3.0f);
        uint64_t cm = rp(win + 0x38);
        typedef void *(*f_ctl)(void *cm, const char *name); typedef void (*f_focus)(void *s, void *ctl, int z);
        static const char *fb[3] = { "Pa_Button_00", "Pa_Button_01", "Pa_Button_02" };   // the button last chosen keeps the focus (Tunic, pressed again and again)
        void *first = cm ? ((f_ctl)fn(F_CONTROL_BY_NAME))((void *)cm, fb[g_lb.focus % 3]) : NULL; if (first) ((f_focus)fn(F_FOCUS))((void *)win, first, 0);
        screen_call(bg, 0x28, 1);
        screen_call(win, 0x28, 1);
        memset(g_lb.rq, 0, sizeof g_lb.rq); g_lb.st = 1; g_lb.t = now_s(); g_lb.armed = 0;
        return 1;
    }
    if (g_lb.st == 1 && open) { g_lb.st = 2; g_lb.t_open = now_s(); }
    if (g_lb.st == 1 && now_s() - g_lb.t > 5 && !open) { g_lb.st = 0; return 0; }   // never opened (the game closed it): give up this time
    if (g_lb.st != 2) return g_lb.st;
    if (!open) { g_lb.st = 0; return 0; }   // closed by the game (a cutscene, the pause menu, a load)
    static const char *path[3] = { "Pa_Button_00/T_Btn_00", "Pa_Button_01/T_Btn_00", "Pa_Button_02/T_Btn_00" };
    for (int k = 0; k < 3; k++)   // a changed label: queued to the open window (the buffer stays ours until the next change)
        if (strcmp(g_lb.lab[k], labels[k]) && !g_lb.rq[k].queue) { snprintf(g_lb.lab[k], sizeof g_lb.lab[k], "%s", labels[k]); screen_text_open(win, &g_lb.rq[k], path[k], g_lb.lab[k]); }
    // The choice (runs/twelfth, each button's words logged as the D-pad and A moved them): the u16 at +0x38 is the focus state (0 idle,
    // 1 gaining focus, 3 focused, 2 losing it, 4 for the frames its Decide animation plays); the byte at +0x3a is 1 for a button that had
    // the focus and 2 for one A decided, and stays 2; bit 0x1000 of +0x40 marks the focused button. So decided = +0x3a == 2 (or the
    // passing 4). Reading "3" as decided picked Play each time the window opened (runs/fifth..eleventh). Logged when they change
    uint64_t cm = rp(win + 0x38); if (!cm) return g_lb.st;
    typedef void *(*f_ctl)(void *cm, const char *name); static const char *btn[3] = { "Pa_Button_00", "Pa_Button_01", "Pa_Button_02" };
    uint32_t stt[3] = { 0 }, fl[3] = { 0 }; int chosen = -1, any = 0;
    for (int k = 0; k < 3; k++) { uint64_t c = (uint64_t)((f_ctl)fn(F_CONTROL_BY_NAME))((void *)cm, btn[k]); if (!c) continue;
        stt[k] = r32(c + 0x38); fl[k] = r32(c + 0x40); if ((stt[k] & 0xff) == 4 || ((stt[k] >> 16) & 0xff) == 2) { any = 1; if (chosen < 0) chosen = k; } }
    static uint32_t last_st[3], last_fl[3];
    if (memcmp(stt, last_st, sizeof stt) || memcmp(fl, last_fl, sizeof fl)) {
        MP_GAME_LOG("lobby: buttons +0x38/+0x40 %08x/%08x %08x/%08x %08x/%08x", stt[0], fl[0], stt[1], fl[1], stt[2], fl[2]);
        memcpy(last_st, stt, sizeof stt); memcpy(last_fl, fl, sizeof fl);
    }
    if (!any) g_lb.armed = 1;
    if (g_lb.armed && chosen >= 0) {
        *decided = chosen; g_lb.focus = chosen; MP_GAME_LOG("lobby: '%s' chosen", g_lb.lab[chosen]);
        *(volatile uint32_t *)(cm + 0x40) = r32(cm + 0x40) & ~2u;   // taken: as the game's own wait for a button does (re/ui/CODE.md 2.5), so the window can decide again
        screen_call(win, 0x30, -1); screen_call(bg, 0x30, -1); g_lb.st = 3; g_lb.fresh = 1;   // both close; if the lobby stays, a new window opens once closed
    }
    return g_lb.st;
}

// ---------------------------------------------------------------- notices in the game's own info bar
// SimpleInfo_00 (runs/second: our sentence in the game's bar and font; it closes by itself after about 3 s, like the game's own
// notices). Lines are queued from any thread and shown one at a time from the game thread, only while the bar is closed (the game's
// own notices go first) and the lobby window is not up. Each shown line keeps its own buffer until the next one replaces it
#define NOTICES 8
static struct { char line[NOTICES][128]; int head, tail; pthread_mutex_t mu; char shown[2][128]; int which; double t; } g_nt = { .mu = PTHREAD_MUTEX_INITIALIZER };
void mp_game_notice(const char *line) {
    pthread_mutex_lock(&g_nt.mu);
    if ((g_nt.tail + 1) % NOTICES != g_nt.head) { snprintf(g_nt.line[g_nt.tail], sizeof g_nt.line[0], "%s", line); g_nt.tail = (g_nt.tail + 1) % NOTICES; }
    pthread_mutex_unlock(&g_nt.mu);
}
void mp_game_ui_tick(void) { if (!ready()) return; test_tick(1); notices_tick(); }
static void notices_tick(void) {
    if (g_nt.head == g_nt.tail || g_lb.st || !player_actor() || now_s() - g_nt.t < 0.4) return;
    int32_t i; uint64_t sc = screen_of("SimpleInfo_00", &i);
    if (!sc || !((f_screen_bool)fn(F_IS_CLOSED))((void *)sc) || ((f_screen_bool)fn(B->f_opening))((void *)sc)) return;   // not loaded, or in use / opening
    pthread_mutex_lock(&g_nt.mu);
    g_nt.which ^= 1; snprintf(g_nt.shown[g_nt.which], sizeof g_nt.shown[0], "%s", g_nt.line[g_nt.head]); g_nt.head = (g_nt.head + 1) % NOTICES;
    pthread_mutex_unlock(&g_nt.mu);
    screen_text(sc, "T_Text_00", g_nt.shown[g_nt.which]); screen_call(sc, 0x28, 1); g_nt.t = now_s();
}

// ---- the others' shots in flight: a display arrow (Pouch_Arrow_Normal, the inventory screen's arrow: a model with no AI or physics,
// as the stand-ins began with the inventory's PouchPlayer) carried along the shot's line at 45 m/s for its range (40 m), or up to our
// Link when it passes him, then deleted. DM_MP_ARROWS=0: none; DM_MP_ARROW_SPEED: another speed (a slow arrow for a test's screenshot).
// Created and moved like a stand-in (puppet_request / puppets_update); hle_mp.c hands the server's shots over on the game thread
#define FLIGHTS 8
typedef struct { uint64_t watch, actor; float p[3], d[3], maxd, speed; double t0; int live; uint32_t scene; uint8_t arg[0xa0] __attribute__((aligned(16))); } flight_t;
static uint32_t g_fl_scene;   // the area the world tick last saw: an arrow made in another one is dropped, not deleted (the area took it)
static flight_t g_fl[FLIGHTS];
static void dir_matrix(float *m, const float *p, const float *d) {   // Matrix34f, row-major: the display arrow's +y along d. Its head is +y (it
    // stands upright on the pouch screen): with -z along d every arrow flew upright, head to the sky (runs/vid23)
    float y[3] = { d[0], d[1], d[2] }, x[3] = { d[2], 0, -d[0] }, z[3];   // x = up x d: level, across the flight
    float l = sqrtf(x[0] * x[0] + x[2] * x[2]); if (l < 1e-4f) { x[0] = 1; x[2] = 0; l = 1; }   // (straight up or down: any level x)
    x[0] /= l; x[2] /= l;
    z[0] = x[1] * y[2] - x[2] * y[1]; z[1] = x[2] * y[0] - x[0] * y[2]; z[2] = x[0] * y[1] - x[1] * y[0];   // z = x * y (right-handed)
    float r[12] = { x[0], y[0], z[0], p[0], x[1], y[1], z[1], p[1], x[2], y[2], z[2], p[2] }; memcpy(m, r, sizeof r);
}
void mp_game_shot_fx(uint16_t s, float x, float y, float z, float dx, float dy, float dz, int k) {
    (void)s; (void)k;
    static env_switch sw = { .name = "DM_MP_ARROWS" };
    if (!ready() || !live() || !strcmp(env_now(&sw), "0") || !player_actor()) return;
    float l = sqrtf(dx * dx + dy * dy + dz * dz); if (l < 1e-3f) return;
    flight_t *f = NULL; for (int i = 0; i < FLIGHTS; i++) if (!g_fl[i].live) { f = &g_fl[i]; break; }
    if (!f) return;   // eight in the air already: this one is not drawn
    memset(f, 0, sizeof *f);
    f->p[0] = x; f->p[1] = y; f->p[2] = z; f->d[0] = dx / l; f->d[1] = dy / l; f->d[2] = dz / l;
    float m[12]; dir_matrix(m, f->p, f->d);
    float pos[3] = { x, y, z }, rot[9] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] }, sc[3] = { 1, 1, 1 };
    memcpy(f->arg + 0x00, pos, 12); memcpy(f->arg + 0x0c, rot, 36); memcpy(f->arg + 0x30, sc, 12);
    static const char *empty = ""; memcpy(f->arg + 0x40, &empty, 8);
    f->arg[0x4a] = 1; *(uint32_t *)(f->arg + 0x98) = 1 | 2 | 4;
    static const char *name = "Pouch_Arrow_Normal"; uint32_t result = 0;
    bool ok = ((f_create)fn(F_CREATE_ACTOR))((void *)rp((uint64_t)M + G_ACTOR_MGR), &name, f->arg, &f->watch, 1, NULL, NULL, NULL, 0, &result, NULL);
    if (!ok) { static int said; if (!said++) MP_GAME_LOG("arrow in flight not requested (result %u)", result); return; }
    f->t0 = game_now(); f->live = 1; f->scene = g_fl_scene;
    f->maxd = 45.0f * 0.9f;   // its range, or up to our Link when the line passes him (it hit us: the server says so separately)
    float me[3] = { 0, 0, 0 }; if (!rd(player_actor() + A_POS, me, sizeof me)) me[1] = -1e9f; me[1] += 1.0f;
    float rel[3] = { me[0] - x, me[1] - y, me[2] - z }, along = rel[0] * f->d[0] + rel[1] * f->d[1] + rel[2] * f->d[2];
    if (along > 0) { float cx = rel[0] - f->d[0] * along, cy = rel[1] - f->d[1] * along, cz = rel[2] - f->d[2] * along; if (cx * cx + cy * cy + cz * cz < 0.8f * 0.8f && along < f->maxd) f->maxd = along; }
    for (int i = 0; i < MAX_PUPPETS; i++) {   // and the other players: it ends in the first one it reaches (runs/vid23: one flew on through its target)
        const Puppet *q = &g_pup[i]; if (!q->id || q->id == s || !q->actor || r32(q->actor + A_STATE) != 4) continue;
        float qp[3]; if (!rd(q->actor + A_POS, qp, sizeof qp)) continue;
        qp[1] += q->child ? 0.7f : 1.0f;   // its chest
        float qr[3] = { qp[0] - x, qp[1] - y, qp[2] - z }, qa = qr[0] * f->d[0] + qr[1] * f->d[1] + qr[2] * f->d[2];
        if (qa < 0.3f || qa >= f->maxd) continue;
        float cx = qr[0] - f->d[0] * qa, cy = qr[1] - f->d[1] * qa, cz = qr[2] - f->d[2] * qa;
        if (cx * cx + cz * cz < 0.45f * 0.45f && fabsf(cy) < (q->child ? 0.8f : 1.0f)) f->maxd = qa - 0.15f;   // (its front, not its middle)
    }
    static env_switch sp = { .name = "DM_MP_ARROW_SPEED" }; f->speed = atof(env_now(&sp)); if (!(f->speed > 0)) f->speed = 45.0f;   // (a slow one for a test's screenshot)
}
static void flights_tick(uint32_t scene) {   // world tick: each arrow in the air one step on, the spent ones deleted
    double t = game_now(); g_fl_scene = scene;
    for (int i = 0; i < FLIGHTS; i++) {
        flight_t *f = &g_fl[i]; if (!f->live) continue;
        if (f->scene != scene) { watch_cancel(&f->watch); f->live = 0; continue; }   // Link changed area meanwhile: that area's actors went with it
        if (!f->actor && (f->actor = watch_take(&f->watch))) {
            static int said; if (said < 3) { said++; MP_GAME_LOG("arrow in flight: actor %#llx taken %.0f ms after the shot", (unsigned long long)f->actor, (t - f->t0) * 1e3); }
        }
        double dt = t - f->t0; float dist = f->speed * (float)dt;
        if (dist > f->maxd + 0.3f || dt > 20) {   // there (a stuck-arrow moment past the end), or a test's slow arrow given up
            if (f->actor && r32(f->actor + A_STATE) >= 3 && r32(f->actor + A_STATE) <= 6) ((f_request_delete)fn(F_REQUEST_DELETE))((void *)f->actor, 0);
            else if (!f->actor) watch_cancel(&f->watch);
            f->live = 0; continue;
        }
        if (!f->actor || r32(f->actor + A_STATE) != 4) continue;
        if (dist > f->maxd) dist = f->maxd;
        float p[3] = { f->p[0] + f->d[0] * dist, f->p[1] + f->d[1] * dist, f->p[2] + f->d[2] * dist }, m[12];
        dir_matrix(m, p, f->d);
        ((f_force_set_matrix)fn(F_FORCE_SET_MATRIX))((void *)f->actor, m, false);
    }
}
int  mp_game_take_local_events(void) { return 0; }
// ---- name tags: where the other players' heads are on screen. The game's world-to-screen (main+0x29d40d0) is a game call, so it runs
// here on the game thread (world tick) for each nearby player; the present thread's tags (hle_mp.c) read the last results through
// mp_game_project, matched by position. It projects with the "Main" camera into the given layer's viewport (layer+0x28; min x/y at
// +0x8/+0xc, max at +0x10/+0x14 of it): out = NDC * viewport size / 2, from the screen's center, y up. Two buffers: the tick fills
// one while the other is read
typedef struct { float x, y, z, sx, sy; int ok; } tag_pos;
static tag_pos g_tp[2][MP_MAX_NEAR]; static atomic_int g_tp_n[2], g_tp_cur;
static void tags_tick(const mp_remote *o, int n) {
    typedef void *(*f_layer)(void *list, uint32_t hash);
    typedef uint64_t (*f_w2s)(void *layer, float *out, float x, float y, float z);
    uint64_t gm = rp((uint64_t)M + B->g_gfx_mgr); void *list = gm ? (void *)rp(gm + 0x38) : NULL; if (!list) return;
    void *layer = ((f_layer)fn(B->f_layer))(list, 0x1b6ce343u); if (!layer) return;   // "Main"
    float vp[4]; if (!rd((uint64_t)layer + 0x30, vp, sizeof vp)) return;
    float w = vp[2] - vp[0], h = vp[3] - vp[1]; if (!(w > 1 && h > 1)) return;
    int b = !atomic_load(&g_tp_cur), k = 0;
    for (int j = 0; j < n && k < MP_MAX_NEAR; j++) {
        if (o[j].flags & MP_PF_DEAD) continue;
        float out[2] = { 0, 0 }, y = o[j].y + 2.1f;
        int ok = (((f_w2s)fn(B->f_w2s))(layer, out, o[j].x, y, o[j].z) & 0xff) != 0;   // true: in front of the camera
        g_tp[b][k++] = (tag_pos){ o[j].x, y, o[j].z, 0.5f + out[0] / w, 0.5f - out[1] / h, ok };
    }
    atomic_store(&g_tp_n[b], k); atomic_store(&g_tp_cur, b);
    static double said; static int probe = -1; if (probe < 0) probe = getenv("DM_MP_PROBE") && atoi(getenv("DM_MP_PROBE"));
    if (probe && k && now_s() - said > 5) { said = now_s();   // where the tags go (the overlay draws them in a window only)
        MP_GAME_LOG("tags: %d, viewport %.0fx%.0f; #1 at %.2f %.2f%s; #2 at %.2f %.2f%s", k, w, h, g_tp[b][0].sx, g_tp[b][0].sy, g_tp[b][0].ok ? "" : " (behind)",
                    k > 1 ? g_tp[b][1].sx : -1, k > 1 ? g_tp[b][1].sy : -1, k > 1 && !g_tp[b][1].ok ? " (behind)" : ""); }
}
int mp_game_project(float x, float y, float z, float *sx, float *sy) {   // present thread: the nearest result within 1.5 m, 0..1 from the top left
    int c = atomic_load(&g_tp_cur), n = atomic_load(&g_tp_n[c]), best = -1; float bd = 1.5f * 1.5f;
    for (int i = 0; i < n; i++) {
        float dx = g_tp[c][i].x - x, dy = g_tp[c][i].y - y, dz = g_tp[c][i].z - z, d = dx * dx + dy * dy + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0 || !g_tp[c][best].ok) return 0;
    *sx = g_tp[c][best].sx; *sy = g_tp[c][best].sy;
    return *sx >= 0 && *sx <= 1 && *sy >= 0 && *sy <= 1;
}
