#!/bin/zsh
# OoT Online's Terminal setup, for developers (it needs Xcode and Homebrew). Players: the installer app, no Terminal
# (https://github.com/bhoneka/oot-online/releases/latest/download/Install-OoT-Online.zip). In Terminal:
#   curl -fsSL https://bhoneka.github.io/oot-online/install.sh | zsh
# It asks for YOUR OWN game file (a .nup of The Legend of Zelda: Ocarina of Time for Nintendo Switch 2), gets OoT Online's
# code from GitHub, and builds "Ocarina of Time Online" into ~/Applications from your file (one to three hours the first
# time; minutes when run again). Nothing from the game is downloaded or uploaded: the game is built from your file, here.
#   OOT_ONLINE_SERVER=<address:port>  another server than the public one;  a game file path as the first argument skips the dialog
main() {
    setopt local_options err_return
    local SITE=https://bhoneka.github.io/oot-online REPO=https://github.com/bhoneka/oot-online
    local BASE="$HOME/Library/Application Support/OoT Online" NUP=${1:-} SERVER=${OOT_ONLINE_SERVER:-}
    local SRC="$BASE/src"
    say() { print -P "%F{green}==>%f $*"; }
    fail() { print -P "%F{red}✗%f $*"; return 1; }

    print -P "%BOoT Online%b: Ocarina of Time, together."
    [[ $(uname -m) == arm64 ]] || { fail "OoT Online needs a Mac with an Apple chip (M1 or later)."; return 1; }
    command -v git >/dev/null || { fail "git is missing: install Xcode from the App Store first, then run this again."; return 1; }

    if [[ -z $NUP ]]; then
        say "Choose your game file: your own dump of Ocarina of Time for Switch 2 (a .nup)"
        NUP=$(osascript -e 'POSIX path of (choose file with prompt "Choose your Ocarina of Time game file (.nup)")' 2>/dev/null) \
            || { fail "no game file chosen"; return 1; }
    fi
    [[ -f $NUP ]] || { fail "can't find the game file: $NUP"; return 1; }

    if [[ -z $SERVER ]]; then   # the public server's address, kept on the website so it can move without a new installer
        SERVER=$(curl -fsSL "$SITE/server.txt" 2>/dev/null | head -1 | tr -d '[:space:]')
    fi
    if [[ -n $SERVER ]]; then say "server: $SERVER"; else say "no public server is up yet: the app is built anyway; run this again once there is one"; fi

    say "Getting OoT Online"
    mkdir -p "$BASE"
    if [[ -d "$SRC/.git" ]]; then git -C "$SRC" pull --ff-only --quiet
    else git clone --depth 1 --quiet "$REPO.git" "$SRC"; fi

    say "Building your copy (this keeps the Mac awake; the first time takes one to three hours)"
    caffeinate -i zsh "$SRC/setup/ootonline-setup.sh" "$NUP" $SERVER
}
main "$@"
