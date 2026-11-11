// The C client against a real server: joins, walks a circle, chats, prints what it hears.  mp_test <server> <name> <seconds>
#include "mp_net.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    mp_config c = { .server = argc > 1 ? argv[1] : "127.0.0.1:47970", .name = argc > 2 ? argv[2] : "CLink", .outfit = 1, .player_key = 777 };
    int secs = argc > 3 ? atoi(argv[3]) : 4;
    if (mp_start(&c)) return 1;
    int said = 0, got_damage = 0, saw_js = 0;
    for (int f = 0; f < secs * 60; f++) {
        float a = f / 60.0f;
        mp_local_state s = { .scene = 7, .x = 10 * cosf(a), .y = 0, .z = 10 * sinf(a), .yaw = (uint16_t)(a * 10430), .anim = 2, .hp = 12, .max_hp = 12 };
        mp_set_local(&s);
        mp_status_t st; mp_status(&st);
        if (st.state == MP_ONLINE && !said) { mp_send_chat("hello from C"); said = 1; }
        mp_event e;
        while (mp_poll_event(&e)) {
            if (e.type == MP_SEV_CHAT) printf("chat [%d] <%s> %s\n", e.u.chat.chan, e.u.chat.name, e.u.chat.text);
            else if (e.type == MP_SEV_DAMAGE) { printf("damage %u from #%u\n", e.u.damage.quarter_hearts, e.u.damage.attacker); got_damage++; }
            else if (e.type == MP_EV_SHOT) printf("shot from #%u dir %.2f %.2f %.2f\n", e.u.shot.shooter, e.u.shot.dx, e.u.shot.dy, e.u.shot.dz);
            else printf("event %d\n", e.type);
        }
        mp_remote r[MP_MAX_NEAR]; int n = mp_get_remote(r, MP_MAX_NEAR);
        for (int i = 0; i < n; i++) if (r[i].name[0] && !saw_js) { printf("see #%u %s at %.2f %.2f %.2f outfit %u\n", r[i].id, r[i].name, r[i].x, r[i].y, r[i].z, r[i].outfit); saw_js = 1; }
        usleep(16667);
    }
    mp_status_t st; mp_status(&st);
    printf("final: state %d id %u online %u ping %d ms name %s damage %d saw %d\n", st.state, st.id, st.online, st.ping_ms, st.name, got_damage, saw_js);
    mp_stop();
    return 0;
}
