#!/bin/zsh
# One test run of OoT (headless, silent, its own save, never the player's) through nxnative's host with OoT Online in it.
#   zsh oot_run.sh <name> [seconds=900] [KEY=VALUE ...]      e.g.  zsh oot_run.sh probe1 900 DM_MP_PROBE=1
# Its folder ~/oot-online/runs/<name>/: log, ctl (append lines to drive the game: tools/ctl.sh), shots/, save/ (a copy of
# runs/_save_ref when there is one, else empty: a new game). A watchdog ends the run after <seconds>, or at once when wired
# memory passes 24 GB or swap 16 GB (the guard nxnative's own OoT work folder uses, scaled to this 64 GB Mac).
set -u
# All of it in one function, called on the last line: zsh reads a script as it runs it, so an edit to this file during a run
# (as happened once) made the running copy read garbage at its end. A function is parsed whole before it starts.
run_main() {
    setopt local_options no_bg_nice   # zsh starts background jobs at nice +5 by default (BG_NICE): the game ran at nice 5 to 10 and mostly on the slower cores
    NAME=${1:?usage: oot_run.sh <name> [seconds] [KEY=VALUE ...]}; SECS=${2:-900}; shift; (( $# )) && shift
    # OOT_WORK=<a build's work folder> OOT_VER=<v65536>: another build of the game (its own host, patched modules, RomFS and shader
    # library, from a --platform mac build into that folder); the defaults are the app's build of the update v131072
    W=${OOT_WORK:-$HOME/oot-native/0400486024DEC000_build}; VER=${OOT_VER:-v131072}; R=$HOME/oot-online/runs/$NAME
    mkdir -p $R/shots $R/temp
    if [[ ! -d $R/save ]]; then   # OOT_NEWSAVE=1: an empty save (a new game) instead of the reference save
        if [[ -d $HOME/oot-online/runs/_save_ref && -z ${OOT_NEWSAVE:-} ]]; then cp -R $HOME/oot-online/runs/_save_ref $R/save; else mkdir -p $R/save; fi
    fi
    : > $R/ctl
    # Two GPU-heavy games at once have crashed this Mac. Every session that runs the game takes the shared lock first:
    # ~/oot-native/.game-run.lock (mkdir is atomic; "owner" holds pid, session, run, time; a dead owner's lock is cleared)
    LOCK=$HOME/oot-native/.game-run.lock
    while ! mkdir $LOCK 2>/dev/null; do
        owner=$(cat $LOCK/owner 2>/dev/null); opid=${owner%% *}
        if [[ -n $opid ]] && ! kill -0 $opid 2>/dev/null; then rm -rf $LOCK; continue; fi
        # our own reservation (a placeholder sleep that kept the lock for this run while the build finished): take it over
        if [[ $owner == *"oot-online reserve"* ]]; then kill $opid 2>/dev/null; echo "$$ oot-online $NAME $(date +%H:%M:%S)" > $LOCK/owner; break; fi
        echo "waiting for the game lock (held: $owner)"; sleep 2
    done
    echo "$$ oot-online $NAME $(date +%H:%M:%S)" > $LOCK/owner
    trap 'rm -rf $LOCK' EXIT
    if pgrep -x dmhost >/dev/null; then echo "another game host is running: $(pgrep -x dmhost | tr '\n' ' ')"; exit 2; fi
    cd $R
    # The shader library the app ships (b-<chain>.img in its Resources/msl: a --seen-shaders build packs its partial library into one
    # image and leaves shader/cache's folder empty, so pointing at shader/cache drew nothing); shader/cache only without an app
    APP="$HOME/oot-native/Ocarina of Time.app"; MSL=$W/shader/cache
    [[ -z ${OOT_WORK:-} ]] && ls "$APP"/Contents/Resources/msl/b-*.img >/dev/null 2>&1 && MSL="$APP/Contents/Resources/msl"
    [[ -n ${OOT_MSL:-} ]] && MSL=$OOT_MSL   # a library folder of another build
    echo "shader library: $MSL"; echo "host: ${OOT_HOST:-$W/tools/dmhost}"   # OOT_HOST=<a test build of the host> to try a changed module
    # The pipelines the host builds as it meets new draws (pso_user.tsv + pso_archive/): kept in one folder for all runs, as the app keeps
    # them between plays. Each run had its own (beside its save), so every run built them all again and drew with the slow stand-in
    # pipelines meanwhile: ~23 fps in the world (GPU ~35 ms a frame), not the game's speed. OOT_PSO_COLD=1: a run's own, empty one
    PSO=$HOME/oot-online/runs/_pso; mkdir -p $PSO
    [[ -n ${OOT_PSO_COLD:-} ]] && PSO=$R
    echo "pipeline cache: $PSO"
    env DM_ROOT=$R DM_ROMFS=$W/extract/$VER/RomFS.img DM_SAVE=$R/save DM_TEMP=$R/temp DM_MSL_DIR="$MSL" DM_FONT_DIR=$W/font DM_PSO_USER=$PSO/pso_user.tsv \
        DM_NVN_SHMAP=${OOT_SHMAP:-$HOME/nxnative/dm/games/0400486024DEC000/shader_map.tsv} DM_PSO_BUNDLE=$W/pso/mac DM_LANG=en-US DM_CTL=$R/ctl DM_FOCUS_KEEP=1 "$@" \
        ${OOT_HOST:-$W/tools/dmhost} $W/patched_$VER > $R/log 2>&1 &
    PID=$!; echo $PID > $R/pid; echo "run $NAME: host pid $PID, log $R/log"
    T0=$(date +%s)
    while kill -0 $PID 2>/dev/null; do
        sleep 2
        (( $(date +%s) - T0 > SECS )) && { echo "time limit ($SECS s)"; break; }
        wired=$(vm_stat | awk '/Pages wired down/ {gsub("\\.",""); print $4 * 16384 / 1073741824}')
        swap=$(sysctl -n vm.swapusage | awk '{gsub("M",""); print $5 / 1024}')
        if (( ${wired%.*} > 24 || ${swap%.*} > 16 )); then echo "memory guard: wired ${wired} GB, swap ${swap} GB"; break; fi
        [[ -f $R/stop ]] && { echo "stop requested"; rm -f $R/stop; break; }
    done
    if kill -0 $PID 2>/dev/null; then
        [[ $(ps -o comm= -p $PID) == *dmhost* ]] && kill -TERM $PID; sleep 3; kill -0 $PID 2>/dev/null && kill -KILL $PID
    fi
    echo "run $NAME ended after $(( $(date +%s) - T0 )) s"; rm -f $R/pid
}
run_main "$@"; exit $?
