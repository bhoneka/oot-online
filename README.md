# OoT Online

**Ocarina of Time, together.** A shared Hyrule Field for The Legend of Zelda: Ocarina of Time (the 2026 Nintendo Switch 2
game), played natively on Apple-silicon Macs. Everyone joins one server: you see each other, chat, pick your tunic in the
game's own menu, and shoot each other with arrows and slingshot seeds. Go down and you are back a few seconds later.

Website: **https://bhoneka.github.io/oot-online/**

![Young Link on the Castle Town drawbridge at night, with another player beside him and the slingshot on ZR](docs/img/drawbridge.jpg)

## You need

- A Mac with an Apple chip (M1 or later) on macOS 26.6 or later, and about 60 GB free.
- **Your own copy of the game**, dumped from your own Switch 2 as a `.nup` file. OoT Online contains nothing from the game
  and downloads nothing from it: it builds the game from your file, on your Mac.

Nothing else: no Xcode, no Homebrew, no Terminal.

## Install

1. **[Download the installer](https://github.com/bhoneka/oot-online/releases/latest/download/Install-OoT-Online.zip)** and open
   **Install OoT Online** (double-click the zip first if your browser left it zipped).
2. The first time, macOS says it cannot check the app for malicious software: it is a free fan project, not signed by Apple.
   Click **Done**, then open **System Settings → Privacy & Security**, scroll down, click **Open Anyway** next to "Install OoT
   Online" and confirm. This happens once.
3. Click **Choose your game file…**, pick your `.nup`, then **Build Ocarina of Time Online**.

It builds **Ocarina of Time Online** into the Applications folder in your home folder. The first build takes one to three
hours (it translates and compiles about 43,000 of the game's shaders for your Mac) and is done once; opening the installer
again later updates it in minutes. On macOS 26 it may ask for your password to install Rosetta, Apple's helper for Intel
code, which one of its tools needs.

## Play

Open **Ocarina of Time Online**. Load your save, or start a **New Game**: online, a new game skips the opening and drops you
in Hyrule Field a few seconds after you pick it.

- When Link is in the world the lobby comes up in the game's own menu: **Play**, **Tunic** (Kokiri, Goron, Zora; the
  Goron and Zora tunics show on adult Link) and **Name**. **Play** takes you to the meeting place in front of the Castle
  Town drawbridge.
- You get your age's ranged weapon, with no cutscene: the Fairy Slingshot for young Link, the Fairy Bow for adult Link.
  **ZR**: hold to aim, let go to shoot.
- **T** opens the chat (**/** for commands: `/help`, `/who`, `/w name text`, `/me`, `/stats`, `/lobby`). Joins and who
  shot whom show in the game's own info bar.

**Your save is never written by online play.** The app plays on a fresh copy of your save every time it starts online;
nothing you do online reaches your real save, and the copy is never synced to the cloud.

## Run your own server

The server is Node.js with no dependencies:

```bash
cd server && node server.js        # UDP 4697 for games, HTTP 4696 (status page)
```

On a rented Linux box (Ubuntu or Debian, about 5 a month): `zsh deploy/push.sh root@<its address>` copies the server
there, installs Node and starts it as a service. `--code SOMEWORD` makes it friends-only. Players point the installer at
it under **Options** (`<address>:4697`).

## How it works

The game runs natively through [nxnative](https://github.com/seedds/nxnative) (MIT), which turns your copy into a Mac app.
OoT Online adds a small module to nxnative's host (`client/`): it reads Link's state each frame, sends it to the server,
and shows the other players as stand-in Links with Link's own animations (their walk and run follow their speed; they
draw and aim their own bow or slingshot as he does; a player who goes down is knocked flat, lies there and gets back up the
way the game knocks Link down) and a Navi each. The lobby, the notices and the item
prompts are the game's own screens and fonts. The installer (`installer/`, `setup/install.py`) does the whole build without Xcode:
it compiles the game's shaders with macOS's own Metal compiler, and the parts that need a compiler come prebuilt from GitHub
Actions; `server/` is the server.
Both game versions work: the launch version and its update. Technical notes: [DEVELOPMENT.md](DEVELOPMENT.md).

## Status

Tested in the game on an M5 Max: a locked 60 fps in Hyrule Field, also with other players around; the lobby; the trip to
the meeting place; your bow or slingshot; shots, hits and arrows in flight; damage, notices and respawns. The server is
tested with 2,000 simulated players.

---

A fan project. Not affiliated with or endorsed by Nintendo. You need your own copy of the game; OoT Online does not
include, download or link to any game files.
