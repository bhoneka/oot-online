'use strict';
// OoT Online wire protocol v1: UDP, little-endian, one message per datagram (<= 1200 bytes).
// The server (server.js), bots.js and the tests use this file; the game side (the nxnative host module, C) mirrors it
// field for field, so any layout change bumps VERSION (a game of another version gets REJECT VERSION).
//
// Reliability: positions are fire-and-forget (the newest wins). Events (chat, hits, deaths, respawns, ...) ride inside
// STATE / SNAPSHOT and are resent in every packet until the other side's ack covers their id: no second channel.
// Shots are only drawn, so they travel unreliably in the snapshot's shot list.
//
// Header (4): u16 magic 'OO' (0x4F4F), u8 version, u8 type
// HELLO    C>S: u32 nonce, u8 kind (KIND), u8 outfit, u32 build (first 4 bytes of the game's main build ID, LE; 0 = any),
//               u32 playerKey (random, made once per install: the same key again is the same player restarting), str name, u32 prevToken (the session a game had before the server went quiet, 0 = none), str code (room code)
// WELCOME  S>C: u32 nonce, u16 id, u32 token, u8 tickHz, u8 maxNearby, u16 online, str name (as the server made it unique), str motd
// REJECT   S>C: u32 nonce, u8 reason (REJECT_REASON), str detail
// STATE    C>S: u32 token, u32 seq, u32 ackSnap, u32 ackSrvEv,
//               u32 scene, f32 x, y, z, u16 yaw, u16 anim, u8 flags (PF), u8 hp, u8 maxHp (quarter hearts), u8 equip, events
// SNAPSHOT S>C: u16 you, u32 seq, u32 ackState, u32 ackCliEv, u32 tMs, u16 online, u8 youFlags (YOU), events,
//               u8 nShots, nShots x shot{u16 shooter, f32 x, y, z, s16 dx, dy, dz (unit x 32767), u8 kind},
//               u8 n, n x player{u16 id, f32 x, y, z, u16 yaw, u16 anim, u8 flags, u8 hpPct, u8 outfit, u8 equip}
// PING     C>S: u32 token, u32 clientTimeMs        PONG S>C: u32 clientTimeMs, u32 serverTimeMs
// INFO_REQ C>S: u32 token, u8 n, n x u16 id        INFO S>C: u8 n, n x {u16 id, u8 kind, u8 outfit, str name}
// BYE      C>S: u32 token
// events = u8 n, n x {u32 id, u8 type, u8 len, payload[len]}   (an unknown type is skipped by its len)
// str    = u8 len, utf8 bytes
//
// Units: positions in the game's own world units; yaw 0..65535 = 0..2 pi; hp in quarter hearts; scene = the game's scene
// identifier as the game side reports it (opaque to the server: only players with the same scene see each other).

const MAGIC = 0x4f4f;
const VERSION = 1;
const MAX_DGRAM = 1200;
const NOBODY = 0xffff;

const T = { HELLO: 1, WELCOME: 2, STATE: 3, SNAPSHOT: 4, PING: 5, PONG: 6, BYE: 7, INFO_REQ: 8, INFO: 9, REJECT: 10 };
const KIND = { PLAYER: 0, BOT: 1 };
// Player flags (STATE.flags, snapshot player.flags)
const PF = { AIM: 1, SWIM: 2, CLIMB: 4, AIR: 8, GUARD: 16, DEAD: 32, INVULN: 64, CHATTING: 128 };
const EQ = { CHILD: 1 };   // the state's equip byte, passed through to the others as is: bit 0 young Link (the stand-in's age)
// SNAPSHOT.youFlags
const YOU = { DEAD: 1, INVULN: 2, MUTED: 4 };
// The outfits a player can pick before joining (their stand-in wears it for everyone)
const OUTFIT = { KOKIRI: 0, GORON: 1, ZORA: 2 };
const OUTFIT_NAME = ['Kokiri Tunic', 'Goron Tunic', 'Zora Tunic'];
// Client -> server events
//   CHAT: str text (the server keeps 120 bytes; '/' commands are the server's)
//   SHOT: f32 x, y, z, s16 dx, dy, dz, u8 kind: an arrow left the player's bow; the others near draw its flight
//   HIT:  u16 target, u8 kind (HIT_KIND): the player's own game saw their arrow hit that player's stand-in
//   DIED: u16 killer (NOBODY = the world), u8 how (HOW): the player's Link reached 0 hearts
//   OUTFIT: u8 outfit
//   RESPAWN_REQ: (no payload) the /spawn command's game-side twin: put me back at a spawn point
const CEV = { CHAT: 1, SHOT: 2, HIT: 3, DIED: 4, OUTFIT: 5, RESPAWN_REQ: 6 };
// Server -> client events
//   CHAT: u16 from (NOBODY = the server), u8 channel (CHAN), str name, str text
//   DAMAGE: u16 attacker, u8 quarterHearts, u8 kind: apply this to your Link
//   KILLFEED: u16 killer, u16 victim, u8 how, str killerName, str victimName
//   RESPAWN: u32 scene, f32 x, y, z, u16 yaw, u16 invulnMs: put your Link here, full hearts (x NaN = your own last safe spot)
//   HITCONFIRM: u16 target, u8 quarterHearts, u8 killed: the hit marker for the shooter
//   STATS: u16 kills, u16 deaths, u16 streak, u16 bestStreak (this visit), u16 allKills, u16 allDeaths, u16 allBest (all time)
//   KICK: str reason (the server then forgets the session)
const SEV = { CHAT: 1, DAMAGE: 2, KILLFEED: 3, RESPAWN: 4, HITCONFIRM: 5, STATS: 6, KICK: 7 };
const CHAN = { ALL: 0, SYSTEM: 1, WHISPER: 2 };
const SHOT_KIND = { ARROW: 0, FIRE: 1, ICE: 2, LIGHT: 3 };
const HIT_KIND = { ARROW: 0, FIRE_ARROW: 1, ICE_ARROW: 2, LIGHT_ARROW: 3, MELEE: 4, OTHER: 5 };
const HOW = { PLAYER: 0, WORLD: 1, LEFT: 2 };
const REJECT_REASON = { FULL: 1, VERSION: 2, CODE: 3, BANNED: 4, BUILD: 5, BUSY: 6 };
const REJECT_TEXT = ['', 'the server is full', 'another version of OoT Online', 'a room code is needed (or it was wrong)',
  'you are banned from this server', 'another build of the game', 'too many players from your address'];

const SHOT_BYTES = 2 + 12 + 6 + 1;
const PLAYER_BYTES = 2 + 12 + 2 + 2 + 1 + 1 + 1 + 1;

class Writer {
  constructor(size = MAX_DGRAM) { this.b = Buffer.allocUnsafe(size); this.o = 0; }
  room() { return this.b.length - this.o; }
  u8(v) { this.b[this.o++] = v & 0xff; return this; }
  u16(v) { this.b.writeUInt16LE(v & 0xffff, this.o); this.o += 2; return this; }
  u32(v) { this.b.writeUInt32LE(v >>> 0, this.o); this.o += 4; return this; }
  f32(v) { this.b.writeFloatLE(v, this.o); this.o += 4; return this; }
  i16(v) { this.b.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(v) || 0)), this.o); this.o += 2; return this; }
  buf(x) { x.copy(this.b, this.o); this.o += x.length; return this; }
  str(s, max = 64) { const x = utf8Cut(String(s ?? ''), max); this.u8(x.length); return this.buf(x); }
  header(type) { return this.u16(MAGIC).u8(VERSION).u8(type); }
  // Writes as many of `list` (oldest first) as fit in `budget` bytes; returns how many were written
  events(list, budget) {
    const at = this.o; this.u8(0);
    let n = 0, used = 1;
    for (const e of list) {
      const sz = 6 + e.payload.length;
      if (n === 255 || used + sz > budget) break;
      this.u32(e.id).u8(e.type).u8(e.payload.length).buf(e.payload);
      used += sz; n++;
    }
    this.b[at] = n;
    return n;
  }
  done() { return this.b.subarray(0, this.o); }
}

class Reader {
  constructor(b) { this.b = b; this.o = 0; }
  need(n) { if (this.o + n > this.b.length) throw new RangeError('short packet'); }
  left() { return this.b.length - this.o; }
  u8() { this.need(1); return this.b[this.o++]; }
  u16() { this.need(2); const v = this.b.readUInt16LE(this.o); this.o += 2; return v; }
  u32() { this.need(4); const v = this.b.readUInt32LE(this.o); this.o += 4; return v; }
  f32() { this.need(4); const v = this.b.readFloatLE(this.o); this.o += 4; return v; }
  i16() { this.need(2); const v = this.b.readInt16LE(this.o); this.o += 2; return v; }
  bytes(n) { this.need(n); const v = this.b.subarray(this.o, this.o + n); this.o += n; return v; }
  str() { return this.bytes(this.u8()).toString('utf8'); }
  events() {
    const n = this.u8(), out = [];
    for (let i = 0; i < n; i++) {
      const id = this.u32(), type = this.u8(), len = this.u8();
      out.push({ id, type, payload: this.bytes(len) });
    }
    return out;
  }
}

// Cuts a string to at most `max` UTF-8 bytes without splitting a character
function utf8Cut(s, max) {
  let x = Buffer.from(s, 'utf8');
  if (x.length <= max) return x;
  let end = max;
  while (end > 0 && (x[end] & 0xc0) === 0x80) end--;
  return x.subarray(0, end);
}

function readHeader(buf) {
  if (buf.length < 4) return null;
  if (buf.readUInt16LE(0) !== MAGIC) return null;
  return { version: buf[2], type: buf[3] };
}

// ---- payload builders and parsers (both sides) ----
const ev = {
  // client -> server
  chat: (text) => new Writer(130).str(text, 120).done(),
  shot: (x, y, z, dx, dy, dz, kind) => new Writer(32).f32(x).f32(y).f32(z).i16(dx * 32767).i16(dy * 32767).i16(dz * 32767).u8(kind).done(),
  hit: (target, kind) => new Writer(8).u16(target).u8(kind).done(),
  died: (killer, how) => new Writer(8).u16(killer).u8(how).done(),
  outfit: (o) => new Writer(4).u8(o).done(),
  respawnReq: () => Buffer.alloc(0),
  // server -> client
  chatOut: (from, chan, name, text) => new Writer(200).u16(from).u8(chan).str(name, 24).str(text, 160).done(),
  damage: (attacker, qh, kind) => new Writer(8).u16(attacker).u8(qh).u8(kind).done(),
  killfeed: (killer, victim, how, kn, vn) => new Writer(64).u16(killer).u16(victim).u8(how).str(kn, 24).str(vn, 24).done(),
  respawn: (scene, x, y, z, yaw, invulnMs) => new Writer(32).u32(scene).f32(x).f32(y).f32(z).u16(yaw).u16(invulnMs).done(),
  hitconfirm: (target, qh, killed) => new Writer(8).u16(target).u8(qh).u8(killed ? 1 : 0).done(),
  stats: (k, d, s, b, ak, ad, ab) => new Writer(16).u16(k).u16(d).u16(s).u16(b).u16(ak).u16(ad).u16(ab).done(),
  kick: (reason) => new Writer(130).str(reason, 120).done(),
};

const parse = {
  [CEV.CHAT]: (r) => ({ text: r.str() }),
  [CEV.SHOT]: (r) => ({ x: r.f32(), y: r.f32(), z: r.f32(), dx: r.i16() / 32767, dy: r.i16() / 32767, dz: r.i16() / 32767, kind: r.u8() }),
  [CEV.HIT]: (r) => ({ target: r.u16(), kind: r.u8() }),
  [CEV.DIED]: (r) => ({ killer: r.u16(), how: r.u8() }),
  [CEV.OUTFIT]: (r) => ({ outfit: r.u8() }),
  [CEV.RESPAWN_REQ]: () => ({}),
};
const parseServer = {
  [SEV.CHAT]: (r) => ({ from: r.u16(), chan: r.u8(), name: r.str(), text: r.str() }),
  [SEV.DAMAGE]: (r) => ({ attacker: r.u16(), qh: r.u8(), kind: r.u8() }),
  [SEV.KILLFEED]: (r) => ({ killer: r.u16(), victim: r.u16(), how: r.u8(), killerName: r.str(), victimName: r.str() }),
  [SEV.RESPAWN]: (r) => ({ scene: r.u32(), x: r.f32(), y: r.f32(), z: r.f32(), yaw: r.u16(), invulnMs: r.u16() }),
  [SEV.HITCONFIRM]: (r) => ({ target: r.u16(), qh: r.u8(), killed: r.u8() === 1 }),
  [SEV.STATS]: (r) => ({ kills: r.u16(), deaths: r.u16(), streak: r.u16(), best: r.u16(), allKills: r.u16(), allDeaths: r.u16(), allBest: r.u16() }),
  [SEV.KICK]: (r) => ({ reason: r.str() }),
};

// ---- whole messages (client side helpers, used by bots.js and the tests; the C client mirrors them) ----
const msg = {
  hello: ({ nonce, kind = KIND.PLAYER, outfit = 0, build = 0, playerKey = 0, name, prevToken = 0, code = '' }) =>
    new Writer(128).header(T.HELLO).u32(nonce).u8(kind).u8(outfit).u32(build).u32(playerKey).str(name, 24).u32(prevToken).str(code, 32).done(),
  state: ({ token, seq, ackSnap, ackSrvEv, scene, x, y, z, yaw = 0, anim = 0, flags = 0, hp = 12, maxHp = 12, equip = 0, events = [] }) => {
    const w = new Writer().header(T.STATE).u32(token).u32(seq).u32(ackSnap).u32(ackSrvEv)
      .u32(scene).f32(x).f32(y).f32(z).u16(yaw).u16(anim).u8(flags).u8(hp).u8(maxHp).u8(equip);
    w.events(events, w.room());
    return w.done();
  },
  ping: (token, t) => new Writer(16).header(T.PING).u32(token).u32(t).done(),
  infoReq: (token, ids) => { const w = new Writer(300).header(T.INFO_REQ).u32(token).u8(Math.min(ids.length, 100)); for (const id of ids.slice(0, 100)) w.u16(id); return w.done(); },
  bye: (token) => new Writer(16).header(T.BYE).u32(token).done(),
};

function readWelcome(r) { return { nonce: r.u32(), id: r.u16(), token: r.u32(), tickHz: r.u8(), maxNearby: r.u8(), online: r.u16(), name: r.str(), motd: r.str() }; }
function readReject(r) { return { nonce: r.u32(), reason: r.u8(), detail: r.str() }; }
function readSnapshot(r) {
  const s = { you: r.u16(), seq: r.u32(), ackState: r.u32(), ackCliEv: r.u32(), tMs: r.u32(), online: r.u16(), youFlags: r.u8() };
  s.events = r.events();
  const ns = r.u8(); s.shots = [];
  for (let i = 0; i < ns; i++) s.shots.push({ shooter: r.u16(), x: r.f32(), y: r.f32(), z: r.f32(), dx: r.i16() / 32767, dy: r.i16() / 32767, dz: r.i16() / 32767, kind: r.u8() });
  const n = r.u8(); s.players = [];
  for (let i = 0; i < n; i++) s.players.push({ id: r.u16(), x: r.f32(), y: r.f32(), z: r.f32(), yaw: r.u16(), anim: r.u16(), flags: r.u8(), hpPct: r.u8(), outfit: r.u8(), equip: r.u8() });
  return s;
}
function readInfo(r) { const n = r.u8(), out = []; for (let i = 0; i < n; i++) out.push({ id: r.u16(), kind: r.u8(), outfit: r.u8(), name: r.str() }); return out; }

module.exports = {
  MAGIC, VERSION, MAX_DGRAM, NOBODY, T, KIND, PF, EQ, YOU, OUTFIT, OUTFIT_NAME, CEV, SEV, CHAN, SHOT_KIND, HIT_KIND, HOW,
  REJECT_REASON, REJECT_TEXT, SHOT_BYTES, PLAYER_BYTES, Writer, Reader, utf8Cut, readHeader, ev, parse, parseServer, msg,
  readWelcome, readReject, readSnapshot, readInfo,
};
