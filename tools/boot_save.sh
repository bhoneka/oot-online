#!/bin/zsh
# Load the run's save from the title the way a player does: three presses of A (Continue, the save, Load), each after the screen
# it answers has had time to come up, and nothing after them (a later press would be taken by the lobby window that opens once
# Link is in the world). Waits for Link (the module's probe line: the run needs DM_MP_PROBE=1). For runs of tools/oot_run.sh.
#   zsh boot_save.sh <run name> [max seconds = 300]
NAME=${1:?run name}; MAX=${2:-300}; R=$HOME/oot-online/runs/$NAME; HERE=${0:A:h}
T0=$(date +%s)
elapsed() { echo $(( $(date +%s) - T0 )); }
while ! grep -q "event requested: Title_Background" $R/log 2>/dev/null; do
    (( $(elapsed) > MAX )) && { echo "no title screen after $(elapsed) s"; exit 1; }
    [[ -f $R/pid ]] || { echo "the run ended"; exit 2; }
    sleep 1
done
sleep 10;  zsh $HERE/ctl.sh $NAME A      # Continue (the title takes input once its fade-in is over)
sleep 6;   zsh $HERE/ctl.sh $NAME A      # the save
sleep 6;   zsh $HERE/ctl.sh $NAME A      # Load (a busy Mac shows the Load dialog late: 3.5 s was too soon once)
for j in {1..40}; do grep -q "mp-oot: probe:" $R/log && break; sleep 1; done
grep -q "mp-oot: probe:" $R/log || zsh $HERE/ctl.sh $NAME A   # still on the Load dialog: once more
for j in {1..60}; do grep -q "mp-oot: probe:" $R/log && break; sleep 1; done
grep -q "mp-oot: probe:" $R/log || { echo "no Link after $(elapsed) s"; zsh $HERE/ctl.sh $NAME snap boot-failed; exit 1; }
echo "Link is in the world after $(elapsed) s"
