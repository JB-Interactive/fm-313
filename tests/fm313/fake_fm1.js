// SPDX-License-Identifier: GPL-3.0-only
// a fake FM-1 for tests/fm313/browser_test.py, after web/test_installer.mjs (the installer's own test)
(() => {
const realTimeout = window.setTimeout.bind(window);
window.setTimeout = (f, ms, ...a) => realTimeout(f, ms >= 200 ? ms / 20 : ms, ...a);
const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];
const IDENTITY = "FM-1_313";
function pack7(data) {
  const out = []; let acc = 0, nb = 0;
  for (const b of data) {
    acc |= b << nb; nb += 8;
    while (nb >= 7) { out.push(acc & 0x7F); acc >>>= 7; nb -= 7; }
  }
  if (nb) out.push(acc & 0x7F);
  return out;
}
function unpack7(s) {
  const out = []; let acc = 0, nb = 0;
  for (const b of s) {
    acc |= (b & 0x7F) << nb; nb += 7;
    while (nb >= 8) { out.push(acc & 0xFF); acc >>>= 8; nb -= 8; }
  }
  return out;
}
class FakeFM1 {
  // fm313: the running firmware is FM-313 0.7 (firmware/src/fm313.c hs_poll): "jump" answers the handshake and restarts
  // into its loader on UPGRADE; "stuck" gets its first answer through and restarts into the loader at the next
  // handshake; "silent" restarts at the first. After the write the new FM-313 is "final": it must not be asked
  constructor(image, { identity = "FM-1_015", name = "FM-1", version = null, loader = "ota-FM-1_900", final = IDENTITY, fm313 = null } = {}) {
    this.image = image; this.served = 0; this.bad = 0; this.upgrades = 0; this.version = version;
    this.loader = loader; this.final = final; this.fm313 = fm313; this.hs = 0; this.postHs = 0;
    this.access = { inputs: new Map(), outputs: new Map() };
    this.boot(identity, name);
  }
  boot(identity, name) {
    this.identity = identity; this.waiting = null; this.queue = [];
    for (const m of [this.access.inputs, this.access.outputs]) { for (const p of m.values()) p.state = "disconnected"; m.clear(); }
    const id = Math.random().toString(36).slice(2);
    this.input = { id: "i" + id, name, state: "connected", onmidimessage: null, open: async () => {} };
    this.output = { id: "o" + id, name, state: "connected", open: async () => {}, send: (d) => {
      if (this.output.state !== "connected") throw new Error("InvalidStateError");
      realTimeout(() => this.rx(Array.from(d)), 1);
    } };
    this.access.inputs.set(this.input.id, this.input);
    this.access.outputs.set(this.output.id, this.output);
  }
  tx(bytes) { const i = this.input; realTimeout(() => { if (i.state === "connected" && i.onmidimessage) i.onmidimessage({ data: Uint8Array.from(bytes) }); }, 1); }
  rx(d) {
    if (d[0] === 0xF0 && d[1] === 0x7D && d[2] === 0x46 && d[3] === 0x4C) {     // INFO
      if (d[4] === 1 && this.version && !this.identity.startsWith("ota-"))
        this.tx([0xF0, 0x7D, 0x46, 0x4C, 1, ...new TextEncoder().encode(this.version), 0, 0xF7]);
      return;
    }
    const playing = this.fm313 && !this.identity.startsWith("ota-");
    if (playing && eq(d, HS)) {
      if (this.fm313 === "final") { this.postHs++; return; }
      this.hs++;
      if (this.fm313 === "silent" || (this.fm313 === "stuck" && this.hs > 1)) {
        realTimeout(() => this.boot(this.loader, "Felucca Update"), 30);
        return;
      }
    }
    if (playing && eq(d, UPGRADE)) {
      if (this.fm313 !== "final") realTimeout(() => this.boot(this.loader, "Felucca Update"), 30);
      return;
    }
    if (eq(d, HS)) {
      const t = [...new TextEncoder().encode(this.identity)];
      this.tx([0xF0, ...pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]);
    } else if (eq(d, UPGRADE)) {
      this.upgrades++;
      this.queue = this.identity.startsWith("ota-")
        ? [...Array.from({ length: 6 }, (_, k) => [k * 512, 512]), [0xF0000000, 8]]
        : [[0, 64], [0x40, 160], [0x1000, 512], [0xE0000000, 8]];
      this.next();
    } else if (this.waiting) {
      const u = unpack7(d.slice(1, -1));
      const [addr, len] = this.waiting;
      const got = u.slice(14, 14 + (addr >= 0xE0000000 ? 8 : len));
      const want = addr >= 0xE0000000 ? [...new TextEncoder().encode("success"), 0] : Array.from(this.image.subarray(addr, addr + len));
      if (!eq(got, want)) this.bad++;
      this.waiting = null;
      this.served++;
      if (addr === 0xE0000000) realTimeout(() => this.boot(this.loader, "Felucca Update"), 20);
      else if (addr === 0xF0000000) realTimeout(() => { if (this.fm313) this.fm313 = "final"; this.boot(this.final, "FM-1"); }, 20);
      else this.next();
    }
  }
  next() {
    const r = this.queue.shift();
    if (!r) return;
    this.waiting = r;
    const [addr, len] = r;
    const u = [0, 0x59, 0x30, 0, 0, 0, 0, addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF, len & 0xFF, len >> 8, 0];
    let s = 0;
    for (let i = 6; i < 14; i++) s += u[i];
    u.push(~s & 0xFF);
    this.tx([0xF0, ...pack7(u), 0xF7]);
  }
}

const dev = new FakeFM1(new Uint8Array(1 << 20), window.__FAKE || { identity: "FM-1_900", name: "FM-1", version: "FELUCCA v1.8.2-beta" });
window.__dev = dev;
Object.defineProperty(navigator, "requestMIDIAccess", { value: async () => dev.access });
})();