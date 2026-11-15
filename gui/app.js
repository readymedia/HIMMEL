// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Himmel Perform (playable slice). Talks only to himmel_ui:
//   GET /events (SSE: bridge, catalog, live), POST /cmd (JSON commands), GET /preview/N (RGBA pictures).
// The page never touches the show directly (gui.md §1.5): every change is a command, and what you
// see is the engine's state coming back. Killing this page or the bridge changes nothing on air.
'use strict';

const $ = id => document.getElementById(id);
const BLEND = ['Normal', 'Add', 'Screen', 'Multiply', 'Difference', 'Lighten'];
const GROUPS = { prev: ['A', 'B', 'C', 'D'], prog: ['E', 'F', 'G', 'H'] };
const CODEC = ['HAP', 'HAP α', 'HAP Q', '?'];
const S = {
  cat: null, live: null, bridge: null,
  sse: false, lastBridgeMs: 0,
  armed: -1,            // clip id waiting for a layer click
  tab: 'layer',
  sent: 0, lastSeq: 0,
  seenRejected: null,
};

// ------------------------------------------------------------------ commands
async function send(cmd) {
  try {
    const r = await fetch('/cmd', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(cmd) });
    const j = await r.json();
    if (!j.ok) toast(j.error || 'refused by the bridge', 'bad');
    else { S.sent++; S.lastSeq = j.seq; }
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
const engineOk = () => S.sse && S.bridge && S.bridge.engine === 'ok';
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
const sleep = ms => new Promise(r => setTimeout(r, ms));

function toast(msg, kind) {
  const t = el('div', 'toast' + (kind === 'bad' ? ' bad' : ''), msg);
  $('toasts').append(t);
  while ($('toasts').children.length > 4) $('toasts').firstChild.remove();
  setTimeout(() => t.remove(), 4000);
}

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
      const pos = el('div', 'pos'); pos.append(el('i'));
      clip.append(sw, nm, miss, pos);
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
      s.addEventListener('dragover', e => { if (e.dataTransfer.types.includes('text/x-himmel-clip')) { e.preventDefault(); s.classList.add('dropover'); } });
      s.addEventListener('dragleave', () => s.classList.remove('dropover'));
      s.addEventListener('drop', e => {
        s.classList.remove('dropover');
        const id = parseInt(e.dataTransfer.getData('text/x-himmel-clip'), 10);
        if (!(id >= 0)) return;
        e.preventDefault();
        send({ type: 'LayerClip', ...t(), value: id });
        send({ type: 'Focus', ...t() });
      });
      strips[side].push({ s, sw, nm, miss, pos: pos.firstChild, opIn, opV, blend, mute, play, fx });
    }
  }
}

function renderStrips() {
  const L = S.live;
  for (const side of ['prev', 'prog']) {
    const d = sideDeck(side);
    strips[side].forEach((u, row) => {
      const ls = L.deck[d].layer[row];
      const ci = clipInfo(ls.clip);
      u.sw.style.background = ci ? rgb(ci.thumb_rgb) : '#000';
      u.nm.textContent = ci ? ci.name.replace(/\.mov$/i, '') : (ls.clip >= 0 ? 'clip ' + ls.clip : 'empty');
      u.nm.classList.toggle('empty', !ci);
      u.nm.title = ci ? `${ci.name} · ${ci.width}×${ci.height} · ${ci.fps} fps · ${ci.seconds.toFixed(1)} s` : '';
      u.miss.classList.toggle('hidden', !ls.missing);
      u.pos.style.width = (ci ? ls.position * 100 : 0) + '%';
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

// ------------------------------------------------------------------ clip library
let clipEls = [];
function buildLibrary() {
  const list = $('clipList');
  list.textContent = '';
  clipEls = [];
  if (!S.cat) return;
  const f = $('clipFilter').value.trim().toLowerCase();
  for (const c of S.cat.clips) {
    if (f && !c.name.toLowerCase().includes(f)) continue;
    const it = el('div', 'ci');
    it.draggable = true;
    const sw = el('div', 'sw'); sw.style.background = `linear-gradient(135deg, #000, ${rgb(c.thumb_rgb)})`;
    const tags = el('div', 'tags'); sw.append(tags);
    const txt = el('div', 'txt');
    txt.append(el('div', 'nm', c.name.replace(/\.mov$/i, '')),
      el('div', 'meta', `${c.width}×${c.height} · ${Math.round(c.fps)} · ${c.seconds.toFixed(0)}s · ${CODEC[c.codec] || '?'}`));
    it.append(sw, txt);
    it.title = c.name + '\nClick, then click a layer. Drag onto a layer. Double-click = into the focused layer.';
    it.addEventListener('click', () => arm(S.armed === c.id ? -1 : c.id));
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
  $('armHint').textContent = ci ? `Click a layer to load “${ci.name.replace(/\.mov$/i, '')}” (Esc = cancel, Shift = keep)` : '';
  renderLibrary();
  if (S.live) renderStrips();
}

// ------------------------------------------------------------------ inspector (FX slots)
const slots = [];
function buildSlots() {
  const box = $('slots');
  for (let k = 0; k < 4; k++) {
    const c = el('div', 'slot');
    const sh = el('div', 'sh');
    const n = el('span', 'n', String(k + 1));
    const sel = el('select'); sel.title = 'Effect in slot ' + (k + 1);
    const byp = el('button', 'byp', 'BYP'); byp.title = 'Bypass';
    sh.append(n, sel, byp);
    const mix = el('div', 'prm');
    const mixIn = el('input'); mixIn.type = 'range'; mixIn.min = 0; mixIn.max = 1; mixIn.step = 0.01;
    const mixV = el('span', 'pv');
    mix.append(el('span', 'pn', 'mix'), mixIn, mixV);
    const params = el('div', 'params');
    const empty = el('div', 'empty', 'empty slot');
    c.append(sh, mix, params, empty);
    box.append(c);
    hold(mixIn);
    const u = { c, sel, byp, mix, mixIn, mixV, params, empty, key: '', rows: [] };
    sel.addEventListener('change', () => send({ type: 'FxSet', ...fxTarget(), slot: k, value: +sel.value }));
    byp.addEventListener('click', () => send({ type: 'FxBypass', ...fxTarget(), slot: k, value: 2 }));
    mixIn.addEventListener('input', () => { mixV.textContent = Math.round(mixIn.value * 100); sendT('mix' + k, { type: 'FxMix', ...fxTarget(), slot: k, value: +mixIn.value }); });
    slots.push(u);
  }
}
function fillEffectOptions() {
  for (const u of slots) {
    u.sel.textContent = '';
    const o = el('option', null, '— none —'); o.value = -1; u.sel.append(o);
    (S.cat?.effects || []).forEach(e => { const op = el('option', null, e.name); op.value = e.id; u.sel.append(op); });
    u.key = '';
  }
}
// What the FX panel acts on: the focused layer, its deck, or master.
function fxTarget() {
  const L = S.live;
  const fd = L && L.focus_deck >= 0 ? L.focus_deck : 1 - progDeck();
  if (S.tab === 'master') return { deck: 'master' };
  if (S.tab === 'deck') return { deck: fd };
  return { deck: fd, layer: L && L.focus_layer >= 0 ? L.focus_layer : 0 };
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
  if (S.live) renderInspector();
}
function renderInspector() {
  const L = S.live, t = fxTarget();
  const onProg = t.deck !== 'master' && t.deck === L.program_deck;
  const sideWord = t.deck === 'master' ? 'MAIN' : onProg ? 'PROGRAM' : 'PREVIEW';
  const grpLetter = t.layer !== undefined ? GROUPS[onProg ? 'prog' : 'prev'][t.layer] : '';
  $('tabLayer').textContent = `LAYER ${grpLetter} · deck ${deckLetter(t.deck === 'master' ? 0 : t.deck)}`;
  $('tabDeck').textContent = `DECK ${deckLetter(t.deck === 'master' ? fxTargetDeck() : t.deck)} FX`;
  $('layerBar').classList.toggle('hidden', S.tab !== 'layer');
  $('inspector').style.borderColor = t.deck === 'master' ? 'var(--accent)' : onProg ? '#6a1a16' : '#1f5a30';
  if (S.tab === 'layer') {
    const ls = L.deck[t.deck].layer[t.layer];
    const ci = clipInfo(ls.clip);
    $('inspClip').textContent = `${sideWord} ${grpLetter}: ${ci ? ci.name : 'empty'}`;
    setVal($('inspSpeed'), ls.speed);
    if (performance.now() >= ($('inspSpeed')._hold || 0)) $('inspSpeedVal').textContent = ls.speed.toFixed(2) + 'x';
    $('inspJump').firstChild.style.left = (ls.position * 100) + '%';
  }
  const fx = fxSlotsNow();
  slots.forEach((u, k) => {
    const f = fx[k];
    const info = fxInfo(f.effect);
    const key = JSON.stringify(t) + ':' + f.effect;
    if (u.key !== key) {                      // a new effect in this slot: rebuild its parameter rows
      u.key = key;
      u.params.textContent = ''; u.rows = [];
      (info?.params || []).forEach((pn, p) => {
        const row = el('div', 'prm');
        const inp = el('input'); inp.type = 'range'; inp.min = 0; inp.max = 1; inp.step = 0.01;
        const pv = el('span', 'pv');
        row.append(el('span', 'pn', pn), inp, pv);
        hold(inp);
        inp.addEventListener('input', () => { pv.textContent = Math.round(inp.value * 100); sendT('fx' + k + '/' + p, { type: 'FxParam', ...fxTarget(), slot: k, param: p, value: +inp.value }); });
        inp.addEventListener('dblclick', () => send({ type: 'FxParam', ...fxTarget(), slot: k, param: p, value: info.defaults[p] ?? 0 }));
        inp.title = pn + ' (double-click = default)';
        u.params.append(row);
        u.rows.push({ inp, pv });
      });
    }
    if (document.activeElement !== u.sel) setVal(u.sel, f.effect);
    const has = f.effect >= 0;
    u.c.classList.toggle('has', has);
    u.c.classList.toggle('byp', has && f.bypass);
    u.byp.classList.toggle('on', has && !!f.bypass);
    u.byp.disabled = !has;
    u.mix.classList.toggle('hidden', !has);
    u.empty.classList.toggle('hidden', has);
    if (has) {
      setVal(u.mixIn, f.mix);
      if (performance.now() >= (u.mixIn._hold || 0)) u.mixV.textContent = Math.round(f.mix * 100);
      u.rows.forEach((r, p) => {
        setVal(r.inp, f.param[p]);
        if (performance.now() >= (r.inp._hold || 0)) r.pv.textContent = Math.round(f.param[p] * 100);
      });
    }
  });
}
function fxTargetDeck() { const L = S.live; return L && L.focus_deck >= 0 ? L.focus_deck : 1 - progDeck(); }

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

// ------------------------------------------------------------------ TAKE column, top bar
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
  else if (b.commands !== 'ok') { title = 'COMMANDS NOT AVAILABLE'; text = `The command block is ${b.commands}. You can watch but not play.`; warn = true; }
  $('overlay').classList.toggle('hidden', !title);
  $('overlay').classList.toggle('warnbox', warn);
  $('ovTitle').textContent = title; $('ovText').textContent = text;
  $('perform').classList.toggle('offline', !engineOk() || !S.live);
}

// ------------------------------------------------------------------ render loop (coalesced)
let dirty = false;
function invalidate() { if (!dirty) { dirty = true; requestAnimationFrame(render); } }
function render() {
  dirty = false;
  renderTop();
  if (!S.live || !S.cat) return;
  renderMonitors(); renderStrips(); renderTake(); renderLibrary(); renderInspector();
}

// ------------------------------------------------------------------ events from the bridge
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
    fillEffectOptions(); buildLibrary(); invalidate();
  });
  es.addEventListener('live', e => { S.live = JSON.parse(e.data); invalidate(); });
  // A bridge that hangs without closing the socket: no "bridge" event for 3 s = offline.
  setInterval(() => { if (S.sse && performance.now() - S.lastBridgeMs > 3000) { S.sse = false; invalidate(); es.close(); setTimeout(connect, 500); } }, 1000);
}

// ------------------------------------------------------------------ wiring
function take() { send({ type: 'Take', value: +$('takeMs').value }); }
function cut() { send({ type: 'Cut' }); }
function wire() {
  $('takeBtn').addEventListener('click', take);
  $('cutBtn').addEventListener('click', cut);
  $('blackBtn').addEventListener('click', () => send({ type: 'Black', value: 2 }));
  const xf = $('xf'); hold(xf);
  xf.addEventListener('input', () => sendT('xf', { type: 'Crossfader', value: +xf.value }));
  for (const b of document.querySelectorAll('.tabs > button')) b.addEventListener('click', () => setTab(b.dataset.tab));
  const sp = $('inspSpeed'); hold(sp);
  sp.addEventListener('input', () => { $('inspSpeedVal').textContent = (+sp.value).toFixed(2) + 'x'; sendT('speed', { type: 'LayerSpeed', ...fxTarget(), value: +sp.value }); });
  sp.addEventListener('dblclick', () => send({ type: 'LayerSpeed', ...fxTarget(), value: 1 }));
  $('inspJump').addEventListener('click', e => {
    const r = e.currentTarget.getBoundingClientRect();
    send({ type: 'LayerJump', ...fxTarget(), value: Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)) });
  });
  $('inspRestart').addEventListener('click', () => send({ type: 'LayerPlay', ...fxTarget(), value: 'Restart' }));
  $('inspEject').addEventListener('click', () => send({ type: 'LayerClip', ...fxTarget(), value: -1 }));
  $('clipFilter').addEventListener('input', buildLibrary);
  // Buttons never keep keyboard focus, so space / enter always mean TAKE / CUT.
  document.addEventListener('pointerup', e => { const b = e.target.closest('button'); if (b) b.blur(); });
  document.addEventListener('change', e => { if (e.target.matches('select, input[type=range]')) e.target.blur(); });
  document.addEventListener('keydown', e => {
    const t = e.target;
    if (t.matches && t.matches('input[type=search], input[type=text], textarea')) { if (e.key === 'Escape') t.blur(); return; }
    if (t.matches && t.matches('select') && e.key !== ' ' && e.key !== 'Enter') return;  // arrows still pick in a menu
    if (e.key === ' ' || e.code === 'Space') { e.preventDefault(); if (!e.repeat) take(); }
    else if (e.key === 'Enter') { e.preventDefault(); if (!e.repeat) cut(); }
    else if (e.key === 'Escape') arm(-1);
  });
}

buildStrips(); buildSlots(); fillEffectOptions(); wire(); connect();
streams.forEach(pull);
invalidate();
