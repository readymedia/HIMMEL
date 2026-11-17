// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// GUI wave 7: getting clips onto layers, from anywhere (the owner's test, 2026-11-17: "I found no way
// to get the clips onto layers"). Shared by PERFORM, LIBRARY, BUILD and the wizard:
//   Flows.rows()              the library rows (GET /library/clips, cached; null = no library in this run)
//   Flows.catOf(row)          the engine catalog id of a library row (D147: library clips are in the
//                             catalog), or -1 when it is not playable yet
//   Flows.rowOf(catId)        the library row of a catalog clip (thumbnail, analysis), or null
//   Flows.where(catId)        the layers a clip is on now: [{deck, layer, side, group}]
//   Flows.load(catId, t)      LayerClip onto t = {deck, layer} (or 'focus' / 'free-prev' / 'hit')
//   Flows.menu(ev, catIds)    the "Load to…" menu at the mouse (PREVIEW A–D, PROGRAM E–H, HIT, free)
//   Flows.picker(t)           a clip picker for one layer (the "empty" cell of a strip)
// Every action is the existing LayerClip command (command-protocol.md); nothing here is new protocol.
'use strict';

(() => {
  const S = window.S;
  const { el, send, toast } = window.H;
  const GROUPS = { prev: ['A', 'B', 'C', 'D'], prog: ['E', 'F', 'G', 'H'] };
  const progDeck = () => (S.live ? S.live.program_deck : 0);
  const sideOf = d => (d === progDeck() ? 'prog' : 'prev');
  const groupOf = (d, l) => GROUPS[sideOf(d)][l];
  const clipName = ci => ci.name.replace(/\.mov$/i, '');
  const thumbUrl = r => (r && r.thumb ? `/library/thumb/${r.hash}.jpg` : '');
  const hex = n => '#' + ((n >>> 0) & 0xffffff).toString(16).padStart(6, '0');

  // ------------------------------------------------------------------ the library rows (shared cache)
  const LR = { list: null, at: -1e9, busy: false, seq: -1, waiters: [], map: null, mapKey: '' };
  async function loadRows(force) {
    if (LR.busy) return new Promise(r => LR.waiters.push(r));
    if (!force && LR.list && performance.now() - LR.at < 4000) return LR.list;
    LR.busy = true;
    try {
      const r = await fetch('/library/clips', { cache: 'no-store' });
      LR.list = await r.json();
    } catch (e) {
      LR.list = LR.list || { ok: false, error: 'himmel_ui is not reachable', clips: [] };
    }
    LR.busy = false; LR.at = performance.now(); LR.mapKey = '';
    LR.waiters.splice(0).forEach(f => f(LR.list));
    window.dispatchEvent(new Event('himmel-librows'));
    window.H.invalidate();
    return LR.list;
  }
  // Re-read when the ingest worker says rows changed (ready_seq), or every 15 s otherwise.
  function tick() {
    const lib = S.library;
    if (lib && lib.present && lib.ready_seq !== LR.seq) { LR.seq = lib.ready_seq; loadRows(true); }
    else if (performance.now() - LR.at > 15000) loadRows(false);
  }

  // ------------------------------------------------------------------ catalog <-> library (D147)
  // The library feed appends ready clips to the catalog in ready order, named after the row; a name
  // already taken gets " (lib)" / " (lib 2)" (library feed.cpp, kNameLen 48 = 47 UTF-8 bytes).
  // GUESS: until the catalog carries the library id, a row is matched by name in ready order (a cursor
  // that only moves forward, because ids are append-only). The rare miss: a media/ clip AND two library
  // clips with the same name. Then the first library clip may point at the media clip.
  const enc = new TextEncoder(), dec = new TextDecoder();
  function trunc(s, bytes) {
    const b = enc.encode(s);
    if (b.length <= bytes) return s;
    let n = bytes;
    while (n > 0 && (b[n] & 0xc0) === 0x80) n--;
    return dec.decode(b.slice(0, n));
  }
  function candidates(name) {
    const base = trunc(name, 35), out = [trunc(name, 47).toLowerCase()];
    for (let k = 1; k < 6; k++) out.push(trunc(base + (k === 1 ? ' (lib)' : ` (lib ${k})`), 47).toLowerCase());
    return out;
  }
  function buildMap() {
    const cat = S.cat?.clips || [], rows = LR.list?.ok ? LR.list.clips : [];
    const key = cat.length + '|' + LR.at;
    if (LR.mapKey === key && LR.map) return LR.map;
    const m = { byRow: new Map(), byCat: new Map() };
    // An engine that publishes the library id with the clip wins (catalog JSON "lib"), when present.
    if (cat.some(c => c.lib != null)) {
      const byId = new Map(rows.map(r => [r.id, r]));
      for (const c of cat) { const r = byId.get(c.lib); if (r) { m.byRow.set(r.id, c.id); m.byCat.set(c.id, r); } }
    } else {
      const names = cat.map(c => c.name.toLowerCase());
      const ready = rows.filter(r => r.state === 'ready' || (r.order > 0 && r.state === 'analysing')).sort((a, b) => (a.order || 0) - (b.order || 0));
      const count = new Map();
      for (const r of ready) count.set(r.name.toLowerCase(), (count.get(r.name.toLowerCase()) || 0) + 1);
      let cursor = 0;
      for (const r of ready) {
        const c = candidates(r.name), exact = c[0];
        let hit = -1;
        for (let i = cursor; i < names.length && hit < 0; i++) if (c.includes(names[i])) hit = i;
        if (hit < 0) continue;
        // An exact name with a " (lib)" twin later: the exact one is a media/ clip, unless another
        // library row has this name too (then the first library clip got the plain name).
        if (names[hit] === exact && (count.get(r.name.toLowerCase()) || 0) < 2) {
          for (let i = hit + 1; i < names.length; i++) if (names[i] !== exact && c.includes(names[i])) { hit = i; break; }
        }
        m.byRow.set(r.id, hit); m.byCat.set(hit, r);
        cursor = hit + 1;
      }
    }
    LR.map = m; LR.mapKey = key;
    return m;
  }
  const catOf = row => (row ? (buildMap().byRow.get(row.id) ?? -1) : -1);
  const rowOf = id => buildMap().byCat.get(id) || null;

  // ------------------------------------------------------------------ where a clip is
  function where(id) {
    const L = S.live, out = [];
    if (!L || id < 0) return out;
    L.deck.forEach((dk, d) => dk.layer.forEach((l, k) => { if (l.clip === id) out.push({ deck: d, layer: k, side: sideOf(d), group: groupOf(d, k) }); }));
    if (S.tr?.hit?.clip === id) out.push({ deck: 'hit', layer: 0, side: 'hit', group: 'HIT' });
    return out;
  }
  // Small tags "A E2" for a card: green = PREVIEW, red = PROGRAM (on air).
  function whereTags(id) {
    const box = el('span', 'wtags');
    for (const w of where(id)) { const t = el('b', 'wt ' + w.side, w.group); t.title = (w.side === 'prog' ? 'On air: PROGRAM ' : w.side === 'hit' ? 'On the HIT layer' : 'PREVIEW ') + w.group; box.append(t); }
    return box;
  }

  // ------------------------------------------------------------------ loading
  function focus() {
    const L = S.live;
    if (!L) return null;
    const d = L.focus_deck >= 0 ? L.focus_deck : 1 - L.program_deck;
    return { deck: d, layer: L.focus_layer >= 0 ? L.focus_layer : 0 };
  }
  // The lowest empty layer of a deck (no clip, no generator); none free = the deck's focused / first layer.
  function freeLayer(d) {
    const L = S.live;
    if (!L) return 0;
    const ls = L.deck[d].layer;
    const k = ls.findIndex(l => l.clip < 0 && !l.fx.some(f => f.effect >= 0));
    if (k >= 0) return k;
    return L.focus_deck === d && L.focus_layer >= 0 ? L.focus_layer : 0;
  }
  function resolve(t) {
    const L = S.live;
    if (!L) return null;
    if (t === 'focus') return focus();
    if (t === 'free-prev') { const d = 1 - L.program_deck; return { deck: d, layer: freeLayer(d) }; }
    if (t === 'free-prog') { const d = L.program_deck; return { deck: d, layer: freeLayer(d) }; }
    if (t && t.deck !== undefined && t.layer === undefined && t.deck !== 'hit') return { deck: t.deck, layer: freeLayer(t.deck) };
    return t;
  }
  // ids: one catalog id, or several (several go onto consecutive layers from the target, max 4).
  function load(ids, target) {
    ids = [].concat(ids).filter(i => i >= 0);
    if (!ids.length) { toast('That clip is not playable yet (it is still importing).', 'bad'); return; }
    if (!S.live) { toast('The engine is not running: nothing to load into.', 'bad'); return; }
    if (target === 'hit' || target?.deck === 'hit') {
      send({ type: 'LayerTrigger', deck: 'hit', value: ids[0], velocity: 1 });
      toast('HIT: ' + nameOf(ids[0]));
      return;
    }
    const t = resolve(target);
    if (!t) return;
    const cmds = [];
    ids.slice(0, 4 - t.layer).forEach((id, k) => cmds.push({ type: 'LayerClip', deck: t.deck, layer: t.layer + k, value: id }));
    cmds.push({ type: 'Focus', deck: t.deck, layer: t.layer });
    send(cmds);
    const onAir = t.deck === S.live.program_deck;
    toast(`${ids.length > 1 ? cmds.length - 1 + ' clips' : '“' + nameOf(ids[0]) + '”'} → ${onAir ? 'PROGRAM (on air)' : 'PREVIEW'} ${groupOf(t.deck, t.layer)}` +
      (ids.length > cmds.length - 1 ? ` (only ${cmds.length - 1} fit)` : ''));
  }
  const nameOf = id => { const c = S.cat?.clips?.[id]; return c ? clipName(c) : 'clip ' + id; };

  // ------------------------------------------------------------------ the "Load to…" menu
  let menuEl = null;
  function closeMenu() { if (menuEl) { menuEl.remove(); menuEl = null; } }
  function menu(ev, ids) {
    ev.preventDefault?.(); ev.stopPropagation?.();
    closeMenu();
    ids = [].concat(ids).filter(i => i >= 0);
    const L = S.live;
    const m = el('div', 'loadmenu');
    m.addEventListener('pointerdown', e => e.stopPropagation());
    if (!ids.length) m.append(el('div', 'lmnote', 'Not playable yet: the clip is still importing (or the catalog is full).'));
    else if (!L) m.append(el('div', 'lmnote', 'The engine is not running.'));
    else {
      m.append(el('div', 'lmhead', ids.length > 1 ? `LOAD ${ids.length} CLIPS TO…` : `LOAD “${nameOf(ids[0])}” TO…`));
      const f = focus();
      const item = (label, sub, t, cls, title) => {
        const b = el('button', 'lmi ' + (cls || ''));
        b.append(el('b', null, label), el('span', 'dim', sub || ''));
        b.title = title || '';
        b.addEventListener('click', () => { closeMenu(); load(ids, t); });
        return b;
      };
      m.append(item('FOCUSED LAYER', f ? `${sideOf(f.deck) === 'prog' ? 'PROGRAM' : 'PREVIEW'} ${groupOf(f.deck, f.layer)}` : '', 'focus', sideOf(f.deck), 'The layer the MK2 / this page has in focus (same as a double-click)'));
      m.append(item('NEXT FREE · PREVIEW', '', 'free-prev', 'prev', 'The lowest empty layer of the PREVIEW deck (off air: you see it before a TAKE)'));
      for (const side of ['prev', 'prog']) {
        const d = side === 'prog' ? L.program_deck : 1 - L.program_deck;
        const row = el('div', 'lmrow');
        row.append(el('span', 'lbl ' + side, side === 'prog' ? 'PROGRAM · on air' : 'PREVIEW'));
        for (let k = 0; k < 4; k++) {
          const ls = L.deck[d].layer[k], ci = S.cat?.clips?.[ls.clip];
          const b = el('button', 'lmg ' + side);
          b.append(el('b', null, GROUPS[side][k]), el('span', null, ci ? clipName(ci) : 'empty'));
          b.title = `${side === 'prog' ? 'PROGRAM (on air!)' : 'PREVIEW'} layer ${k + 1} (MK2 group ${GROUPS[side][k]}): ${ci ? 'replaces ' + clipName(ci) : 'empty'}`;
          b.addEventListener('click', () => { closeMenu(); load(ids, { deck: d, layer: k }); });
          row.append(b);
        }
        m.append(row);
      }
      if (ids.length === 1) m.append(item('HIT LAYER', 'one-shot on top', 'hit', 'hit', 'Fire it once on the HIT layer, on top of PROGRAM'));
    }
    document.body.append(m);
    menuEl = m;
    const r = m.getBoundingClientRect();
    const x = Math.min(ev.clientX ?? 100, innerWidth - r.width - 8), y = Math.min(ev.clientY ?? 100, innerHeight - r.height - 8);
    m.style.left = Math.max(8, x) + 'px'; m.style.top = Math.max(8, y) + 'px';
    setTimeout(() => document.addEventListener('pointerdown', closeMenu, { once: true }), 0);
  }
  document.addEventListener('keydown', e => { if (e.key === 'Escape') { closeMenu(); closePicker(); } }, true);

  // ------------------------------------------------------------------ the clip picker (one layer)
  let pickEl = null;
  function closePicker() { if (pickEl) { pickEl.remove(); pickEl = null; } }
  function picker(target) {
    closePicker();
    const t = resolve(target);
    if (!t || !S.cat) { toast('The engine is not running.', 'bad'); return; }
    loadRows(false);
    const onAir = t.deck === S.live?.program_deck;
    const md = el('div', 'modal'), box = el('div', 'mbox pickbox' + (onAir ? ' onair' : ''));
    const head = el('div', 'mhead');
    head.append(el('span', 'lbl', `CLIP FOR ${onAir ? 'PROGRAM (ON AIR)' : 'PREVIEW'} ${groupOf(t.deck, t.layer)} · deck ${'AB'[t.deck]}`));
    const q = el('input'); q.type = 'search'; q.placeholder = 'search clips'; q.autocomplete = 'off';
    q.addEventListener('keydown', e => e.stopPropagation());
    const close = el('button', null, '✕'); close.title = 'Close (Esc)'; close.addEventListener('click', closePicker);
    head.append(q, el('div', 'spacer'), close);
    const grid = el('div', 'pickgrid');
    const foot = el('div', 'mfoot dim', onAir ? 'This layer is ON AIR: the clip shows at once. Load to PREVIEW and TAKE for a smooth change.' : 'Click a clip to put it on this layer. You see it in the PREVIEW monitor; TAKE sends it on air.');
    box.append(head, grid, foot); md.append(box);
    md.addEventListener('click', e => { if (e.target === md) closePicker(); });
    document.body.append(md); pickEl = md;
    const fill = () => {
      grid.textContent = '';
      const f = q.value.trim().toLowerCase();
      const clips = (S.cat?.clips || []).filter(c => !f || c.name.toLowerCase().includes(f) || (rowOf(c.id)?.tags || []).join(' ').toLowerCase().includes(f));
      if (!S.cat?.clips?.length) grid.append(el('div', 'empty', 'The engine has no clips yet. Import some in LIBRARY (+ IMPORT FOLDER…); they appear here when they are ready.'));
      else if (!clips.length) grid.append(el('div', 'empty', 'Nothing matches.'));
      for (const c of clips) {
        const r = rowOf(c.id);
        const card = el('button', 'pcard');
        const th = el('div', 'pth'); th.style.background = hex(c.thumb_rgb);
        const u = thumbUrl(r);
        if (u) { const img = el('img'); img.loading = 'lazy'; img.src = u; img.alt = ''; img.onerror = () => img.remove(); th.append(img); }
        th.append(whereTags(c.id));
        card.append(th, el('div', 'pnm', clipName(c)), el('div', 'dim mono pmeta', `${c.width}×${c.height} · ${Math.round(c.fps)} fps · ${c.seconds.toFixed(0)} s` + (r?.analysis ? ` · E${r.analysis.energy}` : '')));
        card.title = c.name + '\nClick = put it on this layer';
        card.addEventListener('click', () => { closePicker(); load(c.id, t); });
        grid.append(card);
      }
    };
    q.addEventListener('input', fill);
    fill();
    q.focus();
    // Thumbnails arrive with the rows: redraw once they are in.
    if (!LR.list) loadRows(false).then(() => { if (pickEl === md) fill(); });
  }

  // ------------------------------------------------------------------ drag and drop
  // A drag carries catalog ids ("text/x-himmel-clip" = one id, "text/x-himmel-clips" = a JSON list).
  function setDrag(e, ids) {
    ids = [].concat(ids).filter(i => i >= 0);
    if (!ids.length) { e.preventDefault(); toast('Not playable yet: still importing.', 'bad'); return; }
    e.dataTransfer.setData('text/x-himmel-clip', String(ids[0]));
    if (ids.length > 1) e.dataTransfer.setData('text/x-himmel-clips', JSON.stringify(ids));
    e.dataTransfer.effectAllowed = 'copy';
  }
  function dragIds(e) {
    try { const l = JSON.parse(e.dataTransfer.getData('text/x-himmel-clips') || 'null'); if (Array.isArray(l)) return l; } catch (x) { /* one id */ }
    const id = parseInt(e.dataTransfer.getData('text/x-himmel-clip'), 10);
    return id >= 0 ? [id] : [];
  }
  const isClipDrag = e => e.dataTransfer.types.includes('text/x-himmel-clip');
  // Makes `node` a drop target for clips; target() = {deck, layer} or {deck} (= its first free layer).
  function dropTarget(node, target) {
    node.addEventListener('dragover', e => { if (isClipDrag(e)) { e.preventDefault(); node.classList.add('dropover'); } });
    node.addEventListener('dragleave', () => node.classList.remove('dropover'));
    node.addEventListener('drop', e => {
      node.classList.remove('dropover');
      if (!isClipDrag(e)) return;
      e.preventDefault(); e.stopPropagation();
      load(dragIds(e), target());
    });
  }

  window.Flows = { rows: () => LR.list, loadRows, tick, catOf, rowOf, where, whereTags, load, menu, picker, closeMenu, setDrag, dragIds, dropTarget, focus, freeLayer, nameOf, thumbUrl, groupOf, sideOf };
})();
