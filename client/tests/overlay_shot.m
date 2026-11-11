// Draws the overlay with sample content into a PNG, off screen (no window is shown).  overlay_shot <out.png> [background.png]
#import <AppKit/AppKit.h>
#include "../mp_ui.h"
#include "../mp_net.h"
int main(int argc, char **argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSRect r = NSMakeRect(-20000, -20000, 1600, 900);
        NSWindow *w = [[NSWindow alloc] initWithContentRect:r styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        NSImageView *bg = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, 1600, 900)];
        if (argc > 2 && *argv[2]) { bg.image = [[NSImage alloc] initWithContentsOfFile:@(argv[2])]; bg.imageScaling = NSImageScaleAxesIndependently; }
        else {   // a sky-over-field gradient as a stand-in for the game's picture
            NSImage *im = [NSImage imageWithSize:NSMakeSize(1600, 900) flipped:NO drawingHandler:^BOOL(NSRect dst) {
                [[[NSGradient alloc] initWithStartingColor:[NSColor colorWithSRGBRed:0.33 green:0.52 blue:0.24 alpha:1] endingColor:[NSColor colorWithSRGBRed:0.62 green:0.78 blue:0.93 alpha:1]] drawInRect:dst angle:90];
                return YES; }];
            bg.image = im; bg.imageScaling = NSImageScaleAxesIndependently;
        }
        w.contentView = bg;
        setenv("DM_MP_FONTS", argc > 3 ? argv[3] : "", 1);
        mp_ui_attach(w, NULL, NULL);
        mp_ui_status(MP_ONLINE, 37, "", 24);
        mp_ui_chat(MP_UI_SYSTEM, "", 0, "Welcome to OoT Online. Be nice. /help for commands.");
        mp_ui_chat(MP_UI_SYSTEM, "", 0, "Saria joined");
        mp_ui_chat(MP_UI_CHAT, "Saria", 0, "anyone up for a duel at the ranch?");
        mp_ui_chat(MP_UI_CHAT, "Darunia", 1, "brother, I am already here");
        mp_ui_chat(MP_UI_WHISPER, "Ruto", 2, "watch out, Mido is camping the bridge");
        mp_ui_chat(MP_UI_CHAT, "Mido", 0, "no I'm not");
        mp_ui_feed("Saria", "Mido"); mp_ui_feed("Darunia", "Talon"); mp_ui_feed("", "Ingo");
        mp_ui_hitmarker(0);
        mp_ui_banner("Shot by Saria · back in 3", 5);
        mp_ui_tag tags[3] = { { 0.42f, 0.50f, 1, "Saria", 0, 1.0f }, { 0.58f, 0.47f, 1, "Darunia", 1, 0.5f }, { 0.70f, 0.55f, 1, "Ruto", 2, 0.25f } };
        mp_ui_tags(tags, 3);
        if (getenv("LOBBY")) {
            mp_ui_lobby_t lb = { .shown = 1, .outfit = atoi(getenv("LOBBY")), .can_play = 1, .kills = 12, .deaths = 7, .best = 4 };
            snprintf(lb.name, sizeof lb.name, "Hero"); snprintf(lb.where, sizeof lb.where, "Hyrule Field");
            mp_ui_lobby(&lb); mp_ui_banner("", 0);
            [NSThread sleepForTimeInterval:0.4];   // past the lobby's fade-in
        }
        NSView *v = w.contentView; [v layoutSubtreeIfNeeded];
        for (NSView *s in v.subviews) [s displayIfNeeded];
        NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
        [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[1]) atomically:YES];
    }
    return 0;
}
