// A sheet of a font's private-use glyphs (U+E000..U+E0FF) with their codes, plus sample lines in the game's text fonts.
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *dir = @(argv[1]);
        for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil])
            CTFontManagerRegisterFontsForURL((__bridge CFURLRef)[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:f]], kCTFontManagerScopeProcess, NULL);
        NSImage *im = [NSImage imageWithSize:NSMakeSize(1400, 1000) flipped:YES drawingHandler:^BOOL(NSRect r) {
            [[NSColor colorWithWhite:0.12 alpha:1] setFill]; NSRectFill(r);
            NSFont *ext = [NSFont fontWithName:@"External" size:30];
            NSDictionary *g = @{ NSFontAttributeName: ext ?: [NSFont systemFontOfSize:30], NSForegroundColorAttributeName: NSColor.whiteColor };
            NSDictionary *lab = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular], NSForegroundColorAttributeName: [NSColor colorWithWhite:0.7 alpha:1] };
            int k = 0;
            for (unichar c = 0xE000; c < 0xE100; c++) {
                CGGlyph gl; if (!CTFontGetGlyphsForCharacters((__bridge CTFontRef)ext, &c, &gl, 1)) continue;
                CGFloat x = 20 + (k % 16) * 84, y = 20 + (k / 16) * 64; k++;
                [[NSString stringWithCharacters:&c length:1] drawAtPoint:NSMakePoint(x, y) withAttributes:g];
                [[NSString stringWithFormat:@"%04X", c] drawAtPoint:NSMakePoint(x, y + 40) withAttributes:lab];
            }
            CGFloat y = 20 + ((k + 15) / 16) * 64 + 20;
            NSArray *samples = @[ @[@"CetraDisplayPro-Medium", @"OoT ONLINE · Hyrule Field", @46], @[@"CetraDisplayPro-Regular", @"The Legend of Zelda: Ocarina of Time", @34],
                                  @[@"ClarimoUDPE-DemiBold", @"OUTFIT  Goron Tunic  ·  PLAY  ·  37 players online", @30], @[@"ClarimoUDPE-Regular", @"Saria: anyone up for a duel at the ranch?", @28],
                                  @[@"NinDevNumber-Lining", @"0123456789 12:34", @34], @[@"Nin-ZeldaGlyphsv3-Deco", @"ABCDEFGHIJ abcdefghij 0123456789", @30], @[@"Symbol-Regular", @"A … ← ↑ → ↓ ♪", @30] ];
            for (NSArray *s in samples) {
                NSFont *f = [NSFont fontWithName:s[0] size:[s[2] doubleValue]];
                [[NSString stringWithFormat:@"%@%@", f ? @"" : @"(missing) ", s[1]] drawAtPoint:NSMakePoint(20, y) withAttributes:@{ NSFontAttributeName: f ?: [NSFont systemFontOfSize:20], NSForegroundColorAttributeName: NSColor.whiteColor }];
                [s[0] drawAtPoint:NSMakePoint(1000, y + 8) withAttributes:lab];
                y += [s[2] doubleValue] + 22;
            }
            return YES; }];
        CGImageRef cg = [im CGImageForProposedRect:NULL context:nil hints:nil];
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2]) atomically:YES];
    }
    return 0;
}
