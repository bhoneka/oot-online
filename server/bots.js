'use strict';
// Test players for OoT Online: they join like games do and walk around. For trying the game side without friends.
//   node bots.js [--count 8] [--host 127.0.0.1] [--port 4697] [--around NAME] [--at x,y,z --scene N] [--radius 6]
//                [--speed 1] [--chat] [--shoot] [--code WORD] [--age mix|adult|child]
// --around NAME: they circle that player wherever they are (same scene), read from the server's /api/state (the server's
// own machine only). --shoot: now and then a bot shoots at the player it circles and reports a hit (PvP testing).
const P = require('./protocol');
const { Client } = require('./client');

const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf('--' + n); if (i < 0) return d; const v = argv[i + 1]; return v === undefined || v.startsWith('--') ? true : (typeof d === 'number' ? Number(v) : v); };
const COUNT = opt('count', 8), HOST = opt('host', '127.0.0.1'), PORT = opt('port', 4697), HTTP = opt('http', 4696);
const AROUND = opt('around', ''), RADIUS = opt('radius', 6), SPEED = opt('speed', 1), CHAT = !!opt('chat', false), SHOOT = !!opt('shoot', false);
const AGE = String(opt('age', 'mix'));   // the stand-in each bot shows as: young or adult Link, or every other one
const AT = String(opt('at', '0,0,0')).split(',').map(Number), SCENE = opt('scene', 0) >>> 0, CODE = String(opt('code', '') || '');
const NAMES = ['Saria', 'Mido', 'Darunia', 'Ruto', 'Nabooru', 'Impa', 'Rauru', 'Malon', 'Talon', 'Ingo', 'Kaepora', 'Fado', 'Dampé', 'Biggoron', 'Zora King', 'Sheik'];
const LINES = ['anyone here?', 'nice tunic', 'race you to the ranch', 'hi!', 'brb', 'where is everyone', 'this field is huge', 'ow'];

let target = null;   // the player they circle: {scene, pos}
async function follow() {
  if (!AROUND) return;
  try {
    const st = await (await fetch(`http://${HOST}:${HTTP}/api/state?limit=5000`)).json();
    const p = st.players.find((x) => x.name.toLowerCase() === AROUND.toLowerCase());
    target = p && p.pos ? { scene: p.scene, pos: p.pos, id: p.id } : null;
  } catch (_) { target = null; }
}

(async () => {
  if (AROUND) { await follow(); setInterval(follow, 250); }
  const bots = [];
  for (let i = 0; i < COUNT; i++) {
    const c = new Client({ host: HOST, port: PORT, name: NAMES[i % NAMES.length] + (i >= NAMES.length ? ' ' + (1 + Math.floor(i / NAMES.length)) : ''),
      outfit: i % 3, kind: P.KIND.BOT, code: CODE, scene: SCENE, playerKey: 0xb0700000 + i });
    try { await c.start(); } catch (e) { console.log('refused:', e.message); process.exit(1); }
    c.phase = (i / COUNT) * Math.PI * 2; c.r = RADIUS * (0.7 + 0.6 * Math.random());
    c.equip = AGE === 'child' || (AGE === 'mix' && i % 2) ? P.EQ.CHILD : 0;
    bots.push(c);
  }
  console.log(`${bots.length} bots in` + (AROUND ? `, circling ${AROUND}` : ` scene ${SCENE} at ${AT.join(',')}`));
  let t = 0;
  setInterval(() => {
    t += 0.05 * SPEED;
    for (const c of bots) {
      const center = target ? target.pos : AT, scene = target ? target.scene : SCENE;
      if (scene == null) continue;
      c.scene = scene;
      const a = c.phase + t * 0.6, x = center[0] + Math.cos(a) * c.r, z = center[2] + Math.sin(a) * c.r;
      const yaw = Math.round(((Math.atan2(Math.cos(a), -Math.sin(a)) / (2 * Math.PI)) * 65536 + 65536) % 65536);   // facing along the circle
      c.pos = { ...c.pos, x, y: center[1], z, yaw, anim: 1, flags: 0, equip: c.equip };   // anim 1 = Move (the games' list)
      if (CHAT && Math.random() < 0.0015) c.chat(LINES[Math.floor(Math.random() * LINES.length)]);
      if (SHOOT && target && target.id && Math.random() < 0.004) {
        const dx = center[0] - x, dz = center[2] - z, l = Math.hypot(dx, dz) || 1;
        c.shoot(dx / l, 0, dz / l); c.hit(target.id);
      }
    }
  }, 50);
  process.on('SIGINT', () => { for (const c of bots) c.close(); setTimeout(() => process.exit(0), 100); });
})();
