// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// M11 part 2: the panels over the optional blocks (D121).
//   SHOW     (Local\HimmelShow)    project bar in the top bar + the SCENES tab of the inspector
//   OUTPUTS  (Local\HimmelOutputs) the OUTPUTS view
//   LIBRARY  (Local\HimmelLibrary + the library DB) the LIBRARY view
//   AUDIO    (Local\HimmelAudio)   the meter in the top bar + Settings → Audio
//   OSC      (Local\HimmelOsc)     Settings → Remote / OSC
// Every panel hides itself when its block is missing or has the wrong version ({"present": false});
// nothing here may break Perform. Uses window.S / window.H from app.js; views.js uses window.Panels.W.
'use strict';

(() => {
  const $ = id => document.getElementById(id);
  const { el, send, sendT, toast } = window.H;
  const H = window.H;
  const S = window.S;
  const has = b => !!(b && b.present);
  const hex = n => '#' + (n >>> 0 & 0xffffff).toString(16).padStart(6, '0');
  const btn = (txt, title, fn, cls) => { const b = el('button', cls || null, txt); if (title) b.title = title; if (fn) b.addEventListener('click', fn); return b; };
  const ago = unix => {
    if (!unix) return 'never';
    const s = Math.max(0, Date.now() / 1000 - unix);
    if (s < 60) return Math.round(s) + ' s ago';
    if (s < 3600) return Math.round(s / 60) + ' min ago';
    return new Date(unix * 1000).toLocaleString();
  };
  const PRESET_WORDS = { P720: '720p', P1080: '1080p', P1440: '1440p', P2160: '4K', Custom: 'custom' };
  const REFRESH_WORDS = { Hz60: '60 Hz', Hz50: '50 Hz', Hz59_94: '59.94 Hz', Hz30: '30 Hz', Hz25: '25 Hz' };
  const PLACE_WORDS = { FillAll: 'FILL ALL', Out: 'OUT', Fit: 'FIT', Free: 'FREE' };
  const groupName = k => (k === 8 ? 'HIT' : `${'AB'[k >> 2]}${(k & 3) + 1}`);
  // The page's own focus deck (PREVIEW side unless a PROGRAM group was pressed).
  const focusDeck = () => { const L = S.live; if (!L) return 0; return L.focus_deck >= 0 ? L.focus_deck : 1 - L.program_deck; };

  // A modal asking for one line of text (SaveAs, rename). Resolves to the text or null.
  function ask(title, value, hint) {
    return new Promise(res => {
      const m = el('div', 'modal'), box = el('div', 'mbox askbox');
      const inp = el('input'); inp.type = 'text'; inp.value = value || ''; inp.spellcheck = false;
      const ok = btn('OK', null, () => { done(inp.value.trim() || null); }, 'primary');
      const no = btn('Cancel', null, () => done(null));
      function done(v) { m.remove(); res(v); }
      inp.addEventListener('keydown', e => { e.stopPropagation(); if (e.key === 'Enter') done(inp.value.trim() || null); if (e.key === 'Escape') done(null); });
      box.append(el('div', 'lbl', title), inp);
      if (hint) box.append(el('div', 'dim', hint));
      const row = el('div', 'askrow'); row.append(el('div', 'spacer'), no, ok); box.append(row);
      m.append(box); document.body.append(m); inp.focus(); inp.select();
    });
  }

  // ================================================================ SHOW: the project bar (top bar)
  const P = {};
  function buildProjectBar() {
    const bar = $('projBar');
    P.name = el('span', 'pname', '–'); P.name.title = 'The project (● = unsaved changes)';
    P.save = btn('SAVE', 'Save the project (Ctrl+S)', () => projectSave());
    P.menu = btn('⋯', 'Project: new, open, save as, export, revert, autosave', () => openProjectMenu());
    P.undo = btn('↶', 'Undo (live edits, 64 steps)', () => send({ type: 'Undo', value: 'undo' }));
    P.redo = btn('↷', 'Redo', () => send({ type: 'Undo', value: 'redo' }));
    P.msg = el('span', 'pmsg dim', '');
    bar.append(P.name, P.save, P.menu, P.undo, P.redo, P.msg);
    window.addEventListener('keydown', e => {
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's' && has(S.show)) { e.preventDefault(); projectSave(); }
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z' && has(S.show) && !/INPUT|SELECT|TEXTAREA/.test(document.activeElement?.tagName)) {
        e.preventDefault(); send({ type: 'Undo', value: e.shiftKey ? 'redo' : 'undo' });
      }
    });
  }
  async function projectSave() {
    const sh = S.show;
    if (!has(sh)) return;
    if (!sh.files.path) {   // untitled: Save = Save as
      const n = await ask('SAVE AS · project name or folder', sh.project === 'Untitled' ? '' : sh.project, 'A name goes into the projects folder; a full path makes that folder (<name>.himmel).');
      if (n) send({ type: 'Project', param: 'SaveAs', text: n });
      return;
    }
    send({ type: 'Project', param: 'Save' });
  }
  function renderProjectBar() {
    const sh = S.show, bar = $('projBar');
    bar.classList.toggle('hidden', !has(sh));
    if (!has(sh)) return;
    P.name.textContent = (sh.dirty ? '● ' : '') + sh.project + (sh.busy ? ' · …' : '');
    P.name.classList.toggle('dirty', sh.dirty);
    P.save.disabled = sh.busy || !sh.writable;
    P.undo.disabled = !sh.undo; P.undo.title = `Undo (${sh.undo} steps)`;
    P.redo.disabled = !sh.redo; P.redo.title = `Redo (${sh.redo} steps)`;
    if (P.lastMsg !== sh.message_seq) {
      if (P.lastMsg !== undefined && sh.message) toast(sh.message);
      P.lastMsg = sh.message_seq;
    }
    P.msg.textContent = sh.files.error ? '⚠ ' + sh.files.error : sh.alive ? '' : 'show not updating';
    P.msg.classList.toggle('bad', !!sh.files.error || !sh.alive);
    if (P.menuBox && !P.menuBox.isConnected) P.menuBox = null;
    if (P.menuBox) fillProjectMenu();
  }
  function openProjectMenu() {
    if (P.menuBox) { P.menuBox.closest('.modal').remove(); P.menuBox = null; return; }
    const m = el('div', 'modal'); const box = el('div', 'mbox projbox');
    m.addEventListener('click', e => { if (e.target === m) { m.remove(); P.menuBox = null; } });
    P.menuBox = box; P.menuKey = ''; m.append(box); document.body.append(m);
    fillProjectMenu();
  }
  function fillProjectMenu() {
    const sh = S.show, f = sh.files, box = P.menuBox;
    const key = JSON.stringify([sh.project, sh.dirty, sh.busy, f.path, f.saved_unix, f.autosave_unix, f.recent, f.missing_count, f.restored, f.error]);
    if (key === P.menuKey) return;
    P.menuKey = key; box.textContent = '';
    const head = el('div', 'mhead');
    head.append(el('span', 'lbl', 'PROJECT'), el('span', 'pname', sh.project + (sh.dirty ? ' ●' : '')), el('div', 'spacer'),
      btn('✕', 'Close', () => { box.closest('.modal').remove(); P.menuBox = null; }));
    box.append(head);
    const info = el('div', 'projinfo dim');
    info.append(el('div', 'mono', f.path || 'not saved yet (Untitled)'),
      el('div', null, `Saved ${ago(f.saved_unix)} · autosave ${ago(f.autosave_unix)} · ${f.saves} saves this run` +
        (f.restored !== 'None' ? ` · came back from ${f.restored === 'Autosave' ? 'the autosave' : 'an interrupted save'}` : '')));
    box.append(info);
    const acts = el('div', 'projacts');
    const sure = (q, cmd) => async () => { if (!sh.dirty || await H.confirm(q)) { send(cmd); } };
    acts.append(
      btn('New · 3 screens', 'A new project with the 3-screen template', sure('Unsaved changes are lost (the autosave keeps them). New project?', { type: 'Project', param: 'New', value: 3 })),
      btn('New · 1 screen', 'A new project with the 1-screen template', sure('Unsaved changes are lost (the autosave keeps them). New project?', { type: 'Project', param: 'New', value: 1 })),
      btn('Open…', 'Open a project folder or a .himmelshow file by path', async () => {
        const p = await ask('OPEN · project folder or .himmelshow file', '', 'e.g. C:\\Users\\me\\Documents\\Himmel\\Projects\\Club.himmel');
        if (p) send({ type: 'Project', param: 'Open', text: p });
      }),
      btn('Save', 'Save (Ctrl+S)', () => projectSave(), 'primary'),
      btn('Save as…', 'Save under a new name / folder', async () => {
        const n = await ask('SAVE AS · project name or folder', sh.project, 'A name goes into the projects folder; a full path makes that folder.');
        if (n) send({ type: 'Project', param: 'SaveAs', text: n });
      }),
      btn('Export .himmelshow', 'One file with the project (next to the project folder)', () => send({ type: 'Project', param: 'Export' })),
      btn('Revert', 'Back to the last save', sure('Throw away the changes since the last save?', { type: 'Project', param: 'Revert' })),
      btn('Restore autosave', 'Load the newest autosave of this project', () => send({ type: 'Project', param: 'RestoreAutosave' })),
      btn('Quit Himmel…', 'Stop Himmel: the outputs, the engine and this GUI (the autosave keeps your work)', () => window.H.quit(), 'bad'));
    if (!f.path) for (const b of acts.children) if (/Revert|Export/.test(b.textContent)) b.disabled = true;
    box.append(acts);
    if (f.missing_count) {
      const ms = el('div', 'projmiss');
      ms.append(el('div', 'lbl warn', `${f.missing_count} CLIPS MISSING`), el('div', 'dim', f.missing.join(' · ') + (f.missing_count > f.missing.length ? ' …' : '')),
        btn('Relink…', 'Search a folder for the missing clips (by name)', async () => {
          const d = await ask('RELINK · folder to search', '', 'Himmel looks for the missing clips by name in this folder and below.');
          if (d) send({ type: 'Project', param: 'Relink', text: d });
        }));
      if (f.relink_found) ms.append(el('div', 'dim', `The last relink found ${f.relink_found}.`));
      box.append(ms);
    }
    box.append(el('div', 'lbl', 'OPEN RECENT'));
    const rec = el('div', 'projrecent');
    f.recent.forEach((r, i) => {
      const b = btn('', r.path, sure(`Open "${r.name}"? Unsaved changes are lost (the autosave keeps them).`, { type: 'Project', param: 'Open', slot: i }));
      b.append(el('span', null, r.name), el('span', 'dim mono', `${r.outputs} out · ${ago(r.saved_unix)}`));
      rec.append(b);
    });
    if (!f.recent.length) rec.append(el('div', 'dim', 'No recent projects yet.'));
    box.append(rec);
  }

  // ================================================================ SHOW: the SCENES tab
  const SC = { sel: -1, built: false, key: '' };
  function buildScenes() {
    const box = $('scenesEd'); box.textContent = '';
    const head = el('div', 'schead');
    SC.page = el('span', 'scpage', '–');
    SC.prev = btn('◀', 'Previous bank / song', () => pageStep(-1));
    SC.next = btn('▶', 'Next bank / song', () => pageStep(1));
    SC.setl = btn('SETLIST', 'Pages show the current song (on) or a bank (off)', () => send({ type: 'ShowSelect', param: 'UseSetlist', value: 'toggle' }));
    SC.decks = el('span', 'scdecks mono');
    head.append(el('span', 'lbl', 'SCENES'), SC.prev, SC.page, SC.next, SC.setl, el('div', 'spacer'), SC.decks);
    SC.grid = el('div', 'scgrid');
    SC.pads = [];
    for (let row = 3; row >= 0; --row) for (let c = 0; c < 4; ++c) {   // pad 0 = bottom-left, like the MK2
      const k = row * 4 + c;
      const p = el('button', 'scpad'); p.title = `Scene pad ${k + 1}: click = recall into the focused deck · Shift + click = select · Alt = cut · double-click = rename`;
      p.append(el('span', 'scn', ''), el('span', 'scv', ''), el('i', 'sca'), el('i', 'scb'));
      p.addEventListener('click', e => padClick(k, e));
      p.addEventListener('dblclick', () => renameScene(k));
      SC.pads[k] = p;
      SC.grid.append(p);
    }
    const acts = el('div', 'scacts');
    SC.rec = btn('● REC', 'Save the focused deck\'s look on the selected pad (or the next free one). In PLAY focus: a new variation of its scene.', () => sceneSave(false), 'rec');
    SC.vari = btn('+ VAR', 'Save the focused deck as a new variation of its scene', () => sceneSave(true));
    SC.ren = btn('RENAME', 'Rename the selected scene (or double-click a pad)', () => renameScene(SC.sel));
    SC.reset = btn('RESET', 'Back to the scene as it was recalled (1 beat glide)', () => send({ type: 'SceneReset', deck: focusDeck() }));
    SC.go = btn('GO ▶', 'The next scene of the song / bank (ENTER on the MK2)', () => send({ type: 'SceneRecall', deck: focusDeck(), value: 'next' }), 'primary');
    SC.back = btn('◀', 'The previous scene', () => send({ type: 'SceneRecall', deck: focusDeck(), value: 'prev' }));
    acts.append(SC.rec, SC.vari, SC.ren, SC.reset, el('div', 'spacer'), SC.back, SC.go);
    const dice = el('div', 'scdice');
    SC.roll = btn('🎲 ROLL', 'Roll new values for the focused deck (locked kinds stay)', () => send({ type: 'Dice', param: 'Roll', deck: focusDeck() }));
    SC.mut = btn('MUTATE', 'Nudge the focused deck a little', () => send({ type: 'Dice', param: 'Mutate', deck: focusDeck() }));
    SC.dback = btn('◀', 'Back in the dice history', () => send({ type: 'Dice', param: 'Back', deck: focusDeck() }));
    SC.dfwd = btn('▶', 'Forward in the dice history', () => send({ type: 'Dice', param: 'Forward', deck: focusDeck() }));
    SC.dpos = el('span', 'mono dim', '');
    SC.locks = el('div', 'sclocks');
    for (const n of (S.names?.panels?.locks || ['Clip', 'Fx', 'Colour', 'Motion', 'Layout', 'Source', 'React', 'Mood'])) {
      const b = btn('🔒 ' + n.toUpperCase(), `Lock ${n}: the dice keep it`, () => send({ type: 'Locks', param: 'Toggle', value: [n] }), 'lock');
      b.dataset.lock = n; SC.locks.append(b);
    }
    dice.append(el('span', 'lbl', 'DICE'), SC.roll, SC.mut, SC.dback, SC.dpos, SC.dfwd, SC.locks);
    const mac = el('div', 'scmacros');
    SC.mac = [];
    for (let k = 0; k < 4; ++k) {
      const w = el('label', 'scmac'), n = el('span', 'lbl', `MACRO ${k + 5}`), r = el('input');
      r.type = 'range'; r.min = 0; r.max = 1; r.step = 0.001; r.title = 'Macro value (knobs 5–8 on the MK2)'; window.H.hold(r);
      r.addEventListener('input', () => sendT('macro' + k, { type: 'MacroValue', slot: k, f: +r.value }));
      w.append(n, r); mac.append(w); SC.mac.push({ n, r });
    }
    box.append(head, SC.grid, acts, dice, el('div', 'lbl', 'PROGRAM SCENE MACROS (knobs 5–8)'), mac);
    SC.built = true;
  }
  function pageStep(d) {
    const sh = S.show;
    send({ type: 'ShowSelect', param: sh.use_setlist ? 'Song' : 'Bank', value: d, relative: true });
  }
  function padClick(k, e) {
    const sh = S.show; if (!has(sh)) return;
    SC.sel = k; SC.key = '';
    if (e.shiftKey || !sh.pads[k].used) { renderScenes(); return; }   // Shift = select only
    // Click = recall into the focused deck. Alt = cut (no morph).
    send({ type: 'SceneRecall', deck: focusDeck(), value: k, f: e.altKey ? 0 : -1 });
  }
  async function sceneSave(variation) {
    const d = focusDeck(), sh = S.show;
    const target = SC.sel >= 0 && !variation ? SC.sel : -1;
    if (target >= 0 && sh.pads[target].used && !await H.confirm(`Pad ${target + 1} holds "${sh.pads[target].name}". Overwrite its look?`)) return;
    send({ type: 'SceneSave', deck: d, value: target, variation });
  }
  async function renameScene(k) {
    const sh = S.show; if (!has(sh) || k < 0 || !sh.pads[k].used) return toast('Select a scene first (Shift + click a pad).');
    const n = await ask(`RENAME SCENE ${k + 1}`, sh.pads[k].name);
    if (n) send({ type: 'SongEdit', param: 'RenameScene', slot: k, text: n });
  }
  function renderScenes() {
    const sh = S.show;
    $('tabScenes').classList.toggle('hidden', !has(sh));
    if (!has(sh)) { if (S.tab === 'scenes') window.H.setTab?.('clip'); return; }
    if (S.tab !== 'scenes') return;
    if (!SC.built) buildScenes();
    const fd = focusDeck(), L = S.live, prog = L ? L.program_deck : 0;
    SC.page.textContent = sh.use_setlist ? (sh.song >= 0 ? `SONG ${sh.song + 1}/${sh.song_count} · ${sh.song_name}${sh.song_bpm ? ' · ' + sh.song_bpm + ' bpm' : ''}` : 'no songs yet') : `BANK ${sh.bank + 1}`;
    SC.setl.classList.toggle('on', sh.use_setlist);
    const dw = d => { const x = sh.decks[d]; return x.scene < 0 ? '–' : `${x.pad >= 0 ? x.pad + 1 : '#' + x.scene}${x.variation}${x.edited ? '*' : ''}${x.morphing ? ' ' + Math.round(x.morph * 100) + '%' : ''}`; };
    SC.decks.textContent = `A ${dw(0)} · B ${dw(1)} · focus ${'AB'[fd]} (${fd === prog ? 'PROGRAM' : 'PREVIEW'})`;
    sh.pads.forEach((p, k) => {
      const b = SC.pads[k];
      b.classList.toggle('used', p.used); b.classList.toggle('sel', k === SC.sel);
      b.classList.toggle('ona', !!(p.used && p.on[0])); b.classList.toggle('onb', !!(p.used && p.on[1]));
      b.children[0].textContent = p.used ? p.name : String(k + 1);
      b.children[1].textContent = p.used && p.variations ? `+${p.variations}` : '';
      b.style.setProperty('--sc', p.used ? hex(p.rgb) : 'transparent');
    });
    SC.ren.disabled = SC.sel < 0 || !sh.pads[SC.sel]?.used;
    SC.rec.textContent = SC.sel >= 0 ? `● REC → ${SC.sel + 1}` : '● REC';
    const dk = sh.decks[fd];
    SC.dpos.textContent = dk.dice_count ? `${dk.dice_pos}/${dk.dice_count}` : '';
    SC.dback.disabled = !dk.dice_pos || dk.dice_pos <= 1; SC.dfwd.disabled = !dk.dice_count || dk.dice_pos >= dk.dice_count;
    for (const b of SC.locks.children) b.classList.toggle('on', sh.locks.includes(b.dataset.lock));
    sh.macros.forEach((m, k) => { SC.mac[k].n.textContent = m.name || `MACRO ${k + 5}`; window.H.setVal(SC.mac[k].r, m.value); });
  }

  // ================================================================ OUTPUTS
  const O = { key: '' };
  const presetSel = (cur, fn) => {
    const s = el('select'); s.title = 'Output resolution (the canvas is rebuilt: one hitch, do it before the show)';
    for (const p of ['P720', 'P1080', 'P1440', 'P2160']) { const o = el('option', null, PRESET_WORDS[p] + (p === 'P1080' ? ' · recommended' : p === 'P1440' || p === 'P2160' ? ' · big rigs' : '')); o.value = p; s.append(o); }
    if (cur === 'Custom') { const o = el('option', null, 'custom (command line)'); o.value = 'Custom'; o.disabled = true; s.append(o); }
    s.value = cur; s.addEventListener('change', () => fn(s.value)); return s;
  };
  const refreshSel = (cur, fn) => {
    const s = el('select'); s.title = 'Refresh rate of the outputs (50 Hz where lights and cameras run at 50 Hz)';
    for (const r of ['Hz60', 'Hz50', 'Hz59_94', 'Hz30', 'Hz25']) { const o = el('option', null, REFRESH_WORDS[r]); o.value = r; s.append(o); }
    s.value = cur; s.addEventListener('change', () => fn(s.value)); return s;
  };
  async function setResolution(v, slot) {
    if (!await H.confirm(`Change the resolution to ${PRESET_WORDS[v]}? The canvas is rebuilt (one hitch on the outputs). Do it before the show.`)) { O.key = ''; return; }
    send(slot == null ? { type: 'OutputConfig', param: 'Resolution', value: v } : { type: 'OutputConfig', param: 'Resolution', slot, value: v });
  }
  function identifyBtn(small) {
    const o = S.outputs;
    const b = btn(o?.identify ? `IDENTIFY · ${Math.ceil((o.identify_left_ms || 0) / 1000)} s` : 'IDENTIFY', 'Big numbers on every output for 5 s (click again to stop)',
      () => send({ type: 'Identify', value: S.outputs?.identify ? 'Off' : '5s' }), 'identify' + (o?.identify ? ' on' : '') + (small ? ' small' : ''));
    return b;
  }
  function outputCard(o, k, compact) {
    const c = el('div', 'ocard st-' + o.state.toLowerCase());
    const st = { Ok: 'ON SCREEN', Lost: 'LOST · still rendered', Window: 'WINDOW', Other: 'NEW DISPLAY?', Off: 'OFF' }[o.state] || o.state;
    const h = el('div', 'ohead');
    h.append(el('span', 'onum', `OUT ${k + 1}`), el('span', 'ostate', st));
    if (o.black) h.append(el('span', 'oblack', 'BLACK'));
    c.append(h);
    const d = o.display;
    const disp = o.name || (d.maker ? `${d.maker} ${d.product}` : 'window (no display identity)');
    c.append(el('div', 'oname', disp),
      el('div', 'dim mono', `${o.size[0]}×${o.size[1]} · ${PRESET_WORDS[o.preset] || o.preset} · ${o.hz ? o.hz + ' Hz' : '– Hz'} · x ${o.canvas_x}` + (d.serial || d.serial_text ? ` · s/n ${d.serial_text || d.serial}` : '') + (d.connector ? ` · conn ${d.connector}` : '')));
    if (o.lost_count) c.append(el('div', 'dim', `lost ${o.lost_count}× · last return ${o.returned_ms} ms`));
    if (compact) return c;
    const row = el('div', 'orow');
    row.append(btn(o.black ? 'UNBLACK' : 'BLACK', 'Black on this output only (master BLACK still applies)', () => send({ type: 'OutputBlack', slot: k, value: 'toggle' }), o.black ? 'on' : ''));
    if (o.state === 'Other') {
      row.append(btn('USE NEW DISPLAY', 'Take the new display as OUT ' + (k + 1), () => send({ type: 'OutputConfig', param: 'Adopt', slot: k, value: 1 }), 'primary'),
        btn('KEEP WAITING', 'Wait for the old display', () => send({ type: 'OutputConfig', param: 'Adopt', slot: k, value: 0 })));
    }
    const sim = btn(o.state === 'Lost' ? 'SIM: BACK' : 'SIM: PULL', 'Test on one screen: act as if this display was pulled / came back', () => send({ type: 'OutputConfig', param: 'SimLost', slot: k, value: o.state === 'Lost' ? 0 : 1 }), 'test');
    row.append(el('div', 'spacer'), sim);
    c.append(row);
    return c;
  }
  function renderOutputs() {
    const o = S.outputs, box = $('outBody');
    const key = JSON.stringify(o) + '|' + (S.live?.program_deck ?? '');
    if (key === O.key) return;
    O.key = key; box.textContent = '';
    if (!has(o)) {
      box.append(el('div', 'empty', o?.state === 'version' ? 'The renderer\'s outputs block has another version than this GUI: build both from the same commit.'
        : 'No outputs block (Local\\HimmelOutputs): the renderer is not running, or it is older than M6.'));
      return;
    }
    const top = el('div', 'otop');
    top.append(el('span', 'lbl', 'CANVAS'), el('span', 'mono', `${o.canvas[0]}×${o.canvas[1]}`),
      el('span', 'lbl', 'MODE'));
    const mode = el('select'); mode.title = 'SPAN = one canvas across all outputs; MIRROR = the same picture on each';
    for (const m of ['Span', 'Mirror']) { const op = el('option', null, m.toUpperCase()); op.value = m; mode.append(op); }
    mode.value = o.mode; mode.addEventListener('change', () => send({ type: 'OutputConfig', param: 'Mode', value: mode.value }));
    const blend = el('input'); blend.type = 'number'; blend.min = 0; blend.max = 1024; blend.step = 8; blend.value = o.blend_px; blend.title = 'Overlap between neighbours in canvas pixels (0 = screens butt; projectors overlap)';
    blend.addEventListener('change', () => send({ type: 'OutputConfig', param: 'BlendWidth', value: Math.max(0, Math.min(1024, Math.round(+blend.value))) }));
    const gam = el('input'); gam.type = 'number'; gam.min = 1; gam.max = 3; gam.step = 0.1; gam.value = o.blend_gamma; gam.title = 'Edge-blend gamma (2.2 default)';
    gam.addEventListener('change', () => send({ type: 'OutputConfig', param: 'BlendGamma', f: +gam.value }));
    const res = (o.outputs[0] || {}).preset || 'P1080';
    top.append(mode, el('span', 'lbl', 'RESOLUTION'), presetSel(res, v => setResolution(v)),
      el('span', 'lbl', 'REFRESH'), refreshSel(o.refresh, v => send({ type: 'OutputConfig', param: 'Refresh', value: v })),
      el('span', 'lbl', 'BLEND'), blend, el('span', 'dim', 'px'), el('span', 'lbl', 'γ'), gam,
      el('div', 'spacer'), identifyBtn(false));
    box.append(top);
    box.append(el('div', 'dim', `${o.fullscreen ? 'Full screen' : 'Windows (test mode)'} · ${o.displays} displays found · asked ${o.hz_target} Hz, measured ${o.hz_measured} fps` +
      (o.alive ? '' : ' · ⚠ the renderer is not updating this block') + (o.config_by_command ? ' · set from here (kept over a restart)' : '')));
    // A strip of the canvas: each output's part, to scale.
    const strip = el('div', 'ostrip');
    const W = Math.max(1, o.canvas[0]);
    o.outputs.forEach((x, k) => {
      const seg = el('div', 'oseg st-' + x.state.toLowerCase(), `OUT ${k + 1}`);
      seg.style.left = (100 * x.canvas_x / W) + '%'; seg.style.width = (100 * x.size[0] / W) + '%';
      if (x.black) seg.classList.add('black');
      strip.append(seg);
    });
    box.append(strip);
    const cards = el('div', 'ocards');
    o.outputs.forEach((x, k) => cards.append(outputCard(x, k, false)));
    box.append(cards);
    // Layer places.
    box.append(el('div', 'lbl', 'LAYER PLACES (where each layer covers the canvas)'));
    const tbl = el('div', 'oplaces');
    o.places.forEach((p, k) => {
      const deck = k === 8 ? 'hit' : k >> 2, layer = k === 8 ? 0 : k & 3;
      const r = el('div', 'oplace');
      const sel = el('select'); sel.title = `Where ${groupName(k)} shows: FILL ALL (the whole canvas), FIT, FREE (a rectangle) or one output`;
      for (const pl of ['FillAll', 'Fit', 'Free']) { const op = el('option', null, PLACE_WORDS[pl]); op.value = pl; sel.append(op); }
      o.outputs.forEach((_, n) => { const op = el('option', null, `OUT ${n + 1}`); op.value = 'Out' + n; sel.append(op); });
      sel.value = p.place === 'Out' ? 'Out' + p.out : p.place;
      const rect = el('span', 'mono dim', p.rect.map(v => v.toFixed(2)).join(' '));
      sel.addEventListener('change', async () => {
        const v = sel.value;
        if (v.startsWith('Out')) return send({ type: 'LayerPlace', deck, layer, value: 'Out', param: +v.slice(3) });
        if (v === 'Free') {
          const t = await ask(`FREE PLACE · ${groupName(k)} · x y w h (canvas 0..1)`, p.rect.map(v => v.toFixed(2)).join(' '));
          const f = (t || '').split(/[\s,]+/).map(Number);
          if (f.length === 4 && f.every(Number.isFinite) && f[2] > 0 && f[3] > 0) return send({ type: 'LayerPlace', deck, layer, value: 'Free', f });
          O.key = ''; return t && toast('Free needs four numbers x y w h (w, h > 0)', 'bad');
        }
        send({ type: 'LayerPlace', deck, layer, value: v });
      });
      r.append(el('span', 'opn', groupName(k)), sel, rect);
      tbl.append(r);
    });
    box.append(tbl);
  }

  // ================================================================ LIBRARY
  // GUI wave 7: the LIBRARY view moved to gui/library.js (window.Lib): filters, sort, multi-select,
  // the layer dock, import progress, disk space, the path dialog.

  // ================================================================ AUDIO: the top-bar meter
  const AM = {};
  function buildAudioMeter() {
    const m = $('audMeter');
    m.title = 'Sound in (himmel_audio): level, bass, mid, high, kick. Click = Settings → Audio';
    AM.bars = ['level', 'bass', 'mid', 'high'].map(n => { const b = el('i', 'am-' + n); m.append(b); return b; });
    AM.kick = el('b', 'amkick'); m.append(AM.kick);
    m.addEventListener('click', () => { window.Views?.settingsPage?.('audio'); window.setView('settings'); });
  }
  function renderAudioMeter() {
    const a = S.audio, m = $('audMeter');
    m.classList.toggle('hidden', !has(a));
    if (!has(a)) return;
    const run = a.alive && a.input.state === 'Running';
    m.classList.toggle('dead', !run);
    ['level', 'bass', 'mid', 'high'].forEach((n, k) => { AM.bars[k].style.height = (run ? a.sig[n] * 100 : 0) + '%'; });
    AM.kick.classList.toggle('on', run && a.sig.kick > 0.5);
    m.classList.toggle('clip', a.clipping);
  }

  // ================================================================ Settings pages (Audio, Remote / OSC)
  const AU = { built: false };
  async function loadDevices() {
    try { const r = await fetch('/audio/devices'); AU.dev = await r.json(); } catch (e) { AU.dev = { ok: false }; }
    AU.devKey = ''; window.H.invalidate();
  }
  async function postAudio(cfg) {
    try {
      const r = await fetch('/audio/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(cfg) });
      const j = await r.json();
      if (!j.ok) toast(j.error || 'refused', 'bad');
    } catch (e) { toast('himmel_ui is not reachable', 'bad'); }
  }
  // The input picker + meter, used by Settings → Audio and the wizard (step 4).
  function audioPicker(box, compact) {
    const a = S.audio;
    if (!has(a)) {
      box.append(el('div', 'empty', a?.state === 'version' ? 'himmel_audio has another block version than this GUI: build both from the same commit.'
        : 'himmel_audio is not running (Local\\HimmelAudio). himmel.exe starts it; effects react to nothing until it runs.'));
      return null;
    }
    if (!AU.dev) loadDevices();
    const u = {};
    const row = el('div', 'aurow');
    u.mode = el('select'); u.mode.title = 'MIC / LINE IN = a sound card input; SOUND ON THIS PC = what this PC plays (loopback)';
    for (const [v, t] of [['Capture', 'MIC / LINE IN'], ['Loopback', 'SOUND ON THIS PC']]) { const o = el('option', null, t); o.value = v; u.mode.append(o); }
    u.dev = el('select'); u.dev.title = 'The input device';
    u.mode.addEventListener('change', () => fillDevs(u, true));
    u.ch = el('select'); u.ch.title = 'Which channels Himmel listens to';
    for (const [v, t] of [['1:0', 'mono · ch 1'], ['1:1', 'mono · ch 2'], ['2:0', 'stereo 1+2'], ['2:2', 'stereo 3+4']]) { const o = el('option', null, t); o.value = v; u.ch.append(o); }
    const apply = btn('USE THIS INPUT', 'Tell himmel_audio to switch (it reopens the input)', () => {
      const [cc, cf] = u.ch.value.split(':').map(Number);
      postAudio({ mode: u.mode.value, device: u.dev.value, channel_count: cc, channel_first: cf });
    }, 'primary');
    row.append(el('span', 'lbl', 'INPUT'), u.mode, u.dev, u.ch, apply, btn('↻', 'List the devices again', () => { AU.dev = null; loadDevices(); }));
    box.append(row);
    u.state = el('div', 'dim'); box.append(u.state);
    u.meters = el('div', 'aumeters');
    u.sig = {};
    for (const n of ['level', 'bass', 'mid', 'high', 'kick', 'snare', 'hat', 'energy']) {
      const m = el('div', 'aum'); const fill = el('i'); m.append(fill, el('span', null, n.toUpperCase())); u.sig[n] = fill; u.meters.append(m);
    }
    u.spec = el('div', 'auspec');
    u.bands = [];
    for (let k = 0; k < 32; ++k) { const b = el('i'); u.spec.append(b); u.bands.push(b); }
    const mrow = el('div', 'aumrow'); mrow.append(u.meters, u.spec); box.append(mrow);
    u.db = el('div', 'mono dim'); box.append(u.db);
    if (!compact) {
      const tune = el('div', 'aurow');
      u.ag = el('input'); u.ag.type = 'checkbox'; u.ag.title = 'Auto gain: Himmel levels the input by itself';
      u.ag.addEventListener('change', () => postAudio({ auto_gain: u.ag.checked }));
      const num = (lo, hi, step, key, title) => { const i = el('input'); i.type = 'number'; i.min = lo; i.max = hi; i.step = step; i.title = title; window.H.hold(i); i.addEventListener('change', () => postAudio({ [key]: +i.value })); return i; };
      u.gain = num(-24, 48, 1, 'gain_db', 'Manual gain, dB (-24..+48)');
      u.gate = num(-96, -20, 1, 'gate_db', 'Gate, dBFS: below it the levels stay at 0');
      u.sens = num(0, 1, 0.05, 'onset_sensitivity', 'Kick / snare / hat sensitivity 0..1');
      const agl = el('label', 'nb'); agl.append(u.ag, el('span', null, 'auto gain'));
      tune.append(el('span', 'lbl', 'TUNE'), agl, el('span', 'lbl', 'gain'), u.gain, el('span', 'dim', 'dB'), el('span', 'lbl', 'gate'), u.gate, el('span', 'dim', 'dBFS'), el('span', 'lbl', 'sensitivity'), u.sens);
      box.append(tune);
      const clk = el('div', 'aurow');
      u.midi = el('select'); u.midi.title = 'A MIDI port that sends MIDI clock (a DJ mixer, a drum machine): Himmel follows its tempo';
      u.midi.addEventListener('change', () => postAudio({ midi_clock_port: u.midi.value }));
      u.clk = el('span', 'mono');
      clk.append(el('span', 'lbl', 'MIDI CLOCK IN'), u.midi, u.clk);
      box.append(clk);
      u.engClk = el('div', 'dim'); box.append(u.engClk);
      box.append(el('div', 'dim', 'AV offset (beat signals early / late to match the room): himmel_render --av-offset MS, −200..200 (not in a block yet, so it is not shown or set here).'));
    }
    u.devKey = '';
    return u;
  }
  function fillDevs(u, keepMode) {
    const d = AU.dev, a = S.audio;
    const mode = keepMode ? u.mode.value : a.config.gen ? a.config.mode : a.input.mode;
    const list = d?.ok ? (mode === 'Loopback' ? d.render : d.capture) : [];
    u.dev.textContent = '';
    const def = el('option', null, '(the Windows default)'); def.value = ''; u.dev.append(def);
    for (const x of list) { const o = el('option', null, `${x.default ? '★ ' : ''}${x.name} · ${x.channels} ch · ${x.rate / 1000} kHz`); o.value = x.name; u.dev.append(o); }
    const cur = a.config.gen ? a.config.device : a.input.device;
    if (cur && ![...u.dev.options].some(o => o.value === cur)) { const o = el('option', null, cur + ' (in use)'); o.value = cur; u.dev.append(o); }
    u.dev.value = cur || '';
    if (!keepMode) u.mode.value = mode;
    if (u.midi) {
      u.midi.textContent = '';
      const none = el('option', null, '(none)'); none.value = ''; u.midi.append(none);
      for (const n of d?.midi || []) { const o = el('option', null, n); o.value = n; u.midi.append(o); }
      u.midi.value = a.config.midi_clock_port || '';
    }
    const cc = a.input.channel_count || 1, cf = a.input.channel_first || 0;
    const v = `${cc}:${cf}`; if ([...u.ch.options].some(o => o.value === v)) u.ch.value = v;
  }
  function updatePicker(u) {
    const a = S.audio;
    if (!u || !has(a)) return;
    const dk = JSON.stringify([!!AU.dev, a.config.gen, a.input.device, a.input.mode]);
    if (dk !== u.devKey) { u.devKey = dk; fillDevs(u, false); }
    const i = a.input;
    u.state.textContent = `${a.alive ? '' : '⚠ himmel_audio is not updating · '}${i.state.toUpperCase()} · ${i.device || 'no device'} · ${i.mode === 'Loopback' ? 'loopback' : 'capture'} · ${i.sample_rate ? i.sample_rate / 1000 + ' kHz' : ''} · ch ${i.channel_first + 1}${i.channel_count > 1 ? '+' + (i.channel_first + 2) : ''} of ${i.device_channels} · reopened ${i.opens}× · glitches ${i.glitches}` +
      (a.config.gen && a.config.gen !== a.config.applied ? ' · switching…' : '');
    for (const [n, f] of Object.entries(u.sig)) f.style.height = (a.sig[n] * 100) + '%';
    a.spectrum.forEach((v, k) => { u.bands[k].style.height = (v * 100) + '%'; });
    u.db.textContent = `rms ${a.rms_db} dBFS · peak ${a.peak_db} dBFS · gain ${a.gain_db} dB${a.gated ? ' · GATED' : ''}${a.clipping ? ' · CLIPPING' : ''} · hits ${a.hits.join(' / ')}`;
    if (u.ag) {
      if (document.activeElement !== u.ag) u.ag.checked = a.config.gen ? a.config.auto_gain : true;
      window.H.setVal(u.gain, a.config.gen ? a.config.gain_db : 0);
      window.H.setVal(u.gate, a.config.gen ? a.config.gate_db : -60);
      window.H.setVal(u.sens, a.config.gen ? a.config.onset_sensitivity : 0.5);
      const mc = a.clock.find(c => c.kind === 'Midi');
      u.clk.textContent = mc ? `${mc.run.toUpperCase()} · ${mc.bpm || '–'} bpm · confidence ${Math.round(mc.confidence * 100)}% · beat ${mc.beat}` : 'no MIDI clock';
      const c = S.tr?.clock;
      u.engClk.textContent = c ? `Show clock: ${c.bpm.toFixed(1)} bpm · source ${c.source} (internal / tap, or MIDI when a MIDI clock runs: it leads, tap is ignored).` : '';
    }
  }
  // The default reaction of every effect (effect.json "reaction"), with the live signal it follows.
  function reactionList(box) {
    const wrap = el('div', 'aureact');
    const items = [];
    (S.cat?.effects || []).forEach(e => {
      const m = S.meta?.[e.id], r = m?.reaction;
      if (!r) return;
      const sig = String(r.signal || r.source || '').toLowerCase();
      const row = el('div', 'aur');
      const bar = el('span', 'aubar'); const fill = el('i'); bar.append(fill);
      row.append(el('span', 'aun', e.name), el('span', 'mono dim', `${sig || '?'} → ${r.param || r.target || '?'}${r.amount != null ? ' · ' + r.amount : ''}${r.mode ? ' · ' + r.mode : ''}`), bar);
      wrap.append(row); items.push({ sig, fill });
    });
    if (!items.length) wrap.append(el('div', 'dim', 'No effect metadata with reactions yet.'));
    box.append(el('div', 'lbl', 'DEFAULT REACTION PER EFFECT (the editor comes with FxReact, D131)'), wrap);
    return items;
  }

  const SET = { page: null, u: null, react: null, oscKey: '' };
  function renderSettingsPage(page, body, fresh) {
    if (fresh || SET.page !== page) {
      SET.page = page; body.textContent = ''; SET.u = null; SET.react = null; SET.oscKey = '';
      if (page === 'audio') {
        body.append(el('h2', null, 'Audio'));
        SET.u = audioPicker(body, false);
        SET.react = reactionList(body);
        SET.audioPresent = has(S.audio);
      } else {
        body.append(el('h2', null, 'Remote / OSC'));
        SET.osc = el('div'); body.append(SET.osc);
      }
    }
    if (page === 'audio') {
      if (has(S.audio) !== SET.audioPresent) return renderSettingsPage(page, body, true);
      updatePicker(SET.u);
      for (const it of SET.react || []) it.fill.style.width = (has(S.audio) && S.audio.sig[it.sig] != null ? S.audio.sig[it.sig] * 100 : 0) + '%';
      return;
    }
    const o = S.osc, key = JSON.stringify(o);
    if (key === SET.oscKey) return;
    SET.oscKey = key; const b = SET.osc; b.textContent = '';
    if (!has(o)) { b.append(el('div', 'empty', o?.state === 'version' ? 'himmel_osc has another block version than this GUI.' : 'himmel_osc is not running (Local\\HimmelOsc). himmel.exe starts it (OSC in on UDP 7000, this PC only by default).')); return; }
    b.append(el('div', null, `${o.alive ? '' : '⚠ not updating · '}Listening on UDP ${o.port || '(could not bind)'} · allow: `), el('div', 'mono', o.allow || '(this PC only)'));
    const c = o.counts;
    b.append(el('div', 'mono dim', `received ${c.received} · applied ${c.applied} · refused ${c.refused} · blocked ${c.blocked} · garbage ${c.garbage} · rate-limited ${c.rate_limited} · ring full ${c.ring_full}`));
    b.append(el('div', 'lbl', 'LAST MESSAGES (newest first)'));
    const t = el('table', 'kv osclog');
    for (const m of o.last) {
      const tr = el('tr', 'v-' + m.verdict.toLowerCase());
      tr.append(el('td', 'mono dim', new Date(m.unix_ms).toLocaleTimeString()), el('td', 'mono dim', m.from), el('td', 'mono', m.text), el('td', null, m.verdict.toUpperCase()));
      t.append(tr);
    }
    if (!o.last.length) b.append(el('div', 'dim', 'Nothing received yet. Try: himmel_osc --send /himmel/take'));
    else b.append(t);
    b.append(el('div', 'dim', 'The address list is in docs/interfaces/osc.md. Allow other machines with himmel.exe --osc "--allow 192.168.1.0/24".'));
  }

  // ================================================================ glue
  window.Panels = {
    render() {
      renderProjectBar();
      renderAudioMeter();
      renderScenes();
      if (S.view === 'outputs') renderOutputs();
      if (window.Lib) window.Lib.tick();   // wave 8: ingest replies on every view
      if (S.view === 'library' && window.Lib) window.Lib.render();
    },
    shown(v) {
      if (v === 'outputs') O.key = '';
      if (v === 'library' && window.Lib) window.Lib.shown();
    },
    tabShown(tab) { if (tab === 'scenes') { SC.key = ''; renderScenes(); } },
    settings: renderSettingsPage,
    // Widgets for the wizard (views.js).
    W: { outputCard, identifyBtn, presetSel, refreshSel, setResolution, audioPicker, updatePicker, PRESET_WORDS, REFRESH_WORDS, has, ask },
  };
  buildProjectBar();
  buildAudioMeter();
})();
