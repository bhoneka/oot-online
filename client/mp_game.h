// OoT Online: what the multiplayer module needs from one game. Each function has a weak no-op default in hle_mp.c;
// a game's file (games/oot_v131072.c) gives the real ones once its addresses are known. Two threads call in:
//  - the present thread, once a presented frame (reads only: Link's state, the camera);
//  - the game's own thread, from player 1's input read at the start of its frame (writes: damage, respawn, the stand-ins,
//    the lobby window), so the game never sees its objects change under it mid-update; game code may be called there.
#pragma once
#include "mp_net.h"

int  mp_game_read_local(mp_local_state *st);            // present thread: 1 = Link is in the world and st holds him
int  mp_game_project(float x, float y, float z, float *sx, float *sy);   // present thread: world -> screen 0..1 (top left); 1 = in front of the camera
uint32_t mp_game_build(void);                           // first 4 bytes of main's build ID (for HELLO), 0 = unknown

// Two ticks, both on the game's frame thread. The world tick runs from player 1's input read (the game's own stack: game code
// that locks may be called) while the player is in control; the game stops reading player 1 there while one of its menus is open.
// The UI tick runs from the frame gate every frame, menus or not, on nxnative's gate stack: only calls that never make a system
// call (no lock, no clock read) may run there. World tick, in this order each frame:
void mp_game_apply_damage(int quarter_hearts, uint16_t attacker, int kind);
void mp_game_respawn(uint32_t scene, float x, float y, float z, uint16_t yaw, int own_spot);   // own_spot: the game's last safe spot
int  mp_game_set_outfit(int outfit);                    // 1 = on Link now (0: no Link yet, or child Link)
void mp_game_update_puppets(const mp_remote *others, int n);   // place / animate / add / remove the stand-ins
void mp_game_shot_fx(uint16_t shooter, float x, float y, float z, float dx, float dy, float dz, int kind);
int  mp_game_take_local_events(void);
// The lobby as the game's own system window over its dimmed background (game thread). want: shown or not; text: the window's
// sentence; labels: the three buttons (Play, Tunic, Name). Returns 0 when the game's window cannot be used now (no Link in the
// world, the screens not there: the caller shows its own page), 1 opening, 2 open, 3 closing. *decided: the button the player
// chose this frame (0..2), else -1; the window closes itself after a choice (ask again to show it again)
void mp_game_notice(const char *line);                  // any thread: a one-line notice in the game's own info bar (queued, one at a time)
int  mp_game_go_to_field(void);                         // world tick: take Link to the shared place (Hyrule Field) unless he is there; 1 = under way or there, 0 = not now (try again)
int  mp_game_ready(void);                               // 1 = this build is known: the game's own window is the lobby (else the drawn page)
void mp_game_ui_tick(void);                             // the frame gate (no calls that can make a system call): the notices
void mp_game_world_prep(void);                          // the world tick: what the UI side asked for that can lock (loading a screen)
int  mp_game_lobby(int want, const char *text, const char *const labels[3], int *decided);
// Observation gates in the game's code (the game thread; regs = x0..x30 as saved there, a write to x0..x28 takes effect):
void mp_game_gate_event(uint64_t *regs);       // EventMgr::requestEvent's entry: may refuse a request (story cutscenes)
void mp_game_gate_gameover(uint64_t *regs);    // PlayerComponent::calc's game-over check
void mp_game_gate_shoot(uint64_t *regs);       // a ShooterComponent fires (x0 the shooter)                   // the game's side of shots, hits, deaths: it calls mp_send_* itself; returns how many

