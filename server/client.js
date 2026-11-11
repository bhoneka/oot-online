'use strict';
// A JavaScript OoT Online client: what the game-side mod does over the network, without a game. bots.js and the tests
// use it; the C client in the nxnative host follows the same rules:
//  - HELLO until WELCOME (or REJECT), then STATE at the server's tick rate with the newest position;
//  - our events (chat, shots, hits, deaths) stay queued and ride in every STATE until a SNAPSHOT acks their id;
//  - the server's events are applied once each, in id order (a jump in ids means the server dropped old ones);
//  - silence for 3 s: HELLO again with the token we had, which gets the same session back while the server keeps it.
const dgram = require('dgram');
const EventEmitter = require('events');
const P = require('./protocol');

class Client extends EventEmitter {
  constructor({ host = '127.0.0.1', port = 4697, name = 'Hero', outfit = 0, kind = P.KIND.PLAYER, code = '', build = 0, rateHz = 0, scene = 1 } = {}) {
    super();
    Object.assign(this, { host, port, name, outfit, kind, code, build, rateHz, scene });
    this.sock = dgram.createSocket('udp4');
    this.nonce = (Math.random() * 0xffffffff) >>> 0;
    this.playerKey = arguments[0]?.playerKey ?? (((Math.random() * 0xfffffffe) >>> 0) + 1);
    this.token = 0; this.prevToken = 0; this.id = 0;
    this.seq = 0; this.lastSnap = 0; this.srvEvDone = 0;
    this.outQ = []; this.nextEvId = 1;
    this.pos = { x: 0, y: 0, z: 0, yaw: 0, anim: 0, flags: 0, hp: 12, maxHp: 12, equip: 0 };
    this.players = new Map();   // id -> last snapshot entry
    this.names = new Map();     // id -> {name, kind, outfit}
    this.lastHeard = 0; this.snapCount = 0; this.bytesIn = 0;
    this.sock.on('message', (b) => this.onMessage(b));
    this.sock.on('error', (e) => this.emit('error', e));
  }
  start() {
    this.hello();
    this.timer = setInterval(() => this.step(), 1000 / (this.rateHz || 20));
    return new Promise((ok, fail) => { this.once('welcome', ok); this.once('reject', fail); });
  }
  hello() {
    this.lastHello = Date.now();
    this.send(P.msg.hello({ nonce: this.nonce, kind: this.kind, outfit: this.outfit, build: this.build, playerKey: this.playerKey, name: this.name, prevToken: this.prevToken, code: this.code }));
  }
  send(b) { try { this.sock.send(b, this.port, this.host); } catch (_) { /* closed */ } }
  queue(type, payload) { this.outQ.push({ id: this.nextEvId++, type, payload }); }
  chat(text) { this.queue(P.CEV.CHAT, P.ev.chat(text)); }
  shoot(dx, dy, dz, kind = 0) { const p = this.pos; this.queue(P.CEV.SHOT, P.ev.shot(p.x, p.y + 1.2, p.z, dx, dy, dz, kind)); }
  hit(target, kind = P.HIT_KIND.ARROW) { this.queue(P.CEV.HIT, P.ev.hit(target, kind)); }
  died(killer = P.NOBODY, how = P.HOW.PLAYER) { this.queue(P.CEV.DIED, P.ev.died(killer, how)); }
  setOutfit(o) { this.outfit = o; this.queue(P.CEV.OUTFIT, P.ev.outfit(o)); }
  step() {
    const t = Date.now();
    if (!this.token) { if (t - this.lastHello > 500) this.hello(); return; }
    if (this.lastHeard && t - this.lastHeard > 3000 && t - this.lastHello > 1000) { this.prevToken = this.token; this.hello(); }
    const p = this.pos;
    this.send(P.msg.state({ token: this.token, seq: ++this.seq, ackSnap: this.lastSnap, ackSrvEv: this.srvEvDone, scene: this.scene,
      x: p.x, y: p.y, z: p.z, yaw: p.yaw, anim: p.anim, flags: p.flags, hp: p.hp, maxHp: p.maxHp, equip: p.equip, events: this.outQ }));
  }
  onMessage(b) {
    this.bytesIn += b.length;
    const h = P.readHeader(b); if (!h || h.version !== P.VERSION) return;
    const r = new P.Reader(b); r.o = 4;
    try {
      switch (h.type) {
        case P.T.WELCOME: {
          const w = P.readWelcome(r); if (w.nonce !== this.nonce) return;
          const first = !this.token;
          this.id = w.id; this.token = w.token; this.name = w.name; this.lastHeard = Date.now(); this.tickHz = w.tickHz;
          if (!this.rateHz) { clearInterval(this.timer); this.timer = setInterval(() => this.step(), 1000 / w.tickHz); }
          if (first) this.emit('welcome', w); else this.emit('rejoin', w);
          return;
        }
        case P.T.REJECT: { const x = P.readReject(r); if (x.nonce === this.nonce) this.emit('reject', Object.assign(new Error(x.detail || P.REJECT_TEXT[x.reason]), { reason: x.reason })); return; }
        case P.T.SNAPSHOT: {
          const s = P.readSnapshot(r);
          this.lastHeard = Date.now(); this.snapCount++;
          if (s.seq <= this.lastSnap) return;
          this.lastSnap = s.seq; this.youFlags = s.youFlags; this.online = s.online;
          // our events the server has: off the queue
          let i = 0; while (i < this.outQ.length && this.outQ[i].id <= s.ackCliEv) i++; if (i) this.outQ.splice(0, i);
          for (const e of s.events) {
            if (e.id <= this.srvEvDone) continue;
            this.srvEvDone = e.id;
            const fn = P.parseServer[e.type]; if (!fn) continue;
            const d = fn(new P.Reader(e.payload));
            this.emit('event', e.type, d);
            if (e.type === P.SEV.CHAT) this.emit('chat', d);
          }
          for (const x of s.shots) this.emit('shot', x);
          this.players.clear(); for (const p of s.players) this.players.set(p.id, p);
          const unknown = s.players.filter((p) => !this.names.has(p.id)).map((p) => p.id);
          if (unknown.length) this.send(P.msg.infoReq(this.token, unknown));
          this.emit('snapshot', s);
          return;
        }
        case P.T.INFO: for (const x of P.readInfo(r)) this.names.set(x.id, x); return;
        case P.T.PONG: { const ct = r.u32(); this.emit('pong', Date.now() - ct); return; }
      }
    } catch (e) { if (!(e instanceof RangeError)) throw e; }
  }
  close(bye = true) { clearInterval(this.timer); if (bye && this.token) this.send(P.msg.bye(this.token)); setTimeout(() => { try { this.sock.close(); } catch (_) { /* closed */ } }, 20); }
}
module.exports = { Client };
