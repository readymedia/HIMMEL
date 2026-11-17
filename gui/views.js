// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The views around Perform (M11): the start wizard (gui.md §3, steps 1-4 against what 0.1 has),
// the MK2 picture with live feedback (gui.md §4.5) and the Settings view (gui.md §19).
// M11 part 2: the wizard uses the OUTPUTS / AUDIO / LIBRARY / weapon blocks through window.Panels.
// Uses the state app.js keeps in window.S and its helpers in window.H.
'use strict';

(() => {
  const $ = id => document.getElementById(id);
  const { el } = window.H;
  const S = window.S;

  // ================================================================ start wizard
  const W = { step: 1, auto: true };
  // GUI w7 (D170): steps 5–7 walk a first-time user through the show itself: clips onto layers,
  // TAKE + an effect, a scene + the project saved. Each has a live checklist from the engine's state.
  const NEXT = { 0: 'Next: clips', 1: 'Next: weapon', 2: 'Next: outputs', 3: 'Next: sound', 4: 'Next: layers', 5: 'Next: TAKE + effects', 6: 'Next: save', 7: 'Open Perform' };
  const LAST = 7;
  const TITLES = ['Start: your project', 'Add clips', 'Choose your weapon', 'Set outputs', 'Sound and health', 'Put clips on layers', 'TAKE and add an effect', 'Save a scene and the project'];

  function card(title, body, cls) {
    const c = el('div', 'wcard' + (cls ? ' ' + cls : ''));
    c.append(el('div', 'wct', title));
    if (typeof body === 'string') c.append(el('div', 'dim', body)); else if (body) c.append(body);
    return c;
  }
  function wizNav() {
    const nav = el('div', 'wiznav');
    if (W.step > (S.show?.present ? 0 : 1)) { const b = el('button', null, '◀ Back'); b.title = 'The step before'; b.addEventListener('click', () => go(W.step - 1)); nav.append(b); }
    nav.append(el('div', 'spacer'));
    const n = el('button', 'primary', NEXT[W.step] + ' ▶');
    n.title = W.step < LAST ? 'The next step (you can always come back: click a step in the bar above)' : 'Play the show in PERFORM';
    n.addEventListener('click', () => (W.step < LAST ? go(W.step + 1) : window.setView('perform')));
    nav.append(n);
    return nav;
  }
  function go(step) { W.step = step; W.auto = false; W.key = ''; renderWizard(); }

  // Each step is rebuilt only when what it shows changed (cheap key), so clicks are not eaten.
  function renderWizard() {
    // The start screen (step 0) needs the show block; without it the wizard starts at step 1.
    const hasShow = !!S.show?.present;
    $('wizBar').querySelector('[data-step="0"]')?.classList.toggle('hidden', !hasShow);
    if (W.auto && hasShow && W.step === 1) W.step = 0;
    if (!hasShow && W.step === 0) W.step = 1;
    for (const b of document.querySelectorAll('#wizBar button')) {
      b.classList.toggle('on', +b.dataset.step === W.step);
    }
    const key = W.step + '|' + stepKey(W.step);
    if (W.key === key) { if (W.step === 4 && W.u) window.Panels.W.updatePicker(W.u); if (W.step === 1) stepProgress(); return; }
    W.key = key;
    const body = $('wizBody'); body.textContent = '';
    body.append(el('h2', null, TITLES[W.step]));
    W.u = null;
    ({ 0: step0, 1: step1, 2: step2, 3: step3, 4: step4, 5: step5, 6: step6, 7: step7 })[W.step](body);
    body.append(wizNav());
    if (W.u) window.Panels.W.updatePicker(W.u);
  }
  function stepKey(step) {
    const b = S.bridge, m = S.mk2, L = S.live, lib = S.library, o = S.outputs, a = S.audio;
    if (step === 0) { const sh = S.show, f = sh?.files; return JSON.stringify([sh?.present, sh?.project, sh?.dirty, sh?.busy, f?.path, f?.recent, f?.restored, f?.saved_unix, f?.autosave_unix, f?.missing_count, b?.engine, S.cat?.clips.length, m?.connected, o?.count]); }
    if (step === 1) return JSON.stringify([S.cat?.clips.length ?? -1, S.cat?.effects.length ?? -1, lib?.present, lib?.alive, lib?.ready_total, lib?.failed_total, lib?.catalog_full, !!(lib?.queued || lib?.working)]);
    if (step >= 5) {
      const lay = L ? L.deck.map(d => d.layer.map(x => [x.clip, x.fx.filter(f => f.effect >= 0).length])) : null;
      const sh = S.show;
      return JSON.stringify([step, b?.engine, S.cat?.clips.length, !!window.Flows?.rows(), lay, L?.program_deck, L?.focus_deck, L?.focus_layer, sh?.scene_count, sh?.dirty, sh?.files?.path, sh?.project, sh?.pads?.map(p => p.used)]);
    }
    if (step === 2) return JSON.stringify([m?.present, m?.connected, m?.alive, (m?.pressed || []).join('+'), L?.producer?.[0]?.alive, L?.producer?.[1]?.alive, S.weapons, a?.present]);
    if (step === 3) return JSON.stringify([b?.preview, b?.engine, o]);
    // Step 4 holds a live meter: rebuilt only when the input changes; the meter updates in place.
    return JSON.stringify([b?.engine, S.cat?.clips.length, m?.connected, a?.present, a?.input?.device, a?.input?.state, o?.count, (o?.outputs || []).map(x => x.state)]);
  }

  // Step 0, the start screen (gui.md §3 "Before the wizard", projects.md): new / open / recent, the
  // autosave offer, and the one-line health check. Opening a project during a show is a SHOW command
  // (Project Open / New); the renderer keeps the outputs alive.
  function step0(body) {
    const sh = S.show, f = sh.files, send = window.H.send, ask = window.Panels.W.ask;
    const b = S.bridge, m = S.mk2, o = S.outputs, n = S.cat?.clips.length ?? 0;
    const lost = (o?.outputs || []).filter(x => x.state === 'Lost' || x.state === 'Other').length;
    body.append(el('div', 'health ' + (b?.engine === 'ok' && !lost && !f.missing_count ? 'ok' : 'bad'),
      b?.engine !== 'ok' ? 'The engine is not running yet: start himmel.exe.' :
        `${lost || f.missing_count ? '⚠ ' : 'All good: '}${sh.project}${sh.dirty ? ' ●' : ''} · ${n} clips · ${m?.connected ? 'MK2' : 'no MK2'} · ${o?.count ?? '?'} output${o?.count === 1 ? '' : 's'}` +
        (lost ? ` · ${lost} output${lost > 1 ? 's' : ''} lost` : '') + (f.missing_count ? ` · ${f.missing_count} clips missing (project ⋯ → Relink)` : '')));
    if (f.restored && f.restored !== 'None')
      body.append(card('CAME BACK FROM ' + (f.restored === 'Autosave' ? 'THE AUTOSAVE' : 'AN INTERRUPTED SAVE'), 'Himmel restored your work after a crash. Keep it (save), or go back to the last save with Revert (project ⋯).', 'sel'));
    else if (f.path && f.autosave_unix > f.saved_unix) {
      const c = el('div');
      const r = el('button', 'primary', 'Restore the autosave');
      r.addEventListener('click', () => send({ type: 'Project', param: 'RestoreAutosave' }));
      c.append(el('div', 'dim', `A newer autosave (${new Date(f.autosave_unix * 1000).toLocaleTimeString()}) than the last save is there.`), r);
      body.append(card('AUTOSAVE', c));
    }
    const row = el('div', 'wstart');
    const sure = cmd => () => { if (!sh.dirty || confirm('Unsaved changes are lost (the autosave keeps them). Go on?')) send(cmd); };
    const big = (title, sub, fn, cls) => { const x = el('button', 'wproj' + (cls ? ' ' + cls : '')); x.title = title + ': ' + sub; x.append(el('b', null, title), el('span', 'dim', sub)); x.addEventListener('click', fn); return x; };
    row.append(
      big('Keep playing', `${sh.project}${sh.dirty ? ' (unsaved changes)' : ''}`, () => go(1), 'sel'),
      big('New project · 3 screens', 'Empty, the 3-screen template', sure({ type: 'Project', param: 'New', value: 3 })),
      big('New project · 1 screen', 'Empty, one output', sure({ type: 'Project', param: 'New', value: 1 })),
      big('Open…', 'A project folder or a .himmelshow file', async () => {
        const p = await ask('OPEN · project folder or .himmelshow file', '', 'e.g. C:\\Users\\me\\Documents\\Himmel\\Projects\\Club.himmel');
        if (p && (!sh.dirty || confirm('Unsaved changes are lost (the autosave keeps them). Open?'))) send({ type: 'Project', param: 'Open', text: p });
      }));
    body.append(row, el('div', 'lbl', 'RECENT PROJECTS'));
    const rec = el('div', 'wstart');
    f.recent.forEach((r, i) => rec.append(big(r.name, `${r.outputs} output${r.outputs === 1 ? '' : 's'} · ${r.saved_unix ? new Date(r.saved_unix * 1000).toLocaleString() : ''}`,
      sure({ type: 'Project', param: 'Open', slot: i }), r.path === f.path ? 'sel' : '')));
    if (!f.recent.length) rec.append(el('div', 'dim', 'No recent projects yet: SAVE (Ctrl+S) puts this one here.'));
    body.append(rec);
  }

  function step1(body) {
    const n = S.cat?.clips.length ?? 0, g = (S.cat?.effects || []).filter(e => e.kind === 'generator').length;
    const lib = S.library, PW = window.Panels.W;
    const drop = el('div', 'drop');
    const root = lib?.root || '';
    const imp = el('button', 'primary', '+ IMPORT A FOLDER…'); imp.title = 'Choose a folder of videos, pictures or GIFs (subfolders too). The originals stay where they are.';
    imp.addEventListener('click', () => window.Lib.importDialog('folder'));
    const impF = el('button', null, '+ IMPORT FILES…'); impF.title = 'Tick single files in a folder';
    impF.addEventListener('click', () => window.Lib.importDialog('files'));
    const btns = el('div', 'askrow'); btns.append(imp, impF);
    drop.append(el('div', 'big', 'Import a folder of clips'), btns,
      el('div', 'dim', 'Video, stills, GIFs, SVG and image sequences become fast HAP copies and are analysed (colours, motion, mood, loops) in the background. The first clips are playable while the rest still import: you do not have to wait.'),
      el('div', 'dim mono', root ? 'or copy files into ' + root + '\\originals (watched)' : ''));
    drop.addEventListener('dragover', e => e.preventDefault());
    drop.addEventListener('drop', e => { e.preventDefault(); window.H.toast('A web page is not told where a dropped file lives: use IMPORT A FOLDER… and choose the folder the files are in.'); });
    body.append(drop);
    W.prog = el('div', 'wprog'); body.append(W.prog); stepProgress();
    const st = el('div');
    if (PW.has(lib)) {
      st.append(el('div', 'wstat ' + (lib.alive ? 'ok' : 'warn'), lib.alive ? (lib.working || lib.queued ? 'IMPORTING' : 'READY FOR NEW FILES') : 'IMPORT WORKER NOT RUNNING'),
        el('div', 'mono dim', `${lib.queued} queued · ${lib.working} working · ${lib.ready_total} ready · ${lib.failed_total} failed`));
      if (lib.job.name) st.append(el('div', null, `${lib.job.name} · ${lib.job.stage} · ${Math.round(lib.job.progress * 100)}%`));
      if (lib.catalog_full) st.append(el('div', 'bad', `Catalog full: ${lib.catalog_full} ready clips are not playable (256 max in 0.1).`));
    } else {
      st.append(el('div', 'wstat warn', 'IMPORT WORKER NOT RUNNING'), el('div', 'dim', 'himmel.exe starts himmel_ingest (not with --no-show-files unless --library is given). The engine still plays every HAP .mov in media/.'));
    }
    const open = el('button', null, 'Open the LIBRARY view'); open.title = 'Search, filter, select and load clips'; open.addEventListener('click', () => window.setView('library')); st.append(open);
    const row = el('div', 'wrow');
    const list = el('div', 'wlist');
    (S.cat?.clips || []).slice(0, 40).forEach(c => {
      const it = el('div', 'wclip');
      const sw = el('i'); sw.style.background = window.H.rgb(c.thumb_rgb);
      it.append(sw, el('span', null, c.name.replace(/\.mov$/i, '')), el('span', 'dim mono', `${c.width}×${c.height} · ${c.seconds.toFixed(0)} s · ready`));
      list.append(it);
    });
    if (!n) list.append(el('div', 'dim', S.cat ? 'The engine has no clips yet. Generators still play (step 4 → GENERATORS).' : 'Waiting for the engine catalog…'));
    row.append(card('Import', st), card(`${n} clips ready to play`, list), card('Starter looks', `${g} generators are built in (Lines, Rings, Noise, Shapes, Solid, Ticker): they play on an empty layer, no clips needed.`));
    body.append(row);
  }

  // The import progress in step 1 (updated in place, ~every frame while the step shows).
  function stepProgress() {
    if (!W.prog) return;
    const l = S.library, p = window.Lib?.progress?.();
    const d = window.Lib?.disk?.() || {};
    window.Lib?.loadDisk?.();
    W.prog.textContent = '';
    if (p) {
      const bar = el('div', 'lbpbar'); const f = el('i'); f.style.width = (p.total ? p.done / p.total * 100 : 0) + '%'; bar.append(f);
      W.prog.append(el('div', null, p.busy ? `Importing ${Math.min(p.total, p.done + 1)} of ${p.total}${l?.job?.name ? ' · ' + l.job.name : ''}${p.eta >= 0 ? ' · about ' + (p.eta < 60 ? Math.round(p.eta) + ' s' : Math.round(p.eta / 60) + ' min') + ' left' : ''}` : `Imported ${p.done} of ${p.total}`), bar);
    }
    if (d.free != null) W.prog.append(el('div', d.free < 10e9 ? 'bad' : 'dim', `Library drive: ${(d.free / 1e9).toFixed(1)} GB free · the library uses ${((d.lib || 0) / 1e9).toFixed(1)} GB`));
  }

  // Step 5: clips onto layers. Drag a clip onto a PREVIEW layer, double-click = the next free layer.
  function step5(body) {
    const L = S.live, F = window.Flows, n = S.cat?.clips.length ?? 0;
    if (!L) { body.append(card('Waiting for the engine', 'Start Himmel (himmel.exe); this page waits for it.', 'off')); return; }
    const pd = 1 - L.program_deck;
    const onPrev = L.deck[pd].layer.filter(x => x.clip >= 0).length;
    body.append(checklist([[onPrev > 0, 'A clip is on a PREVIEW layer'], [onPrev > 1, 'Two layers (optional): they mix on top of each other']]));
    body.append(el('div', 'dim', 'PREVIEW (green) is off air: what you build here is not on the screens until you TAKE it. Layer A is at the bottom, D on top.'));
    const row = el('div', 'wlayers');
    for (let k = 0; k < 4; k++) {
      const ls = L.deck[pd].layer[k], ci = S.cat?.clips?.[ls.clip];
      const c = el('div', 'wlayer' + (ci ? ' full' : ''));
      c.append(el('b', null, 'PREVIEW ' + 'ABCD'[k]), el('span', null, ci ? ci.name.replace(/\.mov$/i, '') : 'empty: drop a clip here'));
      const pick = el('button', null, ci ? 'change…' : 'choose a clip…'); pick.title = 'Open the clip picker for this layer';
      pick.addEventListener('click', () => F.picker({ deck: pd, layer: k }));
      c.append(pick);
      F.dropTarget(c, () => ({ deck: pd, layer: k }));
      row.append(c);
    }
    body.append(row);
    const list = el('div', 'wpick');
    (S.cat?.clips || []).slice(0, 48).forEach(c => {
      const it = el('div', 'wpc'); it.draggable = true;
      const th = el('div', 'wpth'); th.style.background = window.H.rgb(c.thumb_rgb);
      const r = F.rowOf(c.id), u = F.thumbUrl(r);
      if (u) { const img = el('img'); img.src = u; img.alt = ''; img.draggable = false; img.onerror = () => img.remove(); th.append(img); }
      th.append(F.whereTags(c.id));
      it.append(th, el('span', null, c.name.replace(/\.mov$/i, '')));
      it.title = 'Drag onto a PREVIEW layer above · double-click = the next free PREVIEW layer';
      it.addEventListener('dragstart', e => F.setDrag(e, c.id));
      it.addEventListener('dblclick', () => F.load(c.id, 'free-prev'));
      list.append(it);
    });
    if (!n) list.append(el('div', 'empty', 'No clips are ready yet. Go back to step 1 and import a folder (or wait: they appear here as soon as they are ready).'));
    body.append(el('div', 'lbl', `CLIPS (${n})`), list);
  }

  // Step 6: TAKE (PREVIEW goes on air) and one effect on a layer.
  function step6(body) {
    const L = S.live, send = window.H.send;
    if (!L) { body.append(card('Waiting for the engine', 'Start Himmel; this page waits for it.', 'off')); return; }
    const progHas = L.deck[L.program_deck].layer.some(x => x.clip >= 0);
    const anyFx = L.deck.some(d => d.layer.some(x => x.fx.some(f => f.effect >= 0)));
    body.append(checklist([[progHas, 'PROGRAM (on air) shows a clip: press TAKE'], [anyFx, 'An effect is on a layer']]));
    const take = el('button', 'primary bigbtn', 'TAKE ▶'); take.title = 'PREVIEW travels to PROGRAM over the TAKE time (space in PERFORM; the wheel on the MK2)';
    take.addEventListener('click', () => send({ type: 'Take', value: 0 }));
    const cut = el('button', 'bigbtn', 'CUT'); cut.title = 'Swap PREVIEW and PROGRAM at once (enter in PERFORM)';
    cut.addEventListener('click', () => send({ type: 'Cut' }));
    const r1 = el('div', 'askrow'); r1.append(take, cut);
    body.append(card('1 · TAKE', r1));
    // Effects on the focused layer (PREVIEW by default): the first free slot.
    // The layer the effect goes on: the focused one when it has a clip, else the first layer with a clip
    // on PROGRAM (after a TAKE that is what you see), else on PREVIEW.
    const F = window.Flows, f0 = F.focus();
    const firstWith = d => { const k = L.deck[d].layer.findIndex(x => x.clip >= 0); return k >= 0 ? { deck: d, layer: k } : null; };
    const t = (f0 && L.deck[f0.deck].layer[f0.layer].clip >= 0 ? f0 : null) || firstWith(L.program_deck) || firstWith(1 - L.program_deck) || f0;
    const ls = t ? L.deck[t.deck].layer[t.layer] : null;
    const fx = (S.cat?.effects || []).filter(e => e.kind !== 'generator').slice(0, 12);
    const chips = el('div', 'wfx');
    for (const e of fx) {
      const b = el('button', 'chip2', e.name); b.title = `Put ${e.name} on ${F.groupOf(t.deck, t.layer)} (its first free effect slot)`;
      b.addEventListener('click', () => {
        const slot = ls.fx.findIndex(f => f.effect < 0);
        if (slot < 0) { window.H.toast('All 4 effect slots of this layer are in use'); return; }
        send({ type: 'FxSet', deck: t.deck, layer: t.layer, slot, value: e.id });
      });
      chips.append(b);
    }
    const where = t ? `${t.deck === L.program_deck ? 'PROGRAM (on air)' : 'PREVIEW'} ${F.groupOf(t.deck, t.layer)}: ${S.cat?.clips?.[ls.clip]?.name.replace(/\.mov$/i, '') || 'empty'}` : '–';
    const c2 = el('div'); c2.append(el('div', 'dim', 'Adds to ' + where + '. Turn its knobs in PERFORM → LAYER FX (or knobs 1–4 on the MK2).'), chips);
    body.append(card('2 · AN EFFECT', c2));
    body.append(el('div', 'dim', 'Every effect reacts to the sound by default (the ~ ear next to a knob in LAYER FX). In PERFORM, double-click a layer to open its clip editor: in / out, loops, cue points, speed.'));
  }

  // Step 7: a scene (the look on a deck, saved on a pad) and the project (a folder on disk).
  function step7(body) {
    const sh = S.show, send = window.H.send, PW = window.Panels.W;
    if (!PW.has(sh)) { body.append(card('Projects are not available', 'The show block is missing (the renderer runs without project files, e.g. --no-show-files).', 'off')); return; }
    const L = S.live;
    body.append(checklist([[sh.scene_count > 0, `A scene is saved (${sh.scene_count} so far)`], [!!sh.files?.path && !sh.dirty, sh.files?.path ? `The project is saved${sh.dirty ? ' (changes since: SAVE again)' : ''}` : 'The project is saved (it has no name yet)']]));
    const rec = el('button', 'primary bigbtn', 'SAVE SCENE (REC)'); rec.title = 'Save the look of the PROGRAM deck on the next free scene pad (REC on the MK2)';
    rec.addEventListener('click', () => send({ type: 'SceneSave', deck: L ? L.program_deck : 0, value: -1 }));
    const recP = el('button', null, 'save the PREVIEW look'); recP.title = 'Save the PREVIEW deck instead';
    recP.addEventListener('click', () => send({ type: 'SceneSave', deck: L ? 1 - L.program_deck : 1, value: -1 }));
    const r1 = el('div', 'askrow'); r1.append(rec, recP);
    const c1 = el('div'); c1.append(el('div', 'dim', 'A scene keeps the clips, effects, knob reactions and macros of a deck. Recall it later from PERFORM → SCENES or the MK2 pads (SCENE).'), r1);
    body.append(card('1 · A SCENE', c1));
    const save = el('button', 'primary bigbtn', sh.files?.path ? 'SAVE PROJECT' : 'SAVE PROJECT AS…');
    save.title = 'A project is a folder on disk with every scene, song and setting. Autosave runs anyway; SAVE makes the version you open next time.';
    save.addEventListener('click', async () => {
      if (sh.files?.path) { send({ type: 'Project', param: 'Save' }); return; }
      const n = await PW.ask('SAVE AS · project name', sh.project === 'Untitled' ? '' : sh.project, 'A name goes into the projects folder (<name>.himmel); a full path makes that folder.');
      if (n) send({ type: 'Project', param: 'SaveAs', text: n });
    });
    const c2 = el('div'); c2.append(el('div', 'dim', sh.files?.path ? 'Saved in ' + sh.files.path : 'The project is "' + sh.project + '" and has no folder yet.'), save);
    body.append(card('2 · THE PROJECT', c2));
    body.append(el('div', 'dim', 'Done! Open PERFORM to play. Ctrl+S saves any time; the ⋯ next to the project name has open / save as / export.'));
  }

  function checklist(items) {
    const c = el('div', 'wcheck');
    for (const [ok, t] of items) c.append(el('div', ok ? 'ok' : 'todo', (ok ? '✓ ' : '○ ') + t));
    return c;
  }

  function step2(body) {
    const m = S.mk2 || {}, L = S.live;
    const row = el('div', 'wrow');
    const mk2 = el('div');
    const st = !m.present ? 'NOT FOUND' : m.connected ? 'CONNECTED' : m.alive ? 'CONTROLLER RUNS · NO USB' : 'CONTROLLER NOT RUNNING';
    mk2.append(el('div', 'wstat ' + (m.connected ? 'ok' : m.present ? 'warn' : 'bad'), st),
      el('div', 'dim', '2 screens · 8 knobs · wheel · 16 pressure pads'),
      el('div', null, 'Layout: PREVIEW left (knobs 1–4, groups A–D), PROGRAM right (knobs 5–8, groups E–H), wheel = TAKE.'),
      el('div', 'dim', (m.pressed && m.pressed.length) ? 'Touched: ' + m.pressed.join(' + ') : 'Touch any control on the MK2: it shows here (a hardware test).'));
    mk2.append(el('div', 'dim', 'No MK2? Start Himmel with --sim and use the simulator: python tools/mk2_live.py --sim'));
    const ws = Array.isArray(S.weapons) ? S.weapons.filter(w => w.state === 'ok') : [];
    const nuvj = ws.find(w => w.weapon === 'nuvj');
    row.append(card('MAIN WEAPON · MK2', mk2, m.connected ? 'sel' : ''),
      card('MAIN WEAPON · NuVJ', nuvj ? `${nuvj.connected ? 'CONNECTED' : 'driver runs, not connected'}${nuvj.simulated ? ' (simulated)' : ''}. The crossfader decides PROGRAM (D73).` : 'Driver not running.', nuvj?.connected ? 'sel' : 'off'),
      card('KEYBOARD + MOUSE', 'Always there: this page plays the MK2 layout. Space = TAKE, Enter = CUT, T = tap.', L?.producer?.[1]?.alive ? 'sel' : ''));
    body.append(el('div', 'lbl', 'MAIN WEAPON'), row);
    const arms = el('div', 'wrow');
    for (const w of ws.filter(x => x.weapon !== 'nuvj')) {
      const t = el('div');
      t.append(el('div', 'wstat ' + (w.connected ? 'ok' : w.alive ? 'warn' : 'bad'), w.connected ? 'CONNECTED' : w.alive ? 'DRIVER RUNS · NOT CONNECTED' : 'DRIVER STOPPED'),
        el('div', 'dim mono', `${w.driver} · slot ${w.slot}${w.simulated ? ' · simulated' : ''}`));
      arms.append(card(w.name || w.weapon, t, w.connected ? 'sel' : ''));
    }
    if (!ws.length) arms.append(card('No side arms found', 'Lightpad, LUMI, NuVJ and APC20 drivers run under himmel.exe (--weapons LIST); none is running now.', 'off'));
    body.append(el('div', 'lbl', `WEAPONS FOUND (${ws.length} driver${ws.length === 1 ? '' : 's'})`), arms);
    const p0 = L?.producer?.[0]?.alive;
    body.append(el('div', 'loadout', 'Loadout: MK2' + (m.connected ? '' : ' (not connected)') + ws.filter(w => w.connected).map(w => ' + ' + (w.name || w.weapon)).join('') + ' + keyboard / mouse' + (p0 ? ' · engine mapper alive' : '')));
  }

  function step3(body) {
    const o = S.outputs, PW = window.Panels.W;
    const row = el('div', 'wrow');
    row.append(card('SCREENS', 'TVs, monitors, LED walls: the edges just meet, no mapping.', 'sel'), card('PROJECTORS', 'Keystone, warp, edge blend (later, §13). The blend width is on the OUTPUTS view.', 'off'));
    body.append(el('div', 'lbl', 'WHERE DOES THE PICTURE GO?'), row);
    if (!PW.has(o)) {
      body.append(card('Outputs', o?.state === 'version' ? 'The renderer is another version than this GUI (outputs block).' : 'The renderer is not running (no outputs block yet). himmel.exe starts it.', 'off'));
      return;
    }
    body.append(el('div', 'lbl', 'THE CANVAS YOU PLAY ON (to scale)'), canvasDrawing(o));
    const cards = el('div', 'wrow');
    o.outputs.forEach((x, k) => cards.append(PW.outputCard(x, k, true)));
    body.append(el('div', 'lbl', `${o.count} OUTPUT${o.count === 1 ? '' : 'S'} · ${o.mode.toUpperCase()} · canvas ${o.canvas[0]}×${o.canvas[1]} · ${o.displays} displays found`), cards);
    const ctl = el('div', 'wctl');
    const res = (o.outputs[0] || {}).preset || 'P1080';
    ctl.append(el('span', 'lbl', 'RESOLUTION PER OUTPUT'), PW.presetSel(res, v => PW.setResolution(v)),
      el('span', 'lbl', 'REFRESH'), PW.refreshSel(o.refresh, v => window.H.send({ type: 'OutputConfig', param: 'Refresh', value: v })),
      PW.identifyBtn(false));
    if (o.count > 1) {
      const bw = el('input'); bw.type = 'range'; bw.min = 0; bw.max = 1024; bw.step = 8; bw.value = o.blend_px; bw.title = 'Edge blend overlap (projectors): 0 = screens, the edges just meet';
      bw.addEventListener('change', () => window.H.send({ type: 'OutputConfig', param: 'BlendWidth', value: Math.round(+bw.value) }));
      ctl.append(el('span', 'lbl', `BLEND ${o.blend_px} px`), bw);
    }
    body.append(ctl,
      el('div', 'dim', 'IDENTIFY shows a big number on every output for 5 s: check that OUT 1 is on the left. 1080p is the safe choice; 1440p / 4K for big rigs. 50 Hz in countries with 50 Hz light and cameras. The number of outputs is set when Himmel starts (himmel.exe -- --outputs N) in 0.1.'));
    const more = el('button', null, 'More on the OUTPUTS view'); more.title = 'Black per output, layer places, blend'; more.addEventListener('click', () => window.setView('outputs')); body.append(more);
  }

  // The canvas to scale: each output where it sits, blend zones hatched, lost outputs hatched red.
  function canvasDrawing(o) {
    const box = el('div', 'wcanvas');
    const W0 = Math.max(1, o.canvas[0]);
    o.outputs.forEach((x, k) => {
      const d = el('div', 'wco' + (x.state === 'Lost' ? ' lost' : ''), String(k + 1));
      d.style.left = (x.canvas_x / W0 * 100) + '%'; d.style.width = (x.size[0] / W0 * 100) + '%';
      d.title = `OUT ${k + 1} · ${x.size[0]}×${x.size[1]} · ${x.state}`;
      box.append(d);
      if (x.blend[1] > 0) { const z = el('div', 'wbl'); z.style.left = ((x.canvas_x + x.size[0] - x.blend[1]) / W0 * 100) + '%'; z.style.width = (x.blend[1] / W0 * 100) + '%'; box.append(z); }
    });
    return box;
  }

  function step4(body) {
    const b = S.bridge, m = S.mk2, o = S.outputs, PW = window.Panels.W;
    const ok = b?.engine === 'ok';
    const n = S.cat?.clips.length ?? 0;
    const lost = (o?.outputs || []).filter(x => x.state === 'Lost' || x.state === 'Other').length;
    body.append(el('div', 'health ' + (ok && !lost ? 'ok' : 'bad'),
      !ok ? 'The engine is not running: start himmel.exe (or himmel_render.exe). This page waits for it.'
        : `${lost ? `⚠ ${lost} output${lost > 1 ? 's' : ''} lost · ` : 'All good: '}${n} clips, ${m?.connected ? 'MK2 connected' : 'no MK2 (keyboard + mouse)'}, ${o?.count ?? '?'} output${o?.count === 1 ? '' : 's'}, engine ${Math.round(b.fps)} fps.`));
    const snd = el('div');
    W.u = PW.audioPicker(snd, true);
    body.append(card('SOUND IN · every effect reacts to it', snd));
    body.append(el('div', 'dim', 'Perform: PREVIEW (green) left, PROGRAM (red) right, TAKE in the middle. Click a layer = focus; the CLIP tab edits its transport, LAYER FX its effects, SCENES saves and recalls looks. Next: put clips on layers.'));
  }

  // ================================================================ MK2 picture
  const M = { prof: null, nodes: null, scr: null };
  async function loadProfile() {
    for (;;) {
      try { const r = await fetch('/weapons/mk2.weapon.json'); if (r.ok) { M.prof = await r.json(); buildMk2(); return; } } catch (e) { /* not yet */ }
      await new Promise(r => setTimeout(r, 2000));
    }
  }
  function buildMk2() {
    const p = M.prof, box = $('mk2Panel'); box.textContent = '';
    const sc = Math.min(1.35, (window.innerWidth - 80) / p.panel.w);
    box.style.width = p.panel.w * sc + 'px'; box.style.height = p.panel.h * sc + 'px';
    const pos = (n, x, y, w, h) => { n.style.left = x * sc + 'px'; n.style.top = y * sc + 'px'; if (w != null) n.style.width = w * sc + 'px'; if (h != null) n.style.height = h * sc + 'px'; };
    const badge = el('div', 'mbadge', p.badge || 'HIMMEL · MK2'); pos(badge, p.panel.w - 200, p.panel.h - 24); box.append(badge);
    for (const s of p.sections || []) { const t = el('div', 'msec', s.label); pos(t, s.x, s.y - 14, s.w); box.append(t); }
    M.scr = [];
    for (const s of p.screens || []) {
      const c = el('canvas', 'mscr'); c.width = s.px_w; c.height = s.px_h; pos(c, s.x, s.y, s.w, s.h); box.append(c); M.scr.push(c);
    }
    M.nodes = [];
    for (const c of p.controls) {
      let n;
      if (c.kind === 'knob' || c.kind === 'encoder') {
        n = el('div', 'mknob' + (c.kind === 'encoder' ? ' enc' : ''));
        n.append(el('i'));
        pos(n, c.x - c.r, c.y - c.r, c.r * 2, c.r * 2);
      } else if (c.kind === 'pad') {
        n = el('div', 'mpad'); n.append(el('i'), el('span', null, String(c.n)));
        pos(n, c.x, c.y, c.w, c.h);
      } else {
        n = el('div', 'mbtn', c.label ?? ''); pos(n, c.x, c.y, c.w, c.h);
        n.style.fontSize = Math.max(7, 8 * sc) + 'px';
      }
      n.title = c.name;
      box.append(n);
      M.nodes.push({ c, n });
    }
    M.sig = '';
  }
  function lvl(v) { return Math.min(1, (v || 0) / 127); }
  const css = a => `rgb(${Math.round(lvl(a[0]) * 255)},${Math.round(lvl(a[1]) * 255)},${Math.round(lvl(a[2]) * 255)})`;
  function renderMk2() {
    const m = S.mk2 || {};
    const st = $('mk2State');
    st.textContent = !m.present ? 'NO CONTROLLER BLOCK' : m.connected ? 'CONNECTED' : m.alive ? 'CONTROLLER RUNS · MK2 NOT CONNECTED' : 'CONTROLLER NOT RUNNING';
    st.className = 'chip ' + (m.connected ? 'ok' : m.present ? 'warn' : 'bad');
    if (!M.nodes || !m.present) return;
    const sig = JSON.stringify([m.leds, m.pressed, m.pads, m.knobs, m.encoder, m.last_knob]) + (m.screen || '').length;
    // screens
    if (m.screen && m.screen !== M.lastScreen) {
      M.lastScreen = m.screen;
      const bin = atob(m.screen);
      for (let k = 0; k < 2 && k < M.scr.length; k++) {
        const ctx = M.scr[k].getContext('2d'), img = ctx.createImageData(256, 64);
        for (let i = 0; i < 2048; i++) {
          const byte = bin.charCodeAt(k * 2048 + i);
          for (let b = 0; b < 8; b++) {
            const on = byte & (0x80 >> b), o = (i * 8 + b) * 4;
            img.data[o] = on ? 235 : 22; img.data[o + 1] = on ? 245 : 46; img.data[o + 2] = on ? 255 : 92; img.data[o + 3] = 255;
          }
        }
        ctx.putImageData(img, 0, 0);
      }
    }
    if (sig === M.sig) return;
    M.sig = sig;
    const pressed = new Set(m.pressed || []);
    const L = m.leds || {}, btn = L.button || {};
    for (const { c, n } of M.nodes) {
      if (c.kind === 'pad') {
        const rgbv = L.pad?.[c.n - 1] || [0, 0, 0];
        n.style.background = css(rgbv);
        const pr = (m.pads?.[c.n - 1] || 0) / 4095;
        n.firstChild.style.boxShadow = pr > 0 ? `0 0 0 ${2 + pr * 8}px #fff` : 'none';
        n.classList.toggle('held', pr > 0);
      } else if (c.kind === 'knob' || c.kind === 'encoder') {
        const k = c.kind === 'encoder' ? ((m.encoder || 0) % 30) / 30 : (m.knobs?.[+c.name.slice(5) - 1] || 0);
        n.firstChild.style.transform = `rotate(${k * 360}deg)`;
        n.classList.toggle('last', c.kind === 'knob' && m.last_knob === +c.name.slice(5) - 1);
      } else {
        let on = 0, col = null;
        const g = /^Group ([A-H])$/.exec(c.name);
        if (g) { const a = L.group?.[g[1].charCodeAt(0) - 65] || [0, 0, 0]; on = Math.max(...a) / 127; col = css(a); }
        else on = lvl(btn[c.name]);
        n.style.background = col && on > 0.02 ? col : on > 0.02 ? `rgba(235,240,255,${0.15 + on * 0.75})` : '';
        n.style.color = on > 0.5 ? '#0B0D10' : '';
        n.classList.toggle('held', pressed.has(c.name));
      }
    }
  }

  // ================================================================ settings (stub)
  const SET = { page: 'status' };
  const PAGES = [['status', 'Status'], ['engine', 'Engine & outputs'], ['controllers', 'Controllers'], ['audio', 'Audio'], ['gui', 'This GUI'], ['remote', 'Remote / OSC'], ['about', 'About']];
  function kv(rows) {
    const t = el('table', 'kv');
    for (const [k, v] of rows) { const tr = el('tr'); tr.append(el('td', 'dim', k), el('td', 'mono', v == null ? '–' : String(v))); t.append(tr); }
    return t;
  }
  function renderSettings() {
    const nav = $('setNav');
    if (!nav.children.length) {
      for (const [id, name] of PAGES) {
        const b = el('button', null, name); b.dataset.p = id; b.title = 'Settings: ' + name;
        b.addEventListener('click', () => { SET.page = id; SET.key = ''; renderSettings(); });
        nav.append(b);
      }
    }
    for (const b of nav.children) b.classList.toggle('on', b.dataset.p === SET.page);
    if (SET.page === 'audio' || SET.page === 'remote') {   // live pages (panels.js), updated in place
      const fresh = SET.key !== SET.page;
      SET.key = SET.page;
      window.Panels.settings(SET.page, $('setBody'), fresh);
      return;
    }
    const b = S.bridge || {}, L = S.live, m = S.mk2 || {}, c = S.tr?.clock;
    const key = SET.page + JSON.stringify([b.engine, b.commands, b.preview, b.epoch, b.pushed, b.dropped, Math.round(b.fps || 0), m.connected, m.present, L?.producer, c?.bpm, c?.grid, SET.page === 'controllers' ? S.weapons : 0]);
    if (key === SET.key) return;
    SET.key = key;
    const body = $('setBody'); body.textContent = '';
    const P = SET.page;
    const prodNames = ['Perform (MK2 mapper)', 'GUI (this page)', 'OSC', 'Tools', 'spare', 'spare', 'spare', 'spare'];
    if (P === 'status') {
      body.append(el('h2', null, 'Status'), kv([
        ['engine', b.engine], ['engine pid', b.engine_pid], ['epoch', b.epoch], ['frame', b.frame], ['fps', (b.fps || 0).toFixed(1)],
        ['command block', b.commands], ['preview block', b.preview],
        ['protocol versions', b.versions ? `command v${b.versions.command} · state v${b.versions.state} · preview v${b.versions.preview}` : '–'],
        ['bridge pid', b.bridge_pid], ['commands pushed', b.pushed], ['commands dropped (ring full)', b.dropped],
        ['beat clock', c ? `${c.bpm} BPM · grid ${c.grid} · ${c.source}` : 'no transport section']]));
      const t = el('table', 'kv'); const h = el('tr'); h.append(el('td', 'lbl', 'producer'), el('td', 'lbl', 'alive'), el('td', 'lbl', 'last applied'), el('td', 'lbl', 'refused'));
      t.append(h);
      (L?.producer || []).forEach((p, i) => { if (i > 3) return; const r = el('tr'); r.append(el('td', null, i + ' ' + prodNames[i]), el('td', null, p.alive ? '● yes' : '○ no'), el('td', 'mono', '#' + p.acked), el('td', 'mono', p.rejected + (p.rejected ? ' · ' + p.last_reject : ''))); t.append(r); });
      body.append(el('h3', null, 'Producers (command-protocol.md §3)'), t);
    } else if (P === 'engine') {
      body.append(el('h2', null, 'Engine & outputs'), el('p', 'dim', 'Resolution, refresh, blend, IDENTIFY, black per output and the layer places are on the OUTPUTS view. The rest is on the renderer command line in 0.1 (himmel.exe -- ARGS).'),
        kv([['outputs', '--outputs N (1..6)'], ['size', '--size WxH (custom)'], ['fullscreen', '--fullscreen'], ['AV offset', '--av-offset MS (−200..200)'], ['flash cap (D65, off by default)', '--flash-cap N'], ['test reaction signal', '--test-signal'], ['ticker text', '--ticker "TEXT"']]));
      const ov = el('button', null, 'Open the OUTPUTS view'); ov.title = 'Displays, IDENTIFY, resolution, blend'; ov.addEventListener('click', () => window.setView('outputs')); body.append(ov);
      const bt = el('button', null, 'Black (master)'); bt.title = 'Master black on / off (the outputs stay alive)'; bt.addEventListener('click', () => window.H.send({ type: 'Black', value: 2 }));
      const qt = el('button', 'bad', 'Quit Himmel…'); qt.title = 'Stop Himmel (asks first). The MK2: SHIFT + wheel → SYSTEM → QUIT, hold 2 s.';
      qt.addEventListener('click', () => window.H.quit());
      body.append(qt);
      body.append(bt);
    } else if (P === 'controllers') {
      body.append(el('h2', null, 'Controllers'), kv([
        ['MK2 controller block', m.present ? 'present' : 'not found'], ['MK2 USB', m.connected ? 'connected' : 'not connected'],
        ['MK2 map', m.map_verified ? 'verified map loaded' : m.present ? 'built-in default' : '–'], ['MK2 events dropped on GUI ring', m.dropped ?? '–'],
        ...(Array.isArray(S.weapons) ? S.weapons : []).map(w => [`slot ${w.slot} · ${w.name || w.weapon || '?'}`, w.state !== 'ok' ? 'other block version' : `${w.connected ? 'connected' : 'not connected'} · ${w.alive ? 'driver alive' : 'driver stopped'}${w.simulated ? ' · simulated' : ''}`])]));
      const b2 = el('button', null, 'Open the MK2 picture'); b2.title = 'The MK2 as the engine drives it'; b2.addEventListener('click', () => window.setView('mk2')); body.append(b2);
    } else if (P === 'gui') {
      body.append(el('h2', null, 'This GUI'), kv([['bridge', 'himmel_ui on http://127.0.0.1:8790 (only answers this PC)'], ['theme', 'dark (D28)'], ['keys', 'space = TAKE · enter = CUT · T = tap · Esc = cancel'], ['restart safety', 'killing himmel_ui or this page changes nothing on the outputs']]));
    } else {
      body.append(el('h2', null, 'About'), el('p', null, 'Himmel · open source (MIT) · © ReadyMedia.no and Himmel contributors. Every dependency is listed in THIRD_PARTY_NOTICES.md.'));
    }
  }

  // ================================================================ glue
  window.Views = {
    render() {
      if (S.view === 'start') renderWizard();
      else if (S.view === 'mk2') renderMk2();
      else if (S.view === 'settings') renderSettings();
    },
    settingsPage(p) { SET.page = p; SET.key = ''; },
    shown(v) {
      if (v === 'start') W.key = '';
      if (v === 'settings') SET.key = '';
      if (v === 'mk2' && M.prof) buildMk2();
    },
  };
  for (const b of document.querySelectorAll('#wizBar button')) b.addEventListener('click', () => go(+b.dataset.step));
  window.addEventListener('resize', () => { if (S.view === 'mk2' && M.prof) buildMk2(); });
  loadProfile();
  window.Views.shown(S.view);
  window.H.invalidate();
})();
