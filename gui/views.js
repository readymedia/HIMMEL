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
  const W = { step: 1 };
  const NEXT = { 1: 'Next: weapon', 2: 'Next: outputs', 3: 'Next: play', 4: 'Open Perform' };

  function card(title, body, cls) {
    const c = el('div', 'wcard' + (cls ? ' ' + cls : ''));
    c.append(el('div', 'wct', title));
    if (typeof body === 'string') c.append(el('div', 'dim', body)); else if (body) c.append(body);
    return c;
  }
  function wizNav() {
    const nav = el('div', 'wiznav');
    if (W.step > 1) { const b = el('button', null, '◀ Back'); b.addEventListener('click', () => go(W.step - 1)); nav.append(b); }
    nav.append(el('div', 'spacer'));
    const n = el('button', 'primary', NEXT[W.step] + ' ▶');
    n.addEventListener('click', () => (W.step < 4 ? go(W.step + 1) : window.setView('perform')));
    nav.append(n);
    return nav;
  }
  function go(step) { W.step = step; W.key = ''; renderWizard(); }

  // Each step is rebuilt only when what it shows changed (cheap key), so clicks are not eaten.
  function renderWizard() {
    for (const b of document.querySelectorAll('#wizBar button')) {
      b.classList.toggle('on', +b.dataset.step === W.step);
    }
    const key = W.step + '|' + stepKey(W.step);
    if (W.key === key) { if (W.step === 4 && W.u) window.Panels.W.updatePicker(W.u); return; }
    W.key = key;
    const body = $('wizBody'); body.textContent = '';
    body.append(el('h2', null, ['', 'Add clips', 'Choose your weapon', 'Set outputs', 'Play'][W.step]));
    W.u = null;
    ({ 1: step1, 2: step2, 3: step3, 4: step4 })[W.step](body);
    body.append(wizNav());
    if (W.u) window.Panels.W.updatePicker(W.u);
  }
  function stepKey(step) {
    const b = S.bridge, m = S.mk2, L = S.live, lib = S.library, o = S.outputs, a = S.audio;
    if (step === 1) return JSON.stringify([S.cat?.clips.length ?? -1, S.cat?.effects.length ?? -1, lib?.present, lib?.alive, lib?.queued, lib?.working, lib?.ready_total, lib?.failed_total, lib?.job?.name, lib?.catalog_full]);
    if (step === 2) return JSON.stringify([m?.present, m?.connected, m?.alive, (m?.pressed || []).join('+'), L?.producer?.[0]?.alive, L?.producer?.[1]?.alive, S.weapons, a?.present]);
    if (step === 3) return JSON.stringify([b?.preview, b?.engine, o]);
    // Step 4 holds a live meter: rebuilt only when the input changes; the meter updates in place.
    return JSON.stringify([b?.engine, S.cat?.clips.length, m?.connected, a?.present, a?.input?.device, a?.input?.state, o?.count, (o?.outputs || []).map(x => x.state)]);
  }

  function step1(body) {
    const n = S.cat?.clips.length ?? 0, g = (S.cat?.effects || []).filter(e => e.kind === 'generator').length;
    const lib = S.library, PW = window.Panels.W;
    const drop = el('div', 'drop');
    const root = lib?.root || '';
    drop.append(el('div', 'big', 'Drop clips into the library folder'),
      el('div', 'mono', root ? root + '\\originals' : 'Documents\\Himmel\\Library\\originals'),
      el('div', 'dim', 'Video, stills, GIFs, SVG and image sequences are converted to HAP, analysed (colours, motion, mood, loops) and tagged, in the background. A browser cannot hand files to Himmel: copy them into that folder (Explorer), they appear in the LIBRARY view when ready.'));
    drop.addEventListener('dragover', e => e.preventDefault());
    drop.addEventListener('drop', e => { e.preventDefault(); window.H.toast('Copy the files into ' + (root ? root + '\\originals' : 'the originals folder') + ' with Explorer: the page cannot import them itself.'); });
    body.append(drop);
    const st = el('div');
    if (PW.has(lib)) {
      st.append(el('div', 'wstat ' + (lib.alive ? 'ok' : 'warn'), lib.alive ? (lib.working || lib.queued ? 'IMPORTING' : 'READY FOR NEW FILES') : 'IMPORT WORKER NOT RUNNING'),
        el('div', 'mono dim', `${lib.queued} queued · ${lib.working} working · ${lib.ready_total} ready · ${lib.failed_total} failed`));
      if (lib.job.name) st.append(el('div', null, `${lib.job.name} · ${lib.job.stage} · ${Math.round(lib.job.progress * 100)}%`));
      if (lib.catalog_full) st.append(el('div', 'bad', `Catalog full: ${lib.catalog_full} ready clips are not playable (256 max in 0.1).`));
    } else {
      st.append(el('div', 'wstat warn', 'IMPORT WORKER NOT RUNNING'), el('div', 'dim', 'himmel.exe starts himmel_ingest (not with --no-show-files unless --library is given). The engine still plays every HAP .mov in media/.'));
    }
    const open = el('button', null, 'Open the LIBRARY view'); open.addEventListener('click', () => window.setView('library')); st.append(open);
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
    const cards = el('div', 'wrow');
    o.outputs.forEach((x, k) => cards.append(PW.outputCard(x, k, true)));
    body.append(el('div', 'lbl', `${o.count} OUTPUT${o.count === 1 ? '' : 'S'} · ${o.mode.toUpperCase()} · canvas ${o.canvas[0]}×${o.canvas[1]} · ${o.displays} displays found`), cards);
    const ctl = el('div', 'wctl');
    const res = (o.outputs[0] || {}).preset || 'P1080';
    ctl.append(el('span', 'lbl', 'RESOLUTION PER OUTPUT'), PW.presetSel(res, v => PW.setResolution(v)),
      el('span', 'lbl', 'REFRESH'), PW.refreshSel(o.refresh, v => window.H.send({ type: 'OutputConfig', param: 'Refresh', value: v })),
      PW.identifyBtn(false));
    body.append(ctl,
      el('div', 'dim', 'IDENTIFY shows a big number on every output for 5 s: check that OUT 1 is on the left. 1080p is the safe choice; 1440p / 4K for big rigs. 50 Hz in countries with 50 Hz light and cameras. The number of outputs is set when Himmel starts (himmel.exe -- --outputs N) in 0.1.'));
    const more = el('button', null, 'More on the OUTPUTS view'); more.addEventListener('click', () => window.setView('outputs')); body.append(more);
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
    body.append(el('div', 'dim', 'Perform: PREVIEW (green) left, PROGRAM (red) right, TAKE in the middle. Click a layer = focus; the CLIP tab edits its transport, LAYER FX its effects, SCENES saves and recalls looks.'));
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
        const b = el('button', null, name); b.dataset.p = id;
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
      const ov = el('button', null, 'Open the OUTPUTS view'); ov.addEventListener('click', () => window.setView('outputs')); body.append(ov);
      const bt = el('button', null, 'Black (master)'); bt.addEventListener('click', () => window.H.send({ type: 'Black', value: 2 }));
      body.append(bt);
    } else if (P === 'controllers') {
      body.append(el('h2', null, 'Controllers'), kv([
        ['MK2 controller block', m.present ? 'present' : 'not found'], ['MK2 USB', m.connected ? 'connected' : 'not connected'],
        ['MK2 map', m.map_verified ? 'verified map loaded' : m.present ? 'built-in default' : '–'], ['MK2 events dropped on GUI ring', m.dropped ?? '–'],
        ...(Array.isArray(S.weapons) ? S.weapons : []).map(w => [`slot ${w.slot} · ${w.name || w.weapon || '?'}`, w.state !== 'ok' ? 'other block version' : `${w.connected ? 'connected' : 'not connected'} · ${w.alive ? 'driver alive' : 'driver stopped'}${w.simulated ? ' · simulated' : ''}`])]));
      const b2 = el('button', null, 'Open the MK2 picture'); b2.addEventListener('click', () => window.setView('mk2')); body.append(b2);
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
