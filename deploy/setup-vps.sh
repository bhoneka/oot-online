#!/bin/sh
# One-time setup on a fresh Ubuntu 22.04/24.04 or Debian 12 server, run as root (push.sh runs it for you):
# Node, an "ootonline" user, the service, the firewall (SSH, the game's UDP port and the status page only).
set -e
if ! command -v node >/dev/null 2>&1 || [ "$(node -p 'process.versions.node.split(".")[0]')" -lt 20 ]; then
  apt-get update -qq
  apt-get install -y -qq ca-certificates curl gnupg
  curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
  apt-get install -y -qq nodejs
fi
id ootonline >/dev/null 2>&1 || useradd --system --home /opt/oot-online --shell /usr/sbin/nologin ootonline
mkdir -p /opt/oot-online/server/data
chown -R ootonline:ootonline /opt/oot-online
install -m 644 /opt/oot-online/deploy/oot-online.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now oot-online.service
if command -v ufw >/dev/null 2>&1; then
  ufw allow OpenSSH >/dev/null
  ufw allow 4697/udp >/dev/null
  ufw allow 4696/tcp >/dev/null
  ufw --force enable >/dev/null
fi
# HTTPS for the status page, so the website (an https page) can show the player count: Caddy with a free name that points at this
# box's own address (<a-b-c-d>.sslip.io), certificate from Let's Encrypt by itself. STATUS_HOST=<name> for a domain of your own
IP=$(curl -fsS -4 https://ifconfig.me 2>/dev/null || hostname -I | awk '{print $1}')
HOSTNAME_STATUS=${STATUS_HOST:-$(echo "$IP" | tr . -).sslip.io}
if ! command -v caddy >/dev/null 2>&1; then apt-get update -qq; apt-get install -y -qq caddy >/dev/null 2>&1 || true; fi
if command -v caddy >/dev/null 2>&1; then
  printf '%s {\n  reverse_proxy 127.0.0.1:4696\n}\n' "$HOSTNAME_STATUS" > /etc/caddy/Caddyfile
  systemctl enable caddy >/dev/null 2>&1 || true; systemctl restart caddy
  if command -v ufw >/dev/null 2>&1; then ufw allow 80/tcp >/dev/null; ufw allow 443/tcp >/dev/null; fi
  echo "status over HTTPS: https://$HOSTNAME_STATUS/"
fi
echo "running: $(systemctl is-active oot-online)"
echo "players join $IP:4697; its status page is http://$IP:4696/"
