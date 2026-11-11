#!/bin/zsh
# Run any heavy job (a build, the setup) while holding the shared game-run lock (~/oot-native/.game-run.lock, the protocol of
# oot_run.sh, locked_build.sh and the open-world session's runner): no game test starts while it runs, and it waits for the
# test that holds the lock to end.
#   zsh locked_job.sh <label> <command> [args...]       e.g.  zsh locked_job.sh setup zsh setup/ootonline-setup.sh "<nup>"
setopt no_bg_nice
LOCK=$HOME/oot-native/.game-run.lock
job_main() {
    local label=${1:?usage: locked_job.sh <label> <command> [args...]}; shift
    while ! mkdir $LOCK 2>/dev/null; do
        owner=$(cat $LOCK/owner 2>/dev/null); opid=${owner%% *}
        if [[ -n $opid ]] && ! kill -0 $opid 2>/dev/null; then rm -rf $LOCK; continue; fi
        echo "waiting for the game lock (held: $owner)"; sleep 2
    done
    echo "$$ oot-online $label $(date +%H:%M:%S)" > $LOCK/owner
    trap 'rm -rf $LOCK' EXIT
    if pgrep -x dmhost >/dev/null; then echo "a game host is running: $(pgrep -x dmhost | tr '\n' ' ')"; return 2; fi
    "$@"
}
job_main "$@"; exit $?
