#!/usr/bin/env python3
"""Footage of other players for the website: a scene of stand-ins on the Castle Town drawbridge, written as the module's DM_MP_DEMO
file (rows: frame id name outfit child x y z yaw anim; 60 a second of game time, played with DM_FSTEP_ONE so slow capture changes nothing).
    python3 tools/demo_compose.py [bridge|walk|shootout] <out.tsv>     (prints the scene's length and the closest two players ever get)
The stand-ins go through the same code as real players' (client/games/oot_v131072.c puppets_update): their legs follow their speed
(puppet_speed), so they have to move as Link moves. Link's own numbers, from recordings of him (DM_MP_TRACE, runs/rec1 young, rec2 adult):
  - a full run is 4.5 m/s young, 5.3 m/s adult; a part-tilt walk about 1.7 m/s; full speed within a tenth of a second, stopped within
    about half a metre; he turns on the spot fast;
  - the drawbridge's planks (the young Field's) are flat: y 150.573 while he runs, 150.598 standing, from the gate (z 32.2) to at least
    z 44.8, and about 8 m wide (he fell into the moat at x -4.4): every path here stays inside |x| < 3.4, z 33.5..44.8.
Facing: yaw 0 looks along +z (away from the gate, where the camera is), 90 degrees along +x (screen left), 180 at the camera."""
import math
import sys

FPS = 60
Y_RUN, Y_STAND = 150.573, 150.598
RUN = {True: 4.5, False: 5.3}      # young, adult
WALK = {True: 1.5, False: 1.7}
ACCEL, BRAKE, TURN = 45.0, 20.0, 720.0   # m/s², m/s², degrees a second
CAMERA_LINK = (-0.04, 32.16)       # the hub: our own Link stands here, the camera behind him looking along +z
PREROLL = 3.0                      # seconds of everyone standing first: the stand-ins are created and dressed meanwhile

class Player:
    def __init__(self, pid, name, outfit, child, x, z, face):
        self.pid, self.name, self.outfit, self.child = pid, name, outfit, child
        self.x, self.z, self.yaw, self.v = x, z, face, 0.0
        self.plan, self.rows, self.acts, self.downs = [], [], [], []

    # the plan: a list of steps, each run until done
    def wait(self, until, face=None): self.plan.append(('wait', until, face)); return self
    def run(self, x, z): self.plan.append(('go', x, z, RUN[self.child])); return self
    def walk(self, x, z): self.plan.append(('go', x, z, WALK[self.child])); return self
    def jump(self, x, z, face): self.plan.append(('jump', x, z, face)); return self    # (out of sight: a respawn)
    # what the network's animation says over a time span, instead of Wait / Move (ANIM in the client: 6 BowCharge, 7 BowShoot,
    # 11 DamageS, 13 DamageL), and spans where the player is down (MP_PF_DEAD: the stand-in goes, as for a real player)
    def act(self, t0, t1, anim): self.acts.append((t0, t1, anim)); return self
    def down(self, t0, t1): self.downs.append((t0, t1)); return self


def turn_toward(cur, want, dt):
    d = (want - cur + 540.0) % 360.0 - 180.0
    step = TURN * dt
    return (cur + max(-step, min(step, d))) % 360.0, abs(d)


BRIDGE = lambda x, z: abs(x) < 3.45 and 33.4 < z < 44.85   # the drawbridge's planks (the young Field's)
GATE = lambda x, z: BRIDGE(x, z) or (abs(x) < 2.6 and 31.5 < z <= 33.4)   # and the gateway's stone floor before them (y 150.61, flat)


def simulate(p, frames, inside=BRIDGE, ground=None):
    dt = 1.0 / FPS
    plan = list(p.plan)
    for i in range(frames):
        t = i / FPS - PREROLL
        step = plan[0] if plan else None
        if step and step[0] == 'wait':
            _, until, face = step
            if face is not None: p.yaw, _ = turn_toward(p.yaw, face, dt)
            p.v = max(0.0, p.v - BRAKE * dt)
            if t >= until: plan.pop(0)
        elif step and step[0] == 'jump':
            _, p.x, p.z, p.yaw = step; p.v = 0.0; plan.pop(0)
        elif step and step[0] == 'go':
            _, tx, tz, vmax = step
            dx, dz = tx - p.x, tz - p.z; dist = math.hypot(dx, dz)
            want = math.degrees(math.atan2(dx, dz)) % 360.0
            p.yaw, off = turn_toward(p.yaw, want, dt)
            stop = p.v * p.v / (2 * BRAKE)
            if dist <= max(0.02, stop * 0.98):
                p.v = max(0.0, p.v - BRAKE * dt)
                if p.v == 0 or dist < 0.02: p.v = 0.0; plan.pop(0)
            elif off < 60:   # (turned mostly around first: no running sideways)
                p.v = min(vmax, p.v + ACCEL * dt)
            move = min(p.v * dt, dist)
            if dist > 1e-6:
                p.x += dx / dist * move; p.z += dz / dist * move
        moving = p.v > 0.05
        y = (Y_RUN if moving else Y_STAND) if ground is None else ground(p.x, p.z) + (0 if moving else 0.025)
        anim = 1 if moving else 0
        for t0, t1, a in p.acts:
            if t0 <= t < t1: anim = a
        flags = 32 if any(t0 <= t < t1 for t0, t1 in p.downs) else 0
        p.rows.append((i, p.pid, p.name, p.outfit, int(p.child), p.x, y, p.z, int(round(p.yaw / 360.0 * 65536)) % 65536, anim, flags))
        assert inside(p.x, p.z), f'{p.name} leaves the ground this scene knows at frame {i}: {p.x:.2f} {p.z:.2f}'


def face_at(a, b):   # the yaw from a looking at b
    return math.degrees(math.atan2(b[0] - a[0], b[1] - a[1])) % 360.0


def scene():
    # three adults in the three tunics, two young Links (outfit: 0 Kokiri, 1 Goron, 2 Zora; the tunic shows on adults)
    A = Player(901, 'Kaze', 2, False, 2.4, 43.6, 190)
    B = Player(902, 'Rinku', 1, False, 0.6, 44.4, 180)
    C = Player(903, 'Hylia', 0, False, -1.3, 43.9, 175)
    D = Player(904, 'Mido', 0, True, -2.7, 44.3, 170)
    E = Player(905, 'Navi', 0, True, 3.0, 34.2, 270)
    # 1. the group runs in from the far end and stops in a loose arc in front of the camera
    A.wait(0.3).run(2.0, 37.4).wait(5.0, 192)
    B.wait(0.0).run(0.7, 38.6).wait(3.8, 182).wait(9.0, face_at((0.7, 38.6), (2.0, 37.4)))
    C.wait(0.45).run(-1.0, 37.9).wait(6.0, 172).wait(7.6, 10).wait(15.0, 185)   # looks back at the Field, then round again
    D.wait(0.6).run(-2.5, 36.6).wait(8.6, 160)
    # 2. a young Link who was standing by the gate walks over to them
    E.wait(4.0, 270).walk(1.5, 35.4).wait(15.0, 200)
    # 3. two run back out to the far end and turn round; one wanders a few steps
    A.wait(8.2, face_at((2.0, 37.4), (0.7, 38.6))).run(2.6, 43.9).wait(15.0, 185)
    D.run(-1.6, 44.2).wait(15.0, 175)
    B.walk(0.2, 37.2).wait(15.0, 178)
    return [A, B, C, D, E]


ZR = 0x200   # player 1's buttons (NpadButton bit 9)


def scene_shootout():
    # Kaze and Rinku draw and hit our Link; ours fires back twice at Hylia, straight ahead (our Link aims along +z): he flinches, then
    # goes down (the info bar says so, as the server's kill feed would) and comes back at the far end; Mido strafes and lands one too.
    # events: (t, kind, ...); input: (t, buttons): our Link holds ZR to aim and lets go to shoot
    K = Player(931, 'Kaze', 2, False, 1.6, 41.0, 182)
    R = Player(932, 'Rinku', 1, False, -1.9, 40.2, 176)
    H = Player(933, 'Hylia', 0, False, 0.0, 38.6, 180)
    M = Player(934, 'Mido', 0, True, 2.9, 36.4, 230)
    K.act(0.4, 1.2, 6).act(1.2, 1.7, 7).act(6.6, 7.4, 6).act(7.4, 7.9, 7)
    R.act(1.9, 2.7, 6).act(2.7, 3.2, 7)
    H.act(4.0, 4.6, 11).act(5.45, 6.2, 13).down(6.2, 8.4)
    H.wait(8.0).jump(-2.4, 44.2, 180).wait(8.6).walk(-1.6, 41.2).wait(30, 175)
    M.wait(5.6).run(2.3, 39.6).wait(6.4, 205).act(6.6, 7.4, 6).act(7.4, 7.9, 7)
    K.wait(30, 182); R.wait(9.0, 176).walk(-1.2, 39.0).wait(30, 178)
    events = [(1.2, 'shot', K), (1.32, 'damage', 2, K), (2.7, 'shot', R), (2.82, 'damage', 2, R),
              (6.25, 'notice', 'You shot Hylia'), (7.4, 'shot', M), (7.52, 'damage', 1, M), (7.4, 'shot', K), (7.55, 'damage', 2, K)]
    pad = [(3.0, ZR), (3.9, 0), (4.5, ZR), (5.4, 0), (5.6, 0)]
    return [K, R, H, M], events, pad


def scene_walk():
    # Our Link walks out across the drawbridge (a part tilt on the left stick, ly 0.45: a young Link's walk) and four walk with him, beside
    # and ahead at about his pace, a little apart in speed and start the way people are; the ones ahead stop where the planks end and turn
    # to the others, then everyone stands. The camera follows our Link from behind
    LY = int(0.45 * 32767)
    K = Player(941, 'Kaze', 2, False, 1.45, 33.0, 0)
    R = Player(942, 'Rinku', 1, False, -1.55, 32.6, 0)
    M = Player(943, 'Mido', 0, True, 0.75, 35.0, 0)
    H = Player(944, 'Hylia', 0, False, -1.0, 36.3, 0)
    K.wait(0.65).walk(1.3, 41.6).wait(8.2, 20).wait(30, 300)
    R.wait(0.5).walk(-1.7, 41.0).wait(30, 40)
    M.wait(0.8).walk(0.5, 43.4).wait(7.3, 160).wait(30, 200)
    H.wait(0.4).walk(-1.2, 44.4).wait(7.0, 150).wait(30, 170)
    pad = [(0.5, (0, 0, LY)), (6.6, (0, 0, 0))]
    return [K, R, M, H], [], pad


def main():
    which = sys.argv[1] if len(sys.argv) > 2 else 'bridge'
    out = sys.argv[-1] if len(sys.argv) > 1 else 'demo.tsv'
    frames = int((PREROLL + 15.5) * FPS)
    events, pad = [], []
    if which == 'shootout': players, events, pad = scene_shootout(); frames = int((PREROLL + 11) * FPS)
    elif which == 'walk': players, events, pad = scene_walk(); frames = int((PREROLL + 10) * FPS)
    else: players = scene()
    for p in players: simulate(p, frames, GATE if which == 'walk' else BRIDGE)
    closest = min((math.hypot(a.rows[i][5] - b.rows[i][5], a.rows[i][7] - b.rows[i][7]), i, a.name, b.name)
                  for i in range(frames) for k, a in enumerate(players) for b in players[k + 1:])
    near_cam = min((math.hypot(p.rows[i][5] - CAMERA_LINK[0], p.rows[i][7] - CAMERA_LINK[1]), i, p.name) for p in players for i in range(frames))
    with open(out, 'w') as f:
        f.write('#frame\tid\tname\toutfit\tchild\tx\ty\tz\tyaw\tanim\tflags\n')
        for i in range(frames):
            for p in players:
                r = p.rows[i]
                f.write(f'{r[0]}\t{r[1]}\t{r[2]}\t{r[3]}\t{r[4]}\t{r[5]:.4f}\t{r[6]:.4f}\t{r[7]:.4f}\t{r[8]}\t{r[9]}\t{r[10]}\n')
        me = (CAMERA_LINK[0], Y_STAND + 0.85, CAMERA_LINK[1])   # our young Link's chest
        for ev in sorted(events, key=lambda e: e[0]):   # (the module plays them in file order)
            t, kind = ev[0], ev[1]; fr = int(round((t + PREROLL) * FPS))
            if kind == 'shot':
                p = ev[2]; r = p.rows[fr]; src = (r[5], r[6] + (0.95 if p.child else 1.3), r[7])
                d = [me[k] - src[k] for k in range(3)]; l = math.sqrt(sum(c * c for c in d)); d = [c / l for c in d]
                f.write(f'E {fr} shot {p.pid} {src[0]:.3f} {src[1]:.3f} {src[2]:.3f} {d[0]:.4f} {d[1]:.4f} {d[2]:.4f}\n')
            elif kind == 'damage': f.write(f'E {fr} damage {ev[2]} {ev[3].pid}\n')
            elif kind == 'notice': f.write(f'E {fr} notice {ev[2]}\n')
        for t, b in pad:
            b, lx, ly = b if isinstance(b, tuple) else (b, 0, 0)
            f.write(f'I {int(round((t + PREROLL) * FPS))} {b:x} {lx} {ly} 0 0\n')
    print(f'{out}: {len(players)} players, {frames} frames ({frames / FPS:.1f} s, the first {PREROLL:.0f} s standing); '
          f'closest pair {closest[0]:.2f} m ({closest[2]}, {closest[3]} at frame {closest[1]}); closest to our Link {near_cam[0]:.2f} m ({near_cam[2]})')


if __name__ == '__main__':
    main()
