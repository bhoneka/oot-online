// OoT Online: what the multiplayer module tells the screen (the Mac overlay, mp_overlay_mac.m). Any thread may call these;
// the overlay draws on the main thread. With no window (headless runs) they do nothing.
#pragma once
#include <stdint.h>

enum { MP_UI_CHAT = 0, MP_UI_SYSTEM = 1, MP_UI_WHISPER = 2, MP_UI_ME = 3 };
void mp_ui_chat(int kind, const char *name, int outfit, const char *text);   // a line in the chat log (name "" = none)
void mp_ui_feed(const char *killer, const char *victim);                     // the kill feed (killer "" = the world got them)
void mp_ui_hitmarker(int killed);                                            // our arrow landed
void mp_ui_banner(const char *text, double seconds);                         // the big centre line ("Shot by X", "Back in 3")
void mp_ui_status(int state, int online, const char *detail, int ping_ms);   // the corner pill (mp_net.h MP_OFF ... MP_REJECTED)
// Name tags: the screen positions of the players drawn this frame (0..1 from the top left; visible 0 = behind or off screen)
typedef struct { float x, y; int visible; char name[25]; int outfit; float hp; } mp_ui_tag;
void mp_ui_tags(const mp_ui_tag *tags, int n);

// The lobby page (in the game window, driven by the controller): shown while `shown`; the module fills it
typedef struct {
    int shown;
    int outfit;                         // 0 Kokiri, 1 Goron, 2 Zora
    int can_play;                       // PLAY lights up (online, or the server didn't answer for a while: PLAY OFFLINE)
    int offline;                        // the server isn't there: A plays the game alone
    int pressed;                        // a press to flash: 1 left, 2 right, 3 play
    char name[25];
    int kills, deaths, best;            // all time, from the server
    char where[64];                     // "Hyrule Field"
} mp_ui_lobby_t;
void mp_ui_lobby(const mp_ui_lobby_t *l);

// The text box. Chat: T or / opens it. Rename: the lobby's Y opens it with the name; Return sends to on_rename
int  mp_ui_editing(void);
void mp_ui_open_rename(const char *current);   // any thread
#ifdef __OBJC__
@class NSEvent, NSWindow;
// A key the game window got: T (or keypad Enter) opens the box, / opens it with a "/"; 1 = taken (not for the game).
// Not Return: nxnative's key map gives it the game's + (front_mac.m key_bit)
int  mp_ui_key(NSEvent *e);
void mp_ui_attach(NSWindow *win, void (*release_input)(void), void (*resume_input)(void));
#endif
// What the box sends (set by the multiplayer module)
extern void (*mp_ui_on_send)(const char *text);
extern void (*mp_ui_on_rename)(const char *name);
