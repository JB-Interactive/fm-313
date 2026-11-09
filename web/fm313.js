// SPDX-License-Identifier: GPL-3.0-only
// The FM-313 web installer: install FM-313 on an FM-1 over Web MIDI, finish an install that was cut off, read what
// the FM-1 runs, install another package file (the way back to M-VAVE's V15). After TransitFM1's transit.js; the
// protocol is fm1ota.js (Felucca's, as ChoralRoot carries it), the package reading fm1pkg.js. The decisions run
// without a page (web/test_installer.mjs drives them against a fake FM-1); the DOM wiring is at the end.
import { Updater, classifyFirmware, IS_OTA } from "./fm1ota.js";
import { Package, productOf, logicalImage, sha256hex, STOCK_V15_SHA256 } from "./fm1pkg.js";

export const IDENTITY = "FM-1_313";               // tools/build.py PRODUCT: FM-313's own identity
const INFO_REQ = [0xF0, 0x7D, 0x46, 0x4C, 0x01, 0xF7];   // the backup protocol's INFO (Felucca, ChoralRoot, ...)

export async function deviceInfo(input, output, ms = 1500) {
  try { await input.open(); await output.open(); } catch (_) { return null; }
  return new Promise((resolve) => {
    const done = (v) => { clearTimeout(timer); input.onmidimessage = null; resolve(v); };
    const timer = setTimeout(() => done(null), ms);
    input.onmidimessage = (e) => {
      const d = e.data;
      if (d.length < 6 || d[0] !== 0xF0 || d[1] !== 0x7D || d[2] !== 0x46 || d[3] !== 0x4C || d[4] !== 1) return;
      const a = Array.from(d.slice(5, -1)), end = a.indexOf(0);
      done(String.fromCharCode(...a.slice(0, end < 0 ? a.length : end)) || null);
    };
    try { output.send(INFO_REQ); } catch (_) { done(null); }
  });
}

// what the connected FM-1 runs -> { none } | { several, list } | { loader, identity } | { identity, port, version, fw }
export async function look(access, log = () => {}, anyPort = false) {
  const up = new Updater(access, log, anyPort);
  let all = await up.all();
  if (!all.length && up.diag.silent.length) {
    // FM-313 (0.7) restarts into its update loader when the host does not collect its answer: wait for the loader
    log("No answer: waiting for the FM-1 to restart into its update mode…");
    const ota = await up.waitFor(IS_OTA, 15000);
    if (ota) { ota.link.close(); return { loader: true, identity: ota.id.text, port: ota.name, restarted: true }; }
    all = await up.all();
  }
  if (!all.length) return { none: true, diag: up.diag, anyPort };
  if (all.length > 1) return { several: true, list: all.map((d) => `${d.id.text} (${d.name})`) };
  const d = all[0];
  if (/^ota-/i.test(d.id.model)) return { loader: true, identity: d.id.text, port: d.name };
  const version = await deviceInfo(d.input, d.output);
  const fw = d.id.text === IDENTITY ? { verdict: "allow", kind: "fm313", name: "FM-313" }   // ours: an update or a way out
                                     : classifyFirmware(d.id.text, version, d.name);
  return { identity: d.id.text, port: d.name, version, fw };
}

export function describe(st) {
  if (st.none) {
    const d = st.diag || { ports: [], noOutput: [], busy: [], silent: [] }, list = (a) => a.map((n) => `“${n}”`).join(", ");
    let t = "No FM-1 found.";
    if (!d.ports.length)
      t += " The browser sees no MIDI device at all. Check that the cable carries data (many USB-C cables only charge), that the FM-1 is switched on and plugged in directly, then reload this page. On Windows, wait until the FM-1 shows up in the Device Manager.";
    else {
      if (d.busy.length) t += ` ${list(d.busy)} is in use by another program. Close DAWs, M-VAVE's app, web editors and every other tab with this page (the claude.ai preview too), then press the button again.`;
      if (d.silent.length) t += ` ${list(d.silent)} does not answer. Switch the FM-1 off and on and try again; if it keeps silent, its firmware does not take updates over USB.`;
      if (d.noOutput.length) t += ` ${list(d.noOutput)} has no matching output port.`;
      if (!d.busy.length && !d.silent.length && !d.noOutput.length)
        t += st.anyPort ? " None of the MIDI ports answered as an FM-1." : ` MIDI ports seen: ${list(d.ports)}. None is named like an FM-1: tick “Try every MIDI port” below and press the button again.`;
    }
    return t;
  }
  if (st.several) return `Several FM-1s answer (${st.list.join(", ")}). Connect only the one to install.`;
  if (st.loader && st.restarted) return `The FM-1 runs FM-313 and has restarted into its update mode (${st.identity}). Press Install, or switch the FM-1 off and on to play again.`;
  if (st.loader) return `The FM-1 is in update mode (${st.identity}): an install was cut off. Press Install to finish it.`;
  if (st.fw.kind === "fm313") return `The FM-1 runs FM-313 (${st.identity}).`;
  if (st.fw.kind === "stock") return `The FM-1 answers as the official firmware (${st.identity}): stock V15, or TransitFM1, which answers the same.`;
  if (st.fw.kind === "bubba") return `The FM-1 runs Bubba Machine (${st.identity}). Its updater accepts only M-VAVE's own package: go back to V15 with Bubba Machine's installer first.`;
  return `The FM-1 runs ${st.fw.name}` + (st.fw.verdict === "allow" ? "." : `, which is not installed over from here (an install over it has left FM-1s that no longer start).`);
}

// the FM-313 package: fetched (the page passes the bytes it carries), its SHA-256 the released one
export async function releasedPackage(rel, fetchFn = fetch) {
  const r = await fetchFn(`firmware/fm313-${rel.version}.fwsc`, { cache: "no-cache" });
  if (!r.ok) throw new Error(`The package for FM-313 ${rel.version} could not be loaded (${r.status}).`);
  const bytes = new Uint8Array(await r.arrayBuffer());
  if (await sha256hex(bytes) !== rel.sha256)
    throw new Error(`The file for FM-313 ${rel.version} is not the released one (its SHA-256 does not match). Nothing was written. Reload the page.`);
  return packageFrom(bytes, `FM-313 ${rel.version}`);
}

export async function packageFrom(bytes, label = null) {
  let product;
  try { product = productOf(bytes); } catch (e) { throw new Error("This is not an FM-1 firmware package (.fwsc)."); }
  if (!/^FM-1_\d+$/.test(product)) throw new Error(`This package is not for the FM-1 (its identity is ${product || "empty"}).`);
  let flashSize = 0;
  try { flashSize = new Package(bytes).flashSize; } catch (_) { /* the FM-1 decides */ }
  if (flashSize > 0x93000 && product === "FM-1_016")
    throw new Error("This is M-VAVE's V16. It is larger than FM-313 can install and brings a new boot area that only M-VAVE's own updater writes. Install the official V15 first, then V16 from V15 with M-VAVE's updater.");
  if (flashSize > 0x93000)
    throw new Error("This firmware is larger than FM-313 can install (as Baud Girl's is). Install the official V15 first, then this firmware with its own installer.");
  const official = await sha256hex(bytes) === STOCK_V15_SHA256;
  return { product, image: logicalImage(bytes), official,
           label: label || (official ? "the official V15 (M-VAVE's FM-1.fwsc)" : `the package ${product}`) };
}

// ui: { say(text, kind), progress(0..1), confirm(text) -> bool or a promise of one (the page: its own dialog), log(text) }
export async function install(access, pkg, ui, opts = {}) {
  const up = new Updater(access, ui.log, !!opts.anyPort);
  const st = await look(access, ui.log, !!opts.anyPort);
  if (st.none || st.several) { ui.say(describe(st), "bad"); return "refused"; }
  const step = (k, n) => {
    if (k === "start") ui.say(`Starting the update on ${n}…`);
    if (k === "verify") ui.progress(Math.min(0.05, n / 1500));
    if (k === "loader") ui.say("The FM-1 restarts into update mode…");
    if (k === "write") { ui.say("Writing. Do not unplug the FM-1 or switch it off."); ui.progress(Math.min(0.99, 0.05 + n / 1250)); }
    if (k === "reboot") ui.say("Restarting…");
    if (k === "done") ui.progress(1);
  };
  if (st.loader) {
    if (st.restarted && !(await ui.confirm(`Install ${pkg.label}? The FM-1 has restarted into its update mode for it. ` +
                  "It must be charged and connected directly to this computer; do not unplug it while it writes. (Cancel: switch the FM-1 off and on to play again.)")))
      return "cancelled";
    ui.say(`The FM-1 is in update mode (${st.identity}): ${st.restarted ? "writing" : "finishing the write of"} ${pkg.label}…`);
    const ours = pkg.product === IDENTITY;
    const ok = await up.resume(pkg.image, step, pkg.official ? { product: pkg.product } : { package: pkg.product, quietBack: ours });
    if (!ok) throw new Error("The write did not finish. Keep the FM-1 connected, switch it off and on if its screen stays black, and press Install again.");
    ui.say(`Done: ${pkg.label} is installed.`, "ok");
    return "resumed";
  }
  if (st.fw.kind === "bubba") { ui.say(describe(st), "bad"); return "refused"; }
  if (st.fw.verdict === "refuse" && !opts.override) { ui.say(describe(st), "bad"); return "refused"; }
  if (!(await ui.confirm(`Install ${pkg.label} over ${st.fw.kind === "stock" ? "the firmware that answers as V15" : st.fw.name}? ` +
                  "The FM-1 must be charged and connected directly to this computer; do not unplug it while it writes.")))
    return "cancelled";
  const fm313 = st.fw.kind === "fm313", ours = pkg.product === IDENTITY;
  try {
    await up.install(pkg.image, pkg.product, step, false, { jumps: fm313, quietBack: ours });
  } catch (e) {
    if (!(fm313 && e.code === "notfound")) throw e;
    // FM-313's answer to this second look did not get through: it restarts into its update loader, which is written to
    ui.say("The FM-1 restarts into update mode…");
    const ota = await up.waitFor(IS_OTA, 20000);
    if (!ota) throw e;
    ota.link.close();
    const ok = await up.resume(pkg.image, step, pkg.official ? { product: pkg.product } : { package: pkg.product, quietBack: ours });
    if (!ok) throw new Error("The write did not finish. Keep the FM-1 connected, switch it off and on if its screen stays black, and press Install again.");
  }
  ui.say(`Done: ${pkg.label} is installed.`, "ok");
  return "done";
}

// ---- the page (the browser only); the single-file page sets window.FM313_RELEASE = { version, sha256, b64 } ----
if (typeof document !== "undefined") {
  const $ = (id) => document.getElementById(id);
  let access = null, busy = false;
  const rel = globalThis.FM313_RELEASE;
  const ui = {
    say(text, kind = "") {
      $("status").textContent = text; $("status").className = "status " + kind;
      if (/^No FM-1 found/.test(text)) $("logbox").open = true;   // the port list is in the log
    },
    progress(f) { $("bar").hidden = false; $("bar").value = Math.round(f * 1000); },
    confirm: (t) => new Promise((res) => {          // the page's own dialog (a sandboxed frame refuses confirm())
      $("ask-text").textContent = t;
      $("ask").hidden = false;
      $("ask-yes").focus();
      const end = (v) => { $("ask").hidden = true; $("ask-yes").onclick = $("ask-no").onclick = null; res(v); };
      $("ask-yes").onclick = () => end(true);
      $("ask-no").onclick = () => end(false);
    }),
    log(t) { $("log").textContent += t + "\n"; },
  };
  const embedded = async () => {      // the package this page carries, as a fetch answer
    const s = atob(rel.b64), b = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i);
    return { ok: true, status: 200, arrayBuffer: async () => b.buffer };
  };
  const midi = async () => {
    if (!navigator.requestMIDIAccess) throw new Error("This browser has no Web MIDI: use Chrome or Edge on a computer.");
    if (!access) access = await navigator.requestMIDIAccess({ sysex: true });
    return access;
  };
  const run = async (what) => {
    if (busy) return;
    busy = true;
    document.body.classList.add("busy");
    $("bar").hidden = true;
    try { await what(); } catch (e) {
      ui.say(e && e.name === "SecurityError"
        ? "The browser refused MIDI access. Allow it (the lock icon in the address bar), or save this page as a file and open it directly in Chrome."
        : (e && e.message) || String(e), "bad");
      ui.log(String(e && e.stack || e));
    } finally {
      busy = false;
      document.body.classList.remove("busy");
    }
  };
  $("go").addEventListener("click", () => run(async () => {
    ui.say("Checking the package…");
    const pkg = await releasedPackage(rel, embedded);
    const r = await install(await midi(), pkg, ui, { override: $("override").checked, anyPort: $("anyport").checked });
    if (r === "refused" && !$("override").checked && !/Bubba/.test($("status").textContent)) $("override-box").hidden = false;
  }));
  $("check").addEventListener("click", () => run(async () => { ui.say("Looking for the FM-1…"); ui.say(describe(await look(await midi(), ui.log, $("anyport").checked))); }));
  $("file").addEventListener("change", () => { $("file-go").disabled = !$("file").files.length; });
  $("file-go").addEventListener("click", () => run(async () => {
    const f = $("file").files[0];
    const pkg = await packageFrom(new Uint8Array(await f.arrayBuffer()), null);
    await install(await midi(), pkg, ui, { override: $("override").checked, anyPort: $("anyport").checked });
  }));
  $("dl").addEventListener("click", async () => {
    const r = await embedded();
    const a = Object.assign(document.createElement("a"), {
      href: URL.createObjectURL(new Blob([await r.arrayBuffer()])), download: `fm313-${rel.version}.fwsc` });
    a.click();
  });
  if (rel) {
    $("go").disabled = false;
    $("ver").textContent = rel.version;
    $("ver2").textContent = rel.version;
    $("sha").textContent = rel.sha256;
  }
}
