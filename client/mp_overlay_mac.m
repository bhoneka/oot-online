// OoT Online: the Mac overlay over the game window. The lobby page (outfit, your card, PLAY), the chat log (bottom left)
// with its input box, the kill feed (top right), the connection pill (top left), the hit marker, the big centre banner
// and name tags over the other players. AppKit views on top of the game's CAMetalLayer view; the mouse goes through
// them. Everything here runs on the main thread; mp_ui_* calls from other threads only fill the shared state under a lock.
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#include <os/lock.h>
#include <string.h>
#include "mp_ui.h"
#include "mp_net.h"

void (*mp_ui_on_send)(const char *text);
void (*mp_ui_on_rename)(const char *name);

typedef struct { char name[25]; char text[200]; int kind, outfit; double t; } Line;
static os_unfair_lock L = OS_UNFAIR_LOCK_INIT;
static Line g_chat[64]; static int g_chat_n;              // newest last
typedef struct { char k[25], v[25]; double t; } Feed;
static Feed g_feed[6]; static int g_feed_n;
static double g_hit_t = -1e9; static int g_hit_killed;
static char g_banner[200]; static double g_banner_until;
static int g_state, g_online, g_ping; static char g_detail[161];
static mp_ui_tag g_tags[MP_MAX_NEAR]; static int g_tags_n;
static mp_ui_lobby_t g_lobby; static double g_lobby_press_t = -1e9; static int g_lobby_press; static double g_lobby_since;
static int g_editing, g_rename;                           // main thread
static char g_rename_req[25]; static int g_rename_pending; // any thread -> main
static NSWindow *g_win; static NSTextField *g_field; static NSView *g_overlay;
static void (*g_release)(void), (*g_resume)(void);

static double tnow(void) { return CACurrentMediaTime(); }

// The game's own typefaces (Font2/Scalable in its RomFS: Cetra Display Pro for titles, Clarimo UD PE for menus, NinDev-Number for
// figures, External for the controller's buttons), taken from the player's own game by tools/game_fonts.py into
// $DM_MP_FONTS (default $DM_ROOT/oot_online/fonts). Without them the system font stands in.
enum { F_TITLE, F_DISPLAY, F_LABEL, F_BODY, F_LIGHT, F_NUMBER, F_ICON };
static const char *FONT_PS[] = { "CetraDisplayPro-Medium", "CetraDisplayPro-Regular", "ClarimoUDPE-DemiBold", "ClarimoUDPE-Regular", "ClarimoUDPE-Light", "NinDevNumber-Lining", "External" };
static int g_fonts_ok;
static void load_game_fonts(void) {
    const char *d = getenv("DM_MP_FONTS"); char buf[1024];
    if (!d || !*d) { const char *root = getenv("DM_ROOT"); snprintf(buf, sizeof buf, "%s/oot_online/fonts", root && *root ? root : "."); d = buf; }
    NSString *dir = @(d); int n = 0;
    for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil])
        if ([f hasSuffix:@".otf"] || [f hasSuffix:@".ttf"]) n += CTFontManagerRegisterFontsForURL((__bridge CFURLRef)[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:f]], kCTFontManagerScopeProcess, NULL) ? 1 : 0;
    g_fonts_ok = [NSFont fontWithName:@"ClarimoUDPE-Regular" size:12] != nil;
    NSLog(@"OoT Online: %d of the game's fonts from %@%@", n, dir, g_fonts_ok ? @"" : @" (none: the system font stands in)");
}
static NSFont *gfont(int role, CGFloat size) {
    NSFont *f = [NSFont fontWithName:@(FONT_PS[role]) size:size];
    if (f) return f;
    NSFontWeight w = role == F_TITLE || role == F_LABEL ? NSFontWeightBold : role == F_LIGHT ? NSFontWeightLight : NSFontWeightRegular;
    return role == F_NUMBER ? [NSFont monospacedDigitSystemFontOfSize:size weight:NSFontWeightBold] : [NSFont systemFontOfSize:size weight:w];
}
static NSString *icon(unichar c) { return [NSString stringWithCharacters:&c length:1]; }   // External: E040 A, E041 B, E042 X, E043 Y, E049 D-pad
static void copy(char *dst, size_t cap, const char *s) { snprintf(dst, cap, "%s", s ? s : ""); }

void mp_ui_chat(int kind, const char *name, int outfit, const char *text) {
    os_unfair_lock_lock(&L);
    if (g_chat_n == 64) { memmove(g_chat, g_chat + 1, sizeof(Line) * 63); g_chat_n--; }
    Line *l = &g_chat[g_chat_n++]; copy(l->name, sizeof l->name, name); copy(l->text, sizeof l->text, text); l->kind = kind; l->outfit = outfit; l->t = tnow();
    os_unfair_lock_unlock(&L);
}
void mp_ui_feed(const char *killer, const char *victim) {
    os_unfair_lock_lock(&L);
    if (g_feed_n == 6) { memmove(g_feed, g_feed + 1, sizeof(Feed) * 5); g_feed_n--; }
    Feed *f = &g_feed[g_feed_n++]; copy(f->k, sizeof f->k, killer); copy(f->v, sizeof f->v, victim); f->t = tnow();
    os_unfair_lock_unlock(&L);
}
void mp_ui_hitmarker(int killed) { os_unfair_lock_lock(&L); g_hit_t = tnow(); g_hit_killed = killed; os_unfair_lock_unlock(&L); }
void mp_ui_banner(const char *text, double seconds) { os_unfair_lock_lock(&L); copy(g_banner, sizeof g_banner, text); g_banner_until = tnow() + seconds; os_unfair_lock_unlock(&L); }
void mp_ui_status(int state, int online, const char *detail, int ping_ms) {
    os_unfair_lock_lock(&L); g_state = state; g_online = online; g_ping = ping_ms; copy(g_detail, sizeof g_detail, detail); os_unfair_lock_unlock(&L);
}
void mp_ui_tags(const mp_ui_tag *tags, int n) {
    os_unfair_lock_lock(&L); if (n > MP_MAX_NEAR) n = MP_MAX_NEAR; memcpy(g_tags, tags, sizeof(mp_ui_tag) * n); g_tags_n = n; os_unfair_lock_unlock(&L);
}
void mp_ui_lobby(const mp_ui_lobby_t *l) {
    os_unfair_lock_lock(&L);
    if (l->shown && !g_lobby.shown) g_lobby_since = tnow();
    if (l->pressed) { g_lobby_press = l->pressed; g_lobby_press_t = tnow(); }
    g_lobby = *l; g_lobby.pressed = 0;
    os_unfair_lock_unlock(&L);
}
int mp_ui_editing(void) { return g_editing; }

// ---------------------------------------------------------------- drawing helpers
static NSColor *rgb(double r, double g, double b) { return [NSColor colorWithSRGBRed:r green:g blue:b alpha:1]; }
static NSColor *outfit_color(int o) { return o == 1 ? rgb(0.96, 0.45, 0.36) : o == 2 ? rgb(0.42, 0.68, 1.00) : rgb(0.47, 0.86, 0.45); }
static NSColor *outfit_deep(int o) { return o == 1 ? rgb(0.62, 0.16, 0.12) : o == 2 ? rgb(0.10, 0.30, 0.66) : rgb(0.13, 0.45, 0.17); }
static const char *OUTFIT_NAME[] = { "KOKIRI TUNIC", "GORON TUNIC", "ZORA TUNIC" };
static const char *OUTFIT_LINE[] = { "The forest's green. Where it all began.", "Woven for Death Mountain's heat.", "Cut for the waters of Zora's Domain." };
static NSShadow *text_shadow(void) {
    static NSShadow *s; if (!s) { s = [NSShadow new]; s.shadowColor = [NSColor colorWithWhite:0 alpha:0.85]; s.shadowBlurRadius = 3; s.shadowOffset = NSMakeSize(0, -1); }
    return s;
}
static NSDictionary *fattrs(int role, CGFloat size, NSColor *c, CGFloat alpha) {
    return @{ NSFontAttributeName: gfont(role, size), NSForegroundColorAttributeName: [c colorWithAlphaComponent:alpha * c.alphaComponent], NSShadowAttributeName: text_shadow() };
}
static NSDictionary *attrs(CGFloat size, NSFontWeight w, NSColor *c, CGFloat alpha) {   // the old weights map onto the game's faces
    return fattrs(w >= NSFontWeightBlack ? F_TITLE : w >= NSFontWeightSemibold ? F_LABEL : w <= NSFontWeightLight ? F_LIGHT : F_BODY, size, c, alpha);
}
static NSDictionary *spaced(CGFloat size, NSFontWeight w, NSColor *c, CGFloat alpha, CGFloat kern) {   // spaced capitals for labels
    NSMutableDictionary *d = [attrs(size, w, c, alpha) mutableCopy]; d[NSKernAttributeName] = @(kern); return d;
}
static NSColor *CREAM(void) { return [NSColor colorWithSRGBRed:0.93 green:0.88 blue:0.76 alpha:1]; }   // the warm off-white of Zelda menus' lines and labels
static NSColor *GOLD(void) { return [NSColor colorWithSRGBRed:0.95 green:0.80 blue:0.42 alpha:1]; }
static void draw_italic(NSString *s, NSPoint p, NSDictionary *a) {   // a sheared, heavy title (the lobby's own look, not a borrowed logo)
    [NSGraphicsContext saveGraphicsState];
    NSAffineTransform *t = [NSAffineTransform transform]; [t translateXBy:p.x yBy:p.y];
    NSAffineTransformStruct m = t.transformStruct; m.m21 = -0.18; t.transformStruct = m; [t concat];
    [s drawAtPoint:NSZeroPoint withAttributes:a];
    [NSGraphicsContext restoreGraphicsState];
}
static void fill_round(NSRect r, CGFloat rad, NSColor *c) { [c setFill]; [[NSBezierPath bezierPathWithRoundedRect:r xRadius:rad yRadius:rad] fill]; }

static void draw_lobby(NSRect b, CGFloat sc, double t) {
    mp_ui_lobby_t *l = &g_lobby;
    double in = MIN(1, (t - g_lobby_since) / 0.35); CGFloat al = in;   // fades in
    // vignette: dark on the left and bottom, the game's picture stays open in the middle
    NSGradient *gl = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithWhite:0 alpha:0.78 * al], 0.0, [NSColor colorWithWhite:0 alpha:0.35 * al], 0.42, [NSColor colorWithWhite:0 alpha:0.0], 0.7, nil];
    [gl drawInRect:b angle:0];
    NSGradient *gb = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithWhite:0 alpha:0.0], 0.55, [NSColor colorWithWhite:0 alpha:0.7 * al], 1.0, nil];
    [gb drawInRect:b angle:90];
    CGFloat L0 = 64 * sc, top = 70 * sc;
    // title
    if (!g_fonts_ok) draw_italic(@"OoT ONLINE", NSMakePoint(L0, top), attrs(64 * sc, NSFontWeightBlack, NSColor.whiteColor, al));
    else {
        [icon(0xE090) drawAtPoint:NSMakePoint(L0, top + 10 * sc) withAttributes:fattrs(F_ICON, 54 * sc, CREAM(), al)];   // the Hyrule crest, from the game's own icon font
        [@"OoT Online" drawAtPoint:NSMakePoint(L0 + 72 * sc, top) withAttributes:fattrs(F_TITLE, 64 * sc, NSColor.whiteColor, al)];
    }
    NSString *sub = l->where[0] ? [NSString stringWithFormat:@"SHARED WORLD  ·  %s", l->where].uppercaseString : @"SHARED WORLD";
    [sub drawAtPoint:NSMakePoint(L0 + (g_fonts_ok ? 76 : 6) * sc, top + 80 * sc) withAttributes:spaced(14 * sc, NSFontWeightHeavy, GOLD(), al, 3)];
    [[NSColor colorWithWhite:1 alpha:0] setFill];
    NSBezierPath *rule = [NSBezierPath bezierPath]; [rule moveToPoint:NSMakePoint(L0, top + 108 * sc)]; [rule lineToPoint:NSMakePoint(L0 + 460 * sc, top + 108 * sc)];
    rule.lineWidth = 1; [[CREAM() colorWithAlphaComponent:0.35 * al] setStroke]; [rule stroke];
    // outfit block
    CGFloat oy = b.size.height * 0.42;
    [@"OUTFIT" drawAtPoint:NSMakePoint(L0, oy) withAttributes:spaced(13 * sc, NSFontWeightBold, CREAM(), 0.8 * al, 2.5)];
    int o = l->outfit % 3;
    double pl = t - g_lobby_press_t; CGFloat bump = (pl < 0.18 && (g_lobby_press == 1 || g_lobby_press == 2)) ? (CGFloat)(1 - pl / 0.18) : 0;
    NSDictionary *big = fattrs(F_DISPLAY, 46 * sc, NSColor.whiteColor, al);
    NSString *on = [@(OUTFIT_NAME[o]) capitalizedString]; NSSize osz = [on sizeWithAttributes:big];
    CGFloat ny = oy + 22 * sc, nx = L0 + 34 * sc + (g_lobby_press == 2 ? -1 : 1) * bump * 10 * sc;
    NSDictionary *chev = attrs(30 * sc, NSFontWeightBold, NSColor.whiteColor, 0.85 * al);
    [@"‹" drawAtPoint:NSMakePoint(L0, ny + 4 * sc) withAttributes:chev];
    [on drawAtPoint:NSMakePoint(nx, ny) withAttributes:big];
    [@"›" drawAtPoint:NSMakePoint(L0 + 34 * sc + osz.width + 18 * sc, ny + 4 * sc) withAttributes:chev];
    [@(OUTFIT_LINE[o]) drawAtPoint:NSMakePoint(L0 + 2 * sc, ny + osz.height + 2 * sc) withAttributes:attrs(15 * sc, NSFontWeightRegular, NSColor.whiteColor, 0.78 * al)];
    // swatches: the three tunics, the picked one ringed
    CGFloat sy = ny + osz.height + 38 * sc, sw = 46 * sc;
    for (int i = 0; i < 3; i++) {
        CGFloat grow = i == o ? 8 * sc : 0;
        NSRect r = NSMakeRect(L0 + i * (sw + 16 * sc) - grow / 2, sy - grow / 2, sw + grow, sw + grow);
        NSGradient *g = [[NSGradient alloc] initWithStartingColor:[outfit_color(i) colorWithAlphaComponent:al] endingColor:[outfit_deep(i) colorWithAlphaComponent:al]];
        NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:r xRadius:10 * sc yRadius:10 * sc]; [g drawInBezierPath:p angle:-90];
        if (i == o) { [[NSColor colorWithWhite:1 alpha:al] setStroke]; p.lineWidth = 3 * sc; [p stroke]; }
        else { [[NSColor colorWithWhite:0 alpha:0.35 * al] setFill]; [p fill]; }
    }
    if (g_fonts_ok) { [icon(0xE049) drawAtPoint:NSMakePoint(L0, sy + sw + 14 * sc) withAttributes:fattrs(F_ICON, 22 * sc, NSColor.whiteColor, 0.85 * al)];
                      [@"Change" drawAtPoint:NSMakePoint(L0 + 30 * sc, sy + sw + 16 * sc) withAttributes:fattrs(F_BODY, 15 * sc, NSColor.whiteColor, 0.75 * al)]; }
    else [@"◀ ▶  change" drawAtPoint:NSMakePoint(L0, sy + sw + 18 * sc) withAttributes:attrs(13 * sc, NSFontWeightMedium, NSColor.whiteColor, 0.6 * al)];
    // the player card, right
    CGFloat cw = 300 * sc, cx = b.size.width - cw - 64 * sc, cy = b.size.height * 0.30;
    NSRect card = NSMakeRect(cx, cy, cw, 196 * sc);
    fill_round(card, 16 * sc, [NSColor colorWithWhite:0.05 alpha:0.62 * al]);
    [[CREAM() colorWithAlphaComponent:0.28 * al] setStroke]; NSBezierPath *cb = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(card, 4 * sc, 4 * sc) xRadius:12 * sc yRadius:12 * sc]; cb.lineWidth = 1; [cb stroke];
    fill_round(NSMakeRect(cx + 24 * sc, cy + card.size.height - 10 * sc, cw - 48 * sc, 3 * sc), 1.5 * sc, [outfit_color(o) colorWithAlphaComponent:0.9 * al]);
    [@"PLAYING AS" drawAtPoint:NSMakePoint(cx + 26 * sc, cy + 20 * sc) withAttributes:spaced(12 * sc, NSFontWeightBold, CREAM(), 0.75 * al, 2.5)];
    [@(l->name[0] ? l->name : "Hero") drawAtPoint:NSMakePoint(cx + 24 * sc, cy + 36 * sc) withAttributes:fattrs(F_DISPLAY, 34 * sc, NSColor.whiteColor, al)];
    NSRect chip = NSMakeRect(cx + 26 * sc, cy + 84 * sc, 92 * sc, 24 * sc);
    fill_round(chip, 12 * sc, [NSColor colorWithWhite:1 alpha:0.14 * al]);
    if (g_fonts_ok) { [icon(0xE043) drawAtPoint:NSMakePoint(chip.origin.x + 10 * sc, chip.origin.y + 1 * sc) withAttributes:fattrs(F_ICON, 18 * sc, NSColor.whiteColor, al)];
                      [@"Rename" drawAtPoint:NSMakePoint(chip.origin.x + 34 * sc, chip.origin.y + 3 * sc) withAttributes:fattrs(F_LABEL, 13 * sc, NSColor.whiteColor, 0.9 * al)]; }
    else [@"Y  rename" drawAtPoint:NSMakePoint(chip.origin.x + 14 * sc, chip.origin.y + 4 * sc) withAttributes:attrs(12.5 * sc, NSFontWeightSemibold, NSColor.whiteColor, 0.9 * al)];
    NSString *stats[3][2] = { { @"KILLS", [NSString stringWithFormat:@"%d", l->kills] }, { @"DEATHS", [NSString stringWithFormat:@"%d", l->deaths] }, { @"BEST STREAK", [NSString stringWithFormat:@"%d", l->best] } };
    for (int i = 0; i < 3; i++) {
        CGFloat x = cx + 26 * sc + i * 88 * sc;
        [stats[i][1] drawAtPoint:NSMakePoint(x, cy + 118 * sc) withAttributes:fattrs(F_NUMBER, 30 * sc, NSColor.whiteColor, al)];
        [stats[i][0] drawAtPoint:NSMakePoint(x, cy + 156 * sc) withAttributes:spaced(10 * sc, NSFontWeightBold, CREAM(), 0.7 * al, 1.5)];
    }
    // players online, bottom left
    NSString *who = g_state == MP_ONLINE ? [NSString stringWithFormat:@"%d PLAYER%s ONLINE", g_online, g_online == 1 ? "" : "S"]
                  : g_state == MP_REJECTED ? [NSString stringWithFormat:@"NOT ONLINE: %s", g_detail].uppercaseString : @"CONNECTING…";
    CGFloat by = b.size.height - 92 * sc, d = 10 * sc;
    [(g_state == MP_ONLINE ? NSColor.systemGreenColor : g_state == MP_REJECTED ? NSColor.systemRedColor : NSColor.systemYellowColor) setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(L0, by + 6 * sc, d, d)] fill];
    [who drawAtPoint:NSMakePoint(L0 + 20 * sc, by) withAttributes:spaced(15 * sc, NSFontWeightHeavy, CREAM(), 0.95 * al, 1.5)];
    // PLAY, bottom right
    CGFloat pw = 300 * sc, ph = 78 * sc; double pp = t - g_lobby_press_t; CGFloat press = (g_lobby_press == 3 && pp < 0.25) ? (CGFloat)(1 - pp / 0.25) : 0;
    NSRect play = NSMakeRect(b.size.width - pw - 64 * sc + press * 4 * sc, b.size.height - ph - 60 * sc + press * 4 * sc, pw - press * 8 * sc, ph - press * 8 * sc);
    NSColor *pc = l->can_play ? GOLD() : [NSColor colorWithWhite:0.55 alpha:1];
    NSBezierPath *pp_ = [NSBezierPath bezierPathWithRoundedRect:play xRadius:10 * sc yRadius:10 * sc];
    NSGradient *pg = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithWhite:0.10 alpha:0.82 * al] endingColor:[NSColor colorWithWhite:0.02 alpha:0.9 * al]];
    [pg drawInBezierPath:pp_ angle:-90];
    double glow = 0.55 + 0.45 * sin(t * 3.2); pp_.lineWidth = 2 * sc; [[pc colorWithAlphaComponent:(l->can_play ? glow : 0.5) * al] setStroke]; [pp_ stroke];
    NSDictionary *pa = @{ NSFontAttributeName: gfont(F_DISPLAY, 38 * sc), NSForegroundColorAttributeName: [(l->can_play ? NSColor.whiteColor : [NSColor colorWithWhite:0.7 alpha:1]) colorWithAlphaComponent:al], NSKernAttributeName: @1, NSShadowAttributeName: text_shadow() };
    NSString *ps = !l->can_play ? @"Wait" : l->offline ? @"Play offline" : @"Play"; NSSize psz = [ps sizeWithAttributes:pa];
    CGFloat ad = 30 * sc; NSRect abtn = NSMakeRect(play.origin.x + (play.size.width - psz.width - ad - 14 * sc) / 2, play.origin.y + (play.size.height - ad) / 2, ad, ad);
    if (g_fonts_ok) { NSDictionary *ia = fattrs(F_ICON, 32 * sc, NSColor.whiteColor, al); NSSize isz = [icon(0xE040) sizeWithAttributes:ia];
                      [icon(0xE040) drawAtPoint:NSMakePoint(NSMidX(abtn) - isz.width / 2, NSMidY(abtn) - isz.height / 2) withAttributes:ia]; }
    else { [[NSColor colorWithWhite:1 alpha:al] setFill]; [[NSBezierPath bezierPathWithOvalInRect:abtn] fill];
           NSDictionary *aa = @{ NSFontAttributeName: [NSFont systemFontOfSize:17 * sc weight:NSFontWeightBlack], NSForegroundColorAttributeName: [NSColor colorWithWhite:0.1 alpha:al] };
           NSSize asz = [@"A" sizeWithAttributes:aa]; [@"A" drawAtPoint:NSMakePoint(NSMidX(abtn) - asz.width / 2, NSMidY(abtn) - asz.height / 2) withAttributes:aa]; }
    [ps drawAtPoint:NSMakePoint(NSMaxX(abtn) + 14 * sc, play.origin.y + (play.size.height - psz.height) / 2) withAttributes:pa];
    [@"T  Chat" drawAtPoint:NSMakePoint(play.origin.x + 4 * sc, play.origin.y - 26 * sc) withAttributes:fattrs(F_BODY, 14 * sc, NSColor.whiteColor, 0.6 * al)];
}

@interface MPOverlay : NSView @end
@implementation MPOverlay
- (BOOL)isFlipped { return YES; }
- (NSView *)hitTest:(NSPoint)p { (void)p; return nil; }   // the mouse goes to the game
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = self.bounds; double t = tnow();
    CGFloat scale = MAX(0.8, MIN(1.6, b.size.height / 900.0));   // text follows the window's size
    os_unfair_lock_lock(&L);
    if (g_lobby.shown) draw_lobby(b, scale, t);
    // ---- connection pill, top left (the lobby page says it itself)
    if (!g_lobby.shown) {
        NSString *s; NSColor *dot;
        if (g_state == MP_ONLINE) { s = [NSString stringWithFormat:@"OoT Online · %d online%@", g_online, g_ping > 0 ? [NSString stringWithFormat:@" · %d ms", g_ping] : @""]; dot = [NSColor systemGreenColor]; }
        else if (g_state == MP_LOST) { s = @"Can't reach the server, trying again…"; dot = [NSColor systemOrangeColor]; }
        else if (g_state == MP_REJECTED) { s = [NSString stringWithFormat:@"Not online: %s", g_detail]; dot = [NSColor systemRedColor]; }
        else if (g_state == MP_OFF) s = nil, dot = nil;
        else { s = @"Connecting to OoT Online…"; dot = [NSColor systemYellowColor]; }
        if (s) {
            NSDictionary *a = attrs(12.5 * scale, NSFontWeightMedium, NSColor.whiteColor, 0.95);
            NSSize sz = [s sizeWithAttributes:a];
            NSRect pill = NSMakeRect(16, 14, sz.width + 34 * scale, sz.height + 10 * scale);
            fill_round(pill, pill.size.height / 2, [NSColor colorWithWhite:0 alpha:0.42]);
            CGFloat d = 8 * scale; [dot setFill]; [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(pill.origin.x + 11 * scale, pill.origin.y + (pill.size.height - d) / 2, d, d)] fill];
            [s drawAtPoint:NSMakePoint(pill.origin.x + 24 * scale, pill.origin.y + 5 * scale) withAttributes:a];
        }
    }
    // ---- name tags
    if (!g_lobby.shown) for (int i = 0; i < g_tags_n; i++) {
        mp_ui_tag *g = &g_tags[i]; if (!g->visible || !g->name[0]) continue;
        NSDictionary *a = attrs(12 * scale, NSFontWeightSemibold, outfit_color(g->outfit), 0.95);
        NSString *s = @(g->name); NSSize sz = [s sizeWithAttributes:a];
        CGFloat x = g->x * b.size.width - sz.width / 2, y = g->y * b.size.height - sz.height - 8 * scale;
        [s drawAtPoint:NSMakePoint(x, y) withAttributes:a];
        if (g->hp < 0.999f) {   // a small bar once they're hurt
            CGFloat w = MAX(36 * scale, sz.width * 0.8), h = 3 * scale, bx = g->x * b.size.width - w / 2, by = y + sz.height + 1;
            [[NSColor colorWithWhite:0 alpha:0.5] setFill]; NSRectFill(NSMakeRect(bx, by, w, h));
            [[NSColor colorWithSRGBRed:0.95 green:0.3 blue:0.3 alpha:0.95] setFill]; NSRectFill(NSMakeRect(bx, by, w * MAX(0, g->hp), h));
        }
    }
    // ---- kill feed, top right
    if (!g_lobby.shown) {
        CGFloat y = 14;
        for (int i = 0; i < g_feed_n; i++) {
            Feed *f = &g_feed[i]; double age = t - f->t; if (age > 7) continue;
            CGFloat al = age < 6 ? 1 : 7 - age;
            NSMutableAttributedString *s = [NSMutableAttributedString new];
            NSDictionary *nm = attrs(13 * scale, NSFontWeightSemibold, NSColor.whiteColor, al), *mid = attrs(13 * scale, NSFontWeightRegular, [NSColor colorWithWhite:0.85 alpha:1], al);
            if (f->k[0]) { [s appendAttributedString:[[NSAttributedString alloc] initWithString:@(f->k) attributes:nm]]; [s appendAttributedString:[[NSAttributedString alloc] initWithString:@"  ➶  " attributes:mid]]; }
            [s appendAttributedString:[[NSAttributedString alloc] initWithString:@(f->v) attributes:nm]];
            if (!f->k[0]) [s appendAttributedString:[[NSAttributedString alloc] initWithString:@" fell" attributes:mid]];
            NSSize sz = s.size; NSRect r = NSMakeRect(b.size.width - sz.width - 28 * scale, y, sz.width + 14 * scale, sz.height + 6 * scale);
            fill_round(r, 6, [NSColor colorWithWhite:0 alpha:0.38 * al]);
            [s drawAtPoint:NSMakePoint(r.origin.x + 7 * scale, r.origin.y + 3 * scale)];
            y += r.size.height + 4;
        }
    }
    // ---- chat log, bottom left (above the input box; in the lobby, above the player count)
    {
        CGFloat maxw = MIN(560 * scale, b.size.width * 0.45), y = b.size.height - 20 - (g_editing ? 34 * scale : 0) - (g_lobby.shown ? 110 * scale : 0);
        int shown = 0;
        for (int i = g_chat_n - 1; i >= 0 && shown < (g_editing ? 14 : g_lobby.shown ? 5 : 8); i--) {
            Line *l = &g_chat[i]; double age = t - l->t;
            CGFloat al = g_editing ? 1 : (age < 12 ? 1 : age < 14 ? (14 - age) / 2 : 0);
            if (al <= 0) break;
            NSMutableAttributedString *s = [NSMutableAttributedString new];
            NSColor *body = l->kind == MP_UI_SYSTEM ? [NSColor colorWithSRGBRed:1 green:0.9 blue:0.55 alpha:1] : l->kind == MP_UI_WHISPER ? [NSColor colorWithSRGBRed:0.95 green:0.7 blue:1 alpha:1] : NSColor.whiteColor;
            if (l->name[0]) [s appendAttributedString:[[NSAttributedString alloc] initWithString:[NSString stringWithFormat:@"%s%@", l->name, l->kind == MP_UI_WHISPER ? @" (whisper): " : @": "]
                                                                                       attributes:attrs(13.5 * scale, NSFontWeightSemibold, l->kind == MP_UI_WHISPER ? body : outfit_color(l->outfit), al)]];
            [s appendAttributedString:[[NSAttributedString alloc] initWithString:@(l->text) attributes:attrs(13.5 * scale, NSFontWeightRegular, body, al)]];
            NSRect br = [s boundingRectWithSize:NSMakeSize(maxw, 1000) options:NSStringDrawingUsesLineFragmentOrigin];
            y -= br.size.height + 3;
            fill_round(NSMakeRect(12, y - 2, br.size.width + 12, br.size.height + 4), 5, [NSColor colorWithWhite:0 alpha:0.30 * al]);
            [s drawWithRect:NSMakeRect(18, y, maxw, br.size.height) options:NSStringDrawingUsesLineFragmentOrigin];
            shown++;
        }
        if (!g_editing && !g_lobby.shown && g_state == MP_ONLINE && (g_chat_n == 0 || t - g_chat[g_chat_n - 1].t > 14)) {
            NSString *hint = @"T to chat"; [hint drawAtPoint:NSMakePoint(18, b.size.height - 24 * scale) withAttributes:attrs(11 * scale, NSFontWeightRegular, NSColor.whiteColor, 0.45)];
        }
    }
    // ---- hit marker
    double ha = t - g_hit_t;
    if (ha < 0.3) {
        CGFloat al = 1 - ha / 0.3, s = 9 * scale, cx = b.size.width / 2, cy = b.size.height / 2;
        NSColor *c = g_hit_killed ? [NSColor colorWithSRGBRed:1 green:0.25 blue:0.2 alpha:al] : [NSColor colorWithWhite:1 alpha:al];
        NSBezierPath *p = [NSBezierPath bezierPath]; p.lineWidth = 2.2 * scale; p.lineCapStyle = NSLineCapStyleRound;
        [p moveToPoint:NSMakePoint(cx - s, cy - s)]; [p lineToPoint:NSMakePoint(cx - s / 3, cy - s / 3)];
        [p moveToPoint:NSMakePoint(cx + s, cy - s)]; [p lineToPoint:NSMakePoint(cx + s / 3, cy - s / 3)];
        [p moveToPoint:NSMakePoint(cx - s, cy + s)]; [p lineToPoint:NSMakePoint(cx - s / 3, cy + s / 3)];
        [p moveToPoint:NSMakePoint(cx + s, cy + s)]; [p lineToPoint:NSMakePoint(cx + s / 3, cy + s / 3)];
        [c setStroke]; [p stroke];
    }
    // ---- banner
    if (t < g_banner_until && g_banner[0]) {
        CGFloat al = MIN(1, (g_banner_until - t) / 0.4);
        NSDictionary *a = attrs(26 * scale, NSFontWeightBold, NSColor.whiteColor, al);
        NSString *s = @(g_banner); NSSize sz = [s sizeWithAttributes:a];
        NSRect r = NSMakeRect((b.size.width - sz.width) / 2 - 20 * scale, b.size.height * 0.28, sz.width + 40 * scale, sz.height + 16 * scale);
        fill_round(r, 10, [NSColor colorWithWhite:0 alpha:0.45 * al]);
        [s drawAtPoint:NSMakePoint(r.origin.x + 20 * scale, r.origin.y + 8 * scale) withAttributes:a];
    }
    os_unfair_lock_unlock(&L);
}
@end

// ---------------------------------------------------------------- the text box (chat, or the lobby's rename)
@interface MPFieldDelegate : NSObject <NSTextFieldDelegate> @end
static void close_box(int send);
@implementation MPFieldDelegate
- (BOOL)control:(NSControl *)c textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)c; (void)tv;
    if (sel == @selector(insertNewline:)) { close_box(1); return YES; }
    if (sel == @selector(cancelOperation:)) { close_box(0); return YES; }
    return NO;
}
@end
static MPFieldDelegate *g_delegate;

static void layout_field(void) {
    NSRect b = g_overlay.superview.bounds; CGFloat scale = MAX(0.8, MIN(1.6, b.size.height / 900.0));
    CGFloat h = 26 * scale, w = MIN(560 * scale, b.size.width * 0.45);
    if (g_rename) g_field.frame = NSMakeRect(b.size.width - 300 * scale - 64 * scale, b.size.height * 0.70 - 196 * scale - 40 * scale, 300 * scale, h);   // over the player card
    else g_field.frame = NSMakeRect(12, 14, w, h);   // the content view is not flipped: y from the bottom
    g_field.font = [NSFont systemFontOfSize:13.5 * scale];
}
static void open_box(NSString *initial, int rename) {
    if (g_editing || !g_field) return;
    g_editing = 1; g_rename = rename;
    if (g_release) g_release();   // the game's buttons let go: Link stands still while you type
    g_field.placeholderString = rename ? @"Your name (Return to save, Esc to cancel)" : @"Say something (Return to send, Esc to cancel, /help)";
    layout_field();
    g_field.stringValue = initial ?: @""; g_field.hidden = NO;
    [g_win makeFirstResponder:g_field];
    NSText *ed = [g_win fieldEditor:YES forObject:g_field]; ed.selectedRange = rename ? NSMakeRange(0, g_field.stringValue.length) : NSMakeRange(g_field.stringValue.length, 0);
    g_overlay.needsDisplay = YES;
}
static void close_box(int send) {
    if (!g_editing) return;
    NSString *s = [g_field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (send && s.length) { if (g_rename) { if (mp_ui_on_rename) mp_ui_on_rename(s.UTF8String); } else if (mp_ui_on_send) mp_ui_on_send(s.UTF8String); }
    g_field.stringValue = @""; g_field.hidden = YES; g_editing = 0; g_rename = 0;
    [g_win makeFirstResponder:g_win.contentView];
    if (g_resume) g_resume();
    g_overlay.needsDisplay = YES;
}
void mp_ui_open_rename(const char *current) {   // any thread: the main thread's timer opens it
    os_unfair_lock_lock(&L); copy(g_rename_req, sizeof g_rename_req, current); g_rename_pending = 1; os_unfair_lock_unlock(&L);
}
int mp_ui_key(NSEvent *e) {
    if (!g_field || g_editing || e.type != NSEventTypeKeyDown || e.isARepeat) return 0;
    if (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) return 0;
    switch (e.keyCode) {
    case 17: case 76: open_box(@"", 0); return 1;   // T, keypad Enter (Return is the game's +: nxnative's key map, front_mac.m key_bit)
    case 44: open_box(@"/", 0); return 1;           // /
    }
    return 0;
}
void mp_ui_attach(NSWindow *win, void (*release_input)(void), void (*resume_input)(void)) {
    g_win = win; g_release = release_input; g_resume = resume_input;
    load_game_fonts();
    NSView *cv = win.contentView;
    g_overlay = [[MPOverlay alloc] initWithFrame:cv.bounds];
    g_overlay.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    g_overlay.wantsLayer = YES; g_overlay.layer.backgroundColor = NSColor.clearColor.CGColor;
    [cv addSubview:g_overlay];
    g_field = [NSTextField textFieldWithString:@""];
    g_field.bezelStyle = NSTextFieldRoundedBezel; g_field.focusRingType = NSFocusRingTypeNone; g_field.hidden = YES;
    g_field.wantsLayer = YES; g_field.layer.cornerRadius = 7; g_field.layer.masksToBounds = YES;
    g_field.backgroundColor = [NSColor colorWithWhite:0.08 alpha:0.78]; g_field.textColor = NSColor.whiteColor; g_field.drawsBackground = YES;
    g_delegate = [MPFieldDelegate new]; g_field.delegate = g_delegate;
    [cv addSubview:g_field];
    layout_field();
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 20 repeats:YES block:^(NSTimer *tm) {
        (void)tm;
        char want[25] = ""; int open = 0;
        os_unfair_lock_lock(&L); if (g_rename_pending) { g_rename_pending = 0; open = 1; memcpy(want, g_rename_req, sizeof want); } os_unfair_lock_unlock(&L);
        if (open) open_box(@(want), 1);
        g_overlay.needsDisplay = YES;
    }];
    [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidResizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; layout_field(); }];
}
