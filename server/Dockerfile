# The OoT Online server as a container (Fly.io: fly.toml beside this file; `fly deploy` from this folder).
# Only the server's own code goes in: nothing from the game, and not data/ (a server's stats, spawn points and bans stay with it).
FROM node:22-alpine
WORKDIR /app
COPY server.js protocol.js ./
# UDP 4697 for the games (bound to fly-global-services, the only address Fly's UDP reaches), HTTP 4696 for /api/state behind Fly's TLS.
# --build: only the two game builds the client knows (the update and the launch version) may join
CMD ["node", "server.js", "--udp-host", "fly-global-services", "--http-host", "0.0.0.0", "--data", "/data", "--build", "e0f3ccfa,33fa6c2c"]
