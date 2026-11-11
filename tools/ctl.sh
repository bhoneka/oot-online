#!/bin/zsh
# Drive a running test: ctl.sh <name> <line ...>   (a press as in DM_KEYS: A, LUp~3, A~0.2; or "snap <label>" = a shot,
# waited for, path printed; or "peek <addr> <n>")
NAME=${1:?name}; shift; R=$HOME/oot-online/runs/$NAME
if [[ $1 == snap ]]; then
    f=$R/shots/$(date +%H%M%S)-${2:-shot}.png; echo "shot $f" >> $R/ctl
    for i in {1..100}; do [[ -s $f ]] && { sleep 0.3; python3 -I ${0:A:h}/trim_shot.py $f >/dev/null 2>&1; echo $f; exit 0; }; sleep 0.2; done   # trimmed to the game's picture (trim_shot.py)
    echo "no shot (game not presenting?)"; exit 1
fi
echo "$*" >> $R/ctl
