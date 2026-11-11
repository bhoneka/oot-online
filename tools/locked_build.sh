#!/bin/zsh
# Rebuild the game app (~/oot-native/tools/build.sh, its arguments passed on) while holding the shared game-run lock
# (~/oot-native/.game-run.lock, the same protocol as oot_run.sh and the open-world session's gamelock.sh): no game test
# starts during the build, and the open-world runner waits for its turn instead of refusing.
#   zsh locked_build.sh --seen-shaders
setopt no_bg_nice
LOCK=$HOME/oot-native/.game-run.lock
build_main() {
    while ! mkdir $LOCK 2>/dev/null; do
        owner=$(cat $LOCK/owner 2>/dev/null); opid=${owner%% *}
        if [[ -n $opid ]] && ! kill -0 $opid 2>/dev/null; then rm -rf $LOCK; continue; fi
        echo "waiting for the game lock (held: $owner)"; sleep 2
    done
    echo "$$ oot-online build $(date +%H:%M:%S)" > $LOCK/owner
    trap 'rm -rf $LOCK' EXIT
    if pgrep -x dmhost >/dev/null; then echo "a game host is running: $(pgrep -x dmhost | tr '\n' ' ')"; return 2; fi
    NICE=${NICE:-0} zsh $HOME/oot-native/tools/build.sh "$@"
}
build_main "$@"; exit $?
