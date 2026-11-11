'use strict';
// OoT Online: the shared-world server. Every game running the OoT Online mod is a UDP client of this one process.
// Each player hears only about the players near them (same scene, nearest first), so thousands can be online while a
// game draws a few dozen stand-ins. Chat, arrows between players, deaths and respawns go through here; everything the
// game itself does (walking, enemies, the world) stays in each player's own game.
//
//   node server.js [--port 4697] [--udp-host ADDR] [--http 4696] [--http-host 127.0.0.1] [--tick 20] [--nearby 24] [--view 150]
//                  [--max 4000] [--per-ip 8] [--code WORD] [--build e0f3ccfa[,33fa6c2c]] [--arrow-dmg 4] [--respawn-ms 4000]
//                  [--invuln-ms 3000] [--join-scene N] [--motd "text"] [--data DIR] [--quiet]
//
// The HTTP port serves a status page and /api/state to anyone who can reach it (127.0.0.1 by default) and the admin
// calls (/api/say, /api/kick, /api/spawns POST) to the server's own machine only.

const dgram = require('dgram');
const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const P = require('./protocol');

// ---------------------------------------------------------------- options
const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  if (i < 0) return def;
  const v = argv[i + 1];
  if (v === undefined || v.startsWith('--')) return true;
  return typeof def === 'number' ? Number(v) : v;
};
const CFG = {
  port: opt('port', 4697),
  udpHost: String(opt('udp-host', '') || ''),   // the address the game port binds (all by default); on Fly.io fly-global-services, the only one its UDP reaches
  http: opt('http', 4696),
  httpHost: opt('http-host', '127.0.0.1'),
  tickHz: Math.max(5, Math.min(60, opt('tick', 20))),
  nearby: Math.max(1, Math.min(40, opt('nearby', 24))),
  view: opt('view', 150),                 // world units: stand-ins farther than this aren't sent
  max: opt('max', 4000),
  perIp: opt('per-ip', 8),
  code: String(opt('code', '') || ''),
  build: String(opt('build', '') || '').toLowerCase(),   // e.g. e0f3ccfa or e0f3ccfa,33fa6c2c (update and base game): only games of these main builds may join; empty: any
  arrowDmg: opt('arrow-dmg', 4),          // quarter hearts per arrow (4 = one heart)
  respawnMs: opt('respawn-ms', 4000),
  invulnMs: opt('invuln-ms', 3000),
  joinScene: opt('join-scene', 0) >>> 0,  // with spawns for this scene, a new player is sent there first
  motd: String(opt('motd', 'Welcome to OoT Online. Be nice. /help for commands.')),
  dataDir: opt('data', path.join(__dirname, 'data')),
  quiet: !!opt('quiet', false),
};
const TICK_MS = 1000 / CFG.tickHz;
const NOWHERE = 0xffffffff;      // the scene a game reports while Link isn't in the world
const SILENT_MS = 10000;        // no packet for this long: shown as gone to the others
const FORGET_MS = 20000;        // and forgotten after this long (until then a game can come back with its token)
const HIT_RANGE = { [P.HIT_KIND.ARROW]: 160, [P.HIT_KIND.FIRE_ARROW]: 160, [P.HIT_KIND.ICE_ARROW]: 160, [P.HIT_KIND.LIGHT_ARROW]: 160,
  [P.HIT_KIND.MELEE]: 6, [P.HIT_KIND.OTHER]: 40 };
const HIT_DMG = (kind) => kind === P.HIT_KIND.MELEE ? Math.max(1, CFG.arrowDmg >> 1) : kind === P.HIT_KIND.LIGHT_ARROW ? CFG.arrowDmg * 2 : CFG.arrowDmg;
const SHOT_KEEP_MS = 400;       // a shot rides in the snapshots of the players near it this long
const SHOT_RANGE = 200;
const FEED_RANGE = 300;         // an elimination this close reaches a player's kill feed
const KILL_CREDIT_MS = 15000;   // a death this soon after a player's hit counts as theirs
const CHAT_MAX = 120;
const CHAT_BURST = 5, CHAT_EVERY_MS = 1200;   // per player: 5 at once, then one per 1.2 s (lines that a lossy line delivers together still pass)
const CHAT_GLOBAL_PER_S = 30;   // the whole server's chat lines per second (more are refused, with a note)
const OUTQ_MAX = 400;           // a game that stops acking keeps at most this many events; the oldest are dropped
const JOIN_NOTES_UNDER = 80;    // "X joined" lines go to everyone while fewer than this are online
const NEAR_EVERY = 3;           // who is nearest is worked out every 3rd tick per player (staggered); positions are fresh every tick
const ALONE_EVERY = 4;          // a player with nobody in view and nothing to hear gets every 4th snapshot
const log = (...a) => { if (!CFG.quiet) console.log(new Date().toISOString().slice(11, 19), ...a); };

// ---------------------------------------------------------------- state
const sessions = new Map();   // token -> session
const byId = new Map();       // id -> session
const byAddr = new Map();     // "ip:port" -> session
const stats = { pktIn: 0, pktOut: 0, bytesIn: 0, bytesOut: 0, ticks: 0, tickSum: 0, tickMax: 0, hits: 0, hitsRefused: 0, chat: 0, chatRefused: 0, bad: 0, deaths: 0 };
const now = () => performance.now();
const T0 = Date.now();
let nextIdHint = 1;
let chatWindow = { t: 0, n: 0 };
const shots = [];             // {n, shooter, scene, x, y, z, dx, dy, dz, kind, t}, oldest first
let shotSerial = 0;
let onlineNow = 0;            // counted once a tick (snapshots carry it)

fs.mkdirSync(CFG.dataDir, { recursive: true });
const STATS_FILE = path.join(CFG.dataDir, 'stats.json');
const SPAWNS_FILE = path.join(CFG.dataDir, 'spawns.json');
const BANS_FILE = path.join(CFG.dataDir, 'bans.json');
const readJson = (f, def) => { try { return JSON.parse(fs.readFileSync(f, 'utf8')); } catch (_) { return def; } };
const lifetime = readJson(STATS_FILE, {});       // name (lowercase) -> {name, kills, deaths, best, joins, lastSeen, outfit}
let spawns = readJson(SPAWNS_FILE, {});           // scene (decimal string) -> [[x, y, z, yaw], ...]
let bans = readJson(BANS_FILE, { names: [], ips: [] });
let statsDirty = false;
function saveJson(f, v) { const tmp = f + '.tmp'; fs.writeFileSync(tmp, JSON.stringify(v, null, 1)); fs.renameSync(tmp, f); }
setInterval(() => { if (statsDirty) { statsDirty = false; try { saveJson(STATS_FILE, lifetime); } catch (e) { log('stats save failed', e.message); } } }, 15000).unref();

function allocId() {
  if (byId.size >= 0xfffe) return null;
  for (let i = 0; i < 0x10000; i++) {
    const id = nextIdHint; nextIdHint = nextIdHint >= 0xfffe ? 1 : nextIdHint + 1;
    if (!byId.has(id)) return id;
  }
  return null;
}
function newToken() { let t; do { t = crypto.randomBytes(4).readUInt32LE(0); } while (t === 0 || sessions.has(t)); return t; }
const isLoopback = (ip) => ip === '127.0.0.1' || ip === '::1' || ip === '::ffff:127.0.0.1';

// Names: letters, digits, space and _-.' only, 1-16 characters, unique without regard to case
function cleanName(s) {
  const x = String(s || '').normalize('NFKC').replace(/[^\p{L}\p{N} _.'-]/gu, '').replace(/\s+/g, ' ').trim().slice(0, 16);
  return x || 'Hero';
}
function uniqueName(base, except) {
  const taken = (n) => { for (const o of sessions.values()) if (o !== except && o.name.toLowerCase() === n.toLowerCase()) return true; return false; };
  if (!taken(base)) return base;
  for (let i = 2; i < 10000; i++) { const n = `${base.slice(0, 13)} ${i}`; if (!taken(n)) return n; }
  return base + ' ' + crypto.randomBytes(2).toString('hex');
}
function cleanChat(s) { return String(s).normalize('NFKC').replace(/[\u0000-\u001f\u007f-\u009f​-‏‪-‮⁦-⁩]/g, '').replace(/\s+/g, ' ').trim(); }

// ---------------------------------------------------------------- events
function push(s, type, payload) {
  s.outQ.push({ id: s.nextEvId++, type, payload });
  if (s.outQ.length > OUTQ_MAX) s.outQ.splice(0, s.outQ.length - OUTQ_MAX);   // the game's reader takes a jump in ids as dropped events
}
function system(s, text) { push(s, P.SEV.CHAT, P.ev.chatOut(P.NOBODY, P.CHAN.SYSTEM, '', text)); }
function everyone(fn) { for (const s of sessions.values()) if (!s.gone) fn(s); }
function near(scene, x, z, r, fn) {
  const r2 = r * r;
  for (const s of sessions.values()) {
    if (s.gone || !s.st || s.st.scene !== scene) continue;
    const dx = s.st.x - x, dz = s.st.z - z;
    if (dx * dx + dz * dz <= r2) fn(s);
  }
}
function online() { let n = 0; for (const s of sessions.values()) if (!s.gone) n++; return n; }

function statsOf(s) {
  const k = s.name.toLowerCase();
  return lifetime[k] || (lifetime[k] = { name: s.name, kills: 0, deaths: 0, best: 0, joins: 0, lastSeen: 0, outfit: s.outfit });
}
function sendStats(s) { const L = statsOf(s); push(s, P.SEV.STATS, P.ev.stats(s.kills, s.deaths, s.streak, s.best, Math.min(L.kills, 65535), Math.min(L.deaths, 65535), Math.max(s.best, L.best))); }

// ---------------------------------------------------------------- joining and leaving
function reject(rinfo, nonce, reason, detail = '') {
  const b = new P.Writer(160).header(P.T.REJECT).u32(nonce).u8(reason).str(detail || P.REJECT_TEXT[reason] || '', 120).done();
  sendRaw(b, rinfo.port, rinfo.address);
}
function onHello(r, rinfo) {
  const nonce = r.u32(), kind = r.u8(), outfit = r.u8(), build = r.u32(), playerKey = r.u32(), rawName = r.str(), prevToken = r.u32();
  let code = ''; try { code = r.str(); } catch (_) { /* older senders */ }
  const ip = rinfo.address, key = ip + ':' + rinfo.port, local = isLoopback(ip);
  if (CFG.code && !local && code !== CFG.code) return reject(rinfo, nonce, P.REJECT_REASON.CODE);
  const name0 = cleanName(rawName);
  if (bans.ips.includes(ip) || bans.names.map((n) => n.toLowerCase()).includes(name0.toLowerCase())) return reject(rinfo, nonce, P.REJECT_REASON.BANNED);
  if (CFG.build && build && !CFG.build.split(',').includes(build.toString(16).padStart(8, '0'))) return reject(rinfo, nonce, P.REJECT_REASON.BUILD, `this server takes builds ${CFG.build}`);

  // The same game again (a lost WELCOME, or a game that comes back after a drop): it keeps its id, name and stats
  let s = byAddr.get(key);
  if (s && s.nonce !== nonce && !(prevToken && prevToken === s.token)) { s = null; }
  if (!s && prevToken && sessions.has(prevToken)) {
    s = sessions.get(prevToken);
    byAddr.delete(s.key); s.key = key; s.ip = ip; s.port = rinfo.port; byAddr.set(key, s);
    log(`back: ${s.name} (#${s.id}) from ${key}`);
  }
  if (s) {
    s.nonce = nonce; s.lastHeard = now(); s.gone = false;
    return sendWelcome(s);
  }
  // The same install again (its game was restarted): the old session gives way. Without a key, the same name from the
  // same address counts as that (two people behind one home router who pick the same name each send their own key)
  for (const o of [...sessions.values()]) if (playerKey ? o.playerKey === playerKey : (o.ip === ip && !o.playerKey && o.name.toLowerCase() === name0.toLowerCase())) drop(o, 'restarted');
  if (online() >= CFG.max) return reject(rinfo, nonce, P.REJECT_REASON.FULL);
  if (!local) { let n = 0; for (const o of sessions.values()) if (o.ip === ip) n++; if (n >= CFG.perIp) return reject(rinfo, nonce, P.REJECT_REASON.BUSY); }
  const id = allocId(); if (id === null) return reject(rinfo, nonce, P.REJECT_REASON.FULL);
  s = {
    id, token: newToken(), nonce, playerKey, kind: kind === P.KIND.BOT ? P.KIND.BOT : P.KIND.PLAYER, outfit: outfit % P.OUTFIT_NAME.length, build,
    name: '', key, ip, port: rinfo.port, created: now(), lastHeard: now(), gone: false,
    st: null, stSeq: 0, snapSeq: 0, ackSnap: 0,
    outQ: [], nextEvId: 1, cliEvDone: 0,
    chat: { tokens: CHAT_BURST, t: now() }, hitTimes: [], lastShotT: -1e9, shotSeen: shotSerial,
    dead: false, respawnAt: 0, invulnUntil: 0, lastHitBy: null,
    kills: 0, deaths: 0, streak: 0, best: 0, muted: false,
  };
  s.name = uniqueName(name0, s);
  sessions.set(s.token, s); byId.set(s.id, s); byAddr.set(key, s);
  const L = statsOf(s); L.joins++; L.lastSeen = Date.now(); L.outfit = s.outfit; L.name = s.name; statsDirty = true;
  log(`join: ${s.name} (#${s.id}, ${s.kind === P.KIND.BOT ? 'bot' : 'player'}, ${P.OUTFIT_NAME[s.outfit]}) from ${key}; ${online()} online`);
  sendWelcome(s);
  system(s, CFG.motd);
  sendStats(s);
  if (s.kind === P.KIND.PLAYER && online() < JOIN_NOTES_UNDER) everyone((o) => { if (o !== s && o.kind === P.KIND.PLAYER) system(o, `${s.name} joined`); });
  if (CFG.joinScene) { const sp = pickSpawn(CFG.joinScene, null); if (sp) push(s, P.SEV.RESPAWN, P.ev.respawn(CFG.joinScene, sp[0], sp[1], sp[2], sp[3] || 0, CFG.invulnMs)); }
}
function sendWelcome(s) {
  const b = new P.Writer(300).header(P.T.WELCOME).u32(s.nonce).u16(s.id).u32(s.token).u8(CFG.tickHz).u8(CFG.nearby).u16(onlineNow || online())
    .str(s.name, 24).str(CFG.motd, 160).done();
  sendRaw(b, s.port, s.ip);
}
function drop(s, why) {
  if (!sessions.has(s.token)) return;
  sessions.delete(s.token); byId.delete(s.id); if (byAddr.get(s.key) === s) byAddr.delete(s.key);
  const L = statsOf(s); L.lastSeen = Date.now(); statsDirty = true;
  log(`leave: ${s.name} (#${s.id}): ${why}; ${online()} online`);
  if (s.kind === P.KIND.PLAYER && why !== 'restarted' && online() < JOIN_NOTES_UNDER) everyone((o) => { if (o.kind === P.KIND.PLAYER) system(o, `${s.name} left`); });
}
function kick(s, reason) {
  // The KICK event goes out in one last snapshot; the session goes with it
  push(s, P.SEV.KICK, P.ev.kick(reason));
  sendSnapshot(s, [], null);
  drop(s, 'kicked: ' + reason);
}

// ---------------------------------------------------------------- a player's packets
function onState(r, s) {
  const seq = r.u32(), ackSnap = r.u32(), ackSrvEv = r.u32();
  const st = { scene: r.u32(), x: r.f32(), y: r.f32(), z: r.f32(), yaw: r.u16(), anim: r.u16(), flags: r.u8(), hp: r.u8(), maxHp: r.u8(), equip: r.u8() };
  st.t = now() - Math.min(r.u16(), 1000);   // when the player was there: now, less the position's age as it left the game (ageMs)
  const events = r.events();
  s.lastHeard = now(); s.gone = false;
  if (ackSnap > s.ackSnap) s.ackSnap = ackSnap;
  if (r.left() > 1 || s.outQ.length) s.wantSnap = true;   // events in it (or ours pending): the ack goes out next tick
  if (ackSrvEv) { let i = 0; while (i < s.outQ.length && s.outQ[i].id <= ackSrvEv) i++; if (i) s.outQ.splice(0, i); }
  // The newest state wins; a reordered older one only carries its events
  if (seq > s.stSeq && Number.isFinite(st.x) && Number.isFinite(st.y) && Number.isFinite(st.z) && Math.abs(st.x) < 1e7 && Math.abs(st.y) < 1e7 && Math.abs(st.z) < 1e7) {
    s.stSeq = seq; s.st = st;
  }
  for (const e of events) {
    if (e.id <= s.cliEvDone) continue;
    if (e.id > s.cliEvDone + 1 && s.cliEvDone !== 0) { /* a gap: the client trimmed its queue; take what comes */ }
    s.cliEvDone = e.id;
    const fn = P.parse[e.type];
    if (!fn) continue;
    let d; try { d = fn(new P.Reader(e.payload)); } catch (_) { stats.bad++; continue; }
    try { onClientEvent(s, e.type, d); } catch (err) { log('event error', s.name, e.type, err.stack || err); }
  }
}
function onClientEvent(s, type, d) {
  switch (type) {
    case P.CEV.CHAT: return onChat(s, d.text);
    case P.CEV.SHOT: return onShot(s, d);
    case P.CEV.HIT: return onHit(s, d);
    case P.CEV.DIED: return onDied(s, d.killer, d.how);
    case P.CEV.OUTFIT: s.outfit = d.outfit % P.OUTFIT_NAME.length; statsOf(s).outfit = s.outfit; statsDirty = true; return;
    case P.CEV.RESPAWN_REQ: return requestRespawn(s);
  }
}

// ---------------------------------------------------------------- chat
function chatAllowed(s) {
  const t = now();
  s.chat.tokens = Math.min(CHAT_BURST, s.chat.tokens + (t - s.chat.t) / CHAT_EVERY_MS); s.chat.t = t;
  if (s.chat.tokens < 1) return 'slow down a little';
  if (t - chatWindow.t >= 1000) chatWindow = { t, n: 0 };
  if (chatWindow.n >= CHAT_GLOBAL_PER_S) return 'the chat is busy, try again in a moment';
  s.chat.tokens -= 1; chatWindow.n++;
  return null;
}
function onChat(s, raw) {
  let text = cleanChat(raw);
  if (!text) return;
  if (text.startsWith('/')) return onCommand(s, text);
  if (s.muted) { stats.chatRefused++; return system(s, 'you are muted on this server'); }
  const why = chatAllowed(s);
  if (why) { stats.chatRefused++; return system(s, why); }
  text = P.utf8Cut(text, CHAT_MAX).toString('utf8');
  stats.chat++;
  log(`chat <${s.name}> ${text}`);
  const payload = P.ev.chatOut(s.id, P.CHAN.ALL, s.name, text);
  everyone((o) => push(o, P.SEV.CHAT, payload));
}
function findByName(n) {
  n = n.toLowerCase();
  let hit = null;
  for (const s of sessions.values()) { const m = s.name.toLowerCase(); if (m === n) return s; if (!hit && m.startsWith(n)) hit = s; }
  return hit;
}
function onCommand(s, text) {
  const [cmd0, ...rest] = text.slice(1).split(' ');
  const cmd = cmd0.toLowerCase(), arg = rest.join(' ');
  switch (cmd) {
    case 'help': case '?':
      return system(s, '/who: who is online and near you · /w name text: whisper · /me text · /spawn: back to a spawn point · /stats');
    case 'who': case 'players': case 'online': {
      const list = [];
      if (s.st) near(s.st.scene, s.st.x, s.st.z, CFG.view, (o) => { if (o !== s && list.length < 12) list.push(o.name); });
      return system(s, `${online()} online` + (list.length ? `; near you: ${list.join(', ')}` : ''));
    }
    case 'w': case 'whisper': case 'msg': case 'tell': {
      const sp = arg.indexOf(' ');
      // names may hold spaces: take the longest prefix that names someone
      let target = null, body = '';
      const words = arg.split(' ');
      for (let i = words.length - 1; i >= 1 && !target; i--) { const t = findByName(words.slice(0, i).join(' ')); if (t && t.name.toLowerCase() === words.slice(0, i).join(' ').toLowerCase()) { target = t; body = words.slice(i).join(' '); } }
      if (!target && sp > 0) { target = findByName(arg.slice(0, sp)); body = arg.slice(sp + 1); }
      if (!target || !body) return system(s, 'whisper: /w name text');
      const why = chatAllowed(s); if (why) return system(s, why);
      const b = P.utf8Cut(body, CHAT_MAX).toString('utf8');
      push(target, P.SEV.CHAT, P.ev.chatOut(s.id, P.CHAN.WHISPER, s.name, b));
      return push(s, P.SEV.CHAT, P.ev.chatOut(target.id, P.CHAN.WHISPER, '→ ' + target.name, b));
    }
    case 'me': {
      if (!arg) return;
      if (s.muted) return system(s, 'you are muted on this server');
      const why = chatAllowed(s); if (why) return system(s, why);
      const line = P.utf8Cut(`* ${s.name} ${arg}`, CHAT_MAX + 20).toString('utf8');
      return everyone((o) => system(o, line));
    }
    case 'spawn': case 'respawn': case 'stuck': return requestRespawn(s);
    case 'stats': { const L = statsOf(s); return system(s, `this visit: ${s.kills} kills, ${s.deaths} deaths, best streak ${s.best} · all time: ${L.kills} kills, ${L.deaths} deaths`); }
    default: return system(s, `unknown command /${cmd}: /help lists them`);
  }
}

// ---------------------------------------------------------------- arrows, hits, deaths, respawns
function onShot(s, d) {
  if (!s.st || s.dead) return;
  const len = Math.hypot(d.dx, d.dy, d.dz);
  if (!(len > 0.5 && len < 1.5) || ![d.x, d.y, d.z].every(Number.isFinite)) return;
  // The bow's origin is at the player: a shot from far away from where they are is not believed
  const dx = d.x - s.st.x, dy = d.y - s.st.y, dz = d.z - s.st.z;
  if (dx * dx + dy * dy + dz * dz > 25 * 25) return;
  s.lastShotT = now();
  shots.push({ n: ++shotSerial, shooter: s.id, scene: s.st.scene, x: d.x, y: d.y, z: d.z, dx: d.dx / len, dy: d.dy / len, dz: d.dz / len, kind: d.kind & 3, t: now() });
}
function onHit(s, d) {
  const t = now();
  const target = byId.get(d.target);
  const refuse = (why) => { stats.hitsRefused++; if (!CFG.quiet && stats.hitsRefused % 50 === 1) log(`hit refused: ${s.name} -> #${d.target}: ${why}`); };
  if (!target || target === s || target.gone) return refuse('no such player');
  if (!s.st || !target.st || s.st.scene !== target.st.scene) return refuse('not in the same place');
  if (s.dead) return refuse('shooter is down');
  if (target.dead) return refuse('target is down');
  if (t < target.invulnUntil) return refuse('target just spawned');
  const range = HIT_RANGE[d.kind] ?? 40;
  const dx = s.st.x - target.st.x, dy = s.st.y - target.st.y, dz = s.st.z - target.st.z;
  if (dx * dx + dy * dy + dz * dz > range * range) return refuse('too far');
  if (d.kind <= P.HIT_KIND.LIGHT_ARROW && t - s.lastShotT > 5000) return refuse('no arrow shot');
  s.hitTimes = s.hitTimes.filter((x) => t - x < 1000);
  if (s.hitTimes.length >= 4) return refuse('too many hits');
  s.hitTimes.push(t);
  const qh = HIT_DMG(d.kind);
  stats.hits++;
  target.lastHitBy = { id: s.id, t, kind: d.kind };
  push(target, P.SEV.DAMAGE, P.ev.damage(s.id, qh, d.kind));
  // The hit marker; "killed" if it takes the last hearts the target's game reported
  const killed = target.st.hp > 0 && target.st.hp <= qh;
  push(s, P.SEV.HITCONFIRM, P.ev.hitconfirm(target.id, qh, killed));
}
function onDied(s, killerId, how) {
  if (s.dead) return;
  const t = now();
  s.dead = true; s.respawnAt = t + CFG.respawnMs; stats.deaths++;
  let killer = null;
  if (s.lastHitBy && t - s.lastHitBy.t < KILL_CREDIT_MS) killer = byId.get(s.lastHitBy.id) || null;   // the server's own record decides
  s.deaths++; s.streak = 0; statsOf(s).deaths++; statsDirty = true;
  if (killer && killer !== s) {
    killer.kills++; killer.streak++; killer.best = Math.max(killer.best, killer.streak);
    const L = statsOf(killer); L.kills++; L.best = Math.max(L.best, killer.streak); statsDirty = true;
    sendStats(killer);
  }
  sendStats(s);
  const kName = killer ? killer.name : '', vName = s.name;
  log(killer ? `${kName} shot ${vName}` : `${vName} fell (${how === P.HOW.LEFT ? 'left' : 'the world'})`);
  const payload = P.ev.killfeed(killer ? killer.id : P.NOBODY, s.id, killer ? P.HOW.PLAYER : P.HOW.WORLD, kName, vName);
  const told = new Set();
  const tell = (o) => { if (!told.has(o)) { told.add(o); push(o, P.SEV.KILLFEED, payload); } };
  tell(s); if (killer) tell(killer);
  if (s.st) near(s.st.scene, s.st.x, s.st.z, FEED_RANGE, tell);
  s.lastHitBy = null;
}
function requestRespawn(s) {
  const t = now();
  if (s.dead) return;   // it comes by itself
  if (s.lastHitBy && t - s.lastHitBy.t < 10000) return system(s, 'not while you are in a fight (10 s after a hit)');
  s.respawnAt = t; s.dead = true; s.selfRespawn = true;
}
function pickSpawn(scene, avoid) {
  const list = spawns[String(scene)];
  if (!list || !list.length) return null;
  // The spawn point farthest from everyone else in the scene (with a little chance, so friends aren't always split)
  let best = null, bestD = -1;
  for (const sp of list) {
    let dmin = Infinity;
    for (const o of sessions.values()) {
      if (o.gone || !o.st || o.st.scene !== scene || o === avoid) continue;
      const dx = o.st.x - sp[0], dz = o.st.z - sp[2]; dmin = Math.min(dmin, dx * dx + dz * dz);
    }
    const score = dmin * (0.75 + Math.random() * 0.5);
    if (score > bestD) { bestD = score; best = sp; }
  }
  return best;
}
function respawnDue() {
  const t = now();
  for (const s of sessions.values()) {
    if (!s.dead || t < s.respawnAt) continue;
    s.dead = false; s.invulnUntil = t + CFG.invulnMs;
    const scene = s.st ? s.st.scene : 0;
    const sp = pickSpawn(scene, s);
    push(s, P.SEV.RESPAWN, sp ? P.ev.respawn(scene, sp[0], sp[1], sp[2], sp[3] || 0, CFG.invulnMs) : P.ev.respawn(scene, NaN, NaN, NaN, 0, CFG.invulnMs));
    s.selfRespawn = false;
  }
}

// ---------------------------------------------------------------- the tick: who sees whom
const CELL = Math.max(20, CFG.view / 2);
function buildGrid() {
  const grid = new Map();   // scene -> Map(cell -> [session])
  for (const s of sessions.values()) {
    if (s.gone || !s.st || s.st.scene === NOWHERE) continue;   // on the title or in the lobby: not in any place
    let g = grid.get(s.st.scene); if (!g) grid.set(s.st.scene, g = new Map());
    const k = cellKey(Math.floor(s.st.x / CELL), Math.floor(s.st.z / CELL));
    let c = g.get(k); if (!c) g.set(k, c = []);
    c.push(s);
  }
  return grid;
}
const cellKey = (cx, cz) => (cx + 0x8000) * 0x10000 + (cz + 0x8000);
// The nearest CFG.nearby others within the view radius, nearest first (a small sorted array: most candidates fail the
// first comparison once it is full, so a crowd of thousands in one spot stays cheap)
function nearestOf(s, grid) {
  const g = grid.get(s.st.scene); if (!g) return [];
  const K = CFG.nearby, r2max = CFG.view * CFG.view;
  const bestS = [], bestD = [];
  const cx = Math.floor(s.st.x / CELL), cz = Math.floor(s.st.z / CELL), R = Math.ceil(CFG.view / CELL);
  for (let ix = cx - R; ix <= cx + R; ix++) for (let iz = cz - R; iz <= cz + R; iz++) {
    const c = g.get(cellKey(ix, iz)); if (!c) continue;
    for (let i = 0; i < c.length; i++) {
      const o = c[i]; if (o === s) continue;
      const dx = o.st.x - s.st.x, dz = o.st.z - s.st.z, d = dx * dx + dz * dz;
      if (d > r2max) continue;
      if (bestD.length === K && d >= bestD[K - 1]) continue;
      let j = bestD.length < K ? bestD.length : K - 1;
      while (j > 0 && bestD[j - 1] > d) { bestD[j] = bestD[j - 1]; bestS[j] = bestS[j - 1]; j--; }
      bestD[j] = d; bestS[j] = o;
    }
  }
  return bestS;
}
function sendSnapshot(s, others, nearShots) {
  const w = new P.Writer().header(P.T.SNAPSHOT);
  const t = now();
  let youFlags = 0;
  if (s.dead) youFlags |= P.YOU.DEAD;
  if (t < s.invulnUntil) youFlags |= P.YOU.INVULN;
  if (s.muted) youFlags |= P.YOU.MUTED;
  w.u16(s.id).u32(++s.snapSeq).u32(s.stSeq).u32(s.cliEvDone).u32((Date.now() - T0) >>> 0).u16(onlineNow).u8(youFlags);
  const shotsN = nearShots ? Math.min(nearShots.length, 8) : 0;
  const reserve = 1 + shotsN * P.SHOT_BYTES + 1 + Math.min(others.length, 8) * P.PLAYER_BYTES;   // keep room for the closest players
  w.events(s.outQ, Math.max(64, w.room() - reserve));
  w.u8(shotsN);
  for (let i = 0; i < shotsN; i++) { const x = nearShots[i]; w.u16(x.shooter).f32(x.x).f32(x.y).f32(x.z).i16(x.dx * 32767).i16(x.dy * 32767).i16(x.dz * 32767).u8(x.kind); }
  const n = Math.min(others.length, Math.floor((w.room() - 1) / P.PLAYER_BYTES), 255);
  w.u8(n);
  for (let i = 0; i < n; i++) {
    const o = others[i], st = o.st;
    let flags = st.flags & ~(P.PF.DEAD | P.PF.INVULN);
    if (o.dead) flags |= P.PF.DEAD;
    if (t < o.invulnUntil) flags |= P.PF.INVULN;
    w.u16(o.id).f32(st.x).f32(st.y).f32(st.z).u16(st.yaw).u16(st.anim).u8(flags)
      .u8(st.maxHp ? Math.min(255, Math.round(255 * st.hp / st.maxHp)) : 255).u8(o.outfit).u8(st.equip)
      .u16(Math.max(0, Math.min(65535, Math.round(t - (st.t ?? t)))));   // ageMs: how long before this snapshot the player was there
  }
  sendRaw(w.done(), s.port, s.ip);
}
function tick() {
  const t0 = now();
  // Silence: shown as gone, then forgotten
  for (const s of [...sessions.values()]) {
    const quiet = t0 - s.lastHeard;
    if (quiet > FORGET_MS) drop(s, 'timed out');
    else if (quiet > SILENT_MS) s.gone = true;
  }
  respawnDue();
  while (shots.length && t0 - shots[0].t > SHOT_KEEP_MS) shots.shift();
  onlineNow = online();
  const grid = buildGrid();
  for (const s of sessions.values()) {
    if (s.gone) continue;
    if (s.kind === P.KIND.BOT && !s.outQ.length && (stats.ticks + s.id) % 4) continue;   // bots only need a snapshot now and then (their acks)
    let others = s.near;
    if (!others || !s.st || s.nearScene !== s.st.scene || (stats.ticks + s.id) % NEAR_EVERY === 0) {
      others = s.st ? nearestOf(s, grid) : [];
      s.near = others; s.nearScene = s.st ? s.st.scene : -1;
    } else if (others.length) others = others.filter((o) => !o.gone && o.st && o.st.scene === s.st.scene && sessions.has(o.token));
    if (!others.length && !s.outQ.length && !s.wantSnap && (stats.ticks + s.id) % ALONE_EVERY) continue;
    s.wantSnap = false;
    // Each shot reaches each game once, in the first snapshot after it (unreliable: a lost one is only a missed streak)
    let nearShots = null;
    if (s.st && shots.length && shots[shots.length - 1].n > s.shotSeen) {
      const r2 = SHOT_RANGE * SHOT_RANGE;
      nearShots = [];
      for (let i = shots.length - 1; i >= 0 && shots[i].n > s.shotSeen; i--) {
        const x = shots[i];
        if (x.shooter !== s.id && x.scene === s.st.scene && (x.x - s.st.x) ** 2 + (x.z - s.st.z) ** 2 < r2) nearShots.push(x);
      }
      s.shotSeen = shots[shots.length - 1].n;
    }
    sendSnapshot(s, others, nearShots);
  }
  const dt = now() - t0;
  stats.ticks++; stats.tickSum += dt; stats.tickMax = Math.max(stats.tickMax, dt);
}
let tickDue = now();
function scheduleTick() {
  tickDue += TICK_MS;
  const wait = Math.max(0, tickDue - now());
  setTimeout(() => {
    try { tick(); } catch (e) { log('tick error', e.stack || e); }
    if (now() - tickDue > TICK_MS * 5) tickDue = now();   // after a stall, don't run a burst of ticks to catch up
    scheduleTick();
  }, wait);
}

// ---------------------------------------------------------------- UDP
const sock = dgram.createSocket({ type: 'udp4', reuseAddr: false });
function sendRaw(buf, port, ip) { stats.pktOut++; stats.bytesOut += buf.length; sock.send(buf, port, ip); }
sock.on('message', (buf, rinfo) => {
  stats.pktIn++; stats.bytesIn += buf.length;
  const h = P.readHeader(buf);
  if (!h) { stats.bad++; return; }
  if (h.version !== P.VERSION) {
    if (h.type === P.T.HELLO && buf.length >= 8) reject(rinfo, buf.readUInt32LE(4), P.REJECT_REASON.VERSION, `this server speaks OoT Online protocol ${P.VERSION}`);
    return;
  }
  const r = new P.Reader(buf); r.o = 4;
  try {
    if (h.type === P.T.HELLO) return onHello(r, rinfo);
    const token = r.u32();
    const s = sessions.get(token);
    if (!s) return;                                   // a stranger's token: nothing
    if (s.key !== rinfo.address + ':' + rinfo.port) { // the same game from a new address (NAT rebinding)
      byAddr.delete(s.key); s.key = rinfo.address + ':' + rinfo.port; s.ip = rinfo.address; s.port = rinfo.port; byAddr.set(s.key, s);
    }
    switch (h.type) {
      case P.T.STATE: return onState(r, s);
      case P.T.PING: { const ct = r.u32(); s.lastHeard = now(); return sendRaw(new P.Writer(16).header(P.T.PONG).u32(ct).u32((Date.now() - T0) >>> 0).done(), s.port, s.ip); }
      case P.T.INFO_REQ: {
        const n = Math.min(r.u8(), 100), w = new P.Writer().header(P.T.INFO);
        const at = w.o; w.u8(0); let k = 0;
        for (let i = 0; i < n; i++) {
          const o = byId.get(r.u16()); if (!o) continue;
          if (w.room() < 6 + 25) break;
          w.u16(o.id).u8(o.kind).u8(o.outfit).str(o.name, 24); k++;
        }
        w.b[at] = k;
        return sendRaw(w.done(), s.port, s.ip);
      }
      case P.T.BYE: return drop(s, 'left');
    }
  } catch (e) {
    stats.bad++;
    if (!(e instanceof RangeError)) log('packet error', e.stack || e);
  }
});
sock.on('error', (e) => { log('udp error', e.message); if (e.code === 'EADDRINUSE') process.exit(1); });

// ---------------------------------------------------------------- HTTP: status and admin
function state(limit = 500) {
  const list = [];
  for (const s of sessions.values()) {
    if (list.length >= limit) break;
    list.push({ id: s.id, name: s.name, kind: s.kind === P.KIND.BOT ? 'bot' : 'player', outfit: P.OUTFIT_NAME[s.outfit], gone: s.gone,
      scene: s.st ? s.st.scene : null, pos: s.st ? [s.st.x, s.st.y, s.st.z].map((v) => Math.round(v * 10) / 10) : null,
      hp: s.st ? s.st.hp : null, dead: s.dead, kills: s.kills, deaths: s.deaths, queue: s.outQ.length });
  }
  return {
    online: online(), sessions: sessions.size, uptimeS: Math.round((Date.now() - T0) / 1000), tickHz: CFG.tickHz,
    tickMs: { avg: stats.ticks ? +(stats.tickSum / stats.ticks).toFixed(3) : 0, max: +stats.tickMax.toFixed(3) },
    packets: { in: stats.pktIn, out: stats.pktOut, bytesIn: stats.bytesIn, bytesOut: stats.bytesOut, bad: stats.bad },
    combat: { hits: stats.hits, refused: stats.hitsRefused, deaths: stats.deaths }, chat: { lines: stats.chat, refused: stats.chatRefused },
    players: list,
  };
}
const PAGE = `<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>OoT Online</title>
<style>:root{color-scheme:light dark;--bg:#f6f3ea;--fg:#1d2a1f;--mute:#5c6b5e;--line:#d8d2c2;--acc:#2f7d4f}@media (prefers-color-scheme:dark){:root{--bg:#121813;--fg:#e9efe6;--mute:#9fb0a2;--line:#2a352c;--acc:#62c189}}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 -apple-system,system-ui,sans-serif}main{max-width:760px;margin:0 auto;padding:24px 16px}
h1{font-size:22px;margin:0 0 4px}.n{font-size:56px;font-weight:700;color:var(--acc);line-height:1}.m{color:var(--mute)}table{width:100%;border-collapse:collapse;margin-top:16px}
td,th{text-align:left;padding:6px 4px;border-bottom:1px solid var(--line);font-variant-numeric:tabular-nums}th{color:var(--mute);font-weight:500}</style>
<main><h1>OoT Online</h1><div class=m>shared world server</div><p><span class=n id=n>–</span> <span class=m>online</span></p>
<table><thead><tr><th>Name<th>Outfit<th>Kills<th>Deaths</thead><tbody id=t></tbody></table></main>
<script>async function f(){try{const s=await (await fetch('/api/state?limit=200')).json();n.textContent=s.online;t.innerHTML='';
for(const p of s.players.filter(p=>p.kind==='player').sort((a,b)=>b.kills-a.kills)){const r=t.insertRow();for(const v of [p.name,p.outfit,p.kills,p.deaths]){r.insertCell().textContent=v}}}catch(e){}}
f();setInterval(f,3000)</script>`;
const httpServer = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  const local = isLoopback(req.socket.remoteAddress);
  const json = (code, v) => { res.writeHead(code, { 'content-type': 'application/json' }); res.end(JSON.stringify(v)); };
  const body = () => new Promise((ok) => { let b = ''; req.on('data', (c) => { if (b.length < 1e5) b += c; }); req.on('end', () => ok(b)); });
  if (u.pathname === '/' && req.method === 'GET') { res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' }); return res.end(PAGE); }
  if (u.pathname === '/api/state' && req.method === 'GET') {   // public: the website shows the player count from it
    res.setHeader('access-control-allow-origin', '*');
    return json(200, state(Math.min(5000, Number(u.searchParams.get('limit')) || 500)));
  }
  if (!local) return json(403, { error: 'admin calls are for the server machine only' });
  if (u.pathname === '/api/say' && req.method === 'POST') {
    const text = cleanChat(u.searchParams.get('text') || ''); if (!text) return json(400, { error: 'text' });
    everyone((o) => system(o, '[server] ' + text)); return json(200, { ok: true });
  }
  if (u.pathname === '/api/kick' && req.method === 'POST') {
    const s = byId.get(Number(u.searchParams.get('id'))) || findByName(u.searchParams.get('name') || '\u0000');
    if (!s) return json(404, { error: 'no such player' });
    kick(s, u.searchParams.get('reason') || 'kicked by the server'); return json(200, { ok: true });
  }
  if (u.pathname === '/api/ban' && req.method === 'POST') {
    const s = byId.get(Number(u.searchParams.get('id'))) || findByName(u.searchParams.get('name') || '\u0000');
    if (!s) return json(404, { error: 'no such player' });
    bans.names.push(s.name); bans.ips.push(s.ip); saveJson(BANS_FILE, bans); kick(s, 'banned'); return json(200, { ok: true });
  }
  if (u.pathname === '/api/mute' && req.method === 'POST') {
    const s = byId.get(Number(u.searchParams.get('id'))) || findByName(u.searchParams.get('name') || '\u0000');
    if (!s) return json(404, { error: 'no such player' });
    s.muted = u.searchParams.get('off') !== '1'; return json(200, { ok: true, muted: s.muted });
  }
  if (u.pathname === '/api/spawns') {
    if (req.method === 'GET') return json(200, spawns);
    if (req.method === 'POST') return body().then((b) => {
      let v; try { v = JSON.parse(b); } catch (_) { return json(400, { error: 'JSON: {scene, x, y, z, yaw} or {scene, clear: true}' }); }
      const k = String(v.scene >>> 0);
      if (v.clear) delete spawns[k];
      else if ([v.x, v.y, v.z].every(Number.isFinite)) (spawns[k] = spawns[k] || []).push([v.x, v.y, v.z, (v.yaw | 0) & 0xffff]);
      else if (v.from) { const s = findByName(String(v.from)); if (!s || !s.st) return json(404, { error: 'no such player' }); (spawns[String(s.st.scene)] = spawns[String(s.st.scene)] || []).push([s.st.x, s.st.y, s.st.z, s.st.yaw]); }
      saveJson(SPAWNS_FILE, spawns); json(200, spawns);
    });
  }
  return json(404, { error: 'not found' });
});

// ---------------------------------------------------------------- start
sock.bind(CFG.port, CFG.udpHost || undefined, () => {
  sock.setRecvBufferSize?.(8 << 20); sock.setSendBufferSize?.(8 << 20);
  log(`OoT Online server: UDP ${CFG.udpHost ? CFG.udpHost + ':' : ''}${CFG.port}, HTTP ${CFG.httpHost}:${CFG.http}, ${CFG.tickHz} Hz, ${CFG.nearby} nearest within ${CFG.view}` + (CFG.code ? ', room code on' : '') + (CFG.build ? `, build ${CFG.build}` : ''));
  scheduleTick();
});
httpServer.listen(CFG.http, CFG.httpHost);
function shutdown() {
  log('shutting down');
  try { saveJson(STATS_FILE, lifetime); } catch (_) { /* nothing */ }
  for (const s of sessions.values()) { try { push(s, P.SEV.KICK, P.ev.kick('the server is restarting')); sendSnapshot(s, [], null); } catch (_) { /* nothing */ } }
  setTimeout(() => process.exit(0), 150);
}
process.on('SIGINT', shutdown); process.on('SIGTERM', shutdown);
module.exports = { CFG };
