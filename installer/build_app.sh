#!/bin/zsh
# Builds "Install OoT Online.app": the installer's window (InstallOoTOnline.swift) with its payload in Contents/Resources/payload
# (python/, prebuilt/, oot-online/, licenses/: .github/workflows/installer.yml puts it together). Needs Xcode (GitHub Actions has it);
# the player's Mac needs none of it.
#   zsh installer/build_app.sh <payload folder> <output folder> <version>
set -e
HERE=${0:A:h}; PAYLOAD=${1:?usage: build_app.sh <payload> <out> <version>}; OUT=${2:?}; VERSION=${3:-0.0.0}
PAYLOAD=${PAYLOAD:A}; mkdir -p $OUT; OUT=${OUT:A}
APP="$OUT/Install OoT Online.app"; EXE="Install OoT Online"
T=$(mktemp -d); trap 'rm -rf $T' EXIT
rm -rf $APP; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
SDK=$(xcrun --sdk macosx --show-sdk-path)
xcrun swiftc -O -parse-as-library -swift-version 5 -target arm64-apple-macos26.6 -sdk $SDK -module-cache-path $T/mc \
    $HERE/InstallOoTOnline.swift -o "$APP/Contents/MacOS/$EXE"
# the icon, every size an .icns holds
xcrun swift -module-cache-path $T/mc $HERE/make_icon.swift $T/icon.png
mkdir $T/AppIcon.iconset
for n in 16 32 128 256 512; do
    sips -z $n $n $T/icon.png --out $T/AppIcon.iconset/icon_${n}x${n}.png >/dev/null
    sips -z $((n * 2)) $((n * 2)) $T/icon.png --out $T/AppIcon.iconset/icon_${n}x${n}@2x.png >/dev/null
done
iconutil -c icns $T/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"
# the payload, and its id (what the app copies it out under, once)
ditto --noextattr "$PAYLOAD" "$APP/Contents/Resources/payload"
find "$APP/Contents/Resources/payload" \( -name __pycache__ -o -name .DS_Store \) -prune -exec rm -rf {} +
PID=$(cd "$APP/Contents/Resources/payload" && find . -type f -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256 | cut -c1-16)
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>$EXE</string>
    <key>CFBundleIdentifier</key><string>io.github.bhoneka.oot-online.installer</string>
    <key>CFBundleName</key><string>Install OoT Online</string>
    <key>CFBundleDisplayName</key><string>Install OoT Online</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>LSMinimumSystemVersion</key><string>26.6</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSHumanReadableCopyright</key><string>A fan project, not affiliated with Nintendo. It contains nothing from the game.</string>
    <key>OOTPayloadID</key><string>$VERSION-$PID</string>
</dict>
</plist>
EOF
plutil -lint "$APP/Contents/Info.plist" >/dev/null
codesign --force --sign - "$APP"
codesign --verify --strict "$APP"
echo "built: $APP ($(du -sh "$APP" | cut -f1), payload $VERSION-$PID)"
