// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// M11 part 3: the BUILD view = the song, scene and kit editor (gui.md §17 "Build › Song and scene
// editor", mockup docs/gui-mockups/v4/V4SceneKit). Three columns:
//   left   the setlist (songs: new / rename / BPM / move / delete) or the bank, and the page's 16 scenes;
//   centre the selected scene: layers, transition, variations, its 4 macros with their bindings;
//   right  the kit: 16 pads (pad 1 bottom-left, like the MK2), the pad editor, "Fill kit".
// What it shows comes from GET /show/document (the show's autosave, read-only; show-files.md §3).
// Edits use the commands of the SHOW contract only: KitSet 33, MacroSet 34, SongEdit 36,
// SceneRecall 25 (REHEARSE = load to PREVIEW), ShowSelect 32. KitSet / MacroSet change the scene
// that is ON a deck, so the editor edits the scene on the PREVIEW deck; another scene shows read-only
// with a REHEARSE button. Nothing here is needed for the show to run: no show block = view hidden.
'use strict';

(() => {
  const $ = id => document.getElementById(id);
  const { el, send, toast } = window.H;
  const S = window.S;

  const B = { adds: [0, 0, 0, 0], doc: null, at: 0, key: '', fetching: false, again: 0, sel: -1, pad: 12, built: false, rkey: '' };
  const TAG = { Selected: 'SEL', Layer1: 'L1', Layer2: 'L2', Layer3: 'L3', Layer4: 'L4', All: 'ALL', Free: 'FREE', Hit: 'HIT' };
  const TAG_FILE = { selected: 'Selected', layer1: 'Layer1', layer2: 'Layer2', layer3: 'Layer3', layer4: 'Layer4', all: 'All', free: 'Free', hit: 'Hit' };
  const KIND_FILE = { empty: 'Empty', clip: 'Clip', cue: 'Cue', effect: 'Effect', scene: 'Scene', action: 'Action' };
  const ACTION_WORDS = { Black: 'BLACK', WhiteFlash: 'WHITE FLASH', Freeze: 'FREEZE', NextScene: 'NEXT SCENE', DiceLayer: 'DICE LAYER' };
  const ACTIONS = Object.keys(ACTION_WORDS);
  const btn = (txt, title, fn, cls) => { const b = el('button', cls || null, txt); if (title) b.title = title; b.addEventListener('click', fn); return b; };
  const ask = (...a) => window.Panels.W.ask(...a);

  const previewDeck = () => (S.live ? 1 - S.live.program_deck : 1);
  // The scene object of page pad k in the document (bank pad or song position).
  function docScene(k) {
    const d = B.doc, sh = S.show;
    if (!d || !d.ok || !sh || k < 0) return null;
    if (sh.use_setlist) return d.setlist?.[sh.song]?.scenes?.[k] || null;
    const bank = (d.banks || []).find(b => b.index === sh.bank);
    return bank?.pads?.[k] || null;
  }
  // The document scene ON deck d (its variation when one is recalled), or null.
  function deckScene(d) {
    const sh = S.show, ds = sh?.decks?.[d];
    if (!ds || ds.pad < 0 || ds.scene < 0) return null;
    const sc = docScene(ds.pad);
    if (!sc) return null;
    const v = ds.variation ? ds.variation.charCodeAt(0) - 97 : 0;
    return v > 0 ? (sc.variations?.[v - 1] || sc) : sc;
  }
  const editable = k => { const sh = S.show, d = previewDeck(); return !!(sh && sh.decks?.[d]?.pad === k && sh.decks[d].scene >= 0); };

  // ---- the document: refetched when the show changed (the autosave is coalesced, <= 4/s: a second fetch follows)
  async function fetchDoc() {
    if (B.fetching) { B.again = 1; return; }
    B.fetching = true;
    try {
      const r = await fetch('/show/document');
      if (r.ok) { B.doc = await r.json(); B.at = performance.now(); B.adds = [0, 0, 0, 0]; B.rkey = ''; window.H.invalidate(); }
    } catch (e) { /* bridge gone: the page shows offline anyway */ }
    B.fetching = false;
    if (B.again) { B.again = 0; setTimeout(fetchDoc, 300); }
  }
  function showKey() {
    const sh = S.show;
    return JSON.stringify([sh.message_seq, sh.scene_count, sh.acked, sh.bank, sh.song, sh.use_setlist, sh.song_count, sh.project, sh.files?.path, sh.files?.saves, sh.decks.map(d => [d.scene, d.pad, d.variation])]);
  }

  // ================================================================ build once
  function build() {
    const box = $('buildBody'); box.textContent = '';
    const head = el('div', 'bdhead');
    head.append(el('span', 'bdstep', '1 Prepare the song'), el('span', 'dim', '→'), el('span', 'bdstep', '2 Rehearse on PREVIEW'), el('span', 'dim', '→'),
      el('span', 'bdstep', '3 Play: ENTER + pads + macros'), el('div', 'spacer'));
    B.status = el('span', 'dim mono', ''); head.append(B.status);
    const cols = el('div', 'bdcols');
    B.left = el('section', 'bdcol bdleft'); B.mid = el('section', 'bdcol bdmid'); B.right = el('section', 'bdcol bdright');
    cols.append(B.left, B.mid, B.right);
    box.append(head, cols);
    B.built = true;
  }

  // ================================================================ left: setlist / bank + the page's scenes
  function renderLeft() {
    const sh = S.show, d = B.doc, L = B.left; L.textContent = '';
    const mode = el('div', 'bdmode');
    mode.append(btn('BANKS', 'Pages show a bank', () => sh.use_setlist && send({ type: 'ShowSelect', param: 'UseSetlist', value: 0 }), sh.use_setlist ? 'chip2' : 'chip2 on'),
      btn('SETLIST', 'Pages show the current song', () => !sh.use_setlist && send({ type: 'ShowSelect', param: 'UseSetlist', value: 1 }), sh.use_setlist ? 'chip2 on' : 'chip2'));
    L.append(mode);
    if (!sh.use_setlist) {
      const bk = el('div', 'bdbank');
      bk.append(btn('◀', 'Previous bank', () => send({ type: 'ShowSelect', param: 'Bank', value: -1, relative: true })),
        el('span', 'bdbankn', `BANK ${sh.bank + 1}`),
        btn('▶', 'Next bank', () => send({ type: 'ShowSelect', param: 'Bank', value: 1, relative: true })));
      L.append(bk);
    } else {
      L.append(el('div', 'lbl', 'SETLIST'));
      const list = el('div', 'bdsongs');
      const songs = d?.setlist || [];
      songs.forEach((so, i) => {
        const r = el('div', 'bdsong' + (i === sh.song ? ' on' : ''));
        const nm = el('button', 'bdsongn', `${i + 1} ${so.name}`);
        nm.title = 'Show this song\'s scenes on the pages';
        nm.addEventListener('click', () => send({ type: 'ShowSelect', param: 'Song', value: i }));
        nm.addEventListener('dblclick', () => renameSong(i, so.name));
        r.append(nm, el('span', 'dim mono', so.bpm ? so.bpm + ' bpm' : ''));
        if (i === sh.song) {
          const tools = el('span', 'bdtools');
          tools.append(btn('✎', 'Rename the song', () => renameSong(i, so.name)),
            btn('BPM', 'The song\'s tempo (0 = none)', () => songBpm(i, so.bpm || 0)),
            btn('↑', 'Move up in the setlist', () => i > 0 && send({ type: 'SongEdit', param: 'Move', value: i, slot: i - 1 })),
            btn('↓', 'Move down', () => i + 1 < songs.length && send({ type: 'SongEdit', param: 'Move', value: i, slot: i + 1 })),
            btn('✕', 'Delete the song and its scenes', () => confirm(`Delete "${so.name}" and its scenes?`) && send({ type: 'SongEdit', param: 'Delete', value: i }), 'bad'));
          r.append(tools);
        }
        list.append(r);
      });
      if (!songs.length) list.append(el('div', 'dim', sh.song_count ? 'Reading the setlist…' : 'No songs yet.'));
      L.append(list, btn('+ NEW SONG', 'Add a song at the end of the setlist', async () => {
        const n = await ask('NEW SONG · name', '', 'Empty = "Song n".');
        if (n === null) return;
        send(n ? { type: 'SongEdit', param: 'New', text: n } : { type: 'SongEdit', param: 'New' });
      }));
    }
    // The page's scenes (the same 16 as the SCENES tab / LOOKS pads).
    const title = sh.use_setlist ? (sh.song >= 0 ? `Scenes in "${sh.song_name}"` : 'No song selected') : `Scenes in bank ${sh.bank + 1}`;
    L.append(el('div', 'lbl', title));
    const sl = el('div', 'bdscenes');
    const pd = previewDeck();
    sh.pads.forEach((p, k) => {
      if (!p.used) return;
      const r = el('button', 'bdscene' + (k === B.sel ? ' sel' : ''));
      const sw = el('i'); sw.style.background = '#' + (p.rgb ?? 0x404040).toString(16).padStart(6, '0');
      const sc = docScene(k);
      const nl = sc ? (sc.look?.layers || []).filter(x => x?.clip?.name).length : 0;
      const kit = sc ? (sc.kit || []).filter(Boolean).length : 0;
      r.append(sw, el('span', 'bdsn', `${k + 1} ${p.name}`), el('span', 'dim', `${nl} layer${nl === 1 ? '' : 's'}${kit ? ` · kit ${kit}` : ''}${p.variations > 1 ? ` · ${p.variations} var` : ''}`));
      const tags = el('span', 'bdon');
      if (p.on?.[pd]) tags.append(el('b', 'prev', 'PREVIEW'));
      if (p.on?.[1 - pd]) tags.append(el('b', 'prog', 'LIVE'));
      r.append(tags);
      r.addEventListener('click', () => { B.sel = k; B.rkey = ''; window.H.invalidate(); });
      r.addEventListener('dblclick', () => rehearse(k));
      sl.append(r);
    });
    if (!sh.pads.some(p => p.used)) sl.append(el('div', 'dim', 'No scenes on this page yet. Build a look on PREVIEW, then REC (SCENES tab) or "+ scene from PREVIEW".'));
    L.append(sl);
    if (sh.use_setlist && sh.song >= 0) {
      L.append(btn('+ SCENE FROM PREVIEW', 'The PREVIEW deck\'s look becomes a new scene at the end of this song', () => send({ type: 'SongEdit', param: 'AddScene', value: sh.song, deck: pd })));
    }
  }
  async function renameSong(i, cur) {
    const n = await ask(`RENAME SONG ${i + 1}`, cur);
    if (n) send({ type: 'SongEdit', param: 'Rename', slot: i, text: n });
  }
  async function songBpm(i, cur) {
    const v = await ask(`SONG ${i + 1} · BPM (20–300, 0 = none)`, String(cur));
    if (v === null) return;
    const b = +v;
    if (!Number.isFinite(b) || (b !== 0 && (b < 20 || b > 300))) return toast('BPM must be 0 or 20–300', 'bad');
    send({ type: 'SongEdit', param: 'Bpm', value: i, f: b });
  }
  function rehearse(k) { send({ type: 'SceneRecall', deck: previewDeck(), value: k, f: 0 }); }

  // ================================================================ centre: the scene
  function renderMid() {
    const sh = S.show, M = B.mid; M.textContent = '';
    const k = B.sel, p = sh.pads[k];
    if (k < 0 || !p?.used) { M.append(el('div', 'empty', 'Pick a scene on the left (double-click = REHEARSE on PREVIEW).')); return; }
    const sc = docScene(k), ed = editable(k);
    const head = el('div', 'bdshead');
    head.append(el('span', 'bdsname', `${k + 1} ${p.name}`));
    if (sc?.variations?.length) head.append(el('span', 'dim', 'variations ' + ['a', ...sc.variations.map((_, i) => String.fromCharCode(98 + i))].map(x => (k + 1) + x).join(' ')));
    head.append(el('div', 'spacer'));
    head.append(ed ? el('span', 'chip prevchip', 'PREVIEW · rehearsing') :
      btn('REHEARSE = load to PREVIEW', 'Recall this scene into the PREVIEW deck (a cut) so you can edit its kit and macros', () => rehearse(k), 'primary'));
    head.append(btn('RENAME', 'Rename the scene', async () => { const n = await ask(`RENAME SCENE ${k + 1}`, p.name); if (n) send({ type: 'SongEdit', param: 'RenameScene', slot: k, text: n }); }));
    if (sh.use_setlist && sh.song >= 0) head.append(btn('✕', 'Remove this scene from the song', () => confirm(`Remove "${p.name}" from the song?`) && send({ type: 'SongEdit', param: 'RemoveScene', value: sh.song, slot: k }), 'bad'));
    M.append(head);
    if (!sc) { M.append(el('div', 'dim', B.doc?.ok === false ? 'The show document is not readable: ' + (B.doc.error || '') : 'Reading the show document…')); return; }
    // Layers.
    M.append(el('div', 'lbl', 'LAYERS'));
    const ly = el('div', 'bdlayers');
    (sc.look?.layers || []).forEach((l, i) => {
      const r = el('div', 'bdlayer');
      const fx = (l?.fx || []).filter(Boolean).map(f => f.effect).join(' · ');
      const tr = l?.transport || {};
      r.append(el('b', null, 'L' + (i + 1)), el('span', null, l?.clip?.name ? l.clip.name.replace(/\.mov$/i, '') : '—'),
        el('span', 'dim', l?.clip?.name ? [String(tr.mode || 'loop').toUpperCase(), (tr.time_base === 'bpm' || tr.time_base === 1) ? (tr.beats || '') + ' beats' : '', (l.blend && l.blend !== 'normal') ? l.blend : '', fx].filter(Boolean).join(' · ') : 'empty (kit pads may use it)'));
      ly.append(r);
    });
    const dfx = (sc.look?.fx || []).filter(Boolean).map(f => f.effect);
    if (dfx.length) { const r = el('div', 'bdlayer'); r.append(el('b', null, 'DECK'), el('span', null, dfx.join(' · '))); ly.append(r); }
    M.append(ly);
    const t = sc.transition || {};
    const trn = el('div', 'bdtrans');
    trn.append(el('span', 'lbl', 'transition in'), el('span', null, `${String(t.kind || 'cut').toUpperCase()}${t.kind && t.kind !== 'cut' ? ' ' + (t.seconds ?? 0) + ' s' : ''}${t.on_beat ? ', on the beat' : ''}`));
    M.append(trn);
    // Macros.
    M.append(el('div', 'lbl', 'MACROS (knobs 5–8 while this scene is live)'));
    const mac = el('div', 'bdmacros');
    for (let m = 0; m < 4; ++m) mac.append(macroCard(sc, m, ed));
    M.append(mac);
    if (!ed) M.append(el('div', 'dim', 'Read-only: REHEARSE puts the scene on PREVIEW; then kit and macro edits go into it (save the project to keep them).'));
  }
  // Macro binding targets: deck FX (layer = none) or a layer's FX; slot 1..4; param 1..16.
  function fxOf(sc, layer, slot) {
    const list = layer < 0 ? sc.look?.fx : sc.look?.layers?.[layer]?.fx;
    return list?.[slot] || null;
  }
  function paramNames(fxName) {
    const e = (S.cat?.effects || []).find(x => x.name === fxName);
    return e ? e.params : [];
  }
  function macroCard(sc, m, ed) {
    const mo = sc.macros?.[m] || { name: '', value: 0, binds: [] };
    const c = el('div', 'bdmac');
    const h = el('div', 'bdmachead');
    h.append(el('b', null, String(m + 5)), el('span', 'bdmacn', mo.name || `MACRO ${m + 5}`));
    if (ed) h.append(btn('✎', 'Rename the macro', async () => { const n = await ask(`MACRO ${m + 5} · name`, mo.name); if (n) send({ type: 'MacroSet', deck: previewDeck(), slot: m, text: n }); }));
    c.append(h);
    const binds = mo.binds || [];
    binds.forEach((b, i) => {
      const r = el('div', 'bdbind');
      const fx = fxOf(sc, b.layer, b.slot);
      const pn = fx ? (paramNames(fx.effect)[b.param] || 'param ' + (b.param + 1)) : 'param ' + (b.param + 1);
      r.append(el('span', null, `${b.layer < 0 ? 'DECK' : 'L' + (b.layer + 1)} FX${b.slot + 1}${fx ? ' ' + fx.effect : ''} · ${pn}`));
      const bar = el('span', 'bdrange'); const f = el('i'); f.style.left = (b.min * 100) + '%'; f.style.width = Math.max(1, (b.max - b.min) * 100) + '%'; bar.append(f);
      bar.title = `${Math.round(b.min * 100)}% → ${Math.round(b.max * 100)}%`;
      r.append(bar);
      if (ed) {
        r.append(btn('edit', 'Change this binding', () => editBind(sc, m, i, b, binds.length)),
          btn('✕', 'Remove this binding', () => removeBind(m, i, binds)));
      }
      c.append(r);
    });
    if (!binds.length) c.append(el('div', 'dim', 'Not bound (a fallback drives it, show-files.md §6).'));
    // Binds added since the last document read count too, so two quick + BINDs never land on one slot.
    const n = binds.length + B.adds[m];
    if (ed && n < 8) c.append(btn('+ BIND', 'Bind a parameter of an effect in this scene', () => editBind(sc, m, n, null, n)));
    return c;
  }
  // The file lists only used binds, in order; the editor keeps them packed (slot index = list index).
  function removeBind(m, i, binds) {
    const d = previewDeck();
    for (let k = i; k + 1 < binds.length; ++k) {
      const b = binds[k + 1];
      send({ type: 'MacroSet', deck: d, slot: m, param: k, layer: b.layer < 0 ? undefined : b.layer, value: b.slot * 16 + b.param, f: [b.min, b.max] });
    }
    send({ type: 'MacroSet', deck: d, slot: m, param: binds.length - 1, clear: true });
  }
  function editBind(sc, m, i, cur, count) {
    const md = el('div', 'modal'), box = el('div', 'mbox bdbindbox');
    box.append(el('div', 'lbl', `MACRO ${m + 5} · binding ${i + 1}`));
    const where = el('select');
    const targets = [[-1, 'DECK FX'], [0, 'L1'], [1, 'L2'], [2, 'L3'], [3, 'L4']];
    for (const [v, t] of targets) { const o = el('option', null, t); o.value = v; where.append(o); }
    const slot = el('select');
    const par = el('select');
    const fillSlots = () => {
      slot.textContent = '';
      for (let s = 0; s < 4; ++s) { const f = fxOf(sc, +where.value, s); const o = el('option', null, `FX ${s + 1}${f ? ' · ' + f.effect : ' (empty)'}`); o.value = s; slot.append(o); }
    };
    const fillParams = () => {
      par.textContent = '';
      const f = fxOf(sc, +where.value, +slot.value);
      const names = f ? paramNames(f.effect) : [];
      const n = Math.max(names.length, 1);
      for (let p = 0; p < (f ? n : 16); ++p) { const o = el('option', null, names[p] || `param ${p + 1}`); o.value = p; par.append(o); }
    };
    where.value = cur ? cur.layer : -1; fillSlots(); slot.value = cur ? cur.slot : 0; fillParams(); if (cur) par.value = cur.param;
    where.addEventListener('change', () => { fillSlots(); fillParams(); });
    slot.addEventListener('change', fillParams);
    const mk = (lbl, v) => { const w = el('label', 'nb'); const x = el('input'); x.type = 'number'; x.min = 0; x.max = 100; x.step = 1; x.value = Math.round(v * 100); w.append(el('span', 'lbl', lbl), x, el('span', 'u', '%')); return { w, x }; };
    const lo = mk('min', cur ? cur.min : 0), hi = mk('max', cur ? cur.max : 1);
    const row = el('div', 'bdbindrow'); row.append(where, slot, par, lo.w, hi.w);
    const close = () => md.remove();
    const ok = btn('SET', 'Send MacroSet', () => {
      const a = Math.min(1, Math.max(0, +lo.x.value / 100)), b = Math.min(1, Math.max(0, +hi.x.value / 100));
      const layer = +where.value;
      send({ type: 'MacroSet', deck: previewDeck(), slot: m, param: Math.min(i, count), layer: layer < 0 ? undefined : layer, value: +slot.value * 16 + +par.value, f: [a, b] });
      if (!cur) { B.adds[m]++; B.rkey = ''; window.H.invalidate(); }
      close();
    }, 'primary');
    const foot = el('div', 'bdbindfoot'); foot.append(el('div', 'spacer'), btn('cancel', null, close), ok);
    box.append(row, el('div', 'dim', 'min > max turns the knob around. The macro moves the parameter between the two.'), foot);
    md.append(box); md.addEventListener('click', e => { if (e.target === md) close(); });
    document.body.append(md);
  }

  // ================================================================ right: the kit
  function padLabel(p) {
    if (!p) return { n: '', tag: '', cls: 'empty' };
    const kind = KIND_FILE[p.kind] || 'Empty', tgt = TAG_FILE[p.target] || 'Selected';
    if (kind === 'Clip') return { n: (p.name || '?').replace(/\.mov$/i, ''), tag: TAG[tgt], cls: 'clip' };
    if (kind === 'Cue') return { n: `CUE ${(p.slot ?? p.id ?? 0) + 1}${p.name ? ' · ' + p.name.replace(/\.mov$/i, '') : ''}`, tag: TAG[tgt], cls: 'clip' };
    if (kind === 'Effect') return { n: p.name || 'effect', tag: 'FX', cls: 'fx' };
    if (kind === 'Scene') return { n: `SCENE ${(p.id ?? 0) + 1}`, tag: 'SCN', cls: 'act' };
    if (kind === 'Action') return { n: ACTION_WORDS[ACTIONS[p.id ?? 0]] || 'action', tag: 'ACT', cls: 'act' };
    return { n: '', tag: '', cls: 'empty' };
  }
  function renderRight() {
    const R = B.right; R.textContent = '';
    const k = B.sel, sh = S.show;
    const head = el('div', 'bdkhead');
    head.append(el('span', 'lbl', 'KIT · 16 pads (click a pad)'), el('div', 'spacer'));
    if (k < 0 || !sh.pads[k]?.used) { R.append(head, el('div', 'empty', 'Pick a scene.')); return; }
    const sc = docScene(k), ed = editable(k);
    if (ed) head.append(btn('FILL KIT', 'Propose the default layout (perform.md §4.2): top row backgrounds on L1, overlays on L3, effect hits, HIT clips + BLACK + NEXT SCENE', () => fillKit(sc)));
    R.append(head);
    const kit = sc?.kit || [];
    const grid = el('div', 'bdkit');
    for (let row = 3; row >= 0; --row) for (let c = 0; c < 4; ++c) {   // pad 1 = bottom-left
      const i = row * 4 + c, p = kit[i] || null, lab = padLabel(p);
      const b = el('button', 'bdpad ' + lab.cls + (i === B.pad ? ' sel' : ''));
      if (p?.kind === 'clip' || p?.kind === 'cue') {
        const ci = (S.cat?.clips || []).find(x => x.name === p.name);
        if (ci) b.style.background = `linear-gradient(135deg, #10141a, ${window.H.rgb(ci.thumb_rgb)})`;
      }
      b.append(el('span', 'bdpn', String(i + 1)), el('span', 'bdpname', lab.n || '—'));
      if (lab.tag) b.append(el('span', 'bdptag t-' + lab.tag, lab.tag));
      b.addEventListener('click', () => { B.pad = i; B.rkey = ''; window.H.invalidate(); });
      grid.append(b);
    }
    R.append(grid);
    if (!sc) { R.append(el('div', 'dim', 'Reading the show document…')); return; }
    if (!kit.some(Boolean)) R.append(el('div', 'dim', 'No scene kit: the pads play the BANK kit (the bank\'s clips on the selected layer).'));
    R.append(padEditor(kit[B.pad] || null, ed));
  }
  function padEditor(p, ed) {
    const box = el('div', 'bdpaded');
    const lab = padLabel(p);
    box.append(el('div', 'bdpadh', `PAD ${B.pad + 1} · ${lab.n || 'empty'}`));
    const kind = KIND_FILE[p?.kind] || 'Empty', tgt = TAG_FILE[p?.target] || 'Selected';
    if (!ed) { box.append(el('div', 'dim', `${kind.toUpperCase()}${lab.tag ? ' → ' + lab.tag : ''}. REHEARSE the scene to change the pad.`)); return box; }
    const kSel = el('select');
    for (const v of ['Empty', 'Clip', 'Cue', 'Effect', 'Scene', 'Action']) { const o = el('option', null, v.toUpperCase()); o.value = v; kSel.append(o); }
    kSel.value = kind;
    const item = el('select');
    const fill = () => {
      item.textContent = ''; const kv = kSel.value;
      const add = (v, t) => { const o = el('option', null, t); o.value = v; item.append(o); };
      if (kv === 'Clip') for (const c of S.cat?.clips || []) add(c.id, c.name.replace(/\.mov$/i, ''));
      else if (kv === 'Cue') for (let q = 0; q < 16; ++q) add(q, `cue ${q + 1} of the target layer's clip`);
      else if (kv === 'Effect') for (const e of (S.cat?.effects || []).filter(x => x.kind !== 'generator')) add(e.id, e.name);
      else if (kv === 'Scene') S.show.pads.forEach((x, q) => add(q, `${q + 1} ${x.used ? x.name : '(empty)'}`));
      else if (kv === 'Action') ACTIONS.forEach(a => add(a, ACTION_WORDS[a]));
      item.classList.toggle('hidden', kv === 'Empty');
      tg.classList.toggle('hidden', !(kv === 'Clip' || kv === 'Cue'));
    };
    // current item
    const tg = el('div', 'bdtargets');
    let target = tgt;
    const tbs = Object.keys(TAG).map(t => {
      const b = btn(TAG[t], { Selected: 'The selected layer of the focus side', All: 'All 4 layers of the focus deck', Free: 'The lowest free layer', Hit: 'The HIT layer on top of PROGRAM' }[t] || 'Always this layer of the focus deck', () => { target = t; tbs.forEach(x => x.classList.toggle('on', x.dataset.t === t)); }, 'chip2 t-' + TAG[t]);
      b.dataset.t = t; b.classList.toggle('on', t === tgt); return b;
    });
    tg.append(el('span', 'lbl', 'target'), ...tbs);
    kSel.addEventListener('change', fill);
    fill();
    if (kind === 'Clip') { const ci = (S.cat?.clips || []).find(x => x.name === p.name); if (ci) item.value = ci.id; }
    else if (kind === 'Cue') item.value = p.slot ?? 0;
    else if (kind === 'Effect') { const e = (S.cat?.effects || []).find(x => x.name === p.name); if (e) item.value = e.id; }
    else if (kind === 'Scene') item.value = p.id ?? 0;
    else if (kind === 'Action') item.value = ACTIONS[p.id ?? 0];
    const row = el('div', 'bdpadrow'); row.append(el('span', 'lbl', 'holds'), kSel, item);
    const set = btn('SET PAD', 'KitSet on the scene on PREVIEW', () => {
      const kv = kSel.value;
      const c = { type: 'KitSet', deck: previewDeck(), slot: B.pad, param: kv };
      if (kv === 'Clip' || kv === 'Cue') c.layer = target;
      if (kv === 'Action') c.value = item.value; else if (kv !== 'Empty') c.value = +item.value;
      send(c);
    }, 'primary');
    const clr = btn('CLEAR', 'Empty this pad', () => send({ type: 'KitSet', deck: previewDeck(), slot: B.pad, param: 'Empty' }));
    const foot = el('div', 'bdpadfoot'); foot.append(set, clr);
    box.append(row, tg, foot, el('div', 'dim', 'On the MK2: hold the pad + a group button sets the target (A/E = L1 … D/H = L4, ALL, SOLO = HIT, SELECT = SEL).'));
    return box;
  }
  // "Fill kit" (perform.md §4.2, GUESS: the default layout): 13–16 backgrounds → L1, 9–12 overlays
  // (alpha first) → L3, 5–8 effect hits, 1–2 HIT clips, 3 BLACK, 4 NEXT SCENE.
  function fillKit(sc) {
    if ((sc?.kit || []).some(Boolean) && !confirm('Replace the kit of this scene with the default layout?')) return;
    const d = previewDeck(), clips = S.cat?.clips || [], fx = (S.cat?.effects || []).filter(e => e.kind !== 'generator');
    if (!clips.length) return toast('The engine has no clips yet', 'bad');
    const alpha = clips.filter(c => c.codec === 1), plain = clips.filter(c => c.codec !== 1);
    const pick = (list, n, from) => Array.from({ length: n }, (_, k) => list.length ? list[(from + k) % list.length] : clips[(from + k) % clips.length]);
    const bg = pick(plain.length ? plain : clips, 4, 0);
    const ov = [...alpha.slice(0, 4), ...pick(plain.length ? plain : clips, 4 - Math.min(4, alpha.length), 4)];
    const hits = pick(plain.length ? plain : clips, 2, 8);
    const want = [/strobe/i, /rgb|split|chroma/i, /zoom|pulse|bulge/i, /freeze|trail|echo/i];
    const used = new Set();
    const fxp = want.map(re => { const e = fx.find(x => re.test(x.name) && !used.has(x.id)) || fx.find(x => !used.has(x.id)); if (e) used.add(e.id); return e; });
    bg.forEach((c, k) => send({ type: 'KitSet', deck: d, slot: 12 + k, param: 'Clip', layer: 'Layer1', value: c.id }));
    ov.forEach((c, k) => send({ type: 'KitSet', deck: d, slot: 8 + k, param: 'Clip', layer: 'Layer3', value: c.id }));
    fxp.forEach((e, k) => send(e ? { type: 'KitSet', deck: d, slot: 4 + k, param: 'Effect', value: e.id } : { type: 'KitSet', deck: d, slot: 4 + k, param: 'Empty' }));
    hits.forEach((c, k) => send({ type: 'KitSet', deck: d, slot: k, param: 'Clip', layer: 'Hit', value: c.id }));
    send({ type: 'KitSet', deck: d, slot: 2, param: 'Action', value: 'Black' });
    send({ type: 'KitSet', deck: d, slot: 3, param: 'Action', value: 'NextScene' });
    toast('Kit filled: change any pad, then save the project');
  }

  // ================================================================ render
  function render() {
    const sh = S.show, ok = !!(sh && sh.present);
    $('viewTabs').querySelector('[data-view="build"]')?.classList.toggle('hidden', !ok);
    if (S.view !== 'build') return;
    if (!B.built) build();
    if (!ok) { B.left.textContent = ''; B.mid.textContent = ''; B.right.textContent = ''; B.mid.append(el('div', 'empty', sh?.state === 'version' ? 'The renderer is another version than this GUI (show block).' : 'No show block: the renderer is not running.')); B.rkey = 'none'; return; }
    const sk = showKey();
    if (sk !== B.key) { B.key = sk; fetchDoc(); setTimeout(fetchDoc, 700); }
    if (B.sel < 0 || !sh.pads[B.sel]?.used) {
      const pd = previewDeck(), dp = sh.decks?.[pd]?.pad;
      B.sel = dp >= 0 && sh.pads[dp]?.used ? dp : sh.pads.findIndex(p => p.used);
    }
    B.status.textContent = B.doc?.ok ? `${sh.project}${sh.dirty ? ' ●' : ''} · document ${Math.round((performance.now() - B.at) / 1000)} s old` : (B.doc?.error || 'reading…');
    // Rebuild the columns only when what they show changed (so selects / clicks are not eaten).
    const rk = JSON.stringify([B.at, B.sel, B.pad, sh.use_setlist, sh.bank, sh.song, sh.song_count, sh.pads.map(p => [p.used, p.name, p.on, p.variations]), sh.decks.map(d => [d.pad, d.scene, d.variation]), S.live?.program_deck, S.cat?.clips.length, S.cat?.effects.length]);
    if (rk === B.rkey || document.querySelector('.modal:not(.hidden) .bdbindbox') || B.right.contains(document.activeElement) && document.activeElement.tagName === 'SELECT') return;
    B.rkey = rk;
    renderLeft(); renderMid(); renderRight();
  }

  window.Build = { render, shown(v) { if (v === 'build') { B.key = ''; B.rkey = ''; } } };
})();
