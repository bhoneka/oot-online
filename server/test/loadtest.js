'use strict';
// Load test: N simulated players against a real server on spare ports. Half stand in one crowded square (the worst
// case for "who sees whom"), half walk around a big field. Reports the server's tick time, its CPU, the bytes it sends,
// and how many snapshots the players got.   node test/loadtest.js [players=2000] [seconds=30]
const { spawn, execSync } = require('child_process');
const dgram = require('dgram');
const path = require('path');
const fs = require('fs');
const os = require('os');
const P = require('../protocol');

const N = Number(process.argv[2] || 2000), SECS = Number(process.argv[3] || 30);
const UDP = 47990, HTTPP = 47980, PER_SOCK = 50;
const DATA = fs.mkdtempSync(path.join(os.tmpdir(), 'ootonline-load-'));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  const srv = spawn(process.execPath, [...(process.env.PROF ? ['--cpu-prof', '--cpu-prof-dir', process.env.PROF] : []), path.join(__dirname, '..', 'server.js'), '--port', UDP, '--http', HTTPP, '--data', DATA, '--quiet', '--max', String(N + 100)],
    { stdio: ['ignore', 'inherit', 'inherit'] });
  await sleep(500);
  // Virtual players share sockets (PER_SOCK each): the server answers each by token, and they tell their snapshots apart by "you"
  const socks = [], players = [];
  let snaps = 0, snapBytes = 0, playersSeen = 0;
  for (let i = 0; i < Math.ceil(N / PER_SOCK); i++) {
    const s = dgram.createSocket('udp4'); socks.push(s);
    s.on('message', (b) => {
      const h = P.readHeader(b); if (!h) return;
      const r = new P.Reader(b); r.o = 4;
      if (h.type === P.T.WELCOME) { const w = P.readWelcome(r); const p = players.find((x) => x.nonce === w.nonce); if (p) { p.token = w.token; p.id = w.id; } return; }
      if (h.type === P.T.SNAPSHOT) {
        const sn = P.readSnapshot(r); const p = players.byId.get(sn.you); if (!p) return;
        snaps++; snapBytes += b.length; playersSeen += sn.players.length;
        p.ackSnap = sn.seq; for (const e of sn.events) p.ev = Math.max(p.ev, e.id);
      }
    });
    await new Promise((ok) => s.bind(0, ok));
  }
  players.byId = new Map();
  for (let i = 0; i < N; i++) {
    const crowd = i < N / 2;
    const p = { i, nonce: 1000 + i, token: 0, id: 0, seq: 0, ackSnap: 0, ev: 0, sock: socks[Math.floor(i / PER_SOCK)],
      x: crowd ? Math.random() * 60 : Math.random() * 5000, z: crowd ? Math.random() * 60 : Math.random() * 5000, a: Math.random() * 6.28 };
    players.push(p);
    p.sock.send(P.msg.hello({ nonce: p.nonce, kind: P.KIND.PLAYER, outfit: i % 3, playerKey: i + 1, name: 'Load' + i }), UDP, '127.0.0.1');
    if (i % 200 === 199) await sleep(20);
  }
  await sleep(1500);
  for (const p of players) if (p.id) players.byId.set(p.id, p);
  console.log(`${players.byId.size} of ${N} joined`);
  const t0 = Date.now(); let chatN = 0;
  const step = setInterval(() => {
    for (const p of players) {
      if (!p.token) continue;
      p.a += (Math.random() - 0.5) * 0.3; p.x += Math.cos(p.a) * 0.3; p.z += Math.sin(p.a) * 0.3;
      const events = [];
      if (Math.random() < 0.0005) { events.push({ id: ++chatN, type: P.CEV.CHAT, payload: P.ev.chat('hey ' + p.i) }); }
      p.sock.send(P.msg.state({ token: p.token, seq: ++p.seq, ackSnap: p.ackSnap, ackSrvEv: p.ev, scene: 1, x: p.x, y: 0, z: p.z, yaw: (p.a * 10430) & 0xffff, anim: 1, events }), UDP, '127.0.0.1');
    }
  }, 50);
  const cpu0 = process.cpuUsage(); let srvCpu0 = cpuOf(srv.pid);
  const t1 = Date.now();
  await sleep(SECS * 1000);
  const srvCpu1 = cpuOf(srv.pid);
  clearInterval(step);
  const st = await (await fetch(`http://127.0.0.1:${HTTPP}/api/state?limit=1`)).json();
  const secs = (Date.now() - t1) / 1000;
  console.log(JSON.stringify({ online: st.online, tickMs: st.tickMs, serverCpuPct: +((srvCpu1 - srvCpu0) / secs * 100).toFixed(1),
    serverOutMBps: +(st.packets.bytesOut / ((Date.now() - t0) / 1000) / 1e6).toFixed(2),
    snapsPerPlayerPerS: +(snaps / players.byId.size / secs).toFixed(1), avgSnapBytes: Math.round(snapBytes / snaps), avgPlayersPerSnap: +(playersSeen / snaps).toFixed(1),
    chatLines: st.chat.lines }, null, 1));
  srv.kill('SIGTERM'); for (const s of socks) s.close();
  await sleep(200); fs.rmSync(DATA, { recursive: true, force: true });
  process.exit(0);
})();
function cpuOf(pid) {   // seconds of CPU the process has used
  const t = execSync(`ps -o cputime= -p ${pid}`).toString().trim();   // [[dd-]hh:]mm:ss.ss
  const parts = t.split(':').map(Number); let s = 0; for (const x of parts) s = s * 60 + x; return s;
}
