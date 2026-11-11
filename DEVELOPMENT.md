# OoT Online: development notes

How OoT Online is built and what is checked in the game. Players want [README.md](README.md). The reverse-engineering notes
the module's addresses come from (the game's code, disassembled from a dump) are kept private and are not in this
repository; the code says where each address came from.

## What works today

| Part | State |
|---|---|
| Native game (nxnative, Mac) | builds from a `.nup`; plays (title, the opening, Link's house, Kokiri Forest, Hyrule Field, save and load) with the full shader library (all 42,848, none failed) and a pipeline archive built for the Mac's GPU (3.3 GB on an M5 Max). In the world a locked 60 fps with the frame-step patch (see "The native game") |
| Server (`server/`) | done and tested: chat, whispers, `/who`, names, outfits, arrows and hits checked by the server, deaths, kill feed, respawn at the game's own start points with spawn protection, reconnects, room codes, bans; 13 tests, a 2,000-player load test, `bots.js` |
| Game-side network client (`client/mp_net.c`) | done; **checked in the game**: Link's area, position, heading, animation and hearts read right and reach the server |
| The lobby, in the game's own menu | **checked in the game**: the game's own system window (its font, frame and buttons) with OoT Online's text and three buttons, Play / Tunic / Name, driven by the game's own D-pad focus; Link stays still while it is up |
| Notices (joins, who shot whom) | in the game's own info bar, which closes itself after about 3 s (**checked** with a test line) |
| Chat box, name tags | drawn in the game window with the game's own fonts and button icons (`client/mp_overlay_mac.m`); the tags' places come from the game's own world-to-screen, run on the game thread each frame (`tags_tick`), since the window's drawing thread may not call the game (the places are checked in the game's log; the tags themselves are drawn only in a window, not in the headless test runs) |
| Map of the game's code (private notes) | gameplay (Link, stand-ins, animations, arrows, damage, death, events, tunic) and the game's menus; the menu part checked in the game  |
| Other players in your world (`client/games/oot_v131072.c`) | **checked in the game**, fully drawn since the full shader library: each nearby player is a stand-in Link of their own age, walking with Link's own animations, adults in their own tunic (dressed after creation the way the game's equipFromGameData does slot 8: the stand-in is the inventory screen's Link, which otherwise wears the watcher's tunic); their arrows (checked by the server) take your hearts, a finishing hit leaves a quarter heart (no Game Over) and the server's respawn heals you at one of the game's start points |
| Game versions | **both checked in the game** (11-11). The update **v131072** (build e0f3ccfa): everything above. The first release, the base game without the update (**v0** in the package; build 33fa6c2c; the private notes call it v65536): nxnative builds it from a profile it derives from the update's (it registers only the update); OoT Online's address table for it (the item functions included) and its own frame-step rule (`frame_step_v65536`, matched at main+0x6b5900) put the frame call in; run `base1`: the game recognized, online on a copy of the save, the frame call on the main thread, 60 fps, a new game straight to Hyrule Field, the trip to the drawbridge, the slingshot on ZR, shots seen. (That run used the update's shader library, so its 3D picture was black: the base game's shaders have their own keys; the setup builds a base-only player's full library itself.) `tools/make_online_mod.py` finds each version's tables (the base game's are Product.100). Players of both versions share one world; a server can take only some builds with `--build e0f3ccfa,33fa6c2c` |
| Your bow | **checked in the game** (runs `kit2`, 11-11): online, Link gets his age's ranged weapon through the game's own inventory code, with no item-get cutscene (adult Link: a quiver and the Fairy Bow; young Link: a seed bag and the Fairy Slingshot), on the item button, **ZR** (hold to aim, let go to shoot; the game's own "Shoot Seed ZR" prompt shows). Each shot is seen and sent to the server; the ammo refills after each respawn and when low. The others' arrows fly as the game's own arrow model along their line and stop at you; hits show in the game's info bar ("Mido shot you"), the hearts drop, a finishing hit sends you back after 4 s with full hearts and ammo. Some of the game's start points the server respawns at are in water: Link is then taken to the hub on dry land (the game would drown him: runs `kit1`) |
| Travel to the shared map | **checked**: Play takes you to Hyrule Field in front of the Castle Town drawbridge through the game's own area change |
| No cutscenes on the way in | **checked**: a new game starts in Hyrule Field (the opening movie and intro are skipped by the Online app's data mod); story scenes are refused while online |
| Stand-in actor (`tools/make_online_mod.py`) | built from the player's own dump: OnlinePlayer_Adult/Child with Link's animations; archives byte-exact to the game's own layout |
| Story cutscenes kept out of the way | the game's own event names are mapped (914 flows); story scenes (`Dm_<area>_*`) are refused while playing online, system ones (area changes, saves) go through |
| True open world (no black screens between areas) | a separate project; OoT Online keeps the game's normal area changes |

## Playing (each player)

```bash
zsh setup/ootonline-setup.sh "/path/to/your game.nup" <server address>
```

Then open `~/Applications/Ocarina of Time Online.app` and load your save, or start a **New Game**: in the Online app a new game
skips the opening (no movie, no intro scene) and starts in Hyrule Field about 6 seconds after you pick it. Once Link is in the world, the lobby comes up in
the game's own menu window: **Play**, **Tunic** (A, or left / right on the D-pad: Kokiri, Goron, Zora; the Goron and Zora
tunics show on adult Link) and **Name**. **Play** takes you to the shared map, Hyrule Field, in front of the Castle Town
drawbridge, through the game's own area change (`DM_MP_STAY=1` keeps you where you are). In the game: **T** opens the chat
(**/** for commands: `/help`, `/who`, `/w name text`, `/me`, `/spawn`, `/stats`, `/lobby`). Joins and who shot whom show in
the game's own info bar. Keyboard: arrows = D-pad, J = A, I = Y.

The first build takes one to three hours (about 43,000 shaders) and about 60 GB of space; it is done once. (The setup was run end to end on 11-11: with a shader cache from an earlier build it took about 30 minutes and
reproduced the tested tree exactly, all 42,848 shaders from the cache.)

**Your save is never written by online play.** When the app starts online, it copies your save and plays on the copy (your
progress, items and hearts come along); the game's autosaves while you are online (the trip to Hyrule Field, a tunic you
picked, hearts lost to arrows, the bow or slingshot OoT Online hands you) go into the copy only, and the copy is made fresh from your save at every start. The copy is
never synced to the cloud. To play the story, start the game without a server (no `DM_MP_SERVER`).

## Running a server

```bash
cd server && node server.js                      # UDP 4697 for games, http://127.0.0.1:4696 for the status page
node test/nettest.js                             # the tests
node test/loadtest.js 2000 20                    # 2,000 simulated players for 20 s
```

On a rented Linux server (about 5 a month): `zsh deploy/push.sh root@<its address>` (see `deploy/`). Friends only:
`--code SOMEWORD`. Admin calls on the server machine: `/api/say?text=`, `/api/kick?name=`, `/api/ban?name=`,
`/api/mute?name=`, `/api/spawns` (POST `{"from": "<player>"}` saves where that player stands as a spawn point).

## The native game

The setup builds the game with nxnative at the commit in `patches/NXNATIVE_COMMIT`, with three differences from upstream,
all in `patches/`:
- **macOS 26:** nxnative's shader translator was built for macOS 27 and crashes at start on macOS 26 (it calls
  `pipe2()`, which macOS 26 doesn't have). The patch runs the same translator's Intel half under Rosetta.
- **Xcode 27.1:** nxnative's shaders were tested with Xcode 27.2's Metal compiler; the build uses 27.1's with
  `--allow-other-metal`. The App Store has 27.2.
- **60 fps in the world** (`nxnative-frame-step-gpu-span.patch`): nxnative's frame step holds a frame for 2 or 3 vsyncs
  (30 or 20 fps) when it thinks 60 cannot hold, judging by the frame's GPU time. It read that time as the sum of the frame's
  command buffers' times, and on an M5 Max (macOS 26.7) the buffers overlap: in Hyrule Field the sum is about 33 ms while
  the GPU really works about 8 ms of each frame (first start to last end). So the game dropped to 20-30 fps as soon as it
  reached the world and stayed there (each try to go back to 60 failed, as the same reading said 2 vsyncs again). The patch takes the
  shorter of the sum and the span. Measured in Hyrule Field, same place, same save: without it 28 fps (run `fps1`, 20 tries back to 60, all failed); with
  the hold off 60.0 fps, frame time p95 16.8 ms (`fps2`); with the patch and the hold on, 60.00 fps over 2.5 minutes standing, slowest frame 17.6 ms, none over 25 ms (`fps3`): one step to 30 fps as the world first appeared, back to 60 thirty-four frames later. OoT Online's own work costs about 0.1 ms a frame. At the online hub, the drawbridge, with four other players' stand-ins drawn
(about 2,700 draws a frame, the full shader library), 2 minutes standing: 60.00 fps, slowest frame 20.1 ms, none over 25 ms (`fps4`).

The first time the game shows a place it builds that place's render pipelines (a few seconds of slower frames, once per
place); they are kept for the next time.

## Folders

- `server/` the server (Node, no dependencies): `protocol.js` is the wire format, `client.js` a test client.
- `client/` the game side: `mp_net.c` (network), `hle_mp.c` (the nxnative host module), `mp_overlay_mac.m` (screens).
- `tools/integrate_nxnative.py` puts `client/` into an nxnative checkout (checked edits; run it again after updating).
- `tools/game_fonts.py`, `tools/romfs_read.py` read the game's own fonts out of a player's own dump; `tools/make_spawns.py` the
  server's respawn points (the game's own start points); `tools/make_online_mod.py` the stand-in actors.
- `tools/oot_run.sh`, `tools/ctl.sh` test runs of the game (headless, silent, their own save) and driving them live;
  `tools/boot_save.sh` loads a run's save from the title. Runs share one pipeline cache, as the app keeps its own;
  `OOT_WORK` / `OOT_VER` run another build of the game. `tools/frames_stats.py` reads a run's per-frame timing (`DM_NVN_TSV`).
- `tools/locked_build.sh` (the app's build) and `tools/locked_job.sh` (any heavy job) hold the shared game-run lock, so no test
  of either project runs at the same time.
- `setup/` the player setup. `deploy/` the server setup. `docs/` the website and the installer.
