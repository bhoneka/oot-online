// OoT Online in the nxnative host: starts the network client when DM_MP_SERVER is set, and once a presented frame
// (dm_mp_present, beside the cheat pass) hands Link's state to it and its news to the screen and the game.
//
//   DM_MP_SERVER  host[:port] of the OoT Online server (unset or empty: multiplayer off, nothing here runs)
//   DM_MP_NAME    the player's name;  DM_MP_OUTFIT  0 Kokiri, 1 Goron, 2 Zora;  DM_MP_CODE  the server's room code
//   DM_MP_LOG=1   log every event
// The lobby page comes up when the module starts (and with /lobby): it holds the game's input (dm_mp_input_filter, on player 1's
// reads in hle_hid.c) and takes the D-pad (outfit), Y (rename) and A (play). Name and outfit are kept in $DM_ROOT/oot_online.cfg.
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <copyfile.h>
#include <removefile.h>
#include <sys/stat.h>
#include "mp_net.h"
#include "mp_ui.h"
#include "mp_game.h"

#ifndef MP_HOST_LOG
#define MP_HOST_LOG(...) do { fprintf(stderr, "[dm] mp: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

// ---- weak defaults: no game knowledge (the module still chats, shows the others' count and the kill feed)
__attribute__((weak)) int  mp_game_read_local(mp_local_state *st) { (void)st; return 0; }
__attribute__((weak)) int  mp_game_project(float x, float y, float z, float *sx, float *sy) { (void)x; (void)y; (void)z; (void)sx; (void)sy; return 0; }
__attribute__((weak)) uint32_t mp_game_build(void) { return 0; }
__attribute__((weak)) void mp_game_apply_damage(int qh, uint16_t attacker, int kind) { (void)qh; (void)attacker; (void)kind; }
__attribute__((weak)) void mp_game_respawn(uint32_t scene, float x, float y, float z, uint16_t yaw, int own) { (void)scene; (void)x; (void)y; (void)z; (void)yaw; (void)own; }
__attribute__((weak)) int  mp_game_set_outfit(int outfit) { (void)outfit; return 0; }
__attribute__((weak)) void mp_game_update_puppets(const mp_remote *o, int n) { (void)o; (void)n; }
__attribute__((weak)) void mp_game_shot_fx(uint16_t s, float x, float y, float z, float dx, float dy, float dz, int k) { (void)s; (void)x; (void)y; (void)z; (void)dx; (void)dy; (void)dz; (void)k; }
__attribute__((weak)) int  mp_game_take_local_events(void) { return 0; }
__attribute__((weak)) int  mp_game_ready(void) { return 0; }
__attribute__((weak)) int  mp_game_go_to_field(void) { return 1; }
__attribute__((weak)) void mp_game_ui_tick(void) { }
__attribute__((weak)) void mp_game_world_prep(void) { }
__attribute__((weak)) void mp_game_notice(const char *line) { (void)line; }
__attribute__((weak)) void mp_game_demo_input(uint64_t *buttons, int32_t axes[4]) { (void)buttons; (void)axes; }
__attribute__((weak)) int  mp_game_lobby(int want, const char *text, const char *const labels[3], int *decided) { (void)want; (void)text; (void)labels; *decided = -1; return 0; }
__attribute__((weak)) void mp_game_gate_event(uint64_t *regs) { (void)regs; }
__attribute__((weak)) void mp_game_gate_gameover(uint64_t *regs) { (void)regs; }
__attribute__((weak)) void mp_game_gate_shoot(uint64_t *regs) { (void)regs; }
// nxnative's observation gates (hle.c); weak so the module links into tests without the host. Numbers: dm.h OBS_MP_* (integrate_nxnative.py)
extern void gate_obs_set(int id, void (*fn)(uint64_t *regs, int param)) __attribute__((weak));
// Where the game-side tick runs. Not in a gate handler: a gate handler runs on nxnative's gate stack, and game code called from there
// that makes a system call (a lock: requestCreateActorAsync did) enters the SVC gate on that same stack, which nxnative stops as fatal
// ("a gate handler ran guest code"). Player 1's input read is a take-over function the game calls on its own thread and stack, where
// calling back into the game is a plain nested call: the tick runs there, once a frame. The frame gate only marks the frame and its thread.
static atomic_int g_tick_gate;        // the frame gate has run
static _Atomic(pthread_t) g_gate_thread;   // the thread the frame gate runs on (the game's frame thread)
void dm_mp_game_tick(void);
static void log_thread(const char *what) {   // once per kind: which thread does what (nxnative names threads after the game's own)
    static char said[4][96]; static int n; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) if (!strcmp(said[i], what)) { pthread_mutex_unlock(&mu); return; }
    if (n < 4) snprintf(said[n++], sizeof said[0], "%s", what);
    pthread_mutex_unlock(&mu);
    char tn[64] = ""; pthread_getname_np(pthread_self(), tn, sizeof tn);
    MP_HOST_LOG("%s on thread '%s'", what, tn);
}
static void ui_tick(void);
static atomic_ullong g_n_gate, g_n_read, g_n_read_other, g_n_world, g_n_call;   // counters for DM_MP_PROBE's line every 5 s
// The frame call (the profile's patches.py, integrate_nxnative.py): a stub in the game's frame-ratio function calls the function whose
// address is at the frame step state page +0x800, once a frame, on the game's main thread and its own stack: both ticks run there, and
// game code may be called (locks included). Until it has run once, the frame gate and player 1's input read stand in as before
extern uint8_t *dm_fps_state __attribute__((weak));   // loader.c: the frame step state page (0 = none)
static atomic_int g_frame_call;
void dm_mp_game_tick(void);
static atomic_ullong g_call_ns, g_call_max_ns, g_pres_ns, g_pres_max_ns;   // what the module's per-frame work costs (DM_MP_PROBE's line)
static void time_add(atomic_ullong *sum, atomic_ullong *max, uint64_t d) {
    atomic_fetch_add(sum, d); unsigned long long m = atomic_load(max); while (d > m && !atomic_compare_exchange_weak(max, &m, d)) {}
}
static void frame_call(void) {
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    atomic_store(&g_frame_call, 1); atomic_fetch_add(&g_n_call, 1);
    static int said; if (!said) { said = 1; char tn[64] = ""; pthread_getname_np(pthread_self(), tn, sizeof tn); MP_HOST_LOG("the frame call runs on thread '%s': both ticks run there", tn); }
    ui_tick(); dm_mp_game_tick();
    time_add(&g_call_ns, &g_call_max_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
}
static void gate_tick(uint64_t *regs, int param) { (void)regs; (void)param; if (atomic_load(&g_frame_call)) return; atomic_store(&g_tick_gate, 1); atomic_store(&g_gate_thread, pthread_self()); atomic_fetch_add(&g_n_gate, 1); log_thread("the frame gate runs (the UI tick)"); ui_tick(); }
static void gate_event(uint64_t *regs, int param) { (void)param; mp_game_gate_event(regs); }
static void gate_gameover(uint64_t *regs, int param) { (void)param; mp_game_gate_gameover(regs); }
static void gate_shoot(uint64_t *regs, int param) { (void)param; mp_game_gate_shoot(regs); }


void dm_mp_game_tick(void);
// The host's presented-frame counter (hle.h); weak so the module also links into tests without the host
__attribute__((weak)) uint64_t dm_guest_frame(void) { static uint64_t n; return ++n; }
static atomic_int g_tick_in_input;   // the game-thread tick runs from player 1's input read (the game's own frame start)
static atomic_int g_on;
static pthread_mutex_t g_lb_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_lobby, g_lobby_press; static double g_lobby_t0; static uint64_t g_prev_btn;
static _Atomic double g_native_t0;   // when the game's own window last came up (a press in its first second is held back)
static atomic_int g_to_field;   // Play was chosen: the world tick takes Link to the shared place (DM_MP_STAY=1: stay where he is)
static atomic_int g_native;   // the lobby is the game's own window right now (mp_game_lobby): the drawn page stays hidden, the game's menu takes the pad
static char g_name[MP_NAME_MAX];
static int g_all_kills, g_all_deaths, g_all_best;
static int g_outfit, g_log;
static int g_dead; static double g_dead_t; static char g_killer[MP_NAME_MAX];
static mp_local_state g_last;     // the last state Link was seen in (the respawn's "own spot" when the server has none)

// Writes queued for the game thread (the game hook drains them; see dm_mp_game_tick)
typedef struct { int kind; mp_event e; } Pending;
static Pending g_pend[64]; static int g_pend_n; static pthread_mutex_t g_pend_mu = PTHREAD_MUTEX_INITIALIZER;
static void pend(int kind, const mp_event *e) {
    pthread_mutex_lock(&g_pend_mu);
    if (g_pend_n < 64) { g_pend[g_pend_n].kind = kind; g_pend[g_pend_n].e = *e; g_pend_n++; }
    pthread_mutex_unlock(&g_pend_mu);
}

static uint32_t load_key(void) {   // the per-install player key: made once, kept next to the save
    const char *root = getenv("DM_ROOT"); char path[1024];
    snprintf(path, sizeof path, "%s/oot_online.key", root && *root ? root : ".");
    uint32_t k = 0; FILE *f = fopen(path, "r");
    if (f) { if (fscanf(f, "%x", &k) != 1) k = 0; fclose(f); }
    if (!k) { k = arc4random() | 1; f = fopen(path, "w"); if (f) { fprintf(f, "%08x\n", k); fclose(f); } }
    return k;
}
static void cfg_path(char *out, size_t cap) { const char *root = getenv("DM_ROOT"); snprintf(out, cap, "%s/oot_online.cfg", root && *root ? root : "."); }
static void prefs_load(void) {
    char path[1024]; cfg_path(path, sizeof path); FILE *f = fopen(path, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "name=", 5) && line[5]) snprintf(g_name, sizeof g_name, "%s", line + 5);
        else if (!strncmp(line, "outfit=", 7)) g_outfit = atoi(line + 7) % 3;
    }
    fclose(f);
}
static void prefs_save(void) {
    char path[1024]; cfg_path(path, sizeof path); FILE *f = fopen(path, "w"); if (!f) return;
    fprintf(f, "name=%s\noutfit=%d\n", g_name, g_outfit); fclose(f);
}
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static void show_lobby(int on) {
    pthread_mutex_lock(&g_lb_mu); if (on && !g_lobby) g_lobby_t0 = now_s(); g_lobby = on; pthread_mutex_unlock(&g_lb_mu);
}
static void on_send(const char *text) {
    if (!strcmp(text, "/lobby")) { show_lobby(1); return; }   // the client's own command: back to the lobby page
    mp_send_chat(text);
}
static void on_rename(const char *name) {
    char clean[MP_NAME_MAX]; size_t n = 0;
    for (const char *s = name; *s && n < sizeof clean - 1; s++) if ((unsigned char)*s >= 0x20) clean[n++] = *s;
    clean[n] = 0; if (!n) return;
    pthread_mutex_lock(&g_lb_mu); snprintf(g_name, sizeof g_name, "%s", clean); pthread_mutex_unlock(&g_lb_mu);
    prefs_save(); mp_rename(clean);
}

// Online play never writes the player's own save. The game autosaves on its own (an area change, a door), so the trip to Hyrule
// Field, a tunic put on in the lobby, hearts lost to arrows would all end up in it. Before the game mounts its save (hle_fs.c reads
// DM_SAVE at the first mount, after this), the host's save setting is pointed at a copy: <DM_ROOT>/oot_online_save, made fresh from
// the player's save at every start (their progress, items and hearts come along; what happens online stays in the copy). The copy
// is never synced to the cloud (DM_SAVE_SYNC off in this process; the launcher syncs only the real save, which is unchanged).
// DM_MP_OWN_SAVE=1 plays on the real save instead
static void use_online_save(void) {
    const char *real = getenv("DM_SAVE"); if (!real || !*real) return;
    if (getenv("DM_MP_OWN_SAVE") && atoi(getenv("DM_MP_OWN_SAVE"))) { MP_HOST_LOG("online play writes the player's own save (DM_MP_OWN_SAVE=1)"); return; }
    char src[1024], copy[1024], dev_src[1100], dev_copy[1100]; const char *root = getenv("DM_ROOT");
    snprintf(src, sizeof src, "%s", real); size_t n = strlen(src); while (n > 1 && src[n - 1] == '/') src[--n] = 0;
    if (root && *root) snprintf(copy, sizeof copy, "%s/oot_online_save", root);
    else snprintf(copy, sizeof copy, "%s.oot_online", src);
    if (!strcmp(src, copy)) return;
    removefile_state_t rs = removefile_state_alloc();
    const char *suffix[3] = { "", ".work", "_device" };   // the copy, the host's working clone of it, its device save
    for (int i = 0; i < 3; i++) { char p[1100]; snprintf(p, sizeof p, "%s%s", copy, suffix[i]); removefile(p, rs, REMOVEFILE_RECURSIVE); }
    removefile_state_free(rs);
    struct stat st;
    if (!stat(src, &st) && S_ISDIR(st.st_mode)) {
        if (copyfile(src, copy, NULL, COPYFILE_ALL | COPYFILE_RECURSIVE) != 0) { MP_HOST_LOG("[RED] could not copy the save for online play (%s): online play stays off", src); setenv("DM_MP_SERVER", "", 1); return; }
    } else mkdir(copy, 0755);   // no save yet: a new game, in the copy
    snprintf(dev_src, sizeof dev_src, "%s_device", src); snprintf(dev_copy, sizeof dev_copy, "%s_device", copy);
    if (!stat(dev_src, &st) && S_ISDIR(st.st_mode)) copyfile(dev_src, dev_copy, NULL, COPYFILE_ALL | COPYFILE_RECURSIVE);
    setenv("DM_SAVE", copy, 1); unsetenv("DM_SAVE_SYNC");
    MP_HOST_LOG("online play uses a copy of the save (%s); the player's own save is not written", copy);
}

void dm_mp_init(void) {
    const char *srv = getenv("DM_MP_SERVER");
    if (!srv || !*srv) return;
    use_online_save();
    srv = getenv("DM_MP_SERVER"); if (!srv || !*srv) return;   // the copy failed: off
    g_log = getenv("DM_MP_LOG") && atoi(getenv("DM_MP_LOG"));
    prefs_load();
    if (getenv("DM_MP_OUTFIT") && *getenv("DM_MP_OUTFIT")) g_outfit = atoi(getenv("DM_MP_OUTFIT")) % 3;
    if (getenv("DM_MP_NAME") && *getenv("DM_MP_NAME")) snprintf(g_name, sizeof g_name, "%s", getenv("DM_MP_NAME"));
    if (!g_name[0]) snprintf(g_name, sizeof g_name, "Hero");
    mp_config c = { .server = srv, .name = g_name, .outfit = g_outfit, .code = getenv("DM_MP_CODE"),
                    .build = mp_game_build(), .player_key = load_key(), .kind = MP_KIND_PLAYER };
    if (mp_start(&c)) { MP_HOST_LOG("[RED] cannot start the network thread"); return; }
    mp_ui_on_send = on_send; mp_ui_on_rename = on_rename;
    atomic_store(&g_on, 1);
    if (&dm_fps_state && dm_fps_state) { void (*fp)(void) = frame_call; memcpy(dm_fps_state + 0x800, &fp, sizeof fp); }   // the frame call's target
    else MP_HOST_LOG("[RED] no frame step state page: the frame call cannot be placed (the ticks fall back to the gate and the input read)");
    if (gate_obs_set) { gate_obs_set(27, gate_tick); gate_obs_set(28, gate_event); gate_obs_set(29, gate_gameover); gate_obs_set(30, gate_shoot); }
    if (!getenv("DM_MP_NO_LOBBY")) show_lobby(1);
    MP_HOST_LOG("OoT Online: %s as %s (%s)", srv, g_name, g_outfit == 1 ? "Goron Tunic" : g_outfit == 2 ? "Zora Tunic" : "Kokiri Tunic");
}

// Once a presented frame (hle_nvn.c, after the cheat pass); timed for DM_MP_PROBE's line
static void present_body(void);
void dm_mp_present(void) {
    if (!atomic_load(&g_on)) return;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    present_body();
    time_add(&g_pres_ns, &g_pres_max_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
}
static void present_body(void) {
    {   // DM_MP_PROBE: where the ticks come from, every 5 s
        static int probe = -1; static double at; static unsigned long long last[4]; if (probe < 0) probe = getenv("DM_MP_PROBE") && atoi(getenv("DM_MP_PROBE"));
        double t = now_s();
        if (probe && t - at >= 5) { at = t; unsigned long long v[4] = { atomic_load(&g_n_gate), atomic_load(&g_n_read), atomic_load(&g_n_read_other), atomic_load(&g_n_world) };
            static unsigned long long lc; unsigned long long c = atomic_load(&g_n_call);
            unsigned long long n = c - lc ? c - lc : 1;
            MP_HOST_LOG("ticks in 5 s: frame calls %llu, frame gate %llu, player 1 reads %llu (%llu on another thread), world ticks from them %llu; lobby %d; "
                        "module time a frame: frame call %.3f ms (max %.3f), present %.3f ms (max %.3f)", c - lc, v[0] - last[0], v[1] - last[1], v[2] - last[2], v[3] - last[3], g_lobby,
                        atomic_exchange(&g_call_ns, 0) / 1e6 / n, atomic_exchange(&g_call_max_ns, 0) / 1e6, atomic_exchange(&g_pres_ns, 0) / 1e6 / n, atomic_exchange(&g_pres_max_ns, 0) / 1e6); lc = c;
            memcpy(last, v, sizeof v); }
    }
    mp_status_t s; mp_status(&s);
    mp_ui_status(s.state, s.online, s.detail, s.ping_ms);
    mp_event e;
    while (mp_poll_event(&e)) {
        if (g_log) MP_HOST_LOG("event %d", e.type);
        switch (e.type) {
        case MP_SEV_CHAT:
            if (e.u.chat.chan == MP_CHAN_SYSTEM && (strstr(e.u.chat.text, " joined") || strstr(e.u.chat.text, " left"))) mp_game_notice(e.u.chat.text);
            mp_ui_chat(e.u.chat.chan == MP_CHAN_SYSTEM ? MP_UI_SYSTEM : e.u.chat.chan == MP_CHAN_WHISPER ? MP_UI_WHISPER : MP_UI_CHAT, e.u.chat.name, 0, e.u.chat.text);
            break;
        case MP_SEV_KILLFEED:
            mp_ui_feed(e.u.killfeed.killer_name, e.u.killfeed.victim_name);
            {   // and as a notice in the game's own info bar
                char line[128]; int me_k = e.u.killfeed.killer == s.id, me_v = e.u.killfeed.victim == s.id;
                if (e.u.killfeed.killer_name[0]) snprintf(line, sizeof line, "%s shot %s", me_k ? "You" : e.u.killfeed.killer_name, me_v ? "you" : e.u.killfeed.victim_name);
                else snprintf(line, sizeof line, "%s fell", me_v ? "You" : e.u.killfeed.victim_name);
                mp_game_notice(line);
            }
            if (e.u.killfeed.victim == s.id) {
                g_dead = 1; g_dead_t = now_s(); snprintf(g_killer, sizeof g_killer, "%s", e.u.killfeed.killer_name);
            }
            break;
        case MP_SEV_HITCONFIRM: mp_ui_hitmarker(e.u.hitconfirm.killed); break;
        case MP_SEV_DAMAGE: pend(MP_SEV_DAMAGE, &e); break;
        case MP_SEV_RESPAWN: g_dead = 0; pend(MP_SEV_RESPAWN, &e); mp_ui_banner("", 0); break;
        case MP_SEV_KICK: mp_ui_banner(e.u.kick.reason, 8); MP_HOST_LOG("kicked: %s", e.u.kick.reason); break;
        case MP_EV_SHOT: pend(MP_EV_SHOT, &e); break;
        case MP_SEV_STATS: g_all_kills = e.u.stats.all_kills; g_all_deaths = e.u.stats.all_deaths; g_all_best = e.u.stats.all_best; break;
        }
    }
    if (g_dead) {   // "Shot by X · back in 3"
        char b[96]; double left = 4.0 - (now_s() - g_dead_t);
        if (g_killer[0]) snprintf(b, sizeof b, "Shot by %s%s", g_killer, left > 0.5 ? "" : "");
        else snprintf(b, sizeof b, "You fell");
        if (left > 0.5) { size_t n = strlen(b); snprintf(b + n, sizeof b - n, " · back in %d", (int)ceil(left)); }
        mp_ui_banner(b, 0.3);
    }
    // name tags over the others, where the camera sees them
    mp_remote r[MP_MAX_NEAR]; int n = mp_get_remote(r, MP_MAX_NEAR);
    mp_ui_tag tags[MP_MAX_NEAR]; int nt = 0;
    for (int i = 0; i < n; i++) {
        float sx, sy;
        if (r[i].flags & MP_PF_DEAD) continue;
        if (!mp_game_project(r[i].x, r[i].y + 2.1f, r[i].z, &sx, &sy)) continue;
        mp_ui_tag *t = &tags[nt++]; t->x = sx; t->y = sy; t->visible = 1; memcpy(t->name, r[i].name, sizeof t->name); t->outfit = r[i].outfit; t->hp = r[i].hp_pct / 255.0f;
    }
    mp_ui_tags(tags, nt);
    {   // the lobby page
        mp_ui_lobby_t lb; memset(&lb, 0, sizeof lb);
        pthread_mutex_lock(&g_lb_mu);
        lb.shown = g_lobby && !mp_game_ready(); lb.outfit = g_outfit;   // the drawn page only where the game's own window cannot be the lobby lb.pressed = g_lobby_press; g_lobby_press = 0;
        snprintf(lb.name, sizeof lb.name, "%s", s.state == MP_ONLINE && s.name[0] ? s.name : g_name);
        lb.offline = s.state != MP_ONLINE && now_s() - g_lobby_t0 > 8;   // no server after 8 s: A plays alone
        lb.can_play = s.state == MP_ONLINE || lb.offline;
        pthread_mutex_unlock(&g_lb_mu);
        lb.kills = g_all_kills; lb.deaths = g_all_deaths; lb.best = g_all_best;
        snprintf(lb.where, sizeof lb.where, "Hyrule Field");
        mp_ui_lobby(&lb);
    }
}

static const char *outfit_name(int o) { return o == 1 ? "Goron" : o == 2 ? "Zora" : "Kokiri"; }
static void set_outfit(int o) {   // the lobby's choice: kept, told to the server
    g_outfit = ((o % 3) + 3) % 3; mp_set_outfit(g_outfit); mp_send_outfit(g_outfit);
    pthread_mutex_unlock(&g_lb_mu); prefs_save(); pthread_mutex_lock(&g_lb_mu);
}
// The lobby in the game's own system window (mp_game_lobby), on the game thread: Play, Tunic (A or left / right), Name (the rename box).
// While the game cannot show it (no Link in the world yet), the drawn page (mp_overlay_mac.m) stands in
static void lobby_tick(void) {
    static int was_shown;
    {   // test switch DM_MP_LOBBY (read once a second; tools/ctl.sh <run> "env DM_MP_LOBBY=0"): 0 closes the lobby, 1 opens it again
        static double at; static char seen[8]; double t = now_s();
        if (t - at >= 1) { at = t; const char *e = getenv("DM_MP_LOBBY"); char v[8]; snprintf(v, sizeof v, "%s", e ? e : "");
            if (strcmp(v, seen)) { snprintf(seen, sizeof seen, "%s", v); if (v[0] == '0') show_lobby(0); else if (v[0] == '1') show_lobby(1); } }
    }
    pthread_mutex_lock(&g_lb_mu);
    int want = g_lobby && !mp_ui_editing();
    mp_status_t s; mp_status(&s);
    char text[256], l0[64], l1[64], l2[64];
    if (s.state == MP_ONLINE) snprintf(text, sizeof text, "Welcome to OoT Online, %s. %d %s online. Pick your tunic, then Play.", s.name[0] ? s.name : g_name, s.online, s.online == 1 ? "player" : "players");
    else snprintf(text, sizeof text, "OoT Online: %s", now_s() - g_lobby_t0 > 8 ? "no server answers. Play alone, or try again later." : "connecting...");
    snprintf(l0, sizeof l0, "%s", s.state == MP_ONLINE || now_s() - g_lobby_t0 > 8 ? "Play" : "Connecting...");
    snprintf(l1, sizeof l1, "Tunic: %s", outfit_name(g_outfit));
    snprintf(l2, sizeof l2, "Name: %s", s.state == MP_ONLINE && s.name[0] ? s.name : g_name);
    const char *const labels[3] = { l0, l1, l2 };
    int decided = -1, st = mp_game_lobby(want, text, labels, &decided);
    atomic_store(&g_native, st != 0);
    if (st && !was_shown) { MP_HOST_LOG("lobby: in the game's own window"); atomic_store(&g_native_t0, now_s()); }
    was_shown = st != 0;
    if (decided == 0 && (s.state == MP_ONLINE || now_s() - g_lobby_t0 > 8)) { g_lobby = 0; g_lobby_press = 3; if (!(getenv("DM_MP_STAY") && atoi(getenv("DM_MP_STAY")))) atomic_store(&g_to_field, 1); }
    if (decided == 1) { set_outfit(g_outfit + 1); g_lobby_press = 2; }
    pthread_mutex_unlock(&g_lb_mu);
    if (decided == 2) mp_ui_open_rename(g_name);
}

// The UI tick: the frame gate, every frame, menus or not (mp_game.h: nothing that can make a system call)
static void ui_tick(void) {
    if (!atomic_load(&g_on)) return;
    lobby_tick();
    mp_game_ui_tick();
}

// The world tick: the game's own thread and stack (player 1's input read); safe to write game state and to call the game
void dm_mp_game_tick(void) {
    if (!atomic_load(&g_on)) return;
    {   // our own state, once a game frame, at its start (Link where the last frame's update left him). Read on the present path it was
        // taken at an uneven point of the game's update, so others saw us step 2, 4.5, 7 m/s for a steady 4.5 (runs/pairB net.tsv)
        mp_local_state st;
        if (mp_game_read_local(&st)) { g_last = st; mp_set_local(&st); }
        else { memset(&st, 0, sizeof st); st.scene = 0xffffffffu; mp_set_local(&st); }   // not in the world (title, loading): nowhere, seen by nobody
    }
    Pending p[64]; int n;
    pthread_mutex_lock(&g_pend_mu); n = g_pend_n; memcpy(p, g_pend, sizeof(Pending) * n); g_pend_n = 0; pthread_mutex_unlock(&g_pend_mu);
    for (int i = 0; i < n; i++) {
        mp_event *e = &p[i].e;
        switch (p[i].kind) {
        case MP_SEV_DAMAGE: mp_game_apply_damage(e->u.damage.quarter_hearts, e->u.damage.attacker, e->u.damage.kind); break;
        case MP_SEV_RESPAWN: mp_game_respawn(e->u.respawn.scene, e->u.respawn.x, e->u.respawn.y, e->u.respawn.z, e->u.respawn.yaw, isnan(e->u.respawn.x)); break;
        case MP_EV_SHOT: mp_game_shot_fx(e->u.shot.shooter, e->u.shot.x, e->u.shot.y, e->u.shot.z, e->u.shot.dx, e->u.shot.dy, e->u.shot.dz, e->u.shot.kind); break;
        }
    }
    if (!atomic_load(&g_tick_gate) && !atomic_load(&g_frame_call)) ui_tick();   // neither a frame call nor a frame gate in this build: the UI side runs here too
    mp_game_world_prep();
    if (atomic_load(&g_to_field) && !g_lobby) {   // after Play: to the shared place, as soon as the game allows it (not in an event, a menu or a load)
        static double since; if (!since) since = now_s();
        if (mp_game_go_to_field()) { atomic_store(&g_to_field, 0); since = 0; }
        else if (now_s() - since > 60) { atomic_store(&g_to_field, 0); since = 0; MP_HOST_LOG("[RED] could not take Link to Hyrule Field for a minute: staying here"); }
    }
    static int applied = -1;   // the lobby's tunic onto Link once he's in the world, and again on each change
    if (!g_lobby && applied != g_outfit && mp_game_set_outfit(g_outfit)) applied = g_outfit;
    mp_remote r[MP_MAX_NEAR]; int nr = mp_get_remote(r, MP_MAX_NEAR);
    mp_game_update_puppets(r, nr);
    mp_game_take_local_events();
}

// Player 1's buttons and sticks as the game is about to read them (hle_hid.c fill, weak hook). While the lobby page or the
// chat box is up the game reads nothing; the lobby takes its presses here (edges, so a held button acts once)
enum { B_A = 1 << 0, B_B = 1 << 1, B_Y = 1 << 3, B_PLUS = 1 << 10, B_LEFT = 1 << 12, B_RIGHT = 1 << 14 };
void dm_mp_input_filter(uint64_t *buttons, int32_t axes[4]) {
    if (!atomic_load(&g_on)) return;
    // The game reads player 1 at the start of its frame, on its own thread: the safe moment for the game-side writes, once a frame
    static uint64_t last_frame = ~0ull; uint64_t f = dm_guest_frame();
    log_thread("player 1's input read runs");
    // the tick: once a frame, on the game's frame thread (the one the frame gate runs on, when the gate is in); never from a gate handler
    int frame_thread = !atomic_load(&g_tick_gate) || pthread_equal(pthread_self(), atomic_load(&g_gate_thread));
    atomic_fetch_add(&g_n_read, 1); if (!frame_thread) atomic_fetch_add(&g_n_read_other, 1);
    if (f != last_frame && frame_thread && !atomic_load(&g_frame_call)) { last_frame = f; atomic_store(&g_tick_in_input, 1); atomic_fetch_add(&g_n_world, 1); log_thread("the game-side tick runs from player 1's input read"); dm_mp_game_tick(); }
    uint64_t b = *buttons;
    if (axes[0] < -16000) b |= B_LEFT;   // the left stick works as the D-pad on the page
    if (axes[0] > 16000) b |= B_RIGHT;
    uint64_t down = b & ~g_prev_btn; g_prev_btn = b;
    int editing = mp_ui_editing();
    pthread_mutex_lock(&g_lb_mu);
    int lobby = g_lobby;
    if (lobby && !editing && mp_game_ready()) {   // the game's own window is the lobby: its menu takes the pad (focus, A, B), left / right
        if (atomic_load(&g_native)) {               // change the tunic; before it is up (title, loading) the game reads the pad as usual
            if (down & B_LEFT) { set_outfit(g_outfit + 2); g_lobby_press = 1; }
            if (down & B_RIGHT) { set_outfit(g_outfit + 1); g_lobby_press = 2; }
            if (now_s() - atomic_load(&g_native_t0) < 1.0) *buttons &= ~(uint64_t)(B_A | B_B);   // a press left over from loading the save must not pick Play
        }
        pthread_mutex_unlock(&g_lb_mu);
        return;
    }
    if (lobby && !editing) {
        int o = g_outfit;
        if (down & B_LEFT) { g_outfit = (g_outfit + 2) % 3; g_lobby_press = 1; }
        if (down & B_RIGHT) { g_outfit = (g_outfit + 1) % 3; g_lobby_press = 2; }
        if (g_outfit != o) { mp_set_outfit(g_outfit); mp_send_outfit(g_outfit); }
        if (down & (B_A | B_PLUS)) {
            mp_status_t s; mp_status(&s);
            if (s.state == MP_ONLINE || now_s() - g_lobby_t0 > 8) { g_lobby = 0; g_lobby_press = 3; }
        }
        if (down & B_Y) mp_ui_open_rename(g_name);
        if (g_outfit != o) { pthread_mutex_unlock(&g_lb_mu); prefs_save(); pthread_mutex_lock(&g_lb_mu); }
    }
    pthread_mutex_unlock(&g_lb_mu);
    if (lobby || editing) { *buttons = 0; axes[0] = axes[1] = axes[2] = axes[3] = 0; }
    else mp_game_demo_input(buttons, axes);   // footage: a scene's scripted pad (DM_MP_DEMO's I rows; nothing otherwise)
}

void dm_mp_shutdown(void) { if (atomic_exchange(&g_on, 0)) mp_stop(); }
