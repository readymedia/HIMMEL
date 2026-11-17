// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// M11 part 3: the reaction editor (gui.md §19 "the ~ ear on every knob tile", design/music.md §4,
// mockup docs/gui-mockups/v4/V4Audio). Contract: AUDIO D131, FxReact = command 40 + the reaction table
// in the state block v3 (docs/interfaces/command-protocol.md §4d, engine-state.md).
//   - Every FX knob 1-4 in the LAYER / DECK / MASTER FX tabs gets an ear "~": dim = no reaction,
//     lit = its effect's default reaction, accent = a custom one, struck = switched off. A thin
//     "knob ghost" on the slider shows the value the shader gets now (after PRESS and the reaction).
//   - Click the ear: the editor. Signal list with live meters, the input → output graph with the
//     live envelope, amount, mode, curve, attack / release, threshold, range, quantise + hold.
//     Every change is one FxReact Set (the whole reaction, read from the table and changed), Range is
//     its own op. DEFAULT / OFF / GOOD RANGE are the other ops.
// Without the react section (an engine before state v3) the ears stay hidden; nothing else changes.
'use strict';

(() => {
  const { el, send, sendT, toast } = window.H;
  const S = window.S;
  const SIGNALS = ['None', 'Level', 'Bass', 'Mid', 'High', 'Kick', 'Snare', 'Hat', 'Onset', 'Beat', 'Bar', 'Energy', 'Build', 'Drop'];
  const MODES = ['Add', 'Follow', 'Trigger'];
  const CURVES = ['Linear', 'Soft', 'Hard', 'Steps', 'Exp'];
  const QUANT = ['Off', 'Sixteenth', 'Eighth', 'Quarter', 'Half', 'Bar'];
  const QWORD = { Off: 'off', Sixteenth: '1/16', Eighth: '1/8', Quarter: '1/4', Half: '1/2', Bar: '1 bar' };
  const QBEATS = [0, 0.25, 0.5, 1, 2, 4];
  const MODE_HELP = { Add: 'ADD: on top of where the knob is', Follow: 'FOLLOW: the signal moves the knob through the range', Trigger: 'TRIGGER: fires on a rising edge over the threshold' };
  const SIG_HELP = { Level: 'loudness', Bass: 'low band', Mid: 'mid band', High: 'high band', Kick: 'kick drum hits', Snare: 'snare hits', Hat: 'hi-hat hits', Onset: 'any new sound', Beat: 'the beat clock (every beat)', Bar: 'the beat clock (every bar)', Energy: 'how busy the music is', Build: 'a build-up rising', Drop: 'the drop after a build' };

  // ---- where a knob's entry lives in the table (same order as the live state)
  function pick(tab, t, k, p) {
    if (!tab) return null;
    try {
      if (t.deck === 'master') return tab.master[k][p];
      if (t.layer === undefined) return tab.deck[t.deck][k][p];
      return tab.layer[t.deck][t.layer][k][p];
    } catch (e) { return null; }
  }
  const entry = (t, k, p) => pick(S.react, t, k, p);
  const live = (t, k, p) => pick(S.reactlive, t, k, p) || [0, 0];
  const has = () => !!S.react;
  const fxNow = (t, k) => {
    const L = S.live; if (!L) return null;
    if (t.deck === 'master') return L.master_fx[k];
    if (t.layer === undefined) return L.deck[t.deck].fx[k];
    return L.deck[t.deck].layer[t.layer].fx[k];
  };
  const tname = t => (t.deck === 'master' ? 'MASTER' : t.layer === undefined ? `DECK ${'AB'[t.deck]}` : `${'AB'[t.deck]}${t.layer + 1}`);

  // ---- the ear + ghost on a param row (app.js calls attach() for knobs 0..3)
  function attach(row, inp, k, p, target) {
    const ear = el('button', 'ear', '~');
    const ghost = el('i', 'kghost');
    row.classList.add('hasear');
    row.append(ear, ghost);
    ear.addEventListener('click', e => { e.stopPropagation(); open(target(), k, p); });
    return {
      update() {
        const ok = has();
        ear.classList.toggle('hidden', !ok); ghost.classList.toggle('hidden', !ok);
        if (!ok) return;
        const t = target(), e = entry(t, k, p), lv = live(t, k, p);
        const on = e && e.signal !== 'None' && e.source !== 'Off';
        ear.className = 'ear' + (e?.source === 'Off' ? ' off' : on ? (e.source === 'Custom' ? ' custom' : ' dflt') : '');
        ear.title = !e ? 'Reaction' : e.source === 'Off' ? 'Reaction switched off (click = edit)' : on
          ? `Reacts to ${e.signal.toUpperCase()} · ${e.mode.toUpperCase()} ${Math.round(e.amount * 100)}%${e.source === 'Default' ? ' (the effect\'s default)' : ''} · click = edit`
          : 'No reaction on this knob (click = give it one)';
        ear.style.setProperty('--env', on ? lv[0].toFixed(3) : 0);
        // The ghost: where the shader is now, over the slider.
        const show = on && Math.abs(lv[1] - +inp.value) > 0.004;
        ghost.classList.toggle('hidden', !show);
        if (show) ghost.style.left = (inp.offsetLeft + 6 + lv[1] * Math.max(0, inp.offsetWidth - 12)) + 'px';
      },
    };
  }

  // ================================================================ the editor
  const E = { box: null };
  function open(t, k, p) {
    close();
    const f = fxNow(t, k);
    if (!f || f.effect < 0) return toast('Put an effect in this slot first');
    const info = window.H.fxInfo(f.effect);
    E.t = t; E.k = k; E.p = p; E.effect = f.effect;
    const md = el('div', 'modal'), box = el('div', 'mbox rxbox');
    md.addEventListener('click', e => { if (e.target === md) close(); });
    md.append(box); document.body.append(md);
    E.md = md; E.box = box;
    const head = el('div', 'mhead');
    E.title = el('span', 'rxtitle', `REACTION · ${tname(t)} FX${k + 1} ${info?.name || ''} · knob ${p + 1} ${info?.params?.[p] || ''}`);
    E.src = el('span', 'chip', '');
    head.append(E.title, E.src, el('div', 'spacer'));
    const b = (txt, title, fn, cls) => { const x = el('button', cls || null, txt); x.title = title; x.addEventListener('click', fn); return x; };
    head.append(b('DEFAULT', 'Back to the effect\'s own reaction', () => op('Default')),
      b('OFF', 'No reaction on this knob, not even the default', () => op('Off')),
      b('✕', 'Close (Esc)', close));
    box.append(head);
    const body = el('div', 'rxbody');
    // left: the signals with live meters
    const sigs = el('div', 'rxsigs');
    sigs.append(el('div', 'lbl', 'SIGNAL'));
    E.sig = SIGNALS.map(s => {
      const r = el('button', 'rxsig'); r.dataset.s = s;
      const m = el('span', 'rxm'); const fill = el('i'); m.append(fill);
      r.append(el('span', null, s === 'None' ? 'none' : s.toUpperCase()), m);
      r.title = SIG_HELP[s] || 'no reaction';
      r.addEventListener('click', () => set({ signal: s }));
      sigs.append(r);
      return { s, r, fill };
    });
    // centre: the graph
    const mid = el('div', 'rxmid');
    E.cv = el('canvas', 'rxgraph'); E.cv.width = 320; E.cv.height = 200;
    E.note = el('div', 'dim rxnote', '');
    mid.append(el('div', 'lbl', 'INPUT → OUTPUT (the dot = now)'), E.cv, E.note);
    // right: the settings
    const ctl = el('div', 'rxctl');
    const chips = (label, list, words, key) => {
      const g = el('div', 'cg'); g.append(el('span', 'lbl', label));
      const bs = list.map(v => { const x = el('button', 'chip2', words ? words[v] : v.toUpperCase()); x.dataset.v = v; x.addEventListener('click', () => set({ [key]: v })); g.append(x); return x; });
      return { g, set(v) { bs.forEach(x => x.classList.toggle('on', x.dataset.v === v)); } };
    };
    const slider = (label, lo, hi, step, key, fmt, title) => {
      const w = el('label', 'rxsl'); w.title = title || '';
      const inp = el('input'); inp.type = 'range'; inp.min = lo; inp.max = hi; inp.step = step;
      const v = el('span', 'mono', '');
      w.append(el('span', 'lbl', label), inp, v);
      window.H.hold(inp);
      inp.addEventListener('input', () => { v.textContent = fmt(+inp.value); set({ [key]: +inp.value }, true); });
      return { w, set(x) { window.H.setVal(inp, x); if (performance.now() >= (inp._hold || 0)) v.textContent = fmt(x); } };
    };
    E.mode = chips('mode', MODES, null, 'mode');
    E.curve = chips('curve', CURVES, null, 'curve');
    E.amount = slider('amount', -1, 1, 0.01, 'amount', x => Math.round(x * 100) + '%', 'How far the signal moves the knob (negative = the other way)');
    E.attack = slider('attack', 0, 2000, 5, 'attack_ms', x => Math.round(x) + ' ms', 'How fast it rises');
    E.release = slider('release', 0, 2000, 5, 'release_ms', x => Math.round(x) + ' ms', 'How fast it falls back');
    E.thr = slider('threshold', 0, 1, 0.01, 'threshold', x => Math.round(x * 100) + '%', 'Below this the signal counts as 0 (TRIGGER fires when it crosses it)');
    E.quant = chips('quantise', QUANT, QWORD, 'quantise');
    E.hold = slider('hold', 0, 16, 1, 'hold', x => (x / 4) + ' beats', 'TRIGGER: hold the hit this long');
    // range: two sliders + "good range"
    const rg = el('div', 'rxrange');
    E.lo = el('input'); E.lo.type = 'range'; E.lo.min = 0; E.lo.max = 1; E.lo.step = 0.01;
    E.hi = el('input'); E.hi.type = 'range'; E.hi.min = 0; E.hi.max = 1; E.hi.step = 0.01;
    window.H.hold(E.lo); window.H.hold(E.hi);
    E.rv = el('span', 'mono', '');
    const sendRange = () => { E.rv.textContent = `${Math.round(E.lo.value * 100)} → ${Math.round(E.hi.value * 100)}%`; sendT('rxrange', { type: 'FxReact', ...E.t, slot: E.k, param: E.p, op: 'Range', f: [+E.lo.value, +E.hi.value] }); };
    E.lo.addEventListener('input', sendRange); E.hi.addEventListener('input', sendRange);
    rg.append(el('span', 'lbl', 'range'), E.lo, E.hi, E.rv,
      b('GOOD RANGE', 'Back to the parameter\'s good range (effect.json)', () => send({ type: 'FxReact', ...E.t, slot: E.k, param: E.p, op: 'Range', f: [-1, 0] }), 'chip2'));
    E.trig = el('div', 'rxtrig'); E.trig.append(E.quant.g, E.hold.w);
    ctl.append(E.mode.g, E.amount.w, E.curve.g, E.attack.w, E.release.w, E.thr.w, rg, E.trig);
    body.append(sigs, mid, ctl);
    box.append(body, el('div', 'mfoot dim', 'Changes apply at once (on PREVIEW first when the slot is on the PREVIEW deck). DEFAULT = the effect\'s own reaction from its effect.json. Save the project to keep them.'));
    E.key = '';
    window.addEventListener('keydown', esc);
    update();
  }
  function esc(e) { if (e.key === 'Escape') close(); }
  function close() { if (E.md) E.md.remove(); E.md = null; E.box = null; window.removeEventListener('keydown', esc); }
  function op(name) { send({ type: 'FxReact', ...E.t, slot: E.k, param: E.p, op: name }); }

  // One Set with the whole reaction: the current entry with `change` on top (the contract's "read, change, Set").
  function set(change, throttled) {
    const e = entry(E.t, E.k, E.p) || {};
    const cur = {
      signal: e.signal && e.signal !== 'None' ? e.signal : 'Bass', mode: e.mode || 'Add', curve: e.curve || 'Linear',
      amount: e.signal && e.signal !== 'None' && e.source !== 'Off' ? e.amount : 0.5, attack_ms: e.attack_ms ?? 10, release_ms: e.release_ms ?? 200,
      threshold: e.threshold ?? 0, quantise: QUANT[Math.max(0, QBEATS.indexOf(e.quantise_beats ?? 0))], hold: Math.round((e.hold_beats ?? 0) * 4),
    };
    Object.assign(cur, change);
    if (cur.signal === 'None') return op('Off');
    const cmd = { type: 'FxReact', ...E.t, slot: E.k, param: E.p, op: 'Set', signal: cur.signal, mode: cur.mode, curve: cur.curve,
      quantise: cur.quantise, hold: Math.max(0, Math.min(16, cur.hold | 0)), f: [cur.amount, cur.attack_ms, cur.release_ms, cur.threshold] };
    if (throttled) sendT('rxset', cmd); else send(cmd);
  }

  // ---- the editor follows the table (settings on gen, the meters / dot every frame)
  function update() {
    if (!E.box) return;
    const f = fxNow(E.t, E.k);
    if (!f || f.effect !== E.effect) { close(); return; }   // the effect changed under us
    const e = entry(E.t, E.k, E.p), lv = live(E.t, E.k, E.p);
    if (!e) { E.note.textContent = 'This engine has no reaction table (state v3).'; return; }
    E.src.textContent = e.source === 'Default' ? 'THE EFFECT\'S DEFAULT' : e.source === 'Custom' ? 'CUSTOM' : 'OFF';
    E.src.className = 'chip ' + (e.source === 'Custom' ? 'ok' : e.source === 'Off' ? 'warn' : '');
    const sigNow = e.source === 'Off' ? 'None' : e.signal;
    const a = S.audio?.present ? S.audio.sig : null;
    const beat = S.tr?.clock ? (window.H.beatNow ? window.H.beatNow() : 0) : 0;
    for (const x of E.sig) {
      x.r.classList.toggle('on', x.s === sigNow);
      let v = 0;
      if (x.s === 'Beat') v = 1 - (beat % 1);
      else if (x.s === 'Bar') v = 1 - ((beat % 4) / 4);
      else if (a && x.s !== 'None') v = a[x.s.toLowerCase()] ?? 0;
      x.fill.style.width = Math.round(Math.max(0, Math.min(1, v)) * 100) + '%';
    }
    E.mode.set(e.mode); E.curve.set(e.curve);
    E.amount.set(e.amount); E.attack.set(e.attack_ms); E.release.set(e.release_ms); E.thr.set(e.threshold);
    E.quant.set(QUANT[Math.max(0, QBEATS.indexOf(e.quantise_beats))] || 'Off'); E.hold.set(Math.round(e.hold_beats * 4));
    E.trig.classList.toggle('dimmed', e.mode !== 'Trigger');
    if (performance.now() >= Math.max(E.lo._hold || 0, E.hi._hold || 0)) {
      window.H.setVal(E.lo, e.range[0]); window.H.setVal(E.hi, e.range[1]);
      E.rv.textContent = `${Math.round(e.range[0] * 100)} → ${Math.round(e.range[1] * 100)}%${e.range_set ? '' : ' (good range)'}`;
    }
    E.note.textContent = sigNow === 'None' ? 'No reaction: pick a signal on the left.' : `${MODE_HELP[e.mode] || ''} · now: envelope ${Math.round(lv[0] * 100)}%, knob ${Math.round(lv[1] * 100)}%`;
    drawGraph(e, lv, sigNow !== 'None');
  }
  const shape = (c, x) => (c === 'Soft' ? Math.sqrt(x) : c === 'Hard' ? x * x : c === 'Steps' ? Math.floor(x * 4) / 4 : c === 'Exp' ? (Math.exp(3 * x) - 1) / (Math.exp(3) - 1) : x);
  function drawGraph(e, lv, on) {
    const c = E.cv, g = c.getContext('2d'), W = c.width, H = c.height, pad = 18;
    g.clearRect(0, 0, W, H);
    g.fillStyle = '#0d1015'; g.fillRect(0, 0, W, H);
    g.strokeStyle = '#2A313A'; g.lineWidth = 1;
    for (let i = 0; i <= 4; i++) { const x = pad + (W - 2 * pad) * i / 4, y = pad + (H - 2 * pad) * i / 4; g.beginPath(); g.moveTo(x, pad); g.lineTo(x, H - pad); g.moveTo(pad, y); g.lineTo(W - pad, y); g.stroke(); }
    const X = v => pad + v * (W - 2 * pad), Y = v => H - pad - v * (H - 2 * pad);
    // the range band
    const lo = Math.min(e.range[0], e.range[1]), hi = Math.max(e.range[0], e.range[1]);
    g.fillStyle = '#B78CFF18'; g.fillRect(pad, Y(hi), W - 2 * pad, Y(lo) - Y(hi));
    if (!on) return;
    // input (signal after threshold + curve) -> knob output (FOLLOW maps into the range; ADD / TRIGGER as an offset from the middle)
    const out = x => {
      const s = x <= e.threshold ? 0 : shape(e.curve, (x - e.threshold) / Math.max(1e-3, 1 - e.threshold));
      if (e.mode === 'Follow') return e.range[0] + (e.range[1] - e.range[0]) * s;
      if (e.mode === 'Trigger') return x > e.threshold ? 0.5 + e.amount * 0.5 : 0.5;
      return Math.max(0, Math.min(1, 0.5 + e.amount * s * (e.range[1] - e.range[0]) * 0.5));
    };
    g.strokeStyle = e.source === 'Custom' ? '#30D158' : '#E8ECF1'; g.lineWidth = 2; g.beginPath();
    for (let i = 0; i <= 100; i++) { const x = i / 100; const y = out(x); i ? g.lineTo(X(x), Y(y)) : g.moveTo(X(x), Y(y)); }
    g.stroke();
    if (e.threshold > 0) { g.strokeStyle = '#FFD60A88'; g.setLineDash([4, 4]); g.beginPath(); g.moveTo(X(e.threshold), pad); g.lineTo(X(e.threshold), H - pad); g.stroke(); g.setLineDash([]); }
    // the dot: the envelope now -> the knob now
    g.fillStyle = '#FF3B30'; g.beginPath(); g.arc(X(lv[0]), Y(lv[1]), 5, 0, 6.2832); g.fill();
    g.fillStyle = '#9AA4B2'; g.font = '10px Consolas, monospace';
    g.fillText('signal →', W - pad - 52, H - 4); g.save(); g.translate(10, pad + 46); g.rotate(-Math.PI / 2); g.fillText('knob →', 0, 0); g.restore();
  }

  window.Reacts = { attach, update, has };
})();
