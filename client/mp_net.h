// OoT Online: the game side's network client (C, no game knowledge). It speaks server/protocol.js's protocol v1 on a
// thread of its own; the game thread hands it Link's state once a frame and takes back the other players (interpolated
// for the moment it draws), the chat and the events (damage, respawn, kill feed, ...).
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MP_PROTOCOL 2   /* 2: ageMs in STATE and in each snapshot player (server/protocol.js) */
#define MP_MAX_NEAR 40
#define MP_NAME_MAX 25
#define MP_CHAT_MAX 161
#define MP_NOBODY 0xffff

enum { MP_KIND_PLAYER = 0, MP_KIND_BOT = 1 };
enum { MP_PF_AIM = 1, MP_PF_SWIM = 2, MP_PF_CLIMB = 4, MP_PF_AIR = 8, MP_PF_GUARD = 16, MP_PF_DEAD = 32, MP_PF_INVULN = 64, MP_PF_CHATTING = 128 };
enum { MP_EQ_CHILD = 1 };   // the state's equip byte (the server passes it through): bit 0 young Link
enum { MP_YOU_DEAD = 1, MP_YOU_INVULN = 2, MP_YOU_MUTED = 4 };
enum { MP_CEV_CHAT = 1, MP_CEV_SHOT = 2, MP_CEV_HIT = 3, MP_CEV_DIED = 4, MP_CEV_OUTFIT = 5, MP_CEV_RESPAWN_REQ = 6 };
enum { MP_SEV_CHAT = 1, MP_SEV_DAMAGE = 2, MP_SEV_KILLFEED = 3, MP_SEV_RESPAWN = 4, MP_SEV_HITCONFIRM = 5, MP_SEV_STATS = 6, MP_SEV_KICK = 7 };
enum { MP_EV_SHOT = 100 };   // someone's arrow near us (from the snapshot's shot list, not a server event)
enum { MP_CHAN_ALL = 0, MP_CHAN_SYSTEM = 1, MP_CHAN_WHISPER = 2 };
enum { MP_HIT_ARROW = 0, MP_HIT_FIRE_ARROW = 1, MP_HIT_ICE_ARROW = 2, MP_HIT_LIGHT_ARROW = 3, MP_HIT_MELEE = 4, MP_HIT_OTHER = 5 };
enum { MP_HOW_PLAYER = 0, MP_HOW_WORLD = 1 };
// Connection states (mp_status)
enum { MP_OFF = 0, MP_RESOLVING, MP_CONNECTING, MP_ONLINE, MP_LOST, MP_REJECTED };

typedef struct {
    uint32_t scene;
    float x, y, z;
    uint16_t yaw, anim;
    uint8_t flags, hp, max_hp, equip;   // hp in quarter hearts; equip: MP_EQ_*
} mp_local_state;

typedef struct {
    uint16_t id;
    float x, y, z;
    uint16_t yaw, anim;
    uint8_t flags, hp_pct, outfit, equip;
    char name[MP_NAME_MAX];             // "" until the server said it
} mp_remote;

typedef struct {
    int type;                           // MP_SEV_*
    union {
        struct { uint16_t from; uint8_t chan; char name[MP_NAME_MAX]; char text[MP_CHAT_MAX]; } chat;
        struct { uint16_t attacker; uint8_t quarter_hearts, kind; } damage;
        struct { uint16_t killer, victim; uint8_t how; char killer_name[MP_NAME_MAX], victim_name[MP_NAME_MAX]; } killfeed;
        struct { uint32_t scene; float x, y, z; uint16_t yaw, invuln_ms; } respawn;   // x NaN: your own last safe spot
        struct { uint16_t target; uint8_t quarter_hearts, killed; } hitconfirm;
        struct { uint16_t kills, deaths, streak, best, all_kills, all_deaths, all_best; } stats;   // this visit, then all time
        struct { char reason[MP_CHAT_MAX]; } kick;
        struct { uint16_t shooter; float x, y, z, dx, dy, dz; uint8_t kind; } shot;
    } u;
} mp_event;

typedef struct {
    int state;                          // MP_OFF ... MP_REJECTED
    uint16_t id;                        // ours, once online
    uint16_t online;                    // players on the server
    uint8_t you_flags;
    int ping_ms;
    char name[MP_NAME_MAX];             // ours, as the server made it unique
    char detail[MP_CHAT_MAX];           // why not online (reject reason, resolver error)
} mp_status_t;

typedef struct {
    const char *server;                 // "host[:port]" (default port 4697)
    const char *name;
    int outfit;
    const char *code;                   // room code, "" = none
    uint32_t build;                     // first 4 bytes of the game's main build ID (0 = don't say)
    uint32_t player_key;                // random per install, never 0
    int kind;                           // MP_KIND_PLAYER
    double interp_delay_ms;             // how far behind the newest snapshot others are drawn (default 2 ticks + 15 ms)
} mp_config;

int  mp_start(const mp_config *cfg);    // starts the network thread; 0 = ok
void mp_stop(void);                     // BYE, then the thread ends
void mp_set_local(const mp_local_state *st);                 // the game thread, once a frame
int  mp_get_remote(mp_remote *out, int max);                 // the others, interpolated for now (nearest first); returns how many
int  mp_poll_event(mp_event *ev);                            // 1 = an event was taken (call until 0, once a frame)
void mp_send_chat(const char *text);
void mp_send_shot(float x, float y, float z, float dx, float dy, float dz, int kind);
void mp_send_hit(uint16_t target, int kind);
void mp_send_died(uint16_t killer, int how);
void mp_send_outfit(int outfit);
void mp_request_respawn(void);
void mp_status(mp_status_t *out);
void mp_rename(const char *name);       // joins again under this name (same player key: the server replaces the old session)
void mp_set_outfit(int outfit);         // the outfit HELLO carries from now on (mp_send_outfit tells the server now)
const char *mp_name_of(uint16_t id);    // "" when unknown (thread-safe copy kept per id)
