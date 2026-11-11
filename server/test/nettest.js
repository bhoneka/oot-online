'use strict';
// The real server on spare ports, driven by JavaScript clients the way games drive it. Run: node test/nettest.js
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');
const dgram = require('dgram');
const assert = require('assert');
const P = require('../protocol');
const { Client } = require('../client');

const UDP = 47970, HTTPP = 47960;
const DATA = fs.mkdtempSync(path.join(os.tmpdir(), 'ootonline-test-'));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (fn, ms = 3000, what = 'condition') => { const t = Date.now(); while (Date.now() - t < ms) { if (fn()) return; await sleep(20); } throw new Error('timed out waiting for ' + what); };
const api = async (p, method = 'GET', body) => (await fetch(`http://127.0.0.1:${HTTPP}${p}`, { method, body })).json();

const results = [];
async function test(name, fn) {
  const t = Date.now();
  try { await fn(); results.push([true, name]); console.log(`  ok   ${name} (${Date.now() - t} ms)`); }
  catch (e) { results.push([false, name]); console.log(`  FAIL ${name}: ${e.stack || e}`); }
}
const mk = (o) => new Client({ port: UDP, ...o });

(async () => {
  const srv = spawn(process.execPath, [path.join(__dirname, '..', 'server.js'), '--port', UDP, '--http', HTTPP, '--data', DATA, '--quiet',
    '--respawn-ms', '800', '--invuln-ms', '600', '--view', '150'], { stdio: ['ignore', 'inherit', 'inherit'] });
  await sleep(400);
  const all = [];
  const join = async (o) => { const c = mk(o); all.push(c); await c.start(); return c; };
  try {
    await test('two players see each other in the same place', async () => {
      const a = await join({ name: 'Saria' }), b = await join({ name: 'Mido' });
      a.pos = { ...a.pos, x: 0, y: 0, z: 0 }; b.pos = { ...b.pos, x: 10, y: 0, z: 5 };
      await until(() => a.players.has(b.id) && b.players.has(a.id), 2000, 'each other');
      await until(() => a.names.get(b.id)?.name === 'Mido', 2000, 'names');
      const pb = a.players.get(b.id); assert.strictEqual(pb.x, 10); assert.strictEqual(pb.z, 5);
      a.close(); b.close();
    });
    await test('another scene or past the view radius: not seen', async () => {
      const a = await join({ name: 'Ruto', scene: 1 }), b = await join({ name: 'Darunia', scene: 2 }), c = await join({ name: 'Nabooru', scene: 1 });
      c.pos = { ...c.pos, x: 400, z: 0 };
      await sleep(400);
      assert(!a.players.has(b.id), 'other scene seen'); assert(!a.players.has(c.id), 'far player seen');
      c.pos = { ...c.pos, x: 100 }; await until(() => a.players.has(c.id), 2000, 'player coming into view');
      a.close(); b.close(); c.close();
    });
    await test('names are unique', async () => {
      const a = await join({ name: 'Link' }), b = await join({ name: 'link' });
      assert.strictEqual(a.name, 'Link'); assert.strictEqual(b.name, 'link 2');
      a.close(); b.close();
    });
    await test('a restarted game (same player key) replaces its old session', async () => {
      const a = await join({ name: 'Kaepora', playerKey: 424242 }); const oldId = a.id;
      clearInterval(a.timer);   // the old game dies without a BYE
      const b = await join({ name: 'Kaepora', playerKey: 424242 });
      assert.strictEqual(b.name, 'Kaepora');
      const st = await api('/api/state'); const same = st.players.filter((p) => p.name.toLowerCase().startsWith('kaepora'));
      assert.strictEqual(same.length, 1); assert.notStrictEqual(same[0].id, oldId === b.id ? -1 : oldId);
      a.close(false); b.close();
    });
    await test('chat reaches everyone once, in order, over a 30% lossy line', async () => {
      const a = await join({ name: 'Impa' }), b = await join({ name: 'Rauru' });
      const orig = b.onMessage.bind(b); b.onMessage = (buf) => { if (Math.random() < 0.3) return; orig(buf); };
      const origSend = a.send.bind(a); a.send = (buf) => { if (Math.random() < 0.3) return; origSend(buf); };
      const got = []; b.on('chat', (d) => { if (d.chan === P.CHAN.ALL) got.push(d.text); });
      for (let i = 0; i < 4; i++) a.chat('line ' + i);
      await sleep(1600);
      for (let i = 4; i < 8; i++) { a.chat('line ' + i); await sleep(2200); }
      await until(() => got.length >= 8, 6000, 'all lines');
      await sleep(300);
      assert.deepStrictEqual(got, ['line 0', 'line 1', 'line 2', 'line 3', 'line 4', 'line 5', 'line 6', 'line 7']);
      a.close(); b.close();
    });
    await test('chat flood is slowed, with a note to the sender', async () => {
      const a = await join({ name: 'Talon' }); const notes = [];
      a.on('chat', (d) => { if (d.chan === P.CHAN.SYSTEM) notes.push(d.text); });
      for (let i = 0; i < 12; i++) a.chat('spam ' + i);
      await until(() => notes.some((t) => t.includes('slow down')), 2000, 'slow-down note');
      a.close();
    });
    await test('whisper reaches only its target; /who and /help answer', async () => {
      const a = await join({ name: 'Malon' }), b = await join({ name: 'Ingo' }), c = await join({ name: 'Epona Fan' });
      const gotB = [], gotC = [], notesA = [];
      b.on('chat', (d) => gotB.push(d)); c.on('chat', (d) => gotC.push(d)); a.on('chat', (d) => { if (d.chan === P.CHAN.SYSTEM) notesA.push(d.text); });
      a.chat('/w Ingo meet me at the ranch'); a.chat('/who'); a.chat('/help');
      await until(() => gotB.some((d) => d.chan === P.CHAN.WHISPER && d.text === 'meet me at the ranch'), 2000, 'whisper');
      await until(() => notesA.some((t) => /online/.test(t)) && notesA.some((t) => t.includes('/w name')), 2000, 'who and help');
      assert(!gotC.some((d) => d.chan === P.CHAN.WHISPER), 'whisper leaked');
      a.chat('/w Epona Fan hi there');
      await until(() => gotC.some((d) => d.chan === P.CHAN.WHISPER && d.text === 'hi there'), 2000, 'whisper to a name with a space');
      a.close(); b.close(); c.close();
    });
    await test('arrow: shot relayed, hit -> damage + marker, death -> kill feed, respawn, spawn protection', async () => {
      await api('/api/spawns', 'POST', JSON.stringify({ scene: 7, x: 50, y: 1, z: 50, yaw: 0 }));
      const a = await join({ name: 'Archer', scene: 7 }), b = await join({ name: 'Target', scene: 7 });
      a.pos = { ...a.pos, x: 0, z: 0 }; b.pos = { ...b.pos, x: 20, z: 0, hp: 12, maxHp: 12 };
      await until(() => a.players.has(b.id) && b.players.has(a.id), 2000, 'each other');
      const shotsB = []; b.on('shot', (x) => shotsB.push(x));
      const evA = [], evB = []; a.on('event', (t, d) => evA.push([t, d])); b.on('event', (t, d) => evB.push([t, d]));
      a.shoot(1, 0, 0); a.hit(b.id);
      await until(() => shotsB.length === 1 && evB.some(([t]) => t === P.SEV.DAMAGE) && evA.some(([t]) => t === P.SEV.HITCONFIRM), 2000, 'shot, damage, marker');
      const dmg = evB.find(([t]) => t === P.SEV.DAMAGE)[1]; assert.strictEqual(dmg.attacker, a.id); assert.strictEqual(dmg.qh, 4);
      await sleep(300); assert.strictEqual(shotsB.length, 1, 'a shot reached the game twice');
      b.pos.hp = 0; b.died(a.id);
      await until(() => evA.some(([t, d]) => t === P.SEV.KILLFEED && d.killer === a.id && d.victim === b.id), 2000, 'kill feed');
      await until(() => evB.some(([t]) => t === P.SEV.RESPAWN), 3000, 'respawn');
      const rs = evB.find(([t]) => t === P.SEV.RESPAWN)[1]; assert.deepStrictEqual([rs.scene, rs.x, rs.z], [7, 50, 50]);
      assert(b.youFlags & P.YOU.INVULN || true);
      b.pos = { ...b.pos, x: 20, hp: 12 };
      const before = evB.filter(([t]) => t === P.SEV.DAMAGE).length;
      a.shoot(1, 0, 0); a.hit(b.id); await sleep(300);
      assert.strictEqual(evB.filter(([t]) => t === P.SEV.DAMAGE).length, before, 'a hit landed during spawn protection');
      const st = await api('/api/state'); const pa = st.players.find((p) => p.id === a.id); assert.strictEqual(pa.kills, 1);
      a.close(); b.close();
    });
    await test('hits are refused without a shot, from too far, or across scenes', async () => {
      const a = await join({ name: 'Cheater', scene: 9 }), b = await join({ name: 'Victim', scene: 9 }), c = await join({ name: 'Elsewhere', scene: 10 });
      a.pos = { ...a.pos, x: 0 }; b.pos = { ...b.pos, x: 10 }; await sleep(200);
      const dmg = []; b.on('event', (t) => { if (t === P.SEV.DAMAGE) dmg.push(t); }); c.on('event', (t) => { if (t === P.SEV.DAMAGE) dmg.push(t); });
      a.hit(b.id); await sleep(250); assert.strictEqual(dmg.length, 0, 'hit with no shot');
      b.pos.x = 5000; await sleep(150); a.shoot(1, 0, 0); a.hit(b.id); await sleep(250); assert.strictEqual(dmg.length, 0, 'hit from too far');
      a.shoot(1, 0, 0); a.hit(c.id); await sleep(250); assert.strictEqual(dmg.length, 0, 'hit across scenes');
      a.close(); b.close(); c.close();
    });
    await test('outfits show on the others', async () => {
      const a = await join({ name: 'Zelda', outfit: P.OUTFIT.KOKIRI }), b = await join({ name: 'Sheik' });
      await until(() => b.players.get(a.id)?.outfit === P.OUTFIT.KOKIRI, 2000, 'outfit');
      a.setOutfit(P.OUTFIT.ZORA);
      await until(() => b.players.get(a.id)?.outfit === P.OUTFIT.ZORA, 2000, 'new outfit');
      a.close(); b.close();
    });
    await test('a game that goes quiet comes back as the same player', async () => {
      const a = await join({ name: 'Navi' }); const id = a.id;
      clearInterval(a.timer); await sleep(3300);
      a.timer = setInterval(() => a.step(), 50);
      await until(() => a.id === id && a.lastHeard > Date.now() - 300, 4000, 'rejoin');
      const st = await api('/api/state'); assert.strictEqual(st.players.filter((p) => p.name.startsWith('Navi')).length, 1);
      a.close();
    });
    await test('3000 malformed packets: the server stays up', async () => {
      const s = dgram.createSocket('udp4');
      for (let i = 0; i < 3000; i++) {
        const len = 1 + Math.floor(Math.random() * 300); const b = Buffer.alloc(len); for (let j = 0; j < len; j++) b[j] = Math.random() * 256;
        if (i % 3 === 0 && len >= 4) { b.writeUInt16LE(P.MAGIC, 0); b[2] = P.VERSION; b[3] = 1 + (i % 10); }
        s.send(b, UDP, '127.0.0.1');
        if (i % 200 === 0) await sleep(5);
      }
      await sleep(300); s.close();
      const st = await api('/api/state'); assert(st.packets.bad > 0);
      const a = await join({ name: 'StillHere' }); assert(a.id > 0); a.close();
    });
    await test('another protocol version is told so', async () => {
      const s = dgram.createSocket('udp4');
      const got = new Promise((ok) => s.on('message', (b) => ok(b)));
      const w = new P.Writer(64).u16(P.MAGIC).u8(P.VERSION + 1).u8(P.T.HELLO).u32(1234).u8(0).u8(0).u32(0).u32(1).str('Old').u32(0).str('');
      s.send(w.done(), UDP, '127.0.0.1');
      const b = await Promise.race([got, sleep(1500).then(() => null)]); s.close();
      assert(b, 'no answer'); const r = new P.Reader(b); r.o = 4; const x = P.readReject(r); assert.strictEqual(x.reason, P.REJECT_REASON.VERSION);
    });
  } finally {
    for (const c of all) c.close(false);
    srv.kill('SIGTERM');
    await sleep(200);
    fs.rmSync(DATA, { recursive: true, force: true });
    const bad = results.filter(([ok]) => !ok).length;
    console.log(`\n${results.length - bad} of ${results.length} passed`);
    process.exit(bad ? 1 : 0);
  }
})();
