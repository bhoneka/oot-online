// OoT Online: the game side's network client. See mp_net.h; the wire format is server/protocol.js's (protocol v1).
#include "mp_net.h"
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x4f4f
#define MAX_DGRAM 1200
enum { T_HELLO = 1, T_WELCOME = 2, T_STATE = 3, T_SNAPSHOT = 4, T_PING = 5, T_PONG = 6, T_BYE = 7, T_INFO_REQ = 8, T_INFO = 9, T_REJECT = 10 };
static const char *REJECT_TEXT[] = { "", "the server is full", "another version of OoT Online", "a room code is needed (or it was wrong)",
    "you are banned from this server", "another build of the game", "too many players from your address" };

#ifndef MP_LOG
#define MP_LOG(...) do { fprintf(stderr, "[mp] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

// ---------------------------------------------------------------- byte writer / reader
typedef struct { uint8_t *b; size_t n, cap; int bad; } W;
static void w_raw(W *w, const void *p, size_t n) { if (w->n + n > w->cap) { w->bad = 1; return; } memcpy(w->b + w->n, p, n); w->n += n; }
static void w_u8(W *w, uint8_t v) { w_raw(w, &v, 1); }
static void w_u16(W *w, uint16_t v) { uint8_t x[2] = { v, v >> 8 }; w_raw(w, x, 2); }
static void w_u32(W *w, uint32_t v) { uint8_t x[4] = { v, v >> 8, v >> 16, v >> 24 }; w_raw(w, x, 4); }
static void w_f32(W *w, float f) { uint32_t v; memcpy(&v, &f, 4); w_u32(w, v); }
static void w_i16(W *w, float f) { long v = lroundf(f); if (v > 32767) v = 32767; if (v < -32768) v = -32768; w_u16(w, (uint16_t)(int16_t)v); }
static size_t utf8_cut(const char *s, size_t max) {   // bytes of s that fit in max without splitting a character
    size_t n = strlen(s); if (n <= max) return n;
    while (max > 0 && ((uint8_t)s[max] & 0xc0) == 0x80) max--;
    return max;
}
static void w_str(W *w, const char *s, size_t max) { size_t n = utf8_cut(s ? s : "", max); w_u8(w, (uint8_t)n); w_raw(w, s, n); }
static void w_header(W *w, uint8_t type) { w_u16(w, MAGIC); w_u8(w, MP_PROTOCOL); w_u8(w, type); }

typedef struct { const uint8_t *b; size_t n, o; int bad; } R;
static int r_need(R *r, size_t k) { if (r->bad || r->o + k > r->n) { r->bad = 1; return 0; } return 1; }
static uint8_t r_u8(R *r) { if (!r_need(r, 1)) return 0; return r->b[r->o++]; }
static uint16_t r_u16(R *r) { if (!r_need(r, 2)) return 0; uint16_t v = r->b[r->o] | r->b[r->o + 1] << 8; r->o += 2; return v; }
static uint32_t r_u32(R *r) { if (!r_need(r, 4)) return 0; uint32_t v = r->b[r->o] | r->b[r->o + 1] << 8 | r->b[r->o + 2] << 16 | (uint32_t)r->b[r->o + 3] << 24; r->o += 4; return v; }
static float r_f32(R *r) { uint32_t v = r_u32(r); float f; memcpy(&f, &v, 4); return f; }
static float r_i16n(R *r) { return (int16_t)r_u16(r) / 32767.0f; }
static void r_str(R *r, char *out, size_t cap) {
    uint8_t n = r_u8(r); if (!r_need(r, n)) { out[0] = 0; return; }
    size_t k = n < cap - 1 ? n : cap - 1; memcpy(out, r->b + r->o, k); out[k] = 0;
    for (size_t i = 0; i < k; i++) if ((uint8_t)out[i] < 0x20) out[i] = ' ';   // no control characters reach the screen
    r->o += n;
}

// ---------------------------------------------------------------- state shared with the game thread
typedef struct { uint32_t id; uint8_t type, len; uint8_t payload[200]; } OutEv;
#define OUTQ 128
#define EVQ 256
#define SNAPS 24
typedef struct { double t_local; uint32_t t_srv; int n; mp_remote p[MP_MAX_NEAR]; } Snap;
// Each player's own track: the positions the snapshots carried, each at the server time the player was there (the snapshot's tMs less
// its ageMs), the same state once however many snapshots repeat it. Others are drawn from their own track: with v1's tick times they
// stuttered, because the server's 20 Hz ticks and the senders' own 20 Hz sends drift against each other (a tick with nothing new, then
// one with two steps: runs/pairA, 32 stalls and 57 jumps in 211 frames)
#define TRACKS 64
#define TRACK_N 12
typedef struct { double ts; mp_remote p; } Sample;
typedef struct { uint16_t id; int used, n, head; double seen; Sample s[TRACK_N]; } Track;

static struct {
    pthread_mutex_t mu;
    pthread_t th;
    atomic_int run;
    mp_config cfg;
    char server[256], name[MP_NAME_MAX], code[64];
    int sock;
    struct sockaddr_in addr;
    // session
    uint32_t nonce, token, prev_token;
    uint16_t id;
    int tick_hz;
    uint32_t seq, last_snap, srv_ev_done;
    double last_heard, last_hello, last_ping, last_send;
    // ours
    mp_local_state local; int have_local;
    OutEv outq[OUTQ]; int outq_n; uint32_t next_ev;
    // theirs
    Snap snaps[SNAPS]; int snap_head, snap_count;
    Track tracks[TRACKS];
    double local_t, local_raw, frame_ms;   // our state's time (mp_set_local: on the frames' beat), the last call's clock, the beat's period
    double clock_offset; int clock_init;   // server ms = local ms + offset (taken from the quickest snapshots)
    mp_event evq[EVQ]; int evq_head, evq_n;
    double info_asked[65536];
    mp_status_t st;
} M = { .mu = PTHREAD_MUTEX_INITIALIZER, .sock = -1 };
static char NAMES[65536][MP_NAME_MAX];   // id -> name, as INFO and chat tell them

static void push_event(const mp_event *e) {
    if (M.evq_n == EVQ) { M.evq_head = (M.evq_head + 1) % EVQ; M.evq_n--; }   // the game stopped draining: the oldest goes
    M.evq[(M.evq_head + M.evq_n++) % EVQ] = *e;
}
static void queue_out(uint8_t type, const uint8_t *p, size_t n) {
    pthread_mutex_lock(&M.mu);
    if (M.outq_n == OUTQ) { memmove(M.outq, M.outq + 1, sizeof(OutEv) * (OUTQ - 1)); M.outq_n--; }
    OutEv *e = &M.outq[M.outq_n++];
    e->id = ++M.next_ev; e->type = type; e->len = (uint8_t)(n > sizeof e->payload ? sizeof e->payload : n); if (e->len) memcpy(e->payload, p, e->len);
    pthread_mutex_unlock(&M.mu);
}

// ---------------------------------------------------------------- sending
static void send_buf(const W *w) { if (!w->bad && M.sock >= 0) sendto(M.sock, w->b, w->n, 0, (struct sockaddr *)&M.addr, sizeof M.addr); }
static void send_hello(void) {
    uint8_t b[160]; W w = { b, 0, sizeof b, 0 };
    w_header(&w, T_HELLO); w_u32(&w, M.nonce); w_u8(&w, (uint8_t)M.cfg.kind); w_u8(&w, (uint8_t)M.cfg.outfit); w_u32(&w, M.cfg.build);
    w_u32(&w, M.cfg.player_key); w_str(&w, M.name, 24); w_u32(&w, M.prev_token); w_str(&w, M.code, 32);
    send_buf(&w); M.last_hello = now_ms();
}
// DM_MP_NETLOG=<file> (tests): each state we send (S: now, the state's time, its age, x, z) and each sample others' tracks take
// (R: id, its server time, x, z, our now), to see where any unevenness in others' movement comes from
static FILE *netlog(void) { static int on = -1; static FILE *f; if (on < 0) { const char *e = getenv("DM_MP_NETLOG"); on = e && *e && (f = fopen(e, "w")) != NULL; } return on ? f : NULL; }
static Track *track_find(uint16_t id) { for (int i = 0; i < TRACKS; i++) if (M.tracks[i].used && M.tracks[i].id == id) return &M.tracks[i]; return NULL; }
static void track_add(const mp_remote *p, double ts, double t) {   // under M.mu
    Track *tr = track_find(p->id);
    if (!tr) {   // a new one: a free slot, else the one heard from longest ago
        for (int i = 0; i < TRACKS && !tr; i++) if (!M.tracks[i].used) tr = &M.tracks[i];
        if (!tr) { tr = &M.tracks[0]; for (int i = 1; i < TRACKS; i++) if (M.tracks[i].seen < tr->seen) tr = &M.tracks[i]; }
        memset(tr, 0, sizeof *tr); tr->used = 1; tr->id = p->id;
    }
    tr->seen = t;
    if (tr->n) {
        const Sample *last = &tr->s[(tr->head + tr->n - 1) % TRACK_N];
        if (ts <= last->ts + 3) return;   // the same state again (a later tick repeats it; ms rounding apart)
    }
    { FILE *f = netlog(); if (f) { fprintf(f, "R\t%u\t%.1f\t%.4f\t%.4f\t%.1f\n", p->id, ts, p->x, p->z, t); fflush(f); } }
    Sample *sl = &tr->s[(tr->head + tr->n) % TRACK_N];
    if (tr->n == TRACK_N) tr->head = (tr->head + 1) % TRACK_N; else tr->n++;
    sl->ts = ts; sl->p = *p;
}
static void send_state(void) {   // under M.mu
    uint8_t b[MAX_DGRAM]; W w = { b, 0, sizeof b, 0 };
    mp_local_state s = M.local;
    w_header(&w, T_STATE); w_u32(&w, M.token); w_u32(&w, ++M.seq); w_u32(&w, M.last_snap); w_u32(&w, M.srv_ev_done);
    w_u32(&w, s.scene); w_f32(&w, s.x); w_f32(&w, s.y); w_f32(&w, s.z); w_u16(&w, s.yaw); w_u16(&w, s.anim);
    w_u8(&w, s.flags); w_u8(&w, s.hp); w_u8(&w, s.max_hp); w_u8(&w, s.equip);
    { double now = now_ms(), age = now - M.local_t; w_u16(&w, (uint16_t)(age < 0 ? 0 : age > 1000 ? 1000 : age));   // ageMs: how old this position is
      FILE *f = netlog(); if (f) { fprintf(f, "S\t%.1f\t%.1f\t%.1f\t%.4f\t%.4f\n", now, M.local_t, age, s.x, s.z); fflush(f); } }
    size_t at = w.n; w_u8(&w, 0); int n = 0;
    for (int i = 0; i < M.outq_n && n < 255; i++) {
        OutEv *e = &M.outq[i];
        if (w.n + 6 + e->len > w.cap) break;
        w_u32(&w, e->id); w_u8(&w, e->type); w_u8(&w, e->len); w_raw(&w, e->payload, e->len); n++;
    }
    b[at] = (uint8_t)n;
    send_buf(&w); M.last_send = now_ms();
}
static void send_info_req(const uint16_t *ids, int n) {
    uint8_t b[300]; W w = { b, 0, sizeof b, 0 };
    w_header(&w, T_INFO_REQ); w_u32(&w, M.token); w_u8(&w, (uint8_t)n); for (int i = 0; i < n; i++) w_u16(&w, ids[i]);
    send_buf(&w);
}

// ---------------------------------------------------------------- receiving
static void on_server_event(uint8_t type, const uint8_t *p, size_t n) {
    R r = { p, n, 0, 0 }; mp_event e; memset(&e, 0, sizeof e); e.type = type;
    switch (type) {
    case MP_SEV_CHAT:
        e.u.chat.from = r_u16(&r); e.u.chat.chan = r_u8(&r); r_str(&r, e.u.chat.name, sizeof e.u.chat.name); r_str(&r, e.u.chat.text, sizeof e.u.chat.text);
        if (e.u.chat.from != MP_NOBODY && e.u.chat.name[0] && e.u.chat.chan == MP_CHAN_ALL) memcpy(NAMES[e.u.chat.from], e.u.chat.name, MP_NAME_MAX);
        break;
    case MP_SEV_DAMAGE: e.u.damage.attacker = r_u16(&r); e.u.damage.quarter_hearts = r_u8(&r); e.u.damage.kind = r_u8(&r); break;
    case MP_SEV_KILLFEED:
        e.u.killfeed.killer = r_u16(&r); e.u.killfeed.victim = r_u16(&r); e.u.killfeed.how = r_u8(&r);
        r_str(&r, e.u.killfeed.killer_name, MP_NAME_MAX); r_str(&r, e.u.killfeed.victim_name, MP_NAME_MAX); break;
    case MP_SEV_RESPAWN:
        e.u.respawn.scene = r_u32(&r); e.u.respawn.x = r_f32(&r); e.u.respawn.y = r_f32(&r); e.u.respawn.z = r_f32(&r);
        e.u.respawn.yaw = r_u16(&r); e.u.respawn.invuln_ms = r_u16(&r); break;
    case MP_SEV_HITCONFIRM: e.u.hitconfirm.target = r_u16(&r); e.u.hitconfirm.quarter_hearts = r_u8(&r); e.u.hitconfirm.killed = r_u8(&r); break;
    case MP_SEV_STATS: e.u.stats.kills = r_u16(&r); e.u.stats.deaths = r_u16(&r); e.u.stats.streak = r_u16(&r); e.u.stats.best = r_u16(&r);
        e.u.stats.all_kills = r_u16(&r); e.u.stats.all_deaths = r_u16(&r); e.u.stats.all_best = r_u16(&r); break;
    case MP_SEV_KICK: r_str(&r, e.u.kick.reason, sizeof e.u.kick.reason); break;
    default: return;   // a newer server's event: skipped by its length
    }
    if (r.bad) return;
    push_event(&e);
}
static void on_packet(const uint8_t *b, size_t n) {
    if (n < 4 || (b[0] | b[1] << 8) != MAGIC || b[2] != MP_PROTOCOL) return;
    R r = { b, n, 4, 0 };
    double t = now_ms();
    pthread_mutex_lock(&M.mu);
    switch (b[3]) {
    case T_WELCOME: {
        uint32_t nonce = r_u32(&r); if (nonce != M.nonce) break;
        uint16_t id = r_u16(&r); uint32_t token = r_u32(&r); uint8_t hz = r_u8(&r); r_u8(&r); uint16_t online = r_u16(&r);
        char name[MP_NAME_MAX]; r_str(&r, name, sizeof name); char motd[MP_CHAT_MAX]; r_str(&r, motd, sizeof motd);
        if (r.bad) break;
        int first = M.token == 0 || M.id != id;
        M.id = id; M.token = token; M.tick_hz = hz ? hz : 20; M.last_heard = t;
        M.st.state = MP_ONLINE; M.st.id = id; M.st.online = online; snprintf(M.st.name, sizeof M.st.name, "%s", name); M.st.detail[0] = 0;
        memcpy(NAMES[id], name, MP_NAME_MAX);
        if (first) { M.seq = 0; M.last_snap = 0; M.srv_ev_done = 0; M.snap_count = 0; M.clock_init = 0; memset(M.tracks, 0, sizeof M.tracks); }
        MP_LOG("online as %s (#%u), %u players", name, id, online);
        break;
    }
    case T_REJECT: {
        uint32_t nonce = r_u32(&r); if (nonce != M.nonce) break;
        uint8_t why = r_u8(&r); char detail[MP_CHAT_MAX]; r_str(&r, detail, sizeof detail);
        M.st.state = MP_REJECTED;
        snprintf(M.st.detail, sizeof M.st.detail, "%s", detail[0] ? detail : why < sizeof REJECT_TEXT / sizeof *REJECT_TEXT ? REJECT_TEXT[why] : "refused");
        MP_LOG("the server refused: %s", M.st.detail);
        break;
    }
    case T_SNAPSHOT: {
        uint16_t you = r_u16(&r); uint32_t seq = r_u32(&r); r_u32(&r); uint32_t ack_cli = r_u32(&r); uint32_t tms = r_u32(&r);
        uint16_t online = r_u16(&r); uint8_t yf = r_u8(&r);
        if (r.bad || you != M.id) break;
        M.last_heard = t;
        if (M.st.state == MP_LOST) M.st.state = MP_ONLINE;
        if (seq <= M.last_snap) break;   // older than one we have: its events came again in the newer one
        M.last_snap = seq; M.st.online = online; M.st.you_flags = yf;
        int k = 0; while (k < M.outq_n && M.outq[k].id <= ack_cli) k++;
        if (k) { memmove(M.outq, M.outq + k, sizeof(OutEv) * (M.outq_n - k)); M.outq_n -= k; }
        uint8_t ne = r_u8(&r);
        for (int i = 0; i < ne && !r.bad; i++) {
            uint32_t id = r_u32(&r); uint8_t type = r_u8(&r), len = r_u8(&r);
            if (!r_need(&r, len)) break;
            const uint8_t *p = r.b + r.o; r.o += len;
            if (id <= M.srv_ev_done) continue;
            M.srv_ev_done = id;   // in order; a jump means the server dropped old ones for a game that stopped acking
            on_server_event(type, p, len);
        }
        uint8_t ns = r_u8(&r);
        for (int i = 0; i < ns && !r.bad; i++) {   // shots: drawn by the game glue, passed as events
            mp_event e; memset(&e, 0, sizeof e); e.type = MP_EV_SHOT;
            e.u.shot.shooter = r_u16(&r); e.u.shot.x = r_f32(&r); e.u.shot.y = r_f32(&r); e.u.shot.z = r_f32(&r);
            e.u.shot.dx = r_i16n(&r); e.u.shot.dy = r_i16n(&r); e.u.shot.dz = r_i16n(&r); e.u.shot.kind = r_u8(&r);
            if (r.bad) break;
            push_event(&e);
        }
        uint8_t np = r_u8(&r);
        Snap *sn = &M.snaps[(M.snap_head + M.snap_count) % SNAPS];
        if (M.snap_count == SNAPS) { M.snap_head = (M.snap_head + 1) % SNAPS; } else M.snap_count++;
        sn->t_local = t; sn->t_srv = tms; sn->n = 0;
        uint16_t unknown[100]; int nu = 0;
        for (int i = 0; i < np && !r.bad; i++) {
            mp_remote p; memset(&p, 0, sizeof p);
            p.id = r_u16(&r); p.x = r_f32(&r); p.y = r_f32(&r); p.z = r_f32(&r); p.yaw = r_u16(&r); p.anim = r_u16(&r);
            p.flags = r_u8(&r); p.hp_pct = r_u8(&r); p.outfit = r_u8(&r); p.equip = r_u8(&r);
            uint16_t age = r_u16(&r);
            if (r.bad || sn->n >= MP_MAX_NEAR) break;
            sn->p[sn->n++] = p;
            track_add(&p, (double)tms - age, t);
            if (!NAMES[p.id][0] && t - M.info_asked[p.id] > 1000 && nu < 100) { unknown[nu++] = p.id; M.info_asked[p.id] = t; }
        }
        // the server's clock: the quickest-arriving snapshots give the offset; it drifts back up slowly
        double off = (double)tms - t;
        if (!M.clock_init || off > M.clock_offset) { M.clock_offset = off; M.clock_init = 1; } else M.clock_offset -= 0.02;
        if (nu) send_info_req(unknown, nu);
        break;
    }
    case T_INFO: {
        uint8_t n2 = r_u8(&r);
        for (int i = 0; i < n2 && !r.bad; i++) { uint16_t id = r_u16(&r); r_u8(&r); r_u8(&r); char nm[MP_NAME_MAX]; r_str(&r, nm, sizeof nm); if (!r.bad) memcpy(NAMES[id], nm, MP_NAME_MAX); }
        break;
    }
    case T_PONG: { uint32_t ct = r_u32(&r); if (!r.bad) M.st.ping_ms = (int)((uint32_t)t - ct); M.last_heard = t; break; }
    }
    pthread_mutex_unlock(&M.mu);
}

// ---------------------------------------------------------------- the thread
static int resolve(void) {
    char host[256]; snprintf(host, sizeof host, "%s", M.server);
    int port = 4697;
    char *colon = strrchr(host, ':');
    if (colon && strchr(host, ':') == colon) { port = atoi(colon + 1); *colon = 0; }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
    for (int tries = 0; tries < 15 && atomic_load(&M.run); tries++) {   // the network may still be coming up
        int e = getaddrinfo(host, NULL, &hints, &res);
        if (!e && res) {
            memcpy(&M.addr, res->ai_addr, sizeof M.addr); M.addr.sin_port = htons((uint16_t)port); freeaddrinfo(res);
            return 0;
        }
        pthread_mutex_lock(&M.mu); snprintf(M.st.detail, sizeof M.st.detail, "can't find %s (%s)", host, gai_strerror(e)); pthread_mutex_unlock(&M.mu);
        sleep(2);
    }
    return -1;
}
static void *net_thread(void *arg) {
    (void)arg;
    M.st.state = MP_RESOLVING;
    if (resolve()) { M.st.state = MP_REJECTED; MP_LOG("%s", M.st.detail); return NULL; }
    M.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (M.sock < 0) { M.st.state = MP_REJECTED; snprintf(M.st.detail, sizeof M.st.detail, "no network socket (errno %d)", errno); return NULL; }
    int big = 1 << 20; setsockopt(M.sock, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    M.st.state = MP_CONNECTING;
    uint8_t buf[2048];
    while (atomic_load(&M.run)) {
        double t = now_ms();
        pthread_mutex_lock(&M.mu);
        int hz = M.tick_hz ? M.tick_hz : 20;
        if (M.st.state != MP_REJECTED) {
            if (!M.token) { if (t - M.last_hello > 1000) send_hello(); }
            else {
                if (t - M.last_heard > 3000) {   // silence: ask for our session back with the token we had
                    if (M.st.state == MP_ONLINE) { M.st.state = MP_LOST; snprintf(M.st.detail, sizeof M.st.detail, "can't reach the server"); MP_LOG("lost the server"); }
                    if (t - M.last_hello > 1000) { M.prev_token = M.token; send_hello(); }
                }
                if (t - M.last_send >= 1000.0 / hz - 1) send_state();
                if (t - M.last_ping > 2000) {
                    uint8_t pb[16]; W w = { pb, 0, sizeof pb, 0 }; w_header(&w, T_PING); w_u32(&w, M.token); w_u32(&w, (uint32_t)t); send_buf(&w); M.last_ping = t;
                }
            }
        }
        double next = M.last_send + 1000.0 / hz;
        pthread_mutex_unlock(&M.mu);
        int wait = (int)(next - now_ms()); if (wait < 1) wait = 1; if (wait > 50) wait = 50;
        struct pollfd pf = { M.sock, POLLIN, 0 };
        if (poll(&pf, 1, wait) > 0) {
            for (;;) {
                ssize_t n = recv(M.sock, buf, sizeof buf, MSG_DONTWAIT);
                if (n <= 0) break;
                on_packet(buf, (size_t)n);
            }
        }
    }
    if (M.token) { uint8_t bb[16]; W w = { bb, 0, sizeof bb, 0 }; w_header(&w, T_BYE); w_u32(&w, M.token); send_buf(&w); }
    close(M.sock); M.sock = -1;
    return NULL;
}

// ---------------------------------------------------------------- the game thread's side
int mp_start(const mp_config *cfg) {
    if (atomic_load(&M.run)) return 0;
    M.cfg = *cfg;
    snprintf(M.server, sizeof M.server, "%s", cfg->server ? cfg->server : "127.0.0.1");
    snprintf(M.name, sizeof M.name, "%s", cfg->name && cfg->name[0] ? cfg->name : "Hero");
    snprintf(M.code, sizeof M.code, "%s", cfg->code ? cfg->code : "");
    if (!M.cfg.player_key) M.cfg.player_key = (uint32_t)arc4random() | 1;
    if (M.cfg.interp_delay_ms <= 0) M.cfg.interp_delay_ms = 115;
    M.nonce = arc4random();
    atomic_store(&M.run, 1);
    if (pthread_create(&M.th, NULL, net_thread, NULL)) { atomic_store(&M.run, 0); return -1; }
    return 0;
}
void mp_stop(void) {
    if (!atomic_exchange(&M.run, 0)) return;
    pthread_join(M.th, NULL);
}
// Our state's time: the game's frames come at an even beat, the calls less evenly; the stamp keeps to the beat (its period followed
// slowly) and drifts a tenth of the way to the clock each frame, starting over after a gap (a load, a pause)
void mp_set_local(const mp_local_state *st) {
    pthread_mutex_lock(&M.mu);
    M.local = *st; M.have_local = 1;
    double t = now_ms(), dt = t - M.local_raw;
    if (M.local_t > 0 && dt > 4 && dt < 70) {
        M.frame_ms += (dt - M.frame_ms) * 0.05;
        double pred = M.local_t + M.frame_ms, s = pred + (t - pred) * 0.1;
        M.local_t = fabs(t - s) > 40 ? t : s;
    } else { M.local_t = t; if (!(M.frame_ms > 4)) M.frame_ms = 1000.0 / 60; }
    M.local_raw = t;
    pthread_mutex_unlock(&M.mu);
}
void mp_status(mp_status_t *out) { pthread_mutex_lock(&M.mu); *out = M.st; pthread_mutex_unlock(&M.mu); }
void mp_rename(const char *name) {
    pthread_mutex_lock(&M.mu);
    snprintf(M.name, sizeof M.name, "%s", name && *name ? name : "Hero");
    M.nonce = arc4random(); M.token = 0; M.prev_token = 0; M.last_hello = 0;
    if (M.st.state != MP_REJECTED || M.sock >= 0) M.st.state = MP_CONNECTING;
    pthread_mutex_unlock(&M.mu);
}
void mp_set_outfit(int outfit) { pthread_mutex_lock(&M.mu); M.cfg.outfit = outfit; pthread_mutex_unlock(&M.mu); }
const char *mp_name_of(uint16_t id) { return NAMES[id]; }
int mp_poll_event(mp_event *ev) {
    pthread_mutex_lock(&M.mu);
    int got = M.evq_n > 0;
    if (got) { *ev = M.evq[M.evq_head]; M.evq_head = (M.evq_head + 1) % EVQ; M.evq_n--; }
    pthread_mutex_unlock(&M.mu);
    return got;
}
static float lerp_angle16(uint16_t a, uint16_t b, float f) {
    int d = (int)b - (int)a; if (d > 32768) d -= 65536; if (d < -32768) d += 65536;
    return (float)a + d * f;
}
int mp_get_remote(mp_remote *out, int max) {
    pthread_mutex_lock(&M.mu);
    int n = 0;
    if (M.snap_count) {
        double render = now_ms() + M.clock_offset - M.cfg.interp_delay_ms;   // server time we draw
        const Snap *newest = &M.snaps[(M.snap_head + M.snap_count - 1) % SNAPS];
        for (int i = 0; i < newest->n && n < max; i++) {   // the newest snapshot says who is near, nearest first (and their flags, tunic, hearts)
            mp_remote p = newest->p[i];
            const Track *tr = track_find(p.id);
            if (tr && tr->n) {
                const Sample *a = NULL, *b = NULL, *prev = NULL;
                for (int k = tr->n - 1; k >= 0; k--) {   // the two samples around the render time, by the times the player was there
                    const Sample *s = &tr->s[(tr->head + k) % TRACK_N];
                    if (s->ts <= render) { a = s; b = k + 1 < tr->n ? &tr->s[(tr->head + k + 1) % TRACK_N] : NULL; prev = k > 0 ? &tr->s[(tr->head + k - 1) % TRACK_N] : NULL; break; }
                }
                if (a && b && b->ts > a->ts) {
                    float f = (float)((render - a->ts) / (b->ts - a->ts)); if (f < 0) f = 0; if (f > 1) f = 1;
                    p.x = a->p.x + (b->p.x - a->p.x) * f; p.y = a->p.y + (b->p.y - a->p.y) * f; p.z = a->p.z + (b->p.z - a->p.z) * f;
                    p.yaw = (uint16_t)lrintf(lerp_angle16(a->p.yaw, b->p.yaw, f)); p.anim = f < 0.5f ? a->p.anim : b->p.anim;
                } else if (a) {   // past their newest: on along their last step for at most 150 ms, then held
                    double over = render - a->ts; if (over > 150) over = 150;
                    if (prev && a->ts > prev->ts) {
                        float f = (float)(over / (a->ts - prev->ts));
                        p.x = a->p.x + (a->p.x - prev->p.x) * f; p.y = a->p.y + (a->p.y - prev->p.y) * f; p.z = a->p.z + (a->p.z - prev->p.z) * f;
                    } else { p.x = a->p.x; p.y = a->p.y; p.z = a->p.z; }
                    p.yaw = a->p.yaw; p.anim = a->p.anim;
                } else {   // all of their track is still ahead of the render time (just in view): their oldest
                    const Sample *o = &tr->s[tr->head]; p.x = o->p.x; p.y = o->p.y; p.z = o->p.z; p.yaw = o->p.yaw; p.anim = o->p.anim;
                }
            }
            memcpy(p.name, NAMES[p.id], MP_NAME_MAX);
            out[n++] = p;
        }
    }
    pthread_mutex_unlock(&M.mu);
    return n;
}

// ---------------------------------------------------------------- events from the game
void mp_send_chat(const char *text) { uint8_t b[130]; W w = { b, 0, sizeof b, 0 }; w_str(&w, text, 120); queue_out(MP_CEV_CHAT, b, w.n); }
void mp_send_shot(float x, float y, float z, float dx, float dy, float dz, int kind) {
    float l = sqrtf(dx * dx + dy * dy + dz * dz); if (!(l > 0)) return; dx /= l; dy /= l; dz /= l;
    uint8_t b[32]; W w = { b, 0, sizeof b, 0 }; w_f32(&w, x); w_f32(&w, y); w_f32(&w, z);
    w_i16(&w, dx * 32767); w_i16(&w, dy * 32767); w_i16(&w, dz * 32767); w_u8(&w, (uint8_t)kind); queue_out(MP_CEV_SHOT, b, w.n);
}
void mp_send_hit(uint16_t target, int kind) { uint8_t b[4]; W w = { b, 0, sizeof b, 0 }; w_u16(&w, target); w_u8(&w, (uint8_t)kind); queue_out(MP_CEV_HIT, b, w.n); }
void mp_send_died(uint16_t killer, int how) { uint8_t b[4]; W w = { b, 0, sizeof b, 0 }; w_u16(&w, killer); w_u8(&w, (uint8_t)how); queue_out(MP_CEV_DIED, b, w.n); }
void mp_send_outfit(int outfit) { uint8_t b[1] = { (uint8_t)outfit }; queue_out(MP_CEV_OUTFIT, b, 1); }
void mp_request_respawn(void) { queue_out(MP_CEV_RESPAWN_REQ, NULL, 0); }
