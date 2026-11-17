// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Himmel Perform (M11). Talks only to himmel_ui:
//   GET /events (SSE: bridge, catalog, live, transport, mk2), POST /cmd (JSON commands), GET /preview/N (RGBA),
//   GET /names (transport enum names), GET /fxmeta (fx/<id>/effect.json), GET /weapons/<id>.weapon.json.
// The page never touches the show directly (gui.md §1.5): every change is a command, and what you
// see is the engine's state coming back. Killing this page or the bridge changes nothing on air.
'use strict';

const $ = id => document.getElementById(id);
const BLEND = ['Normal', 'Add', 'Screen', 'Multiply', 'Difference', 'Lighten'];
const GROUPS = { prev: ['A', 'B', 'C', 'D'], prog: ['E', 'F', 'G', 'H'] };
const CODEC = ['HAP', 'HAP α', 'HAP Q', '?'];
const S = {
  cat: null, live: null, bridge: null, tr: null, mk2: null,
  // M11 part 2 (optional blocks; {present:false} = hide the panel): panels.js
  show: null, outputs: null, audio: null, osc: null, library: null, weapons: null,
  names: null,          // GET /names
  meta: [],             // per catalog effect id: its effect.json (or null)
  sse: false, lastBridgeMs: 0,
  armed: -1,            // clip id waiting for a layer click
  tab: 'clip', lib: 'clips', view: 'perform',
  sent: 0, lastSeq: 0,
  seenRejected: null,
};
window.S = S;

// ------------------------------------------------------------------ commands
async function send(cmd) {
  try {
    const r = await fetch('/cmd', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(cmd) });
    const j = await r.json();
    const list = Array.isArray(j) ? j : [j];
    for (const x of list) { if (!x.ok) toast(x.error || 'refused by the bridge', 'bad'); else { S.sent++; S.lastSeq = x.seq; } }
    return j;
  } catch (e) {
    toast('himmel_ui is not reachable', 'bad');
    return { ok: false };
  }
}
// One stream of values per control: at most ~30 per second, the last value always goes out.
const pending = new Map();
function sendT(key, cmd) {
  const p = pending.get(key);
  if (p) { p.cmd = cmd; return; }
  send(cmd);
  const slot = { cmd: null };
  pending.set(key, slot);
  setTimeout(function flush() {
    if (slot.cmd) { const c = slot.cmd; slot.cmd = null; send(c); setTimeout(flush, 33); }
    else pending.delete(key);
  }, 33);
}

// A control the hand is on is not overwritten by the state coming back.
function hold(el) {
  el.addEventListener('pointerdown', () => { el._hold = Infinity; });
  const rel = () => { el._hold = performance.now() + 400; };
  el.addEventListener('pointerup', rel); el.addEventListener('change', rel); el.addEventListener('blur', rel);
  el.addEventListener('keydown', () => { el._hold = performance.now() + 400; });
  el.addEventListener('focus', () => { if (el.type === 'number') el._hold = Infinity; });
}
function setVal(el, v) {
  if (performance.now() < (el._hold || 0)) return;
  if (String(el.value) !== String(v)) el.value = v;
}

// ------------------------------------------------------------------ helpers
const progDeck = () => (S.live ? S.live.program_deck : 0);
const sideDeck = side => (side === 'prog' ? progDeck() : 1 - progDeck());
const deckLetter = d => (d === 0 ? 'A' : 'B');
const rgb = n => '#' + (n >>> 0).toString(16).padStart(6, '0').slice(-6);
const clipInfo = id => (S.cat && id >= 0 && id < S.cat.clips.length ? S.cat.clips[id] : null);
const fxInfo = id => (S.cat && id >= 0 && id < S.cat.effects.length ? S.cat.effects[id] : null);
const fxMeta = id => S.meta[id] || null;
const engineOk = () => S.sse && S.bridge && S.bridge.engine === 'ok';
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
const sleep = ms => new Promise(r => setTimeout(r, ms));
const clipName = ci => ci.name.replace(/\.mov$/i, '');
const groupOf = (d, l) => GROUPS[d === progDeck() ? 'prog' : 'prev'][l];
window.H = { send, sendT, hold, setVal, el, toast: (...a) => toast(...a), invalidate: () => invalidate(), clipInfo, fxInfo, fxMeta, rgb, setTab: t => setTab(t) };

function toast(msg, kind) {
  const t = el('div', 'toast' + (kind === 'bad' ? ' bad' : ''), msg);
  $('toasts').append(t);
  while ($('toasts').children.length > 4) $('toasts').firstChild.remove();
  setTimeout(() => t.remove(), 4000);
}

// A knob position 0..1 in the effect's real units (effect-metadata.md §2).
function fmtParam(pm, v) {
  if (!pm) return Math.round(v * 100) + '';
  const lo = pm.min ?? 0, hi = pm.max ?? 1;
  if (pm.steps >= 2) {
    const i = Math.min(pm.steps - 1, Math.floor(v * pm.steps));
    if (pm.labels && pm.labels[i] != null) return String(pm.labels[i]);
    return fmtNum(lo + (hi - lo) * i / (pm.steps - 1)) + (pm.unit ? ' ' + pm.unit : '');
  }
  const x = pm.curve === 'exp' && lo > 0 ? lo * Math.pow(hi / lo, v) : lo + (hi - lo) * v;
  return fmtNum(x) + (pm.unit ? ' ' + pm.unit : '');
}
function fmtNum(x) { const a = Math.abs(x); return a >= 100 ? x.toFixed(0) : a >= 10 ? x.toFixed(1) : x.toFixed(2); }
const fmtBeats = b => (b >= 1 ? (+b.toFixed(2)) + '' : '1/' + Math.round(1 / b));

// ------------------------------------------------------------------ layer strips
const strips = { prev: [], prog: [] };
function buildStrips() {
  for (const side of ['prev', 'prog']) {
    const box = $(side === 'prev' ? 'stripsPrev' : 'stripsProg');
    for (let row = 0; row < 4; row++) {
      const s = el('div', 'strip');
      const grp = el('div', 'grp', GROUPS[side][row]);
      grp.title = (side === 'prev' ? 'PREVIEW' : 'PROGRAM') + ' layer ' + (row + 1) + ' (MK2 group ' + GROUPS[side][row] + ')';
      const clip = el('div', 'clip');
      const sw = el('div', 'sw'), nm = el('div', 'nm empty', 'empty'), miss = el('span', 'miss hidden', 'MISSING');
      const badge = el('span', 'badge hidden');
      const pos = el('div', 'pos'); pos.append(el('i'));
      clip.append(sw, nm, miss, badge, pos);
      const op = el('label', 'op'); const opIn = el('input'); opIn.type = 'range'; opIn.min = 0; opIn.max = 1; opIn.step = 0.01; opIn.title = 'Opacity';
      const opV = el('span', null, '100'); op.append(opIn, opV);
      const blend = el('select', 'blend'); blend.title = 'Blend mode';
      BLEND.forEach((b, i) => { const o = el('option', null, b); o.value = i; blend.append(o); });
      const mute = el('button', 'mute', 'M'); mute.title = 'Mute';
      const play = el('button', 'play', '❚❚'); play.title = 'Play / pause';
      const fx = el('button', 'fx', 'FX'); fx.title = 'Effects of this layer (in the panel below)';
      s.append(grp, clip, op, blend, mute, play, fx);
      box.append(s);

      const t = () => ({ deck: sideDeck(side), layer: row });
      hold(opIn);
      opIn.addEventListener('input', () => { opV.textContent = Math.round(opIn.value * 100); sendT('op' + side + row, { type: 'LayerOpacity', ...t(), value: +opIn.value }); });
      opIn.addEventListener('dblclick', () => send({ type: 'LayerOpacity', ...t(), value: 1 }));
      blend.addEventListener('change', () => send({ type: 'LayerBlend', ...t(), value: +blend.value }));
      mute.addEventListener('click', () => send({ type: 'LayerMute', ...t(), value: 2 }));
      play.addEventListener('click', () => send({ type: 'LayerPlay', ...t(), value: 'Toggle' }));
      fx.addEventListener('click', () => { send({ type: 'Focus', ...t() }); setTab('layer'); });
      // Clicking the strip itself (label, clip, empty space) = press the group: focus it, or drop the armed clip.
      s.addEventListener('click', e => {
        if (e.target.closest('input,select,button')) return;
        if (S.armed >= 0) { send({ type: 'LayerClip', ...t(), value: S.armed }); if (!e.shiftKey) arm(-1); }
        send({ type: 'Focus', ...t() });
      });
      s.addEventListener('dblclick', e => { if (!e.target.closest('input,select,button')) setTab('clip'); });
      s.addEventListener('dragover', e => {
        const ty = e.dataTransfer.types;
        if (ty.includes('text/x-himmel-clip') || ty.includes('text/x-himmel-gen')) { e.preventDefault(); s.classList.add('dropover'); }
      });
      s.addEventListener('dragleave', () => s.classList.remove('dropover'));
      s.addEventListener('drop', e => {
        s.classList.remove('dropover');
        const id = parseInt(e.dataTransfer.getData('text/x-himmel-clip'), 10);
        const gen = parseInt(e.dataTransfer.getData('text/x-himmel-gen'), 10);
        e.preventDefault();
        if (id >= 0) send({ type: 'LayerClip', ...t(), value: id });
        else if (gen >= 0) putGenerator(t(), gen);
        send({ type: 'Focus', ...t() });
      });
      strips[side].push({ s, sw, nm, miss, badge, pos: pos.firstChild, opIn, opV, blend, mute, play, fx });
    }
  }
}

// The layer's source in words: a clip, a generator on an empty layer, or nothing.
function layerSource(ls) {
  const ci = clipInfo(ls.clip);
  if (ci) return { name: clipName(ci), color: rgb(ci.thumb_rgb), kind: 'clip', ci };
  const g = ls.fx.find(f => f.effect >= 0 && fxInfo(f.effect)?.kind === 'generator');
  if (g) return { name: fxInfo(g.effect).name, color: 'var(--accent)', kind: 'gen' };
  return null;
}

function renderStrips() {
  const L = S.live;
  for (const side of ['prev', 'prog']) {
    const d = sideDeck(side);
    strips[side].forEach((u, row) => {
      const ls = L.deck[d].layer[row];
      const tr = S.tr ? S.tr.layer[d][row] : null;
      const src = layerSource(ls);
      u.sw.style.background = src ? src.color : '#000';
      u.sw.classList.toggle('gen', !!src && src.kind === 'gen');
      u.nm.textContent = src ? src.name : (ls.clip >= 0 ? 'clip ' + ls.clip : 'empty');
      u.nm.classList.toggle('empty', !src);
      u.nm.title = src && src.ci ? `${src.ci.name} · ${src.ci.width}×${src.ci.height} · ${src.ci.fps} fps · ${src.ci.seconds.toFixed(1)} s` : src ? 'generator (the layer has no clip)' : '';
      u.miss.classList.toggle('hidden', !ls.missing);
      u.pos.style.width = (src && src.ci ? ls.position * 100 : 0) + '%';
      // A small badge: the transport mode / beat loop / a launch waiting for the snap.
      let badge = '';
      if (tr && src && src.ci) {
        if (tr.pending) badge = '⏱ ' + tr.pending_beats;
        else if (tr.beat_loop) badge = '⟳ ' + fmtBeats(tr.beat_loop_beats);
        else if (tr.scratching) badge = 'SCR';
        else if (tr.mode !== 'Loop') badge = tr.mode.toUpperCase();
      }
      u.badge.textContent = badge; u.badge.classList.toggle('hidden', !badge);
      u.badge.classList.toggle('wait', !!(tr && tr.pending));
      setVal(u.opIn, ls.opacity);
      if (performance.now() >= (u.opIn._hold || 0)) u.opV.textContent = Math.round(ls.opacity * 100);
      if (document.activeElement !== u.blend) setVal(u.blend, ls.blend);
      u.mute.classList.toggle('on', ls.mute);
      u.play.textContent = ls.playing ? '❚❚' : '▶';
      u.play.classList.toggle('off', !ls.playing);
      u.play.title = ls.playing ? 'Playing (click = pause)' : 'Paused (click = play)';
      const used = ls.fx.filter(f => f.effect >= 0);
      u.fx.textContent = used.length ? (fxInfo(used[0].effect)?.name || 'FX') + (used.length > 1 ? ' +' + (used.length - 1) : '') : 'FX';
      u.fx.classList.toggle('has', used.length > 0);
      u.s.classList.toggle('muted', ls.mute);
      u.s.classList.toggle('focus', L.focus_deck === d && L.focus_layer === row);
      u.s.classList.toggle('armed', S.armed >= 0);
    });
  }
}

// ------------------------------------------------------------------ library (clips + generators)
let clipEls = [];
function buildLibrary() {
  const list = $('clipList');
  list.textContent = '';
  clipEls = [];
  if (!S.cat) return;
  const f = $('clipFilter').value.trim().toLowerCase();
  if (S.lib === 'gens') {
    $('libFoot').textContent = 'Click = put the generator on the focused layer (slot 1 if it is free, else the first free slot). On an empty layer the generator is the picture; over a clip it paints on top. Drag onto a layer works too.';
    for (const e of S.cat.effects) {
      if (e.kind !== 'generator') continue;
      if (f && !e.name.toLowerCase().includes(f)) continue;
      const m = fxMeta(e.id);
      const it = el('div', 'ci gen');
      it.draggable = true;
      const sw = el('div', 'sw gensw', '✦');
      const txt = el('div', 'txt');
      txt.append(el('div', 'nm', e.name), el('div', 'meta', (m ? m.family + ' · cost ' + (m.cost?.class || '?') : 'generator')));
      it.append(sw, txt);
      it.title = (m?.help || []).join('\n');
      it.addEventListener('click', () => {
        const L = S.live;
        if (!L || L.focus_deck < 0) { toast('No layer has the focus: click a layer first'); return; }
        putGenerator({ deck: L.focus_deck, layer: L.focus_layer }, e.id);
      });
      it.addEventListener('dragstart', ev => { ev.dataTransfer.setData('text/x-himmel-gen', String(e.id)); ev.dataTransfer.effectAllowed = 'copy'; });
      list.append(it);
    }
    return;
  }
  $('libFoot').textContent = 'Click a clip, then a layer. Or drag it onto a layer. Double-click = into the focused layer. Alt + click = fire it on the HIT layer.';
  for (const c of S.cat.clips) {
    if (f && !c.name.toLowerCase().includes(f)) continue;
    const it = el('div', 'ci');
    it.draggable = true;
    const sw = el('div', 'sw'); sw.style.background = `linear-gradient(135deg, #000, ${rgb(c.thumb_rgb)})`;
    const tags = el('div', 'tags'); sw.append(tags);
    const txt = el('div', 'txt');
    txt.append(el('div', 'nm', clipName(c)),
      el('div', 'meta', `${c.width}×${c.height} · ${Math.round(c.fps)} · ${c.seconds.toFixed(0)}s · ${CODEC[c.codec] || '?'}`));
    it.append(sw, txt);
    it.title = c.name + '\nClick, then click a layer. Drag onto a layer. Double-click = into the focused layer. Alt + click = HIT.';
    it.addEventListener('click', e => {
      if (e.altKey) { fireHit(c.id); return; }
      arm(S.armed === c.id ? -1 : c.id);
    });
    it.addEventListener('dblclick', () => {
      const L = S.live;
      if (!L || L.focus_deck < 0) { toast('No layer has the focus: click a layer first'); return; }
      send({ type: 'LayerClip', deck: L.focus_deck, layer: L.focus_layer, value: c.id });
      arm(-1);
    });
    it.addEventListener('dragstart', e => { e.dataTransfer.setData('text/x-himmel-clip', String(c.id)); e.dataTransfer.effectAllowed = 'copy'; });
    list.append(it);
    clipEls.push({ id: c.id, it, tags });
  }
  renderLibrary();
}
function renderLibrary() {
  const L = S.live;
  for (const c of clipEls) {
    c.it.classList.toggle('armed', S.armed === c.id);
    if (!L) continue;
    let inPrev = false, inProg = false;
    L.deck.forEach((dk, d) => dk.layer.forEach(l => { if (l.clip === c.id) { if (d === L.program_deck) inProg = true; else inPrev = true; } }));
    const want = (inPrev ? 'p' : '') + (inProg ? 'l' : '');
    if (c.tags.dataset.k !== want) {
      c.tags.dataset.k = want; c.tags.textContent = '';
      if (inPrev) c.tags.append(el('i', 'p'));
      if (inProg) c.tags.append(el('i', 'l'));
    }
  }
}
function arm(id) {
  S.armed = id;
  const ci = clipInfo(id);
  $('armHint').textContent = ci ? `Click a layer to load “${clipName(ci)}” (Esc = cancel, Shift = keep)` : '';
  renderLibrary();
  if (S.live) renderStrips();
}
// A generator as the source (effect-metadata.md §6): the first free slot of the layer.
function putGenerator(t, id) {
  const L = S.live;
  const fx = L.deck[t.deck].layer[t.layer].fx;
  let slot = fx.findIndex(f => f.effect === id);
  if (slot < 0) slot = fx.findIndex(f => f.effect < 0);
  if (slot < 0) { toast('All 4 FX slots of this layer are in use'); return; }
  send({ type: 'FxSet', ...t, slot, value: id });
  send({ type: 'Focus', ...t });
}
function fireHit(clip) {
  send({ type: 'LayerTrigger', deck: 'hit', value: clip, velocity: 1 });
  toast('HIT: ' + clipName(clipInfo(clip)));
}

// ------------------------------------------------------------------ FX slots (layer / deck / master)
const slots = [];
function buildSlots() {
  const box = $('slots');
  for (let k = 0; k < 4; k++) {
    const c = el('div', 'slot');
    const sh = el('div', 'sh');
    const n = el('span', 'n', String(k + 1));
    const pick = el('button', 'pick', '— empty —'); pick.title = 'Choose an effect (opens the effect browser)';
    const byp = el('button', 'byp', 'BYP'); byp.title = 'Bypass';
    sh.append(n, pick, byp);
    const mix = el('div', 'prm');
    const mixIn = el('input'); mixIn.type = 'range'; mixIn.min = 0; mixIn.max = 1; mixIn.step = 0.01;
    const mixV = el('span', 'pv');
    mix.append(el('span', 'pn', 'MIX'), mixIn, mixV);
    const params = el('div', 'params');
    const foot = el('div', 'sfoot');
    const presets = el('div', 'presets');
    const press = el('button', 'press'); press.title = 'PRESS: hold to push the PRESS target (a pen / touch screen gives pressure). Released, the engine shapes the fall.';
    const pressBar = el('i'); const pressTxt = el('span', null, 'PRESS'); press.append(pressBar, pressTxt);
    foot.append(presets, press);
    const empty = el('div', 'empty', 'empty slot · click the name to choose');
    c.append(sh, mix, params, foot, empty);
    box.append(c);
    hold(mixIn);
    const u = { c, pick, byp, mix, mixIn, mixV, params, foot, presets, press, pressBar, pressTxt, empty, key: '', rows: [] };
    pick.addEventListener('click', () => openBrowser(k));
    byp.addEventListener('click', () => send({ type: 'FxBypass', ...fxTarget(), slot: k, value: 2 }));
    mixIn.addEventListener('input', () => { mixV.textContent = Math.round(mixIn.value * 100); sendT('mix' + k, { type: 'FxMix', ...fxTarget(), slot: k, value: +mixIn.value }); });
    mixIn.addEventListener('dblclick', () => send({ type: 'FxMix', ...fxTarget(), slot: k, value: 1 }));
    // PRESS: pointer down = pressure (pen / touch pressure when there is one, else full), up = 0.
    let pressing = null;
    const pv = e => (e.pointerType !== 'mouse' && e.pressure > 0 ? e.pressure : 1);
    press.addEventListener('pointerdown', e => { pressing = fxTarget(); press.setPointerCapture(e.pointerId); press.classList.add('held'); send({ type: 'FxPress', ...pressing, slot: k, value: pv(e) }); });
    press.addEventListener('pointermove', e => { if (pressing && e.pointerType !== 'mouse' && e.pressure > 0) sendT('press' + k, { type: 'FxPress', ...pressing, slot: k, value: e.pressure }); });
    const up = () => { if (!pressing) return; send({ type: 'FxPress', ...pressing, slot: k, value: 0 }); pressing = null; press.classList.remove('held'); };
    press.addEventListener('pointerup', up); press.addEventListener('pointercancel', up); press.addEventListener('lostpointercapture', up);
    slots.push(u);
  }
}
// What the FX panel acts on: the focused layer, its deck, or master.
function focusTarget() {
  const L = S.live;
  const fd = L && L.focus_deck >= 0 ? L.focus_deck : 1 - progDeck();
  return { deck: fd, layer: L && L.focus_layer >= 0 ? L.focus_layer : 0 };
}
function fxTarget() {
  const t = focusTarget();
  if (S.tab === 'master') return { deck: 'master' };
  if (S.tab === 'deck') return { deck: t.deck };
  return t;
}
function fxSlotsNow() {
  const L = S.live, t = fxTarget();
  if (t.deck === 'master') return L.master_fx;
  if (t.layer === undefined) return L.deck[t.deck].fx;
  return L.deck[t.deck].layer[t.layer].fx;
}
function setTab(tab) {
  S.tab = tab;
  for (const b of document.querySelectorAll('.tabs > button')) b.classList.toggle('on', b.dataset.tab === tab);
  slots.forEach(u => (u.key = ''));
  $('clipEd').classList.toggle('hidden', tab !== 'clip');
  $('slots').classList.toggle('hidden', !['layer', 'deck', 'master'].includes(tab));
  $('hitEd').classList.toggle('hidden', tab !== 'hit');
  $('scenesEd').classList.toggle('hidden', tab !== 'scenes');
  if (window.Panels) window.Panels.tabShown(tab);
  if (S.live) renderInspector();
}
// A preset's values (by param id) -> FxParam per param; missing ones = the default (effect-metadata.md §2).
function applyPreset(k, info, m, preset) {
  const t = fxTarget();
  const cmds = m.params.map((pm, p) => ({ type: 'FxParam', ...t, slot: k, param: p, value: preset.values[pm.id] ?? pm.default ?? info.defaults[p] ?? 0 }));
  send(cmds);
}

function renderFxSlots() {
  const L = S.live, t = fxTarget();
  const fx = fxSlotsNow();
  slots.forEach((u, k) => {
    const f = fx[k];
    const info = fxInfo(f.effect), m = fxMeta(f.effect);
    const key = JSON.stringify(t) + ':' + f.effect + ':' + (m ? 1 : 0);
    if (u.key !== key) {                      // a new effect in this slot: rebuild its parameter rows
      u.key = key;
      u.params.textContent = ''; u.rows = []; u.presets.textContent = '';
      (info?.params || []).forEach((pn, p) => {
        const pm = m?.params?.[p];
        const row = el('div', 'prm');
        const inp = el('input'); inp.type = 'range'; inp.min = 0; inp.max = 1; inp.step = pm?.steps >= 2 ? 1 / (pm.steps - 1) / 2 : 0.005;
        const pv = el('span', 'pv');
        const name = el('span', 'pn', pn);
        if (m && m.main && m.main.includes(pm?.id)) name.classList.add('main');
        if (m && m.press && m.press.param === pm?.id) name.classList.add('ptarget');
        row.append(name, inp, pv);
        hold(inp);
        inp.addEventListener('input', () => { pv.textContent = fmtParam(pm, +inp.value); sendT('fx' + k + '/' + p, { type: 'FxParam', ...fxTarget(), slot: k, param: p, value: +inp.value }); });
        inp.addEventListener('dblclick', () => send({ type: 'FxParam', ...fxTarget(), slot: k, param: p, value: info.defaults[p] ?? 0 }));
        inp.title = pn + (pm ? ` (${pm.min}..${pm.max} ${pm.unit || ''})` : '') + ' · double-click = default' +
          (m?.press?.param === pm?.id ? ' · PRESS target' : '') + (m?.main?.includes(pm?.id) ? ' · main param (knobs 1–2)' : '');
        u.params.append(row);
        u.rows.push({ inp, pv, pm });
      });
      if (m && m.presets) {
        for (const pr of m.presets) {
          const b = el('button', 'chip2', pr.name);
          b.title = 'Preset “' + pr.name + '”: ' + Object.entries(pr.values).map(([a, v]) => a + ' ' + v).join(', ');
          b.addEventListener('click', () => applyPreset(k, info, m, pr));
          u.presets.append(b);
        }
        const d = el('button', 'chip2', 'default'); d.title = 'Every param to its default';
        d.addEventListener('click', () => applyPreset(k, info, m, { values: {} }));
        u.presets.append(d);
      }
      u.pick.textContent = info ? info.name : '— empty —';
      u.pick.title = info ? (m?.help || []).join('\n') + '\n(click = choose another)' : 'Choose an effect';
    }
    const has = f.effect >= 0;
    u.c.classList.toggle('has', has);
    u.c.classList.toggle('gen', has && info?.kind === 'generator');
    u.c.classList.toggle('byp', has && f.bypass);
    u.byp.classList.toggle('on', has && !!f.bypass);
    u.byp.disabled = !has;
    u.mix.classList.toggle('hidden', !has);
    u.foot.classList.toggle('hidden', !has);
    u.empty.classList.toggle('hidden', has);
    if (has) {
      setVal(u.mixIn, f.mix);
      if (performance.now() >= (u.mixIn._hold || 0)) u.mixV.textContent = Math.round(f.mix * 100);
      u.rows.forEach((r, p) => {
        setVal(r.inp, f.param[p]);
        if (performance.now() >= (r.inp._hold || 0)) r.pv.textContent = fmtParam(r.pm, f.param[p]);
      });
      u.pressBar.style.width = Math.round((f.press || 0) * 100) + '%';
      const pt = m?.press ? m.params.find(x => x.id === m.press.param) : null;
      u.pressTxt.textContent = 'PRESS' + (pt ? ' → ' + pt.name : '') + (f.press > 0 ? ' ' + Math.round(f.press * 100) + '%' : '');
    }
  });
}

// ------------------------------------------------------------------ effect browser (gui.md §5)
const FB = { slot: 0, fam: 'all' };
function openBrowser(slot) {
  FB.slot = slot;
  const t = fxTarget();
  const lvl = t.deck === 'master' ? 'master' : t.layer === undefined ? 'deck' : 'layer';
  FB.level = lvl;
  const where = t.deck === 'master' ? 'MASTER' : t.layer === undefined ? 'DECK ' + deckLetter(t.deck) : 'LAYER ' + groupOf(t.deck, t.layer) + ' · deck ' + deckLetter(t.deck);
  $('fxbTitle').textContent = `EFFECT BROWSER · ${where} · slot ${slot + 1}`;
  $('fxBrowser').classList.remove('hidden');
  buildBrowser();
  $('fxbFilter').focus();
}
function closeBrowser() { $('fxBrowser').classList.add('hidden'); }
function buildBrowser() {
  const effects = S.cat?.effects || [];
  const fams = ['all', 'generators', ...new Set(effects.filter(e => e.kind !== 'generator').map(e => fxMeta(e.id)?.family || 'other'))];
  const fb = $('fxbFams'); fb.textContent = '';
  for (const f of fams) {
    const b = el('button', 'chip2' + (FB.fam === f ? ' on' : ''), f.toUpperCase());
    b.addEventListener('click', () => { FB.fam = f; buildBrowser(); });
    fb.append(b);
  }
  const q = $('fxbFilter').value.trim().toLowerCase();
  const cur = fxSlotsNow()[FB.slot]?.effect;
  const grid = $('fxbGrid'); grid.textContent = '';
  for (const e of effects) {
    const m = fxMeta(e.id);
    const isGen = e.kind === 'generator';
    if (FB.fam === 'generators' && !isGen) continue;
    if (FB.fam !== 'all' && FB.fam !== 'generators' && (isGen || (m?.family || 'other') !== FB.fam)) continue;
    if (q && !(e.name.toLowerCase().includes(q) || (m?.family || '').includes(q) || (m?.help || []).join(' ').toLowerCase().includes(q))) continue;
    const allowed = !m?.levels || m.levels.includes(FB.level);
    const card = el('button', 'fxcard' + (isGen ? ' gen' : '') + (e.id === cur ? ' cur' : '') + (allowed ? '' : ' no'));
    const head = el('div', 'fh');
    head.append(el('span', 'fn', e.name), el('span', 'cost c' + (m?.cost?.class || 'x'), m?.cost?.class || '?'));
    const tags = el('div', 'ft');
    tags.append(el('span', null, isGen ? 'GENERATOR' : (m?.family || '').toUpperCase()));
    if (m?.state === 'feedback') tags.append(el('span', null, 'FEEDBACK'));
    if (m?.flash) tags.append(el('span', 'warn', 'FLASH'));
    if (!allowed) tags.append(el('span', 'warn', 'not on ' + FB.level));
    const help = el('div', 'fhelp', (m?.help || []).join(' '));
    const prm = el('div', 'fp', (m?.params || []).map(p => (m.main?.includes(p.id) ? '●' : '') + p.name).join(' · ') || e.params.join(' · '));
    const react = m?.reaction ? el('div', 'fr', '~ reacts to ' + m.reaction.signal + ' → ' + m.reaction.param + (m.press ? ' · PRESS → ' + m.press.param : '')) : null;
    card.append(head, tags, help, prm);
    if (react) card.append(react);
    card.title = allowed ? 'Put “' + e.name + '” in slot ' + (FB.slot + 1) : 'This effect is not meant for the ' + FB.level + ' level (effect.json "levels")';
    card.addEventListener('click', () => {
      if (!allowed && !confirm(e.name + ' is not meant for the ' + FB.level + ' level. Use it anyway?')) return;
      send({ type: 'FxSet', ...fxTarget(), slot: FB.slot, value: e.id });
      closeBrowser();
    });
    grid.append(card);
  }
  if (!grid.children.length) grid.append(el('div', 'dim', 'Nothing matches.'));
}

// ------------------------------------------------------------------ clip editor drawer (gui.md §16)
const CE = {};
const MODE_WORDS = { Loop: 'LOOP', PingPong: 'PING-PONG', OnceHold: 'ONCE·HOLD', OnceEject: 'ONCE·EJECT', OnceNext: 'ONCE·NEXT', Random: 'RANDOM', Chop: 'CHOP', Step: 'STEP', Scratch: 'SCRATCH' };
const BEATLOOPS = [1 / 8, 1 / 4, 1 / 2, 1, 2, 4, 8, 16];
const CUE_HUE = i => `hsl(${(i * 360 / 16 + 190) % 360} 80% 60%)`;
function setP(param, value, extra) { const t = focusTarget(); return send({ type: 'LayerSet', ...t, param, value, ...(extra || {}) }); }
function setPT(param, value) { const t = focusTarget(); sendT('set' + param, { type: 'LayerSet', ...t, param, value }); }

function chipGroup(label, param, values, words, title) {
  const g = el('div', 'cg'); g.title = title || '';
  g.append(el('span', 'lbl', label));
  const bs = values.map(v => {
    const b = el('button', 'chip2', words ? (words[v] ?? v) : v.toUpperCase());
    b.dataset.v = v;
    b.addEventListener('click', () => setP(param, v));
    g.append(b);
    return b;
  });
  return { g, bs, set(v) { bs.forEach(b => b.classList.toggle('on', b.dataset.v === v)); } };
}
function numBox(label, param, lo, hi, step, unit, title) {
  const w = el('label', 'nb'); w.title = title || '';
  const inp = el('input'); inp.type = 'number'; inp.min = lo; inp.max = hi; inp.step = step;
  w.append(el('span', 'lbl', label), inp);
  if (unit) w.append(el('span', 'u', unit));
  hold(inp);
  inp.addEventListener('change', () => { const v = +inp.value; if (Number.isFinite(v)) setP(param, v); inp.blur(); });
  return { w, inp, set(v) { setVal(inp, +(+v).toFixed(3)); } };
}
function toggleBtn(label, param, title) {
  const b = el('button', 'chip2', label); b.title = title || '';
  b.addEventListener('click', () => setP(param, 2));
  return { b, set(on) { b.classList.toggle('on', !!on); } };
}

function buildClipEditor() {
  const box = $('clipEd'); box.textContent = '';
  const N = S.names;
  if (!N) { box.append(el('div', 'dim', 'Waiting for the bridge (GET /names)…')); return; }
  const pv = n => N.params.find(p => p.name === n)?.values || [];

  // Row 1: the timeline.
  const head = el('div', 'cehead');
  CE.title = el('span', 'cetitle', '–'); CE.phase = el('span', 'chip ph', '–'); CE.info = el('span', 'dim mono ceinfo', '');
  head.append(CE.title, CE.phase, CE.info);
  const tl = el('div', 'timeline'); tl.title = 'Click = jump · drag the brackets = in / out (thin = inner loop) · Alt + drag = scratch';
  CE.slices = el('div', 'slices'); for (let i = 0; i < 16; i++) CE.slices.append(el('i'));
  CE.range = el('div', 'range'); CE.inner = el('div', 'inner'); CE.bl = el('div', 'beatloop');
  CE.cues = el('div', 'cues');
  CE.ph = el('div', 'playhead');
  const hIn = el('div', 'hdl in', '['), hOut = el('div', 'hdl out', ']'), hLi = el('div', 'hdl li', '⌜'), hLo = el('div', 'hdl lo', '⌝');
  hIn.title = 'IN'; hOut.title = 'OUT'; hLi.title = 'LOOP-IN (inner loop)'; hLo.title = 'LOOP-OUT (inner loop)';
  tl.append(CE.slices, CE.range, CE.inner, CE.bl, CE.cues, CE.ph, hIn, hOut, hLi, hLo);
  CE.tl = tl; CE.h = { In: hIn, Out: hOut, LoopIn: hLi, LoopOut: hLo };
  const frac = e => { const r = tl.getBoundingClientRect(); return Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)); };
  for (const [param, h] of Object.entries(CE.h)) {
    h.addEventListener('pointerdown', e => {
      e.stopPropagation(); h.setPointerCapture(e.pointerId); h._drag = true; CE.dragging = param;
    });
    h.addEventListener('pointermove', e => { if (h._drag) { h.style.left = frac(e) * 100 + '%'; setPT(param, frac(e)); } });
    const end = e => { if (!h._drag) return; h._drag = false; CE.dragging = null; setP(param, frac(e)); };
    h.addEventListener('pointerup', end); h.addEventListener('pointercancel', end);
  }
  let scr = false;
  tl.addEventListener('pointerdown', e => {
    if (e.target.closest('.hdl')) return;
    const t = focusTarget();
    if (e.altKey) { scr = true; tl.setPointerCapture(e.pointerId); send({ type: 'LayerScratch', ...t, value: 1, pos: frac(e) }); }
    else if (e.target.closest('.cue')) { const q = +e.target.closest('.cue').dataset.q; send({ type: 'LayerCue', ...t, cue: q, value: 'Jump' }); }
    else send({ type: 'LayerJump', ...t, value: frac(e) });
  });
  tl.addEventListener('pointermove', e => { if (scr) sendT('scratch', { type: 'LayerScratch', ...focusTarget(), value: 1, pos: frac(e) }); });
  const scrEnd = e => { if (!scr) return; scr = false; send({ type: 'LayerScratch', ...focusTarget(), value: 0, pos: frac(e) }); };
  tl.addEventListener('pointerup', scrEnd); tl.addEventListener('pointercancel', scrEnd);

  // Row 2: cue pads + beat loop + direction / reverse.
  const row2 = el('div', 'cerow');
  const cueBox = el('div', 'cuepads'); cueBox.title = 'Cue points 1–16 of the clip: click = jump · Shift + click = set at the playhead · right-click = clear. No cue point = jump to that 16th of in..out.';
  CE.cuePads = [];
  for (let i = 0; i < 16; i++) {
    const b = el('button', 'cuepad', String(i + 1));
    b.addEventListener('click', e => send({ type: 'LayerCue', ...focusTarget(), cue: i, value: e.shiftKey ? 'Set' : 'Jump' }));
    b.addEventListener('contextmenu', e => { e.preventDefault(); send({ type: 'LayerCue', ...focusTarget(), cue: i, value: 'Clear' }); });
    cueBox.append(b); CE.cuePads.push(b);
  }
  const blBox = el('div', 'cg'); blBox.title = 'Beat loop from the playhead (needs the beat clock). Click the lit one again = off.';
  blBox.append(el('span', 'lbl', 'beat loop'));
  CE.blBtns = BEATLOOPS.map(b => {
    const x = el('button', 'chip2', fmtBeats(b)); x.dataset.b = b;
    x.addEventListener('click', () => {
      const tr = curTransport();
      const on = tr && tr.beat_loop && Math.abs(tr.beat_loop_beats - b) < 1e-3;
      send({ type: 'LayerBeatLoop', ...focusTarget(), value: on ? 0 : b });
    });
    blBox.append(x); return x;
  });
  const off = el('button', 'chip2', 'OFF'); off.addEventListener('click', () => send({ type: 'LayerBeatLoop', ...focusTarget(), value: 0 })); blBox.append(off);
  const dirBox = el('div', 'cg');
  dirBox.append(el('span', 'lbl', 'dir'));
  CE.fwd = el('button', 'chip2', '▶'); CE.fwd.title = 'Forward'; CE.fwd.addEventListener('click', () => setP('Direction', 0));
  CE.bwd = el('button', 'chip2', '◀'); CE.bwd.title = 'Backward'; CE.bwd.addEventListener('click', () => setP('Direction', 1));
  CE.rev = el('button', 'chip2', 'REVERSE'); CE.rev.title = 'Hold: momentary reverse (catch-up when you let go)';
  CE.rev.addEventListener('pointerdown', e => { CE.rev.setPointerCapture(e.pointerId); setP('ReverseHold', 1); });
  const revUp = () => setP('ReverseHold', 0);
  CE.rev.addEventListener('pointerup', revUp); CE.rev.addEventListener('pointercancel', revUp);
  dirBox.append(CE.bwd, CE.fwd, CE.rev);
  row2.append(cueBox, blBox, dirBox);

  // Row 3: mode + its options, time base.
  const row3 = el('div', 'cerow');
  CE.mode = chipGroup('mode', 'Mode', pv('Mode'), MODE_WORDS, 'How the playhead runs (clips-and-playback.md §2)');
  CE.opt = el('div', 'cg opts');
  CE.interval = numBox('every', 'Interval', 1 / 16, 64, 0.25, 'beats/s', 'RANDOM / CHOP: jump every n (beats with the BPM time base, else seconds)');
  CE.distance = numBox('distance', 'Distance', 0, 1, 0.05, '', 'RANDOM: how far a jump may go (part of the range)');
  CE.slicesN = numBox('slices', 'Slices', 2, 16, 1, '', 'CHOP: slices');
  CE.stepF = numBox('frames', 'StepFrames', 1, 64, 1, '', 'STEP: frames per step');
  CE.stepD = numBox('every', 'StepDiv', 1 / 16, 16, 0.25, 'beats/s', 'STEP: one step every n');
  CE.scrSnap = toggleBtn('SNAP BACK', 'ScratchSnap', 'After a scratch: snap back to where the clip would be (on) or stay (off)');
  CE.opt.append(CE.interval.w, CE.distance.w, CE.slicesN.w, CE.stepF.w, CE.stepD.w, CE.scrSnap.b);
  CE.tb = chipGroup('time', 'TimeBase', pv('TimeBase'), { Free: 'FREE ×speed', Bpm: 'BPM' }, 'FREE = the clip at its speed; BPM = in..out lasts n beats');
  CE.beats = numBox('', 'Beats', 0.25, 64, 0.25, 'beats', 'BPM time base: in..out lasts this many beats');
  CE.tb.g.append(CE.beats.w);
  row3.append(CE.mode.g, CE.opt, CE.tb.g);

  // Row 4: trigger, start, target, snap, fades, glide.
  const row4 = el('div', 'cerow');
  CE.trig = chipGroup('trigger', 'Trigger', pv('Trigger'), null, 'How a pad launch behaves: NORMAL, TOGGLE, PIANO (plays while held), LEGATO (keeps the position)');
  CE.start = chipGroup('start', 'StartPos', pv('StartPos'), null, 'A launch restarts the clip or continues where it was');
  CE.target = chipGroup('target', 'Target', pv('Target'), null, 'Where a pad launch lands: OWN layer, a FREE layer, or the HIT layer');
  const snapW = el('label', 'nb'); snapW.title = 'Beat snap of launches and jumps on PROGRAM (PREVIEW never waits)';
  CE.snap = el('select');
  for (const v of ['Global', ...pv('Snap')]) { const o = el('option', null, v === 'Global' ? 'global grid' : v); o.value = v === 'Global' ? -1 : v; CE.snap.append(o); }
  CE.snap.addEventListener('change', () => setP('Snap', CE.snap.value === '-1' ? -1 : CE.snap.value));
  snapW.append(el('span', 'lbl', 'snap'), CE.snap);
  CE.fin = numBox('fade in', 'FadeIn', 0, 10, 0.1, 's');
  CE.fout = numBox('fade out', 'FadeOut', 0, 10, 0.1, 's', 'STOP / RELEASE / eject fade out over this time');
  CE.glide = numBox('glide', 'Glide', 0, 16, 0.25, 'beats', 'Speed changes glide over this many beats (0 = jump)');
  row4.append(CE.trig.g, CE.start.g, CE.target.g, snapW, CE.fin.w, CE.fout.w, CE.glide.w);

  // Row 5: inner loop, catch-up, autopilot, transition, seed.
  const row5 = el('div', 'cerow');
  CE.innerT = toggleBtn('INNER LOOP', 'InnerLoop', 'The 4-point loop: in → loop-out, loop between loop-in / loop-out, RELEASE plays the outro');
  CE.catch = toggleBtn('CATCH-UP', 'CatchUp', 'A beat loop / reverse returns to where the clip would be');
  CE.auto = chipGroup('autopilot', 'Autopilot', pv('Autopilot'), null, 'After n loops, load the next / previous / a random clip of the bank (or the same)');
  CE.autoN = numBox('after', 'AutopilotCount', 1, 64, 1, 'loops');
  CE.auto.g.append(CE.autoN.w);
  CE.trans = chipGroup('new clip', 'Transition', pv('Transition'), { Cut: 'CUT', Fade: 'FADE', BlackDip: 'BLACK DIP' }, 'How the layer changes to a new clip');
  CE.transT = numBox('', 'TransitionTime', 0, 10, 0.1, 's');
  CE.trans.g.append(CE.transT.w);
  CE.seed = numBox('seed', 'Seed', 0, 2147483647, 1, '', 'RANDOM / CHOP / autopilot pattern (same seed = same pattern)');
  row5.append(CE.innerT.b, CE.catch.b, CE.auto.g, CE.trans.g, CE.seed.w);

  CE.none = el('div', 'cenone dim', '');
  CE.body = el('div', 'cebody');
  CE.body.append(tl, row2, row3, row4, row5);
  box.append(head, CE.none, CE.body);
}
function curTransport() {
  if (!S.tr || !S.live) return null;
  const t = focusTarget();
  return S.tr.layer[t.deck][t.layer];
}
function renderClipEditor() {
  if (!CE.body) return;
  const L = S.live, t = focusTarget();
  const ls = L.deck[t.deck].layer[t.layer], tr = curTransport();
  const ci = clipInfo(ls.clip);
  const onProg = t.deck === L.program_deck;
  $('inspector').classList.toggle('progframe', onProg && S.tab === 'clip');
  CE.title.textContent = `${onProg ? 'PROGRAM' : 'PREVIEW'} ${groupOf(t.deck, t.layer)} · deck ${deckLetter(t.deck)}: ${ci ? clipName(ci) : 'no clip'}`;
  CE.title.className = 'cetitle ' + (onProg ? 'prog' : 'prev');
  CE.none.textContent = !tr ? 'This engine does not publish the transport section (state v2).' : !ci ? 'Load a clip on this layer to edit its transport (the settings stay with the layer).' : '';
  CE.none.classList.toggle('hidden', !!(tr && ci));
  CE.body.classList.toggle('noclip', !ci);
  if (!tr) return;
  CE.phase.textContent = tr.phase.toUpperCase() + (tr.pending ? ' · waits ' + tr.pending_beats + ' beats' : '');
  CE.phase.className = 'chip ph ' + tr.phase.toLowerCase() + (tr.pending ? ' wait' : '');
  CE.info.textContent = ci ? `frame ${tr.frame} · loops ${tr.loops} · speed ${tr.speed_now.toFixed(2)}x · gain ${Math.round(tr.gain * 100)}%` + (tr.scratching ? ' · SCRATCH' : '') : '';
  const pct = v => (v * 100).toFixed(2) + '%';
  CE.range.style.left = pct(tr.in); CE.range.style.width = pct(Math.max(0, tr.out - tr.in));
  if (ci) CE.range.style.background = rgb(ci.thumb_rgb) + '55';
  CE.inner.classList.toggle('hidden', !tr.inner_loop);
  CE.inner.style.left = pct(tr.loop_in); CE.inner.style.width = pct(Math.max(0, tr.loop_out - tr.loop_in));
  CE.h.LoopIn.classList.toggle('hidden', !tr.inner_loop); CE.h.LoopOut.classList.toggle('hidden', !tr.inner_loop);
  if (CE.dragging !== 'In') CE.h.In.style.left = pct(tr.in);
  if (CE.dragging !== 'Out') CE.h.Out.style.left = pct(tr.out);
  if (CE.dragging !== 'LoopIn') CE.h.LoopIn.style.left = pct(tr.loop_in);
  if (CE.dragging !== 'LoopOut') CE.h.LoopOut.style.left = pct(tr.loop_out);
  CE.ph.style.left = pct(ls.position);
  CE.bl.classList.toggle('hidden', !tr.beat_loop);
  // cue points (they belong to the clip)
  const anyCue = tr.cue.some(q => q >= 0);
  CE.slices.classList.toggle('hidden', anyCue);
  const ck = tr.cue.join(',');
  if (CE.cues.dataset.k !== ck) {
    CE.cues.dataset.k = ck; CE.cues.textContent = '';
    tr.cue.forEach((q, i) => {
      if (q < 0) return;
      const m = el('div', 'cue', String(i + 1)); m.dataset.q = i; m.style.left = pct(q); m.style.borderColor = CUE_HUE(i); m.style.color = CUE_HUE(i);
      m.title = 'Cue ' + (i + 1) + ' (click = jump)';
      CE.cues.append(m);
    });
  }
  CE.cuePads.forEach((b, i) => { b.classList.toggle('set', tr.cue[i] >= 0); b.style.borderColor = tr.cue[i] >= 0 ? CUE_HUE(i) : ''; });
  CE.blBtns.forEach(b => b.classList.toggle('on', !!tr.beat_loop && Math.abs(tr.beat_loop_beats - +b.dataset.b) < 1e-3));
  CE.fwd.classList.toggle('on', !tr.backward); CE.bwd.classList.toggle('on', tr.backward); CE.rev.classList.toggle('on', tr.reverse_hold);
  CE.mode.set(tr.mode);
  const m = tr.mode;
  CE.interval.w.classList.toggle('hidden', m !== 'Random' && m !== 'Chop');
  CE.distance.w.classList.toggle('hidden', m !== 'Random');
  CE.slicesN.w.classList.toggle('hidden', m !== 'Chop');
  CE.stepF.w.classList.toggle('hidden', m !== 'Step'); CE.stepD.w.classList.toggle('hidden', m !== 'Step');
  CE.scrSnap.b.classList.toggle('hidden', m === 'Random' || m === 'Chop' || m === 'Step');
  CE.interval.set(tr.interval); CE.distance.set(tr.distance); CE.slicesN.set(tr.slices); CE.stepF.set(tr.step_frames); CE.stepD.set(tr.step_div);
  CE.scrSnap.set(tr.scratch_snap);
  CE.tb.set(tr.time_base); CE.beats.w.classList.toggle('hidden', tr.time_base !== 'Bpm'); CE.beats.set(tr.beats);
  CE.trig.set(tr.trigger); CE.start.set(tr.start); CE.target.set(tr.target);
  if (document.activeElement !== CE.snap) CE.snap.value = tr.snap === 'Global' ? '-1' : tr.snap;
  CE.fin.set(tr.fade_in); CE.fout.set(tr.fade_out); CE.glide.set(tr.glide);
  CE.innerT.set(tr.inner_loop); CE.catch.set(tr.catch_up);
  CE.auto.set(tr.autopilot); CE.autoN.set(tr.autopilot_count); CE.autoN.w.classList.toggle('hidden', tr.autopilot === 'Off');
  CE.trans.set(tr.transition); CE.transT.set(tr.transition_time); CE.transT.w.classList.toggle('hidden', tr.transition === 'Cut');
  CE.seed.set(tr.seed);
}

// ------------------------------------------------------------------ HIT layer (clips-and-playback.md §3.3)
const HE = {};
function buildHitEditor() {
  const box = $('hitEd'); box.textContent = '';
  const row = el('div', 'cerow');
  HE.sel = el('select'); HE.sel.title = 'The clip to fire';
  const fire = el('button', 'fire', 'FIRE'); fire.title = 'Hold = fire the clip on the HIT layer (PIANO-style when its trigger is PIANO); let go = release';
  fire.addEventListener('pointerdown', e => { fire.setPointerCapture(e.pointerId); const id = +HE.sel.value; if (id >= 0) send({ type: 'LayerTrigger', deck: 'hit', value: id, velocity: 1 }); });
  const rel = () => { const id = +HE.sel.value; if (id >= 0) send({ type: 'LayerTrigger', deck: 'hit', value: id, release: true }); };
  fire.addEventListener('pointerup', rel); fire.addEventListener('pointercancel', rel);
  const stop = el('button', null, 'STOP'); stop.title = 'Fade the HIT layer out'; stop.addEventListener('click', () => send({ type: 'LayerPlay', deck: 'hit', value: 'Stop' }));
  const ej = el('button', null, 'eject'); ej.addEventListener('click', () => send({ type: 'LayerClip', deck: 'hit', value: -1 }));
  HE.op = el('input'); HE.op.type = 'range'; HE.op.min = 0; HE.op.max = 1; HE.op.step = 0.01; HE.op.title = 'HIT opacity'; hold(HE.op);
  HE.op.addEventListener('input', () => sendT('hitop', { type: 'LayerOpacity', deck: 'hit', value: +HE.op.value }));
  HE.blend = el('select'); BLEND.forEach((b, i) => { const o = el('option', null, b); o.value = i; HE.blend.append(o); });
  HE.blend.addEventListener('change', () => send({ type: 'LayerBlend', deck: 'hit', value: +HE.blend.value }));
  const setH = (param, value) => send({ type: 'LayerSet', deck: 'hit', param, value });
  const nb = (label, param, lo, hi, step, unit) => {
    const w = el('label', 'nb'); const inp = el('input'); inp.type = 'number'; inp.min = lo; inp.max = hi; inp.step = step; hold(inp);
    inp.addEventListener('change', () => { setH(param, +inp.value); inp.blur(); });
    w.append(el('span', 'lbl', label), inp, el('span', 'u', unit)); return { w, inp };
  };
  HE.fin = nb('fade in', 'FadeIn', 0, 10, 0.1, 's'); HE.fout = nb('fade out', 'FadeOut', 0, 10, 0.1, 's');
  HE.trig = el('div', 'cg'); HE.trig.append(el('span', 'lbl', 'trigger'));
  HE.trigBtns = (S.names?.params.find(p => p.name === 'Trigger')?.values || []).map(v => {
    const b = el('button', 'chip2', v.toUpperCase()); b.dataset.v = v; b.addEventListener('click', () => setH('Trigger', v)); HE.trig.append(b); return b;
  });
  HE.mode = el('div', 'cg'); HE.mode.append(el('span', 'lbl', 'mode'));
  HE.modeBtns = ['Loop', 'OnceHold', 'OnceEject'].map(v => { const b = el('button', 'chip2', MODE_WORDS[v]); b.dataset.v = v; b.addEventListener('click', () => setH('Mode', v)); HE.mode.append(b); return b; });
  row.append(el('span', 'lbl', 'clip'), HE.sel, fire, stop, ej, el('span', 'lbl', 'opacity'), HE.op, HE.blend);
  const row2 = el('div', 'cerow'); row2.append(HE.trig, HE.mode, HE.fin.w, HE.fout.w);
  HE.state = el('div', 'dim mono', '');
  box.append(el('div', 'dim', 'The HIT layer sits on top of PROGRAM for one-shots (a pad with target HIT, or Alt + click a clip). It is never saved in a look and has no FX slots in 0.1. Velocity = start opacity; a release fades it out.'), row, row2, HE.state);
}
function fillHitClips() {
  if (!HE.sel) return;
  const cur = HE.sel.value; HE.sel.textContent = '';
  (S.cat?.clips || []).forEach(c => { const o = el('option', null, clipName(c)); o.value = c.id; HE.sel.append(o); });
  if (cur) HE.sel.value = cur;
}
function renderHit() {
  if (!S.tr) { $('hitName').textContent = '–'; return; }
  const h = S.tr.hit, ht = S.tr.hit_transport, ci = clipInfo(h.clip);
  $('hitName').textContent = ci ? clipName(ci) : 'empty';
  $('hitGain').style.width = (ci ? Math.round(ht.gain * h.opacity * 100) : 0) + '%';
  $('hitCard').classList.toggle('on', !!ci && ht.gain > 0.01);
  if (S.tab !== 'hit' || !HE.sel) return;
  setVal(HE.op, h.opacity);
  if (document.activeElement !== HE.blend) setVal(HE.blend, h.blend);
  setVal(HE.fin.inp, ht.fade_in); setVal(HE.fout.inp, ht.fade_out);
  HE.trigBtns.forEach(b => b.classList.toggle('on', b.dataset.v === ht.trigger));
  HE.modeBtns.forEach(b => b.classList.toggle('on', b.dataset.v === ht.mode));
  HE.state.textContent = `HIT: ${ci ? clipName(ci) : 'empty'} · ${ht.phase} · gain ${Math.round(ht.gain * 100)}% · velocity ${ht.velocity} · position ${(h.position * 100).toFixed(0)}%`;
}

// ------------------------------------------------------------------ inspector
function renderInspector() {
  const L = S.live, t = fxTarget(), ft = focusTarget();
  const onProg = ft.deck === L.program_deck;
  const grpLetter = groupOf(ft.deck, ft.layer);
  $('tabClip').textContent = `CLIP ${grpLetter}`;
  $('tabLayer').textContent = `LAYER ${grpLetter} FX`;
  $('tabDeck').textContent = `DECK ${deckLetter(ft.deck)} FX`;
  $('layerBar').classList.toggle('hidden', S.tab !== 'layer' && S.tab !== 'clip');
  $('inspector').style.borderColor = S.tab === 'master' ? 'var(--accent)' : S.tab === 'hit' ? 'var(--warn)' : onProg ? '#6a1a16' : '#1f5a30';
  if (S.tab === 'layer' || S.tab === 'clip') {
    const ls = L.deck[ft.deck].layer[ft.layer];
    const src = layerSource(ls);
    $('inspClip').textContent = `${onProg ? 'PROGRAM' : 'PREVIEW'} ${grpLetter}: ${src ? src.name : 'empty'}`;
    setVal($('inspSpeed'), ls.speed);
    if (performance.now() >= ($('inspSpeed')._hold || 0)) $('inspSpeedVal').textContent = ls.speed.toFixed(2) + 'x';
    $('inspPlay').textContent = ls.playing ? '❚❚' : '▶';
  }
  if (S.tab === 'clip') renderClipEditor();
  else if (['layer', 'deck', 'master'].includes(S.tab)) renderFxSlots();
  void t;
}

// ------------------------------------------------------------------ monitors and previews
const cv = [document.importNode($('cvTpl').content, true).firstElementChild, document.importNode($('cvTpl').content, true).firstElementChild];
const streams = [
  { id: 0, canvas: cv[0], since: 0, ok: false },
  { id: 1, canvas: cv[1], since: 0, ok: false },
  { id: 2, canvas: $('cvMain'), since: 0, ok: false },
];
async function pull(st) {
  const ctx = st.canvas.getContext('2d');
  for (;;) {
    let wait = 40;
    if (S.view !== 'perform') { await sleep(250); continue; }
    try {
      const r = await fetch(`/preview/${st.id}?since=${st.since}`, { cache: 'no-store' });
      if (r.status === 200) {
        const buf = await r.arrayBuffer();
        const dv = new DataView(buf);
        const w = dv.getUint16(4, true), h = dv.getUint16(6, true);
        st.since = dv.getUint32(12, true);
        if (buf.byteLength >= 16 + w * h * 4) {
          if (st.canvas.width !== w || st.canvas.height !== h) { st.canvas.width = w; st.canvas.height = h; }
          ctx.putImageData(new ImageData(new Uint8ClampedArray(buf, 16, w * h * 4), w, h), 0, 0);
          st.ok = true;
        }
      } else if (r.status !== 204) wait = 1000;
    } catch (e) { wait = 1000; }
    if (!engineOk()) wait = Math.max(wait, 500);
    await sleep(wait);
  }
}

function renderMonitors() {
  const L = S.live, pd = L.program_deck;
  const place = (mon, deck) => {
    const c = cv[deck];
    if (c.parentElement !== mon) mon.prepend(c);
    mon.classList.toggle('haspic', streams[deck].ok);
  };
  place($('monPrev'), 1 - pd);
  place($('monProg'), pd);
  $('monMain').classList.toggle('haspic', streams[2].ok);
  for (const [id, d] of [['prevDeck', 1 - pd], ['progDeck', pd]]) {
    $(id).textContent = 'Deck ' + deckLetter(d);
    $(id).className = 'deckname ' + (d === 0 ? 'a' : 'b');
  }
  const fPrev = L.focus_deck === 1 - pd, fProg = L.focus_deck === pd;
  $('sidePrev').classList.toggle('focus', fPrev);
  $('sideProg').classList.toggle('focus', fProg);
  for (const [mon, on, word, col] of [['monPrev', fPrev, 'PREP · pads land here', 'var(--preview)'], ['monProg', fProg, 'PLAY · pads land here', 'var(--live)']]) {
    const m = $(mon);
    let tag = m.querySelector('.focustag');
    if (on && !tag) { tag = el('div', 'focustag'); m.append(tag); }
    if (!on && tag) tag.remove();
    if (on) { tag.textContent = word + ' · ' + GROUPS[mon === 'monPrev' ? 'prev' : 'prog'][L.focus_layer]; tag.style.background = col; }
  }
}

// ------------------------------------------------------------------ TAKE column, clock, top bar
function renderTake() {
  const L = S.live;
  const p = L.program_deck === 0 ? L.crossfader : 1 - L.crossfader;  // how far PREVIEW has travelled in
  $('takeRing').style.setProperty('--p', p.toFixed(3));
  $('takePct').textContent = p > 0.001 ? Math.round(p * 100) + '%' : '';
  $('takeBtn').classList.toggle('active', !!L.take_active);
  setVal($('xf'), L.crossfader);
  $('blackBtn').classList.toggle('on', !!L.black);
  $('blackBtn').textContent = L.black ? 'BLACK ON' : 'BLACK';
}

// The beat runs between transport events: extrapolate from the last one (BPM is known).
const CLK = { beat: 0, at: 0, bpm: 0 };
function renderClock() {
  const c = S.tr?.clock;
  $('clock').classList.toggle('off', !c);
  if (!c) { $('bpmVal').textContent = '–'; return; }
  if (performance.now() >= ($('bpmVal')._hold || 0)) $('bpmVal').textContent = c.bpm.toFixed(c.bpm % 1 ? 1 : 0);
  $('bpmVal').title = `BPM ${c.bpm} · ${c.source}${c.taps ? ' · ' + c.taps + ' taps' : ''} (drag up / down, double-click = type)`;
  if (document.activeElement !== $('gridSel')) setVal($('gridSel'), c.grid);
}
function tickBeat() {
  if (CLK.bpm > 0) {
    const b = CLK.beat + (performance.now() - CLK.at) / 60000 * CLK.bpm;
    const i = ((Math.floor(b) % 4) + 4) % 4;
    const frac = b - Math.floor(b);
    const dots = $('beatDots').children;
    for (let k = 0; k < 4; k++) dots[k].className = k === i ? (frac < 0.25 ? 'hit' : 'cur') : '';
  }
  requestAnimationFrame(tickBeat);
}

function renderTop() {
  const b = S.bridge, L = S.live;
  const ec = $('engineChip');
  let word = 'BRIDGE OFFLINE', cls = 'bad';
  if (S.sse && b) {
    word = { ok: 'ENGINE OK', absent: 'ENGINE NOT RUNNING', stale: 'ENGINE NOT RESPONDING', starting: 'ENGINE STARTING', version: 'ENGINE VERSION MISMATCH' }[b.engine] || b.engine;
    cls = b.engine === 'ok' ? 'ok' : b.engine === 'starting' ? 'warn' : 'bad';
    if (b.engine === 'ok' && b.commands !== 'ok') { word = 'COMMANDS ' + b.commands.toUpperCase(); cls = 'warn'; }
  }
  ec.textContent = word; ec.className = 'chip ' + cls;
  $('fpsChip').textContent = engineOk() ? `${b.fps.toFixed(0)} fps · epoch ${b.epoch}` : '– fps';
  const alive = (id, p) => { const e = $(id); const a = !!(L && L.producer[p] && L.producer[p].alive && engineOk()); e.classList.toggle('alive', a); e.classList.toggle('dead', !a); };
  alive('mk2Chip', 0); alive('guiChip', 1);
  if (L) {
    const me = L.producer[1];
    $('seqChip').textContent = `sent ${S.sent} · on air #${me.acked}` + (b && b.dropped ? ` · dropped ${b.dropped}` : '');
    $('seqChip').classList.toggle('warn', !!(b && b.dropped));
    if (S.seenRejected !== null && me.rejected > S.seenRejected) toast(`The engine refused a command: ${me.last_reject}`, 'bad');
    S.seenRejected = me.rejected;
    $('rejChip').classList.toggle('hidden', me.rejected === 0);
    $('rejChip').textContent = `refused ${me.rejected} · ${me.last_reject}`;
  }
  // overlay
  let title = '', text = '', warn = false;
  if (!S.sse) { title = 'BRIDGE NOT REACHABLE'; text = 'himmel_ui is not running. The show is not affected. Reconnecting…'; }
  else if (!b) { title = 'CONNECTING…'; }
  else if (b.engine === 'absent') { title = 'ENGINE NOT RUNNING'; text = 'Waiting for himmel_render (or fake_engine). Start Himmel; this page reconnects by itself.'; }
  else if (b.engine === 'stale') { title = 'ENGINE NOT RESPONDING'; text = 'No heartbeat for over 1.5 s; showing the last state. The supervisor restarts the renderer.'; }
  else if (b.engine === 'starting') { title = 'ENGINE STARTING…'; warn = true; }
  else if (b.engine === 'version') { title = 'ENGINE VERSION MISMATCH'; text = `This GUI speaks command v${b.versions.command} / state v${b.versions.state} / preview v${b.versions.preview}. Build himmel_ui and the engine from the same commit.`; }
  else if (b.watch) { title = 'WATCH ONLY'; text = 'This bridge runs with --watch: you see the show, but nothing you do here is sent.'; warn = true; }
  else if (b.commands !== 'ok') { title = 'COMMANDS NOT AVAILABLE'; text = `The command block is ${b.commands}. You can watch but not play.`; warn = true; }
  $('overlay').classList.toggle('hidden', !title || S.view !== 'perform');  // other views show it in their own status
  $('overlay').classList.toggle('warnbox', warn);
  $('ovTitle').textContent = title; $('ovText').textContent = text;
  $('perform').classList.toggle('offline', !engineOk() || !S.live || (b && b.engine === 'ok' && b.commands !== 'ok' && !b.watch));
}

// ------------------------------------------------------------------ render loop (coalesced)
let dirty = false;
function invalidate() { if (!dirty) { dirty = true; requestAnimationFrame(render); } }
function render() {
  dirty = false;
  renderTop();
  renderClock();
  if (window.Panels) window.Panels.render();
  if (window.Views) window.Views.render();
  if (S.view !== 'perform' || !S.live || !S.cat) return;
  renderMonitors(); renderStrips(); renderTake(); renderLibrary(); renderInspector(); renderHit();
}

// ------------------------------------------------------------------ events from the bridge
async function loadStatic() {
  // Names and metadata: retried until the bridge answers (they never change while it runs).
  for (;;) {
    try {
      if (!S.names) { const r = await fetch('/names'); if (r.ok) { S.names = await r.json(); fillGrid(); buildClipEditor(); buildHitEditor(); fillHitClips(); } }
      if (!S.metaRaw) { const r = await fetch('/fxmeta'); if (r.ok) { S.metaRaw = await r.json(); matchMeta(); } }
      if (S.names && S.metaRaw) return;
    } catch (e) { /* bridge not there yet */ }
    await sleep(1000);
  }
}
// Catalog effect id -> effect.json: same position in fx/catalog.json, checked by name; else by name.
function matchMeta() {
  S.meta = [];
  const list = S.metaRaw?.effects || [];
  for (const e of S.cat?.effects || []) {
    let m = list[e.id];
    if (!m || m.name !== e.name) m = list.find(x => x.name === e.name) || null;
    S.meta[e.id] = m;
  }
  slots.forEach(u => (u.key = ''));
  if (S.lib === 'gens') buildLibrary();
  invalidate();
}
function fillGrid() {
  const g = $('gridSel'); g.textContent = '';
  for (const v of S.names.grid) { const o = el('option', null, v === 'Off' ? 'off' : v); o.value = v; g.append(o); }
}

function connect() {
  const es = new EventSource('/events');
  es.onopen = () => { S.sse = true; invalidate(); };
  es.onerror = () => { S.sse = false; invalidate(); };   // EventSource retries on its own (retry: 1000)
  es.addEventListener('bridge', e => {
    const b = JSON.parse(e.data);
    if (S.bridge && S.bridge.epoch !== b.epoch && b.epoch) { S.seenRejected = null; toast(`Engine (re)started · epoch ${b.epoch}`); }
    S.bridge = b; S.sse = true; S.lastBridgeMs = performance.now(); invalidate();
  });
  es.addEventListener('catalog', e => {
    S.cat = JSON.parse(e.data);
    matchMeta(); buildLibrary(); fillHitClips(); invalidate();
  });
  es.addEventListener('live', e => { S.live = JSON.parse(e.data); invalidate(); });
  es.addEventListener('transport', e => {
    S.tr = JSON.parse(e.data);
    const c = S.tr.clock;
    if (c) { CLK.beat = c.beat; CLK.at = performance.now(); CLK.bpm = c.bpm; }
    invalidate();
  });
  es.addEventListener('mk2', e => { S.mk2 = JSON.parse(e.data); if (S.view === 'mk2' || S.view === 'start') invalidate(); });
  // M11 part 2: the optional panels. AUDIO arrives ~30 Hz; only the views that show it redraw.
  for (const name of ['show', 'outputs', 'osc', 'library', 'weapons'])
    es.addEventListener(name, e => { S[name] = JSON.parse(e.data); invalidate(); });
  es.addEventListener('audio', e => { S.audio = JSON.parse(e.data); invalidate(); });
  // A bridge that hangs without closing the socket: no "bridge" event for 3 s = offline.
  setInterval(() => { if (S.sse && performance.now() - S.lastBridgeMs > 3000) { S.sse = false; invalidate(); es.close(); setTimeout(connect, 500); } }, 1000);
}

// ------------------------------------------------------------------ views
const VIEWS = ['perform', 'start', 'outputs', 'library', 'mk2', 'settings'];
function setView(v) {
  S.view = v;
  for (const id of VIEWS) $(id).classList.toggle('hidden', id !== v);
  for (const b of document.querySelectorAll('#viewTabs button')) b.classList.toggle('on', b.dataset.view === v);
  document.title = 'Himmel · ' + v[0].toUpperCase() + v.slice(1);
  if (location.hash !== '#' + v) history.replaceState(null, '', '#' + v);
  if (window.Views) window.Views.shown(v);
  if (window.Panels) window.Panels.shown(v);
  invalidate();
}
window.setView = setView;

// ------------------------------------------------------------------ wiring
function take() { send({ type: 'Take', value: +$('takeMs').value }); }
function cut() { send({ type: 'Cut' }); }
function tap() { send({ type: 'Clock', param: 'Tap' }); }
function wire() {
  $('takeBtn').addEventListener('click', take);
  $('cutBtn').addEventListener('click', cut);
  $('blackBtn').addEventListener('click', () => send({ type: 'Black', value: 2 }));
  const xf = $('xf'); hold(xf);
  xf.addEventListener('input', () => sendT('xf', { type: 'Crossfader', value: +xf.value }));
  for (const b of document.querySelectorAll('.tabs > button')) b.addEventListener('click', () => setTab(b.dataset.tab));
  for (const b of document.querySelectorAll('.libtabs > button')) b.addEventListener('click', () => {
    S.lib = b.dataset.lib;
    for (const x of document.querySelectorAll('.libtabs > button')) x.classList.toggle('on', x === b);
    buildLibrary();
  });
  for (const b of document.querySelectorAll('#viewTabs button')) b.addEventListener('click', () => setView(b.dataset.view));
  const sp = $('inspSpeed'); hold(sp);
  sp.addEventListener('input', () => { $('inspSpeedVal').textContent = (+sp.value).toFixed(2) + 'x'; sendT('speed', { type: 'LayerSpeed', ...focusTarget(), value: +sp.value }); });
  sp.addEventListener('dblclick', () => send({ type: 'LayerSpeed', ...focusTarget(), value: 1 }));
  $('inspPlay').addEventListener('click', () => send({ type: 'LayerPlay', ...focusTarget(), value: 'Toggle' }));
  $('inspRestart').addEventListener('click', () => send({ type: 'LayerPlay', ...focusTarget(), value: 'Restart' }));
  $('inspRelease').addEventListener('click', () => send({ type: 'LayerPlay', ...focusTarget(), value: 'Release' }));
  $('inspStop').addEventListener('click', () => send({ type: 'LayerPlay', ...focusTarget(), value: 'Stop' }));
  $('inspEject').addEventListener('click', () => send({ type: 'LayerClip', ...focusTarget(), value: -1 }));
  $('clipFilter').addEventListener('input', buildLibrary);
  // clock
  $('tapBtn').addEventListener('click', tap);
  const bump = (e, s) => send({ type: 'Clock', param: 'Bpm', value: e.shiftKey ? s * 0.1 : s, relative: true });
  $('bpmUp').addEventListener('click', e => bump(e, 1));
  $('bpmDown').addEventListener('click', e => bump(e, -1));
  $('nudgeL').addEventListener('click', () => send({ type: 'Clock', param: 'Nudge', value: -0.125 }));
  $('nudgeR').addEventListener('click', () => send({ type: 'Clock', param: 'Nudge', value: 0.125 }));
  $('resyncBtn').addEventListener('click', () => send({ type: 'Clock', param: 'Resync' }));
  $('gridSel').addEventListener('change', e => send({ type: 'Clock', param: 'Grid', value: e.target.value }));
  const bv = $('bpmVal'); let drag = null;
  bv.addEventListener('pointerdown', e => { drag = { y: e.clientY, bpm: S.tr?.clock?.bpm || 120 }; bv.setPointerCapture(e.pointerId); bv._hold = Infinity; });
  bv.addEventListener('pointermove', e => {
    if (!drag) return;
    const v = Math.min(300, Math.max(20, Math.round((drag.bpm + (drag.y - e.clientY) * (e.shiftKey ? 0.02 : 0.25)) * 10) / 10));
    bv.textContent = v; sendT('bpm', { type: 'Clock', param: 'Bpm', value: v });
  });
  bv.addEventListener('pointerup', () => { drag = null; bv._hold = performance.now() + 500; });
  bv.addEventListener('dblclick', () => {
    const v = parseFloat(prompt('BPM (20–300)', S.tr?.clock?.bpm || 120));
    if (Number.isFinite(v)) send({ type: 'Clock', param: 'Bpm', value: v });
  });
  // effect browser
  $('fxbClose').addEventListener('click', closeBrowser);
  $('fxbNone').addEventListener('click', () => { send({ type: 'FxSet', ...fxTarget(), slot: FB.slot, value: -1 }); closeBrowser(); });
  $('fxbFilter').addEventListener('input', buildBrowser);
  $('fxBrowser').addEventListener('click', e => { if (e.target === $('fxBrowser')) closeBrowser(); });
  // Buttons never keep keyboard focus, so space / enter always mean TAKE / CUT.
  document.addEventListener('pointerup', e => { const b = e.target.closest('button'); if (b) b.blur(); });
  document.addEventListener('change', e => { if (e.target.matches('select, input[type=range]')) e.target.blur(); });
  document.addEventListener('keydown', e => {
    const t = e.target;
    if (t.matches && t.matches('input[type=search], input[type=text], input[type=number], textarea')) { if (e.key === 'Escape') t.blur(); return; }
    if (t.matches && t.matches('select') && e.key !== ' ' && e.key !== 'Enter') return;  // arrows still pick in a menu
    if (e.key === 'Escape') { if (!$('fxBrowser').classList.contains('hidden')) closeBrowser(); else arm(-1); return; }
    if (S.view !== 'perform' || !$('fxBrowser').classList.contains('hidden')) return;
    if (e.key === ' ' || e.code === 'Space') { e.preventDefault(); if (!e.repeat) take(); }
    else if (e.key === 'Enter') { e.preventDefault(); if (!e.repeat) cut(); }
    else if ((e.key === 't' || e.key === 'T') && !e.repeat) tap();
  });
}

window.addEventListener('hashchange', () => { const v = location.hash.slice(1); if (VIEWS.includes(v) && v !== S.view) setView(v); });
buildStrips(); buildSlots(); wire(); connect(); loadStatic();
setView(VIEWS.includes(location.hash.slice(1)) ? location.hash.slice(1) : 'perform');
setTab('clip');
streams.forEach(pull);
requestAnimationFrame(tickBeat);
invalidate();
