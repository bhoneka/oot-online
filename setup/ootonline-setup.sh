#!/bin/zsh
# OoT Online setup for developers (Xcode and Homebrew): turns YOUR OWN Ocarina of Time (Switch 2) dump into a Mac app with OoT Online inside.
# Players use the installer app instead (installer/, setup/install.py), which needs neither.
#   zsh ootonline-setup.sh "/path/to/your game.nup" [server address]
# What it does, in order (each step says what it's doing; nothing is uploaded anywhere):
#   1. checks this Mac (Apple chip, macOS 26.6+, Xcode with its Metal compiler, Homebrew, Python 3.12+)
#   2. gets nxnative (the open-source tool that runs the game natively, MIT) at the version OoT Online was tested with
#   3. adds OoT Online to it (the network client, the lobby, the chat)
#   4. builds the game from your dump (the first time takes 1-3 hours: it compiles about 43,000 shaders)
#   5. takes the game's own fonts out of your dump for OoT Online's screens, makes the other players' stand-ins from it, sets the server
# Everything from the game stays on this Mac. You need about 60 GB free.
set -e
NUP=${1:?usage: zsh ootonline-setup.sh "/path/to/your game.nup" [server]}
SERVER=${2:-${OOT_ONLINE_SERVER:-}}
HERE=${0:A:h}; ROOT=${HERE:h}
BASE="$HOME/Library/Application Support/OoT Online"
NX="$BASE/nxnative"; WORK="$BASE/build"; APP="$HOME/Applications/Ocarina of Time Online.app"
DATA="$HOME/Library/Application Support/nxnative/0400486024DEC000"
say() { print -P "%F{green}==>%f $*"; }
fail() { print -P "%F{red}✗%f $*"; exit 1; }

say "1/5 Checking this Mac"
[[ $(uname -m) == arm64 ]] || fail "this needs a Mac with an Apple chip (M1 or later)"
ver=$(sw_vers -productVersion); [[ ${ver%%.*} -ge 27 || ( ${ver%%.*} -eq 26 && ${${ver#*.}%%.*} -ge 6 ) ]] || fail "macOS 26.6 or later is needed (this Mac has $ver)"
[[ -f "$NUP" ]] || fail "can't find your game file: $NUP"
xcrun -f metal >/dev/null 2>&1 || fail "Xcode's Metal compiler is missing: install Xcode from the App Store, then run: xcodebuild -downloadComponent MetalToolchain"
command -v brew >/dev/null || fail "Homebrew is missing: see https://brew.sh"
for p in xcodegen zstd glslang; do brew list --versions $p >/dev/null || { say "installing $p (Homebrew)"; brew install $p; }; done
PY=$(command -v python3.14 || command -v python3.13 || command -v python3.12 || command -v python3 || true)
if [[ -z $PY ]] || ! $PY -c 'import sys; sys.exit(sys.version_info < (3, 12))' 2>/dev/null; then   # macOS's own python3 is older: Homebrew's
    say "installing Python 3.13 (Homebrew)"; brew list --versions python@3.13 >/dev/null || brew install python@3.13
    PY="$(brew --prefix)/bin/python3.13"
fi
$PY -c 'import sys; sys.exit(sys.version_info < (3, 12))' || fail "Python 3.12 or newer is needed (brew install python@3.13)"
free=$(df -g "$HOME" | awk 'NR==2 {print $4}'); (( free >= 60 )) || say "warning: only ${free} GB free; the first build needs about 60 GB"

say "2/5 Getting nxnative (the tested version)"
mkdir -p "$BASE"
COMMIT=$(cat "$ROOT/patches/NXNATIVE_COMMIT")
if [[ ! -d "$NX/.git" ]]; then git clone --filter=blob:none https://github.com/seedds/nxnative.git "$NX"; fi
git -C "$NX" fetch --quiet origin || true
git -C "$NX" checkout --quiet -B oot-online "$COMMIT" 2>/dev/null || git -C "$NX" checkout --quiet -B oot-online origin/main
git -C "$NX" checkout --quiet -- . 2>/dev/null || true

say "3/5 Adding OoT Online to it"
for patch in nxnative-mac26-translator.patch nxnative-xcode27.1-blend-ref.patch nxnative-frame-step-gpu-span.patch nxnative-no-xcode.patch; do   # macOS 26 (the translator), Xcode 27.1 (a shader helper) and frame-step (60 fps held: the GPU cost read from its span) fixes, and the no-Xcode build the installer uses (this setup builds with Xcode: the same tree)
    git -C "$NX" apply --check "$ROOT/patches/$patch" 2>/dev/null && git -C "$NX" apply "$ROOT/patches/$patch" || say "($patch is already there)"
done
$PY -I "$ROOT/tools/integrate_nxnative.py" "$NX"
[[ -d "$BASE/venv" ]] || $PY -m venv "$BASE/venv"
"$BASE/venv/bin/pip" install -q lz4 "capstone>=5,<6" fonttools zbic zstandard
METAL_FLAG=()
[[ $(xcrun metal --version 2>/dev/null | head -1) == *"32023.922"* ]] || METAL_FLAG=(--allow-other-metal)   # nxnative tested Xcode 27.2's compiler

say "4/5 Building your game (the first time: 1-3 hours; a log is in $BASE/build.log)"
mkdir -p "$HOME/Applications"
# OoT Online's switches go into this app's own env file (Contents/Resources/dm/env.txt): the plain game app, which reads the same
# per-game env.txt in Application Support, stays the plain game. Online play runs on a copy of the save (hle_mp.c use_online_save)
ONLINE_ENV="$BASE/online_env.txt"
{ echo "DM_ROMFS_OVERLAY=$DATA/oot_online/romfs"; echo "DM_MP_STANDIN=OnlinePlayer"; echo "DM_MP_LIVE=1"; if [[ -n "$SERVER" ]]; then echo "DM_MP_SERVER=$SERVER"; fi; } > "$ONLINE_ENV"   # (an if, not &&: set -e would stop the setup on an empty server)
( cd "$NX" && PATH="$BASE/venv/bin:$PATH" python scripts/build_ipa_from_nsp.py "$NUP" --platform mac-app --work-dir "$WORK" -o "$APP" --env-file "$ONLINE_ENV" $METAL_FLAG --progress lines ) 2>&1 | tee "$BASE/build.log" | grep --line-buffered -E "^\[.*\] (\[[0-9]/[0-9]\]|✓|\[RED\])"
[[ -d "$APP" ]] || fail "the build did not finish: see $BASE/build.log"

say "5/5 The game's fonts for OoT Online's screens, the other players' stand-ins, and the server"
mkdir -p "$DATA/oot_online/fonts"
romfs_list=("$WORK"/extract/v<->/RomFS.img(Nn)); ROMFS=${romfs_list[-1]:-}   # the newest version the build unpacked (the one the app runs; n = numeric order)
[[ -f "$ROMFS" ]] || fail "the build left no RomFS image in $WORK/extract"
"$BASE/venv/bin/python" -I "$ROOT/tools/game_fonts.py" "$ROMFS" "$DATA/oot_online/fonts" Clarimo Cetra External NinDev Symbol ZeldaGlyphs >/dev/null
"$BASE/venv/bin/python" -I "$ROOT/tools/make_online_mod.py" "$ROMFS" "$DATA/oot_online/romfs" >/dev/null
if [[ -f "$DATA/env.txt" ]]; then   # an earlier version of this setup put OoT Online's switches in the shared env.txt: taken out again
    grep -v -E '^(DM_MP_SERVER|DM_ROMFS_OVERLAY|DM_MP_STANDIN|DM_MP_LIVE)=' "$DATA/env.txt" > "$DATA/env.txt.new" || true; mv "$DATA/env.txt.new" "$DATA/env.txt"
fi
say "Done: open \"$APP\" and load your save. The lobby comes up in the game's own menu: Play takes you to Hyrule Field; T opens the chat."
say "Your save is safe: online play runs on a copy of it, made fresh at every start."
[[ -n "$SERVER" ]] || say "(no server given: add one with  zsh $0 \"$NUP\" <server address>)"
