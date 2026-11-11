// hle_mp.c + mp_net.c without the game: "frames" at 60 Hz call dm_mp_present; prints what reaches the screen layer.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "../mp_ui.h"
void dm_mp_init(void); void dm_mp_present(void); void dm_mp_shutdown(void);
void (*mp_ui_on_send)(const char *text);
void mp_ui_chat(int kind, const char *name, int outfit, const char *text) { printf("ui chat %d <%s> %s\n", kind, name, text); }
void mp_ui_feed(const char *k, const char *v) { printf("ui feed %s -> %s\n", k, v); }
void mp_ui_hitmarker(int killed) { printf("ui hit %d\n", killed); }
void mp_ui_banner(const char *t, double s) { static char last[200]; if (strcmp(last, t)) { printf("ui banner '%s'\n", t); snprintf(last, sizeof last, "%s", t); } }
void mp_ui_status(int st, int online, const char *d, int ping) { static int ls = -1; if (st != ls) { printf("ui status %d online %d %s\n", st, online, d); ls = st; } }
void mp_ui_tags(const mp_ui_tag *t, int n) {}
void (*mp_ui_on_rename)(const char *name);
static int lobby_shown = -1, lobby_outfit = -1;
void mp_ui_lobby(const mp_ui_lobby_t *l) { if (l->shown != lobby_shown || l->outfit != lobby_outfit) { printf("ui lobby shown=%d outfit=%d can_play=%d name=%s\n", l->shown, l->outfit, l->can_play, l->name); lobby_shown = l->shown; lobby_outfit = l->outfit; } }
void mp_ui_open_rename(const char *cur) { printf("ui rename box (%s)\n", cur); }
int mp_ui_editing(void) { return 0; }
void dm_mp_input_filter(uint64_t *buttons, int32_t axes[4]);
static void press(uint64_t bit) { int32_t a[4] = {0}; uint64_t b = bit; dm_mp_input_filter(&b, a); if (b) printf("LEAK: the game saw buttons\n"); b = 0; dm_mp_input_filter(&b, a); }
int main(int argc, char **argv) {
    setenv("DM_MP_SERVER", argc > 1 ? argv[1] : "127.0.0.1:47970", 1); setenv("DM_MP_NAME", "HostSim", 1); setenv("DM_MP_OUTFIT", "0", 1);
    setenv("DM_ROOT", "/tmp", 0);
    dm_mp_init();
    for (int f = 0; f < 60 * 4; f++) { dm_mp_present(); if (f == 60 && mp_ui_on_send) mp_ui_on_send("typed in the box"); if (f == 90) press(1 << 14); if (f == 120) press(1 << 3); if (f == 150) press(1 << 0);
        if (f == 180) { int32_t a[4] = {0}; uint64_t b = 1 << 12; dm_mp_input_filter(&b, a); printf("after play, the game sees %s\n", b ? "its buttons" : "nothing"); }
        usleep(16667); }
    dm_mp_shutdown();
    return 0;
}
