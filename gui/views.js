// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The views around Perform (M11): the start wizard (gui.md §3, steps 1-4 against what 0.1 has),
// the MK2 picture with live feedback (gui.md §4.5) and the Settings view stub (gui.md §19).
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
    if (W.key === key) return;
    W.key = key;
    const body = $('wizBody'); body.textContent = '';
    body.append(el('h2', null, ['', 'Add clips', 'Choose your weapon', 'Set outputs', 'Play'][W.step]));
    ({ 1: step1, 2: step2, 3: step3, 4: step4 })[W.step](body);
    body.append(wizNav());
  }
  function stepKey(step) {
    const b = S.bridge, m = S.mk2, L = S.live;
    if (step === 1) return (S.cat?.clips.length ?? -1) + ',' + (S.cat?.effects.length ?? -1);
    if (step === 2) return [m?.present, m?.connected, m?.alive, (m?.pressed || []).join('+'), L?.producer?.[0]?.alive, L?.producer?.[1]?.alive].join(',');
    if (step === 3) return [b?.preview, b?.engine].join(',');
    return [b?.engine, S.cat?.clips.length, m?.connected, b?.fps ? Math.round(b.fps) : 0].join(',');
  }

  function step1(body) {
    const n = S.cat?.clips.length ?? 0, g = (S.cat?.effects || []).filter(e => e.kind === 'generator').length;
    const drop = el('div', 'drop');
    drop.append(el('div', 'big', 'Drop folders or clips here'),
      el('div', 'dim', 'Import (convert → analyse → tag → ready) comes with M7. For now the engine plays every HAP .mov in media/ (or --clip PATH). Make test clips with: python tools/make_test_clips.py'));
    drop.addEventListener('dragover', e => e.preventDefault());
    drop.addEventListener('drop', e => { e.preventDefault(); window.H.toast('Import is not built yet (M7). Put HAP .mov files in media/ and restart the renderer.'); });
    body.append(drop);
    const row = el('div', 'wrow');
    const list = el('div', 'wlist');
    (S.cat?.clips || []).slice(0, 40).forEach(c => {
      const it = el('div', 'wclip');
      const sw = el('i'); sw.style.background = window.H.rgb(c.thumb_rgb);
      it.append(sw, el('span', null, c.name.replace(/\.mov$/i, '')), el('span', 'dim mono', `${c.width}×${c.height} · ${c.seconds.toFixed(0)} s · ready`));
      list.append(it);
    });
    if (!n) list.append(el('div', 'dim', S.cat ? 'The engine has no clips. Generators still play (step 4 → GENERATORS).' : 'Waiting for the engine catalog…'));
    row.append(card(`${n} clips in the library`, list), card('Starter looks', `${g} generators are built in (Lines, Rings, Noise, Shapes, Solid, Ticker): they play on an empty layer, no clips needed.`));
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
    const sim = el('div', 'dim', 'No MK2? Start Himmel with --sim and use the simulator: python tools/mk2_live.py --sim');
    mk2.append(sim);
    const c1 = card('MAIN WEAPON · MK2', mk2, m.connected ? 'sel' : '');
    const nu = card('MAIN WEAPON · NuVJ', 'The NuVJ as main weapon comes with M10 (its driver already feeds the engine as a side arm).', 'off');
    const kb = card('KEYBOARD + MOUSE', 'Always there: this page plays the MK2 layout. Space = TAKE, Enter = CUT, T = tap.', L?.producer?.[1]?.alive ? 'sel' : '');
    row.append(c1, nu, kb);
    body.append(el('div', 'lbl', 'MAIN WEAPON (pick one)'), row);
    const arms = el('div', 'wrow');
    for (const [nm, txt] of [['Lightpad Block', 'TOUCH / MIX / SCRUB with M9'], ['LUMI Keys', 'keys + pressure (driver runs, mapping later)'], ['Ohm64 · APC20 · KF4', 'simulated only so far']])
      arms.append(card(nm, txt, 'off'));
    body.append(el('div', 'lbl', 'SIDE ARMS (add any)'), arms);
    const p0 = L?.producer?.[0]?.alive;
    body.append(el('div', 'loadout', 'Loadout: MK2' + (m.connected ? '' : ' (not connected)') + ' + keyboard / mouse' + (p0 ? ' · engine mapper alive' : '')));
    body.append(card('Sound in', 'Mic / line in / "sound on this PC", live meter and the three bands come with M8 (himmel_audio). Until then effects react to nothing (or --test-signal).', 'off'));
  }

  function step3(body) {
    const row = el('div', 'wrow');
    row.append(card('SCREENS', 'TVs, monitors, LED walls: the edges just meet, no mapping.', 'sel'), card('PROJECTORS', 'Keystone, warp, edge blend (later, §13).', 'off'));
    body.append(el('div', 'lbl', 'WHERE DOES THE PICTURE GO?'), row);
    const how = el('div', 'chips');
    for (const n of ['1', '2 side by side', '3 side by side']) how.append(el('span', 'chip2 off', n));
    const res = el('div', 'chips');
    for (const [n, t] of [['720p', ''], ['1080p', 'recommended'], ['1440p', 'big rigs'], ['4K', 'big rigs']]) {
      const c = el('span', 'chip2 off' + (n === '1080p' ? ' on' : ''), n + (t ? ' · ' + t : '')); res.append(c);
    }
    const hz = el('div', 'chips'); hz.append(el('span', 'chip2 on off', '60 Hz'), el('span', 'chip2 off', '50 Hz'));
    const box = el('div');
    box.append(el('div', 'lbl', 'HOW MANY'), how, el('div', 'lbl', 'RESOLUTION PER OUTPUT'), res, el('div', 'lbl', 'REFRESH'), hz,
      el('div', 'dim', 'Outputs, resolution presets, IDENTIFY and 50 / 60 Hz are set on the renderer command line for now (himmel_render --outputs N --size WxH --mode span|mirror); the GUI controls come with M6. The chips show the defaults.'));
    const ident = el('button', null, 'IDENTIFY'); ident.disabled = true; ident.title = 'Big numbers on every display (M6)';
    body.append(card('Outputs', box), ident);
    const b = S.bridge;
    body.append(el('div', 'dim', `Preview block: ${b?.preview || '–'} · engine: ${b?.engine || '–'}. The GUI screens are never show outputs.`));
  }

  function step4(body) {
    const b = S.bridge, m = S.mk2;
    const ok = b?.engine === 'ok';
    const n = S.cat?.clips.length ?? 0;
    body.append(el('div', 'health ' + (ok ? 'ok' : 'bad'),
      ok ? `All good: ${n} clips, ${m?.connected ? 'MK2 connected' : 'no MK2 (keyboard + mouse)'}, engine ${Math.round(b.fps)} fps.` : 'The engine is not running: start himmel.exe (or himmel_render.exe). This page waits for it.'));
    body.append(el('div', 'dim', 'Perform: PREVIEW (green) left, PROGRAM (red) right, TAKE in the middle. Click a layer = focus; the CLIP tab edits its transport, LAYER FX its effects.'));
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
    const b = S.bridge || {}, L = S.live, m = S.mk2 || {}, c = S.tr?.clock;
    const key = SET.page + JSON.stringify([b.engine, b.commands, b.preview, b.epoch, b.pushed, b.dropped, Math.round(b.fps || 0), m.connected, m.present, L?.producer, c?.bpm, c?.grid]);
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
      body.append(el('h2', null, 'Engine & outputs'), el('p', 'dim', 'The renderer is configured on its command line in 0.1. Output settings (count, resolution presets 720p / 1080p / 1440p / 4K, 50 / 60 Hz, IDENTIFY, black per output) come with M6.'),
        kv([['outputs', '--outputs N (1..6)'], ['size', '--size WxH'], ['mode', '--mode span | mirror'], ['fullscreen', '--fullscreen'], ['flash cap (D65, off by default)', '--flash-cap N'], ['test reaction signal', '--test-signal'], ['ticker text', '--ticker "TEXT"']]));
      const bt = el('button', null, 'Black (master)'); bt.addEventListener('click', () => window.H.send({ type: 'Black', value: 2 }));
      body.append(bt);
    } else if (P === 'controllers') {
      body.append(el('h2', null, 'Controllers'), kv([
        ['MK2 controller block', m.present ? 'present' : 'not found'], ['MK2 USB', m.connected ? 'connected' : 'not connected'],
        ['MK2 map', m.map_verified ? 'verified map loaded' : m.present ? 'built-in default' : '–'], ['MK2 events dropped on GUI ring', m.dropped ?? '–'],
        ['Lightpad / LUMI / NuVJ / APC20', 'drivers run under himmel.exe; their pages come with M9 / M10']]));
      const b2 = el('button', null, 'Open the MK2 picture'); b2.addEventListener('click', () => window.setView('mk2')); body.append(b2);
    } else if (P === 'audio') {
      body.append(el('h2', null, 'Audio'), el('p', 'dim', 'Sound in (mic / line in / this PC), auto gain, gate, the live meter, bands, onsets and the reaction editor come with M8 (himmel_audio). Every effect already has a default reaction (fx/<id>/effect.json); the effect browser shows what each one reacts to.'));
      if (c) body.append(kv([['clock', `${c.bpm} BPM (${c.source})`], ['grid', c.grid]]));
    } else if (P === 'gui') {
      body.append(el('h2', null, 'This GUI'), kv([['bridge', 'himmel_ui on http://127.0.0.1:8790 (only answers this PC)'], ['theme', 'dark (D28)'], ['keys', 'space = TAKE · enter = CUT · T = tap · Esc = cancel'], ['restart safety', 'killing himmel_ui or this page changes nothing on the outputs']]));
    } else if (P === 'remote') {
      body.append(el('h2', null, 'Remote / OSC'), el('p', 'dim', 'OSC in on UDP 7000 (basic set), allowed addresses and the last messages come with M13. Producer slot 2 is reserved for it.'));
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
