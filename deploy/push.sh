#!/bin/zsh
# Copy the OoT Online server to your server box and (re)start it:
#   zsh deploy/push.sh root@203.0.113.7
# Sends server/ (code only: nothing from the game is on the server) and deploy/, runs setup-vps.sh the first time,
# restarts the service after. The live server's data (stats, spawn points, bans) is never overwritten by the Mac's copy.
set -e
HOST=${1:?usage: zsh deploy/push.sh user@host}
ROOT=${0:A:h:h}
SSH=(ssh -o StrictHostKeyChecking=accept-new)
if [[ -n ${OOT_SSH_KEY:-} ]]; then SSH+=(-i "$OOT_SSH_KEY"); fi   # OOT_SSH_KEY: a key of its own for the box (an if: set -e)
"${SSH[@]}" "$HOST" "mkdir -p /opt/oot-online"
rsync -az --delete -e "${SSH[*]}" --exclude node_modules --exclude test --exclude data "$ROOT/server/" "$HOST:/opt/oot-online/server/"
rsync -az --delete -e "${SSH[*]}" "$ROOT/deploy/" "$HOST:/opt/oot-online/deploy/"
"${SSH[@]}" "$HOST" 'if [ ! -f /etc/systemd/system/oot-online.service ]; then sh /opt/oot-online/deploy/setup-vps.sh;
  else install -m 644 /opt/oot-online/deploy/oot-online.service /etc/systemd/system/ && systemctl daemon-reload &&
  chown -R ootonline:ootonline /opt/oot-online && systemctl restart oot-online && echo "restarted: $(systemctl is-active oot-online)"; fi'
