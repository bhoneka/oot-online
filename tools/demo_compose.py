#!/usr/bin/env python3
"""Footage of other players for the website: a scene of stand-ins on the Castle Town drawbridge, written as the module's DM_MP_DEMO
file (rows: frame id name outfit child x y z yaw anim; 60 a second of game time, played with DM_FSTEP_ONE so slow capture changes nothing).
    python3 tools/demo_compose.py [bridge|walk|shootout|duel|fieldwalk|forestwalk] <out.tsv>     (prints the scene's length and the closest two players ever get)
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
        y = (Y_RUN if moving else Y_STAND) if ground is None else ground.y(p.x, p.z) - (0.025 if moving else 0)   # (the survey is Link at rest: 2.5 cm higher than running)
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


class Ground:
    """The ground's height from a survey (the module's DM_MP_SURVEY: x, z, y where Link came to rest over a grid): bilinear between
    grid points; a point that stands well off its neighbours (a tree, a rock, a ledge) is no ground for walking, and neither is
    anywhere outside the survey. inside(x, z) for simulate(); y(x, z) for the rows"""
    def __init__(self, path, step=1.0):
        import statistics
        self.step = step; self.h = {}
        for line in open(path):
            if line.startswith('#'): continue
            x, z, y = (float(v) for v in line.split('\t')[:3])
            self.h[(round(x / step), round(z / step))] = y
        self.bad = set()
        for (i, j), y in self.h.items():
            nb = [self.h[(i + a, j + b)] for a in (-1, 0, 1) for b in (-1, 0, 1) if (a or b) and (i + a, j + b) in self.h]
            if len(nb) < 5 or abs(y - statistics.median(nb)) > 0.35: self.bad.add((i, j))
    def _cell(self, x, z):
        fx, fz = x / self.step, z / self.step; i, j = int(math.floor(fx)), int(math.floor(fz))
        return i, j, fx - i, fz - j
    def inside(self, x, z):
        i, j, _, _ = self._cell(x, z)
        return all((i + a, j + b) in self.h and (i + a, j + b) not in self.bad for a in (0, 1) for b in (0, 1))
    def y(self, x, z):
        i, j, u, v = self._cell(x, z)
        y00, y10, y01, y11 = self.h[(i, j)], self.h[(i + 1, j)], self.h[(i, j + 1)], self.h[(i + 1, j + 1)]
        return (y00 * (1 - u) + y10 * u) * (1 - v) + (y01 * (1 - u) + y11 * u) * v


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


def scene_duel():
    # Shooting at each other, close and readable: Kaze (left) and Rinku (right) trade arrows across the bridge in profile, in front of
    # the chain posts (runs/vid22: farther out, the right post hid Rinku and all four were small); Hylia (far, facing us, inside the
    # posts) and Mido (left, farther) shoot at our Link, who fires back twice at Hylia (straight ahead): she flinches, then goes down
    # (the info bar says so) and comes back at the far end. Every shot is the aim (BowCharge), the release (BowShoot) and the arrow's
    # flight; a hit is the target's flinch (DamageS / DamageL) or our hearts. events: (t, kind, ...); input: (t, buttons)
    K = Player(951, 'Kaze', 2, False, 2.2, 35.3, 0)
    R = Player(952, 'Rinku', 1, False, -2.2, 36.7, 0)
    H = Player(953, 'Hylia', 0, False, 0.5, 42.0, 180)
    M = Player(954, 'Mido', 0, True, 1.3, 40.8, 180)
    kr = face_at((2.2, 35.3), (-2.2, 36.7)); rk = face_at((-2.2, 36.7), (2.2, 35.3))
    K.wait(30, kr); R.plan.append(('wait', 6.6, rk)); M.wait(30, face_at((1.3, 40.8), CAMERA_LINK))
    K.act(0.3, 1.5, 6).act(1.5, 1.85, 7).act(3.55, 4.1, 11).act(6.9, 7.8, 6).act(7.8, 8.15, 7)
    R.act(1.65, 2.2, 11).act(2.35, 3.4, 6).act(3.4, 3.75, 7).act(7.95, 8.6, 13).down(8.6, 10.5)
    R.run(-1.0, 38.6).wait(30, face_at((-1.0, 38.6), (2.2, 35.3)))    # (6.6 s: off toward the middle, turning to face Kaze again)
    H.act(1.9, 3.0, 6).act(3.0, 3.35, 7).act(4.42, 4.95, 11).act(5.55, 6.3, 13).down(6.3, 8.4)
    H.wait(8.3, 180).jump(-0.9, 44.4, 180).wait(8.6).walk(-0.3, 42.4).wait(30, 182)
    M.act(4.9, 5.9, 6).act(5.9, 6.25, 7)
    events = [(1.5, 'shot', K, R), (3.4, 'shot', R, K), (3.0, 'shot', H, None), (3.12, 'damage', 2, H), (5.9, 'shot', M, None),
              (6.02, 'damage', 1, M), (6.4, 'notice', 'You shot Hylia'), (7.8, 'shot', K, R)]
    pad = [(3.35, ZR), (4.28, 0), (4.6, ZR), (5.42, 0), (5.6, 0)]
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


def load_trace(path):
    """A recording of Link (the module's DM_MP_TRACE): frame -> (x, y, z, yaw, anim)"""
    out = {}
    for line in open(path):
        if line.startswith('#'): continue
        f = line.rstrip('\n').split('\t')
        out[int(f[0])] = (float(f[1]), float(f[2]), float(f[3]), int(f[4]), int(f[5]))
    return out


def replay(trace, at, first, last, offset_frames, frames, pid, name, outfit, child):
    """Rows for a player who is the recording: trace frames first..last (absolute), played so that trace frame first + k is scene
    frame offset_frames + k; before it the player stands at the first frame's spot, after it at the last's"""
    rows = []
    for i in range(frames):
        src = first + (i - offset_frames)
        src = min(max(src, first), last)
        while src not in trace and src > first: src -= 1   # (a frame the recording skipped: the one before)
        x, y, z, yaw, anim = trace[src]
        if i < offset_frames or i > offset_frames + (last - first): anim = 0 if anim == 1 else anim
        rows.append((i, pid, name, outfit, int(child), x, y, z, yaw, anim, 0))
    return rows


FIELD_CAST = [(1, 981, 'Kaze', 2, False, 0.15), (2, 982, 'Rinku', 1, False, 0.0), (3, 983, 'Mido', 0, True, 0.35), (4, 984, 'Hylia', 0, False, 0.2),
              (5, 985, 'Navi', 0, True, 2.2)]
# The Deku Tree's meadow (runs/recf1, Kokiri Forest from its FrontDekuTree arrival: run the capture with DM_MP_FIELD_BANC=KokiriForest_Child
# DM_MP_FIELD_POS=FrontDekuTree): four walk into the mist with our Link, a young one runs up from behind
FOREST_CAST = [(1, 991, 'Saria', 1, False, 0.1), (2, 992, 'Kaze', 2, False, 0.3), (3, 993, 'Mido', 0, True, 0.0), (4, 994, 'Hylia', 0, False, 0.2),
               (5, 995, 'Navi', 0, True, 1.6)]


def scene_fieldwalk(rec_dir='runs/recw1', meta='runs/demo_recwalk.json', cast=FIELD_CAST, secs=13):
    # Five walk out across the drawbridge into the Field with our Link: each the recording of a walk Link made from his own spot near
    # the gate (runs/recw1: the ground and the gait are his), started a little apart; a young one runs past them
    import json
    m = json.load(open(meta)); trace = load_trace(f'{rec_dir}/trace.tsv'); at = 6000; pre = m['pre']
    def frame_of(t): return at + int(round((t + pre) * FPS))
    frames = int((PREROLL + secs) * FPS); out = []
    for k, pid, name, outfit, child, delay in cast:
        _, t_walk = m['marks'][k]; x, z, ly, secs = m['paths'][k]
        first, last = frame_of(t_walk - 0.6), frame_of(t_walk + secs + 1.4)
        offset = int(round((PREROLL + 0.5 + delay - 0.6) * FPS))   # the walk begins at scene time 0.5 s (+ its delay), as ours does
        out.append(replay(trace, at, first, last, offset, frames, pid, name, outfit, child))
    pad = [(0.5, (0, 0, int(0.45 * 32767))), (11.5, (0, 0, 0))]   # our Link: path 0's stick, from scene time 0.5 s
    return out, frames, pad


def main():
    which = sys.argv[1] if len(sys.argv) > 2 else 'bridge'
    out = sys.argv[-1] if len(sys.argv) > 1 else 'demo.tsv'
    frames = int((PREROLL + 15.5) * FPS)
    events, pad = [], []
    if which in ('fieldwalk', 'forestwalk'):   # rows straight from recordings (no simulate)
        rows, frames, pad = scene_fieldwalk() if which == 'fieldwalk' else \
            scene_fieldwalk('runs/recf1', 'runs/demo_recforest.json', FOREST_CAST, 13)
        with open(out, 'w') as f:
            f.write('#frame\tid\tname\toutfit\tchild\tx\ty\tz\tyaw\tanim\tflags\n')
            for i in range(frames):
                for pr in rows:
                    r = pr[i]; f.write(f'{r[0]}\t{r[1]}\t{r[2]}\t{r[3]}\t{r[4]}\t{r[5]:.4f}\t{r[6]:.4f}\t{r[7]:.4f}\t{r[8]}\t{r[9]}\t{r[10]}\n')
            for t, b in pad:
                b, lx, ly = b; f.write(f'I {int(round((t + PREROLL) * FPS))} {b:x} {lx} {ly} 0 0\n')
        print(f'{out}: {len(rows)} players from recordings, {frames} frames'); return
    if which == 'shootout': players, events, pad = scene_shootout(); frames = int((PREROLL + 11) * FPS)
    elif which == 'duel': players, events, pad = scene_duel(); frames = int((PREROLL + 10.5) * FPS)
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
                to = me
                if len(ev) > 3 and ev[3] is not None:   # at another stand-in: its chest
                    q = ev[3]; qr = q.rows[fr]; to = (qr[5], qr[6] + (0.8 if q.child else 1.15), qr[7])
                d = [to[k] - src[k] for k in range(3)]; l = math.sqrt(sum(c * c for c in d)); d = [c / l for c in d]
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
