// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// GUI wave 7: the LIBRARY view, usable with hundreds of clips (gui.md §6 "Library", library-and-analysis.md).
//   top      ingest status (AUTO / BACKGROUND / FULL / HOLD, visible and switchable), disk free + library size,
//            import buttons, the progress bar of the current import (n of m, current file, ETA), CLEAR QUEUE
//   dock     the 8 layers (PREVIEW A–D, PROGRAM E–H) + both decks as drop targets: drag cards onto them
//   filters  search, state, kind, energy, fps, tags / moods, sort; multi-select with batch actions
//   grid     cards: thumbnail, analysis, which layers the clip is on; double-click = focused layer,
//            LOAD ▾ = the Load to… menu, drag = onto the dock / a strip in PERFORM
// Ingest commands go through POST /library/cmd (MEDIA, library-db.md §10). The view never writes the DB.
'use strict';

(() => {
  const $ = id => document.getElementById(id);
  const S = window.S;
  const { el, toast } = window.H;
  const F = () => window.Flows;
  const has = b => !!(b && b.present);
  const btn = (txt, title, fn, cls) => { const b = el('button', cls || null, txt); if (title) b.title = title; if (fn) b.addEventListener('click', fn); return b; };
  const ask = (...a) => window.Panels.W.ask(...a);
  const gb = n => (n >= 1e9 ? (n / 1e9).toFixed(n >= 1e10 ? 0 : 1) + ' GB' : n >= 1e6 ? Math.round(n / 1e6) + ' MB' : Math.round((n || 0) / 1e3) + ' kB');
  const dur = s => (s < 0 ? '–' : s < 60 ? Math.round(s) + ' s' : s < 3600 ? Math.round(s / 60) + ' min' : (s / 3600).toFixed(1) + ' h');
  const LOW_DISK = 10e9;   // GUESS: warn below 10 GB free (MEDIA's guard decides what the worker does)

  const LB = {
    built: false, key: '', sel: new Set(), last: -1, shown: [], limit: 300,
    q: '', state: 'all', kind: 'all', energy: [1, 5], fps: 'all', tag: 'all', sort: 'new',
    disk: null, diskAt: -1e9, seenReply: 0, relink: {}, batch: null, imports: {}, opts: { height: 0, hapq: false, skip_known: false, conform_hz: 0 },
  };

  // ------------------------------------------------------------------ ingest commands
  async function ingest(cmd) {
    try {
      const r = await fetch('/library/cmd', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(cmd) });
      const j = await r.json();
      if (!j.ok) toast(j.error || 'refused', 'bad');
      else if (cmd.type === 'Relink' && cmd.clip) LB.relink[j.seq] = cmd;
      return j;
    } catch (e) { toast('himmel_ui is not reachable', 'bad'); return { ok: false }; }
  }
  const ingestOk = () => { const l = S.library; return has(l) && l.alive && l.writable !== false && !!l.cmd; };
  const supports = type => (S.names?.panels?.ingest || []).includes(type);

  // ------------------------------------------------------------------ disk (GET /library/disk; MEDIA's block fields win)
  async function loadDisk() {
    if (performance.now() - LB.diskAt < 5000) return;
    LB.diskAt = performance.now();
    try { const r = await fetch('/library/disk', { cache: 'no-store' }); LB.disk = await r.json(); window.H.invalidate(); } catch (e) { /* offline */ }
  }
  function diskInfo() {
    const l = S.library, d = LB.disk || {};
    const dk = has(l) && l.disk && l.disk.total ? l.disk : null;   // library block v3 (MEDIA wave 7, §11.1)
    return {
      free: dk ? dk.free : d.free, total: dk ? dk.total : d.total, lib: dk ? dk.library_bytes : d.library_bytes,
      guard: dk ? (l.disk_low ? 'low' : 'ok') : null, reserve: dk ? dk.reserve : null, queue: dk ? dk.queue_bytes : 0,
      root: l?.root_info?.path || l?.root || d.root || '',
    };
  }

  // ------------------------------------------------------------------ progress (n of m, current file, ETA)
  // MEDIA's block fields win when present (batch_done / batch_total / eta_s). Otherwise the page counts:
  // a batch starts when the queue goes from empty to busy; done = ready + failed since then.
  function progress(l) {
    if (!has(l)) return null;
    // The worker may hold a job before it counts it as working: count the job too.
    const jobOn = !!(l.job?.name && !/^(ready|failed|done|removed)?$/i.test(l.job.stage || ''));
    const busy = l.queued + Math.max(l.working, jobOn ? 1 : 0);
    const fin = l.ready_total + l.failed_total;
    const pg = l.progress;
    if (pg && pg.total > 0) {
      return { done: pg.done, total: pg.total, eta: pg.eta_s, busy: pg.done < pg.total || !!pg.file, file: pg.file, fileP: pg.file_progress, error: pg.error, v3: true };
    }
    if (busy > 0 && !LB.batch) LB.batch = { start: fin, t0: performance.now(), lastFin: fin, lastAt: performance.now() };
    if (!LB.batch) return null;
    const b = LB.batch;
    if (fin !== b.lastFin) { b.lastFin = fin; b.lastAt = performance.now(); }
    const done = Math.max(0, fin - b.start), total = done + busy;
    if (busy === 0) { const out = { done, total, eta: 0, busy: false }; if (performance.now() - b.lastAt > 8000) LB.batch = null; return out; }
    const per = done > 0 ? (b.lastAt - b.t0) / 1000 / done : -1;
    const eta = per > 0 ? Math.max(0, busy * per - (performance.now() - b.lastAt) / 1000 * (l.working ? 1 : 0)) : -1;
    return { done, total, eta, busy: true };
  }

  // ------------------------------------------------------------------ build
  function build() {
    const top = $('libHead'); top.textContent = '';
    LB.status = el('div', 'lbstatus2');
    LB.modeBox = el('div', 'cg lbmode');
    LB.modeBox.title = 'How hard the import works. AUTO = full speed with no show, in the background while a show runs. BACKGROUND = always gentle. FULL = full speed even during a show (not show-safe). HOLD = start nothing new and pause the running file.';
    LB.modeBox.append(el('span', 'lbl', 'import'));
    LB.modeBtns = ['Auto', 'Background', 'Full', 'Hold'].map(m => {
      const b = btn(m.toUpperCase(), { Auto: 'AUTO: full speed with no show, gentle while a show runs (default)', Background: 'BACKGROUND: always gentle', Full: 'FULL: full speed even while a show runs (may cost frames)', Hold: 'HOLD: start nothing new; the running file pauses' }[m], () => {
        if (m === 'Full' && !confirm('FULL speed even while a show runs: the import may cost frames on the outputs. Go on?')) return;
        ingest({ type: 'Mode', mode: m });
      }, 'chip2');
      b.dataset.m = m; LB.modeBox.append(b); return b;
    });
    LB.disk = null;
    LB.diskEl = el('div', 'lbdisk');
    LB.acts = el('div', 'lbacts');
    LB.acts.append(
      btn('+ IMPORT FOLDER…', 'Pick a folder: every video, picture and GIF in it (and its subfolders) is imported. The originals stay where they are.', () => importDialog('folder'), 'primary'),
      btn('+ IMPORT FILES…', 'Pick single files in a folder to import', () => importDialog('files')),
      btn('OPEN DROP FOLDER', 'Open the library\'s originals folder in Explorer: files you copy there are imported by themselves', () => openOriginals()),
      btn('RESCAN', 'Scan the watch folders now and check that every original is still there', () => ingest({ type: 'Rescan', clip: 0 })),
      btn('RETRY FAILED', 'Queue every failed clip again', () => ingest({ type: 'Retry', clip: 0 })),
      btn('MOVE LIBRARY…', 'Move the library (its HAP copies, thumbnails and database) to another folder or disk', () => moveDialog()),
      btn('FIND MISSING…', 'Search a folder for clips whose original file moved (matched by name, then size and content)', async () => {
        const d = await pathDialog('folder', 'FIND MISSING ORIGINALS · choose the folder to search');
        if (d) ingest({ type: 'Relink', clip: 0, path: d.path });
      }));
    LB.prog = el('div', 'lbprog hidden');
    LB.progBar = el('div', 'lbpbar'); LB.progFill = el('i'); LB.progBar.append(LB.progFill);
    LB.progTxt = el('div', 'lbptxt');
    LB.clear = btn('CLEAR QUEUE', 'Drop every file that waits in the import queue (the running file finishes; nothing already imported is touched)', async () => {
      const l = S.library;
      if (!confirm(`Clear the import queue?\n${l?.queued || 0} waiting files are dropped. Clips that are already in the library stay.`)) return;
      ingest({ type: 'ClearQueue' });
    }, 'bad');
    LB.hold = btn('PAUSE', 'Pause the import (HOLD); press again to resume (AUTO)', () => ingest({ type: 'Mode', mode: S.library?.mode === 'Hold' ? 'Auto' : 'Hold' }));
    LB.prog.append(LB.progTxt, LB.progBar, LB.hold, LB.clear);
    LB.reply = el('div', 'dim lbreply');
    top.append(LB.status, LB.acts, LB.prog, LB.reply);

    // The layer dock: drop targets for cards (and a click target for the selection).
    LB.dock = el('div', 'lbdock');
    top.after(LB.dock);

    // Filters.
    LB.filters = el('div', 'lbfilters');
    const q = el('input'); q.type = 'search'; q.placeholder = 'search: name, tag, mood, loop style, folder'; q.autocomplete = 'off';
    q.addEventListener('input', () => { LB.q = q.value.trim().toLowerCase(); LB.key = ''; LB.limit = 300; window.H.invalidate(); });
    q.addEventListener('keydown', e => e.stopPropagation());
    const sel = (title, opts, key) => {
      const s = el('select'); s.title = title;
      for (const [v, t] of opts) { const o = el('option', null, t); o.value = v; s.append(o); }
      s.value = LB[key];
      s.addEventListener('change', () => { LB[key] = s.value; LB.key = ''; LB.limit = 300; window.H.invalidate(); });
      return s;
    };
    LB.tagSel = sel('Tag, mood or loop style', [['all', 'any tag']], 'tag');
    const en = el('div', 'cg lben'); en.title = 'Energy 1 (calm) … 5 (wild), from the analysis. Click a number = only that energy; click it again = all.';
    en.append(el('span', 'lbl', 'energy'));
    LB.enBtns = [1, 2, 3, 4, 5].map(k => {
      const b = btn(String(k), `Energy ${k}`, () => {
        LB.energy = LB.energy[0] === k && LB.energy[1] === k ? [1, 5] : [k, k];
        LB.key = ''; window.H.invalidate();
      }, 'chip2');
      en.append(b); return b;
    });
    LB.filters.append(q,
      sel('Import state', [['all', 'all states'], ['ready', 'ready'], ['working', 'importing'], ['failed', 'failed'], ['missing', 'original missing'], ['oncatalog', 'playable'], ['onlayer', 'on a layer now']], 'state'),
      sel('Kind', [['all', 'all kinds'], ['video', 'video'], ['still', 'still'], ['animation', 'animation'], ['sequence', 'sequence'], ['vector', 'vector']], 'kind'),
      en,
      sel('Frame rate', [['all', 'any fps'], ['le25', '≤ 25 fps'], ['30', '29.97 / 30'], ['50', '50'], ['60', '59.94 / 60'], ['still', 'stills']], 'fps'),
      LB.tagSel,
      sel('Sort', [['new', 'newest first'], ['old', 'oldest first'], ['name', 'name A–Z'], ['energy', 'energy ↓'], ['calm', 'energy ↑'], ['long', 'longest'], ['short', 'shortest'], ['fps', 'fps ↓'], ['state', 'state']], 'sort'));
    LB.bulk = el('div', 'lbbulk');
    LB.filters.after(LB.bulk);
    LB.dock.after(LB.filters);
    LB.filters.after(LB.bulk);

    // The page as a drop zone for files from Explorer: browsers never tell us the path, so explain.
    const page = $('library');
    page.addEventListener('dragover', e => { if (e.dataTransfer.types.includes('Files')) { e.preventDefault(); page.classList.add('filedrag'); } });
    page.addEventListener('dragleave', e => { if (e.target === page || !page.contains(e.relatedTarget)) page.classList.remove('filedrag'); });
    page.addEventListener('drop', e => {
      if (!e.dataTransfer.types.includes('Files')) return;
      e.preventDefault(); page.classList.remove('filedrag');
      const names = [...e.dataTransfer.files].map(f => f.name);
      fileDropHelp(names);
    });
    const ov = el('div', 'lbdropov', 'Drop files here: Himmel will show you how to import them');
    page.append(ov);
    LB.built = true;
  }

  function fileDropHelp(names) {
    const md = el('div', 'modal'), box = el('div', 'mbox askbox');
    const close = () => md.remove();
    box.append(el('div', 'lbl', 'IMPORT DROPPED FILES'),
      el('div', null, `${names.length} file${names.length === 1 ? '' : 's'}: ${names.slice(0, 4).join(', ')}${names.length > 4 ? ' …' : ''}`),
      el('div', 'dim', 'A web page is never told where a dropped file lives on your PC, so Himmel cannot pick it up from here. Two ways that work:'),
      el('div', null, '1. IMPORT FOLDER… / IMPORT FILES… and choose the folder the files are in.'),
      el('div', null, '2. OPEN DROP FOLDER, and drop the files into that Explorer window. Himmel imports them by itself.'));
    const row = el('div', 'askrow');
    row.append(el('div', 'spacer'), btn('Close', null, close), btn('OPEN DROP FOLDER', 'Open the library\'s originals folder in Explorer', () => { close(); openOriginals(); }), btn('IMPORT FOLDER…', null, () => { close(); importDialog('folder'); }, 'primary'));
    box.append(row); md.append(box); document.body.append(md);
    md.addEventListener('click', e => { if (e.target === md) close(); });
  }
  async function openOriginals() {
    try {
      const r = await fetch('/library/open-originals', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' });
      const j = await r.json();
      if (!j.ok) toast(j.error || 'cannot open the folder', 'bad'); else toast('Explorer: ' + j.path);
    } catch (e) { toast('himmel_ui is not reachable', 'bad'); }
  }

  // ------------------------------------------------------------------ the path dialog (GET /fs/list)
  // mode 'folder' = choose a folder; 'files' = tick files in a folder. Resolves {path, files[], watch} or null.
  function pathDialog(mode, title) {
    return new Promise(res => {
      const md = el('div', 'modal'), box = el('div', 'mbox fsbox');
      const head = el('div', 'mhead'); head.append(el('span', 'lbl', title));
      const pathIn = el('input'); pathIn.type = 'text'; pathIn.spellcheck = false; pathIn.placeholder = 'a folder, e.g. D:\\Clips (Enter = go)';
      pathIn.title = 'Type or paste a folder path and press Enter';
      const up = btn('↑ UP', 'The folder above', () => go(cur.parent ?? ''));
      const home = btn('DRIVES', 'Drives and places', () => go(''));
      const bar = el('div', 'fsbar'); bar.append(home, up, pathIn);
      const list = el('div', 'fslist');
      const info = el('div', 'fsinfo dim');
      const watch = el('label', 'fswatch'); const wcb = el('input'); wcb.type = 'checkbox';
      watch.append(wcb, el('span', null, ' keep watching this folder (new files are imported by themselves)'));
      watch.title = 'Stored in the library: the folder is checked every 2 s, also after a restart';
      const ok = btn(mode === 'folder' ? 'IMPORT THIS FOLDER' : 'IMPORT TICKED FILES', null, () => done(), 'primary');
      if (title.startsWith('FIND')) { ok.textContent = 'SEARCH THIS FOLDER'; watch.classList.add('hidden'); }
      const cancel = btn('Cancel', null, () => finish(null));
      const foot = el('div', 'askrow'); foot.append(watch, el('div', 'spacer'), cancel, ok);
      // Import options (library block v3, §11.4): size, codec, skip known, frame-rate conform, estimate.
      const opts = el('div', 'fsopts');
      const isImport = !title.startsWith('FIND') && !title.startsWith('MOVE');
      if (isImport && supports('ClearQueue')) {
        const o = LB.opts;
        const sz = el('select'); sz.title = 'Keep the source size, or make big clips smaller (saves disk: 720p is about half of 1080p)';
        for (const [v, t] of [[0, 'keep size'], [1080, 'max 1080p'], [720, 'max 720p (half the disk)']]) { const x = el('option', null, t); x.value = v; sz.append(x); }
        sz.value = o.height; sz.addEventListener('change', () => { o.height = +sz.value; });
        const q = el('label', 'fswatch'); const qc = el('input'); qc.type = 'checkbox'; qc.checked = o.hapq; qc.addEventListener('change', () => { o.hapq = qc.checked; });
        q.append(qc, el('span', null, ' HAP Q')); q.title = 'Better colour, about 2× the disk';
        const k = el('label', 'fswatch'); const kc = el('input'); kc.type = 'checkbox'; kc.checked = o.skip_known; kc.addEventListener('change', () => { o.skip_known = kc.checked; });
        k.append(kc, el('span', null, ' skip clips I removed')); k.title = 'Content already in the library is skipped, removed clips too';
        const cf = el('select'); cf.title = 'Conform 24 / 25 fps clips to the refresh (smooth, about 2.5× the disk). Off = as shot.';
        for (const [v, t] of [[0, 'fps as shot'], [50, 'conform to 50'], [60, 'conform to 60']]) { const x = el('option', null, t); x.value = v; cf.append(x); }
        cf.value = o.conform_hz; cf.addEventListener('change', () => { o.conform_hz = +cf.value; });
        const est = btn('ESTIMATE DISK', 'How much space this import needs (nothing is imported)', async () => {
          if (!cur.path) return;
          info.textContent = 'Estimating…';
          LB.estimate = null; LB.estWait = true;
          const base = mode === 'files' && ticked.size ? { files: [...ticked].map(n => cur.path.replace(/[\\/]$/, '') + '\\' + n) } : { path: cur.path };
          const j = await sendImport(base, { dry_run: true });
          if (!j.ok) { info.textContent = j.error || 'refused'; return; }
          const t0 = performance.now();
          const poll = () => { if (LB.estimate) { info.textContent = '≈ ' + LB.estimate.text; info.classList.toggle('bad', !!(LB.estimate.pl && !LB.estimate.pl.fits)); } else if (performance.now() - t0 < 20000 && md.isConnected) setTimeout(poll, 300); else info.textContent = 'No estimate (the worker is busy); try again.'; };
          poll();
        });
        opts.append(el('span', 'lbl', 'options'), sz, q, k, cf, est);
      }
      box.append(head, bar, list, info, opts, foot); md.append(box); document.body.append(md);
      md.addEventListener('click', e => { if (e.target === md) finish(null); });
      pathIn.addEventListener('keydown', e => { e.stopPropagation(); if (e.key === 'Enter') go(pathIn.value.trim()); if (e.key === 'Escape') finish(null); });
      let cur = { path: '' };
      const ticked = new Set();
      function finish(v) { md.remove(); res(v); }
      function done() {
        if (!cur.path) { toast('Open a folder first', 'bad'); return; }
        if (mode === 'files' && !ticked.size) { toast('Tick at least one file (or use IMPORT FOLDER…)', 'bad'); return; }
        finish({ path: cur.path, files: [...ticked].map(n => cur.path.replace(/[\\/]$/, '') + '\\' + n), watch: wcb.checked });
      }
      async function go(p) {
        list.textContent = ''; list.append(el('div', 'dim', 'Reading…'));
        let j;
        try { j = await (await fetch('/fs/list?path=' + encodeURIComponent(p))).json(); } catch (e) { j = { ok: false, error: 'himmel_ui is not reachable' }; }
        list.textContent = ''; ticked.clear();
        if (!j.ok) { list.append(el('div', 'bad', j.error || 'cannot read that folder')); return; }
        cur = j; pathIn.value = j.path || '';
        up.disabled = !j.path;
        ok.disabled = !j.path;
        if (!j.path) {
          for (const d of j.places || []) list.append(row('★', d.name, d.path, () => go(d.path)));
          for (const d of j.drives || []) list.append(row('⛁', d.name + (d.kind !== 'fixed' ? ` (${d.kind})` : ''), d.free != null ? `${gb(d.free)} free of ${gb(d.total)}` : '', () => go(d.path)));
          info.textContent = 'Choose a drive or a place, then a folder.';
          return;
        }
        for (const d of j.dirs) list.append(row('▸', d.name, '', () => go(j.path.replace(/[\\/]$/, '') + '\\' + d.name)));
        for (const f of j.files) {
          const r = el('label', 'fsrow file' + (mode === 'files' ? '' : ' nocb'));
          const cb = el('input'); cb.type = 'checkbox'; cb.classList.toggle('hidden', mode !== 'files');
          cb.addEventListener('change', () => { if (cb.checked) ticked.add(f.name); else ticked.delete(f.name); });
          r.append(cb, el('span', 'fsic', { video: '▶', still: '▣', animation: '◎', vector: '◇' }[f.kind] || '·'), el('span', 'fsnm', f.name), el('span', 'dim mono', gb(f.size)));
          list.append(r);
        }
        if (!j.dirs.length && !j.files.length) list.append(el('div', 'dim', 'No subfolders and nothing importable here.'));
        const need = j.importable_bytes || 0;
        let txt = `${j.importable} importable file${j.importable === 1 ? '' : 's'} here (${gb(need)}; subfolders are imported too).`;
        if (j.free != null) txt += ` ${gb(j.free)} free on this drive.`;
        const di = diskInfo();
        // GUESS: HAP copies are often 3–8× the size of an H.264 source; warn with 5×.
        if (di.free != null && need * 5 > di.free) txt += ` ⚠ The HAP copies may need about ${gb(need * 5)}, and the library drive has ${gb(di.free)} free.`;
        info.textContent = txt;
        info.classList.toggle('bad', di.free != null && need * 5 > di.free);
      }
      function row(ic, name, sub, fn) {
        const r = el('button', 'fsrow'); r.append(el('span', 'fsic', ic), el('span', 'fsnm', name), el('span', 'dim mono', sub));
        r.addEventListener('click', fn); return r;
      }
      go(LB.lastDir || '');
    });
  }
  async function importDialog(mode) {
    if (!ingestOk()) { toast(has(S.library) ? 'The import worker does not answer right now.' : 'The import worker is not running (start Himmel with himmel.exe).', 'bad'); return; }
    const di = diskInfo();
    if (di.guard === 'full') { toast('The library drive is full: free some space first (or remove clips).', 'bad'); return; }
    const r = await pathDialog(mode, mode === 'folder' ? 'IMPORT A FOLDER' : 'IMPORT FILES');
    if (!r) return;
    LB.lastDir = r.path;
    sendImport(mode === 'folder' ? { path: r.path, watch: r.watch } : { files: r.files });
  }
  // One Import with the dialog's options (v3 §11.4). A refusal (NoSpace) offers the clips that fit.
  async function sendImport(base, extra) {
    const o = LB.opts, cmd = { type: 'Import', ...base, ...extra };
    if (o.height) cmd.height = o.height;
    if (o.hapq) cmd.hapq = true;
    if (o.skip_known) cmd.skip_known = true;
    if (o.conform_hz) cmd.conform_hz = o.conform_hz;
    const j = await ingest(cmd);
    if (j.ok) LB.imports[j.seq] = cmd;
    return j;
  }
  function afterImport(cmd, r) {
    const plan = S.library?.plan;
    const pl = plan && plan.mine && plan.seq === r.seq ? plan : null;
    if (cmd.dry_run) { LB.estimate = { text: pl?.message || r.message, at: performance.now(), pl }; window.H.invalidate(); return; }
    if (r.status !== 'NoSpace') return;
    const md = el('div', 'modal'), box = el('div', 'mbox askbox');
    const close = () => md.remove();
    box.append(el('div', 'lbl', 'NOT ENOUGH DISK SPACE'),
      el('div', null, pl?.message || r.message),
      el('div', 'dim', 'Nothing was imported. Himmel keeps a reserve free so the show and Windows keep running. You can import only the clips that fit, choose a smaller size (720p) in the import options, move the library to a bigger disk, or free some space.'));
    const row = el('div', 'askrow');
    row.append(el('div', 'spacer'), btn('Cancel', null, close));
    if (pl && pl.fit_clips > 0) row.append(btn(`IMPORT THE ${pl.fit_clips} THAT FIT`, 'Queue the clips that fit above the reserve, in the order found', () => { close(); const c = { ...cmd }; delete c.dry_run; sendImport({}, { ...c, fit: true }); }, 'primary'));
    row.append(btn('MOVE LIBRARY…', 'Move the library to another folder or disk', () => { close(); moveDialog(); }));
    box.append(row); md.append(box); document.body.append(md);
    md.addEventListener('click', e => { if (e.target === md) close(); });
  }
  // Library (69): move the library to another folder (v3 §11.5); the progress shows in the status line.
  async function moveDialog() {
    if (!supports('Library')) { toast('This import worker cannot move the library (needs library block v3).', 'bad'); return; }
    const r = await pathDialog('folder', 'MOVE THE LIBRARY · choose an empty folder (or a new one on another disk)');
    if (!r) return;
    const d = diskInfo();
    if (!confirm(`Move the library (${gb(d.lib || 0)}) to\n${r.path}?\n\nThe import pauses while it copies. The old copy is deleted after the next restart of Himmel. Clips keep playing.`)) return;
    ingest({ type: 'Library', path: r.path, move: true });
  }

  // ------------------------------------------------------------------ filters
  const FPS = { le25: c => c.fps > 0 && c.fps <= 25.5, 30: c => c.fps > 29 && c.fps < 31, 50: c => c.fps > 49 && c.fps < 51, 60: c => c.fps > 59 && c.fps < 61, still: c => c.kind === 'still' || c.kind === 'vector' };
  const words = c => [c.name, c.kind, c.source, c.analysis?.loop_style, ...(c.tags || []), ...Object.entries(c.analysis?.mood || {}).filter(([, v]) => v > 0.5).map(([k]) => k)].join(' ').toLowerCase();
  function want(c) {
    const st = c.state;
    if (LB.state === 'ready' && st !== 'ready') return false;
    if (LB.state === 'failed' && st !== 'failed') return false;
    if (LB.state === 'working' && (st === 'ready' || st === 'failed')) return false;
    if (LB.state === 'missing' && !c.source_missing) return false;
    if (LB.state === 'oncatalog' && F().catOf(c) < 0) return false;
    if (LB.state === 'onlayer' && !F().where(F().catOf(c)).length) return false;
    if (LB.kind !== 'all' && c.kind !== LB.kind) return false;
    if (LB.fps !== 'all' && !FPS[LB.fps](c)) return false;
    if ((LB.energy[0] > 1 || LB.energy[1] < 5) && !(c.analysis && c.analysis.energy >= LB.energy[0] && c.analysis.energy <= LB.energy[1])) return false;
    if (LB.tag !== 'all') {
      const [k, v] = LB.tag.split(':');
      if (k === 'tag' && !(c.tags || []).includes(v)) return false;
      if (k === 'mood' && !((c.analysis?.mood?.[v] || 0) > 0.5)) return false;
      if (k === 'loop' && c.analysis?.loop_style !== v) return false;
    }
    if (LB.q && !words(c).includes(LB.q)) return false;
    return true;
  }
  const SORTS = {
    new: (a, b) => b.id - a.id, old: (a, b) => a.id - b.id, name: (a, b) => a.name.localeCompare(b.name),
    energy: (a, b) => (b.analysis?.energy || 0) - (a.analysis?.energy || 0), calm: (a, b) => (a.analysis?.energy || 9) - (b.analysis?.energy || 9),
    long: (a, b) => b.seconds - a.seconds, short: (a, b) => a.seconds - b.seconds, fps: (a, b) => b.fps - a.fps,
    state: (a, b) => a.state.localeCompare(b.state) || b.id - a.id,
  };
  function fillTags(clips) {
    const tags = new Set(), loops = new Set();
    for (const c of clips) { (c.tags || []).forEach(t => tags.add(t)); if (c.analysis?.loop_style) loops.add(c.analysis.loop_style); }
    const key = [...tags].sort().join(',') + '|' + [...loops].sort().join(',');
    if (LB.tagKey === key) return;
    LB.tagKey = key;
    const s = LB.tagSel, v = s.value; s.textContent = '';
    const add = (val, txt) => { const o = el('option', null, txt); o.value = val; s.append(o); };
    add('all', 'any tag / mood');
    for (const m of ['chill', 'groove', 'techno', 'crazy']) add('mood:' + m, 'mood: ' + m);
    for (const l of [...loops].sort()) add('loop:' + l, 'loop: ' + l);
    for (const t of [...tags].sort()) add('tag:' + t, '#' + t);
    s.value = [...s.options].some(o => o.value === v) ? v : 'all';
  }

  // ------------------------------------------------------------------ render
  function renderStatus() {
    const l = S.library;
    // mode chips
    for (const b of LB.modeBtns) {
      const m = has(l) ? l.mode : '';
      // v3: the stored mode (it survives a restart); older workers: guess it from the running mode.
      const on = l?.stored_mode ? l.stored_mode === b.dataset.m : b.dataset.m === 'Hold' ? m === 'Hold' : b.dataset.m === 'Auto' ? (m === 'Idle' || m === 'Background') : m === b.dataset.m;
      b.classList.toggle('on', !!on);
      b.disabled = !ingestOk();
    }
    const s = LB.status; s.textContent = '';
    if (has(l)) {
      const word = !l.alive ? 'IMPORT WORKER NOT ANSWERING' : l.mode === 'Hold' ? 'IMPORT ON HOLD' : l.paused ? 'IMPORT PAUSED · the show needs the PC' : (l.queued + l.working) ? `IMPORTING · ${String(l.mode).toUpperCase()}` : 'IMPORT IDLE';
      s.append(el('span', 'chip ' + (!l.alive ? 'bad' : l.mode === 'Hold' || l.paused ? 'warn' : 'ok'), word));
      s.append(el('span', 'mono dim', `${l.ready_total} ready · ${l.queued} waiting · ${l.working} working · ${l.failed_total} failed`));
      if (l.catalog_full) s.append(el('span', 'chip warn', `CATALOG FULL · ${l.catalog_full} ready clips cannot play yet (256 max in 0.1)`));
    } else {
      s.append(el('span', 'chip warn', l?.state === 'version' ? 'IMPORT WORKER: other version' : 'IMPORT WORKER NOT RUNNING'),
        el('span', 'dim', 'himmel.exe starts it. Without it you can still browse the library, but not import.'));
    }
    s.append(LB.modeBox);
    const ri = l?.root_info;
    if (ri && ri.move_state && ri.move_state !== 'None' && ri.move_state !== 'Done')
      s.append(el('span', 'chip ' + (ri.move_state === 'Failed' ? 'bad' : 'warn'), `LIBRARY MOVE · ${ri.move_state.toUpperCase()} ${Math.round(ri.move_progress * 100)}% · ${ri.move_message}`));
    // disk line
    loadDisk();
    const d = diskInfo(), de = LB.diskEl; de.textContent = '';
    if (d.free != null) {
      const low = d.guard ? d.guard !== 'ok' : d.free < LOW_DISK;
      const bar = el('span', 'lbdbar'); const used = el('i'); used.style.width = (d.total ? Math.min(100, (1 - d.free / d.total) * 100) : 0) + '%'; bar.append(used);
      de.append(el('span', 'lbl', 'disk'), bar, el('span', low ? 'bad' : 'dim', `${gb(d.free)} free${d.total ? ' of ' + gb(d.total) : ''}`),
        el('span', 'dim', `· library ${gb(d.lib || 0)}`));
      if (low) de.append(el('span', 'chip ' + (d.guard === 'full' ? 'bad' : 'warn'), d.guard === 'full' ? 'DISK FULL · import stopped' : 'DISK LOW'));
      de.title = `${d.root}\nThe library keeps a HAP copy of every clip (often several times the original's size). The originals are never copied or touched.` + (d.reserve != null ? `\nThe import stops when less than ${gb(d.reserve)} would be left.` : '');
      de.classList.toggle('low', low);
    } else de.append(el('span', 'dim', 'disk: –'));
    s.append(de);
    for (const b of LB.acts.querySelectorAll('button')) if (b.textContent !== 'OPEN DROP FOLDER') b.disabled = !ingestOk();
    LB.acts.title = ingestOk() ? '' : 'The import worker is not running or does not answer.';
    // progress
    const p = progress(l);
    LB.prog.classList.toggle('hidden', !p);
    if (p) {
      const job = l.job || {};
      const pct = p.total ? (p.done + (p.busy ? Math.min(0.99, (p.v3 ? p.fileP : job.progress) || 0) : 0)) / p.total : 1;
      LB.progFill.style.width = (pct * 100).toFixed(1) + '%';
      LB.progTxt.textContent = '';
      LB.progTxt.append(el('b', null, p.busy ? `Importing ${Math.min(p.total, p.done + 1)} of ${p.total}` : `Imported ${p.done} of ${p.total}`));
      if (p.busy && (p.file || job.name)) LB.progTxt.append(el('span', null, ` · ${p.file || job.name} · ${job.stage || ''} ${Math.round(((p.v3 ? p.fileP : job.progress) || 0) * 100)}%`));
      if (p.busy) LB.progTxt.append(el('span', 'dim', ` · ${p.eta < 0 ? 'estimating time…' : 'about ' + dur(p.eta) + ' left'}`));
      if (l.paused) LB.progTxt.append(el('span', 'warn', ' · paused while the show is busy'));
      if (p.error || job.last_error) LB.progTxt.append(el('span', 'bad', ' · ⚠ ' + (p.error || job.last_error)));
      if (l.disk_low) LB.progTxt.append(el('span', 'bad', ' · DISK LOW: the import waits (free space, remove clips, or CLEAR QUEUE)'));
      LB.hold.textContent = l.mode === 'Hold' ? 'RESUME' : 'PAUSE';
      LB.hold.disabled = !ingestOk();
      LB.clear.classList.toggle('hidden', !supports('ClearQueue'));
      LB.clear.disabled = !ingestOk() || !l.queued;
    }
    // replies → toasts (once each), the newest under the bar
    const cmd = l?.cmd;
    if (cmd) {
      const top = cmd.replies[0];
      LB.reply.textContent = top ? `${top.type}: ${top.message || top.status}` : '';
      LB.reply.className = 'lbreply ' + (top && !top.ok ? 'bad' : 'dim');
      for (const r of [...cmd.replies].reverse()) {
        if (r.seq <= LB.seenReply) continue;
        LB.seenReply = r.seq;
        if (r.seq > (LB.startSeq ?? 0)) toast(`${r.type}: ${r.message || r.status}`, r.ok ? '' : 'bad');
        const op = LB.imports[r.seq];
        if (op && r.type === 'Import') afterImport(op, r);
        const rl = LB.relink[r.seq];
        if (rl && r.status === 'Mismatch' && confirm(`${rl.path} has other content than the clip.\nImport the clip again from this file (same name, new analysis)?`)) ingest({ ...rl, force: true });
        F().loadRows(true);
      }
    }
  }

  function renderDock() {
    const L = S.live, box = LB.dock;
    const key = L ? JSON.stringify([L.program_deck, L.focus_deck, L.focus_layer, L.deck.map(d => d.layer.map(x => x.clip)), S.cat?.clips?.length, LB.sel.size]) : 'off';
    if (LB.dockKey === key) return;
    LB.dockKey = key; box.textContent = '';
    if (!L) { box.append(el('span', 'dim', 'The engine is not running: start Himmel to put clips on layers.')); return; }
    box.append(el('span', 'lbl', 'drag cards onto a layer'));
    for (const side of ['prev', 'prog']) {
      const d = side === 'prog' ? L.program_deck : 1 - L.program_deck;
      const grp = el('div', 'dgrp ' + side);
      const deckT = el('div', 'ddeck', side === 'prog' ? 'PROGRAM · on air' : 'PREVIEW');
      deckT.title = `Drop here = the next free layer of the ${side === 'prog' ? 'PROGRAM deck (on air at once)' : 'PREVIEW deck'}`;
      F().dropTarget(deckT, () => ({ deck: d }));
      grp.append(deckT);
      for (let k = 0; k < 4; k++) {
        const ls = L.deck[d].layer[k], ci = S.cat?.clips?.[ls.clip];
        const c = el('div', 'dcell' + (L.focus_deck === d && L.focus_layer === k ? ' focus' : '') + (ci ? '' : ' empty'));
        const g = F().groupOf(d, k);
        c.append(el('b', null, g), el('span', null, ci ? ci.name.replace(/\.mov$/i, '') : 'empty'));
        c.title = `${side === 'prog' ? 'PROGRAM (on air)' : 'PREVIEW'} layer ${k + 1}: drop a card here${LB.sel.size ? ', or click = load the selected clips here' : ''}`;
        F().dropTarget(c, () => ({ deck: d, layer: k }));
        c.addEventListener('click', () => { const ids = selectedIds(); if (ids.length) F().load(ids, { deck: d, layer: k }); else F().picker({ deck: d, layer: k }); });
        grp.append(c);
      }
      box.append(grp);
    }
    const go = btn('PERFORM ▶', 'Go to PERFORM: TAKE sends PREVIEW on air', () => window.setView('perform'));
    box.append(go);
  }

  function selectedRows() { const list = F().rows(); return list?.ok ? list.clips.filter(c => LB.sel.has(c.id)) : []; }
  function selectedIds() { return selectedRows().map(c => F().catOf(c)).filter(i => i >= 0); }

  function renderBulk() {
    const b = LB.bulk; const n = LB.sel.size;
    const key = n + '|' + LB.shown.length + '|' + ingestOk();
    if (LB.bulkKey === key) return;
    LB.bulkKey = key; b.textContent = '';
    const all = btn(n && n === LB.shown.length ? 'SELECT NONE' : 'SELECT ALL', 'Select every clip that matches the filters (Ctrl + A)', () => {
      if (n && n === LB.shown.length) LB.sel.clear(); else LB.shown.forEach(c => LB.sel.add(c.id));
      LB.key = ''; window.H.invalidate();
    }, 'chip2');
    b.append(all, el('span', 'dim', n ? `${n} selected` : 'Click a card to select it · Ctrl / Shift + click = more · double-click = load to the focused layer'));
    if (!n) return;
    const ids = selectedIds();
    b.append(
      btn('LOAD TO…', 'Put the selected clips on layers (consecutive layers from the one you pick)', e => F().menu(e, ids), 'primary'),
      btn('REANALYSE', 'Run the analysis again on the selected clips (they stay playable)', () => { selectedRows().filter(c => c.state === 'ready').forEach(c => ingest({ type: 'Reanalyse', clip: c.id, thumbs: true })); }),
      btn('RETRY', 'Import the selected failed clips again', () => { selectedRows().filter(c => c.state === 'failed').forEach(c => ingest({ type: 'Retry', clip: c.id })); }),
      btn('REMOVE…', 'Remove the selected clips from the library (the original files are never touched)', () => {
        const rows = selectedRows();
        const live = rows.filter(c => F().where(F().catOf(c)).length);
        if (!confirm(`Remove ${rows.length} clip${rows.length === 1 ? '' : 's'} from the library?\nThe HAP copies, stills and thumbnails are deleted; the original files stay where they are.` + (live.length ? `\n${live.length} of them are on a layer now and keep playing until the layer lets go.` : ''))) return;
        rows.forEach(c => ingest({ type: 'Remove', clip: c.id }));
        LB.sel.clear(); LB.key = '';
      }, 'bad'),
      btn('CLEAR SELECTION', null, () => { LB.sel.clear(); LB.key = ''; window.H.invalidate(); }, 'chip2'));
    for (const x of b.querySelectorAll('button')) if (/REANALYSE|RETRY|REMOVE/.test(x.textContent)) x.disabled = !ingestOk();
  }

  function render() {
    if (!LB.built) build();
    if (LB.startSeq == null && S.library?.cmd) { LB.startSeq = S.library.cmd.sent; LB.seenReply = Math.max(LB.seenReply, ...(S.library.cmd.replies || []).map(r => r.seq), 0); }
    renderStatus();
    renderDock();
    const list = F().rows();
    const L = S.live;
    const where = L ? JSON.stringify(L.deck.map(d => d.layer.map(x => x.clip))) + L.program_deck : '';
    const key = JSON.stringify([F().rows() && LB.listAt, LB.q, LB.state, LB.kind, LB.fps, LB.tag, LB.sort, LB.energy, LB.limit, S.cat?.clips?.length, where, LB.sel.size, [...LB.sel].slice(0, 50)]);
    LB.enBtns.forEach((b, i) => b.classList.toggle('on', LB.energy[0] <= i + 1 && i + 1 <= LB.energy[1] && !(LB.energy[0] === 1 && LB.energy[1] === 5)));
    if (key === LB.key) { renderBulk(); return; }
    LB.key = key;
    const g = $('libGrid'); g.textContent = '';
    if (!list) { g.append(el('div', 'empty', 'Reading the library…')); F().loadRows(true); return; }
    if (!list.ok) { g.append(emptyState(list.error || 'No library.')); LB.shown = []; renderBulk(); return; }
    const clips = list.clips.filter(c => c.state !== 'removed');
    fillTags(clips);
    LB.shown = clips.filter(want).sort(SORTS[LB.sort] || SORTS.new);
    for (const id of [...LB.sel]) if (!clips.some(c => c.id === id)) LB.sel.delete(id);
    $('libCount').textContent = `${LB.shown.length} of ${clips.length} clips`;
    for (const c of LB.shown.slice(0, LB.limit)) g.append(card(c));
    if (LB.shown.length > LB.limit) g.append(btn(`SHOW ${Math.min(300, LB.shown.length - LB.limit)} MORE (${LB.shown.length - LB.limit} hidden)`, 'Big libraries are drawn in pages', () => { LB.limit += 300; LB.key = ''; window.H.invalidate(); }, 'more'));
    if (!LB.shown.length) g.append(clips.length ? el('div', 'empty', 'Nothing matches the filters. Clear the search or set the filters back to "all".') : emptyState());
    renderBulk();
  }
  function emptyState(err) {
    const e = el('div', 'empty big');
    e.append(el('h3', null, err ? 'No library here' : 'Your library is empty'));
    if (err) e.append(el('div', 'dim', err));
    e.append(el('div', null, 'Import a folder of videos, pictures or GIFs. Himmel makes a fast HAP copy of each one and analyses its colours, motion and energy. You can play the first clips while the rest still import.'));
    const row = el('div', 'askrow');
    row.append(btn('+ IMPORT FOLDER…', 'Pick a folder to import', () => importDialog('folder'), 'primary'), btn('OPEN DROP FOLDER', 'Open the library\'s originals folder in Explorer', () => openOriginals()));
    e.append(row);
    return e;
  }

  function card(c) {
    const a = c.analysis, cat = F().catOf(c), w = F().where(cat);
    const k = el('div', 'lbcard st-' + c.state + (LB.sel.has(c.id) ? ' sel' : '') + (w.length ? ' live' : ''));
    k.tabIndex = 0;
    const th = el('div', 'lbthumb');
    if (a) th.style.background = a.avg_rgb;
    if (c.thumb) { const img = el('img'); img.loading = 'lazy'; img.src = `/library/thumb/${c.hash}.jpg`; img.alt = ''; img.draggable = false; img.onerror = () => img.remove(); th.append(img); }
    if (c.state !== 'ready') {
      const st = el('span', 'lbstate', c.state === 'failed' ? 'FAILED' : c.state.toUpperCase());
      th.append(st);
      if (c.state !== 'failed' && S.library?.job?.name === c.name && !/^(ready|failed|done)?$/i.test(S.library.job.stage || '')) { const pb = el('span', 'lbcprog'); const f = el('i'); f.style.width = Math.round((S.library.job.progress || 0) * 100) + '%'; pb.append(f); th.append(pb); }
    }
    if (w.length) { const t = F().whereTags(cat); t.classList.add('lbwhere'); th.append(t); }
    else if (cat >= 0) th.append(el('span', 'lbin', 'PLAYABLE'));
    const cb = el('span', 'lbcheck', LB.sel.has(c.id) ? '✓' : ''); cb.title = 'Select'; th.append(cb);
    k.append(th);
    k.append(el('div', 'lbname', c.name));
    const hz = { Hz60: 60, Hz50: 50, Hz59_94: 60, Hz30: 30, Hz25: 25 }[S.outputs?.refresh] || 0;
    const jud = hz ? c.judder.filter(x => x === hz) : c.judder;
    k.append(el('div', 'dim mono lbmeta', `${c.kind} · ${c.size[0]}×${c.size[1]} · ${c.fps} fps · ${c.seconds} s` + (c.alpha ? ' · α' : '')));
    if (jud.length) { const jb = el('div', 'warn lbjud', `⚠ judders on ${jud.join('/')} Hz outputs`); jb.title = 'The frame rate does not divide the refresh: motion steps a little. Import with "conform" for a smooth copy.'; k.append(jb); }
    if (a) {
      const chips = el('div', 'lbchips');
      const e = el('span', 'chip2 en', '●'.repeat(a.energy) + '○'.repeat(Math.max(0, 5 - a.energy))); e.title = `Energy ${a.energy} of 5`;
      chips.append(e, el('span', 'chip2', a.loop_style.toUpperCase()));
      if (a.flicker !== 'ok') chips.append(el('span', 'chip2 warn', 'FLICKER ' + a.flicker.toUpperCase()));
      const pal = el('span', 'lbpal');
      for (const [h, wt] of a.palette) { const i = el('i'); i.style.background = h; i.style.flex = String(Math.max(0.05, wt)); pal.append(i); }
      chips.append(pal);
      k.append(chips);
      const mood = el('div', 'lbmood');
      for (const [m, ab] of [['chill', 'CH'], ['groove', 'GR'], ['techno', 'TE'], ['crazy', 'CR']]) {
        const v = a.mood[m] || 0, s = el('span', null); s.title = `${m} ${Math.round(v * 100)}%`;
        const b = el('i'); b.style.width = (v * 100) + '%'; s.append(el('b', null, ab), b); mood.append(s);
      }
      k.append(mood);
    }
    if (c.tags?.length) k.append(el('div', 'dim lbtags', c.tags.map(t => '#' + t).join(' ')));
    if (c.state === 'failed' && c.error) k.append(el('div', 'bad lberr', c.error));
    if (c.source_missing) k.append(el('div', 'bad lberr', 'ORIGINAL MISSING · the clip still plays. RELINK… or FIND MISSING…'));
    // actions
    const ca = el('div', 'lbcardacts');
    const load = btn('LOAD ▾', cat >= 0 ? 'Load to… a layer (PREVIEW A–D, PROGRAM E–H, HIT)' : 'Not playable yet (still importing)', e => { e.stopPropagation(); F().menu(e, LB.sel.has(c.id) && LB.sel.size > 1 ? selectedIds() : [cat]); }, 'primary');
    load.disabled = cat < 0;
    ca.append(load);
    const more = btn('⋯', 'More: rescan, analyse again, relink, remove', e => { e.stopPropagation(); cardMenu(e, c, cat); });
    ca.append(more);
    k.append(ca);
    k.title = `${c.source}\nDouble-click = load to the focused layer · drag = onto a layer · click = select`;
    // selection
    k.addEventListener('click', e => {
      if (e.target.closest('button')) return;
      const i = LB.shown.findIndex(x => x.id === c.id);
      if (e.shiftKey && LB.last >= 0) { const [a0, b0] = [Math.min(LB.last, i), Math.max(LB.last, i)]; for (let j = a0; j <= b0; j++) LB.sel.add(LB.shown[j].id); }
      else if (e.ctrlKey || e.metaKey) { if (LB.sel.has(c.id)) LB.sel.delete(c.id); else LB.sel.add(c.id); }
      else { const only = LB.sel.size === 1 && LB.sel.has(c.id); LB.sel.clear(); if (!only) LB.sel.add(c.id); }
      LB.last = i; LB.key = ''; window.H.invalidate();
    });
    k.addEventListener('dblclick', e => { if (e.target.closest('button')) return; if (cat < 0) { toast('Not playable yet: still importing.', 'bad'); return; } F().load(cat, 'focus'); });
    k.addEventListener('contextmenu', e => F().menu(e, LB.sel.has(c.id) && LB.sel.size > 1 ? selectedIds() : [cat]));
    k.draggable = cat >= 0;
    k.addEventListener('dragstart', e => F().setDrag(e, LB.sel.has(c.id) && LB.sel.size > 1 ? selectedIds() : [cat]));
    return k;
  }

  function cardMenu(ev, c, cat) {
    F().closeMenu();
    const m = el('div', 'loadmenu');
    m.addEventListener('pointerdown', e => e.stopPropagation());
    m.append(el('div', 'lmhead', c.name));
    const item = (label, title, fn, cls) => { const b = btn(label, title, () => { m.remove(); fn(); }, 'lmi ' + (cls || '')); b.disabled = !ingestOk() && !/CLIP EDITOR/.test(label); m.append(b); };
    if (cat >= 0) item('OPEN IN THE CLIP EDITOR', 'Load it to the focused layer and open the CLIP tab in PERFORM', () => { F().load(cat, 'focus'); window.setView('perform'); window.H.setTab('clip'); });
    item('RESCAN THE ORIGINAL', 'Check that the original file is still there', () => ingest({ type: 'Rescan', clip: c.id }));
    if (c.state === 'ready') item('ANALYSE AGAIN', 'Run the analysis again (the clip stays playable)', () => ingest({ type: 'Reanalyse', clip: c.id, thumbs: true }));
    if (c.state === 'failed') item('RETRY', 'Import it again from the start', () => ingest({ type: 'Retry', clip: c.id }));
    item('RELINK…', 'Point this clip at its original in a new place', async () => {
      const p = await ask(`RELINK · ${c.name}`, c.source || '', 'The file\'s new absolute path. Other content is refused unless you confirm it.');
      if (p) ingest({ type: 'Relink', clip: c.id, path: p });
    });
    item('REMOVE FROM LIBRARY…', 'The original file is never touched', () => {
      if (confirm(`Remove "${c.name}" from the library?\nThe HAP copy, still and thumbnails are deleted; the original file stays.${cat >= 0 ? '\nA layer playing it keeps it until it lets go.' : ''}`)) ingest({ type: 'Remove', clip: c.id });
    }, 'bad');
    document.body.append(m);
    const r = m.getBoundingClientRect();
    m.style.left = Math.max(8, Math.min(ev.clientX, innerWidth - r.width - 8)) + 'px';
    m.style.top = Math.max(8, Math.min(ev.clientY, innerHeight - r.height - 8)) + 'px';
    setTimeout(() => document.addEventListener('pointerdown', () => m.remove(), { once: true }), 0);
  }

  document.addEventListener('keydown', e => {
    if (S.view !== 'library' || e.target.matches?.('input, textarea, select')) return;
    if ((e.ctrlKey || e.metaKey) && e.key === 'a') { e.preventDefault(); LB.shown.forEach(c => LB.sel.add(c.id)); LB.key = ''; window.H.invalidate(); }
    if (e.key === 'Escape' && LB.sel.size) { LB.sel.clear(); LB.key = ''; window.H.invalidate(); }
  });
  window.addEventListener('himmel-librows', () => { LB.listAt = performance.now(); LB.key = ''; });

  window.Lib = {
    render() { if (S.view === 'library') render(); },
    shown() { LB.key = ''; LB.dockKey = ''; F().loadRows(true); },
    importDialog: mode => { if (!LB.built) build(); importDialog(mode); },
    progress: () => progress(S.library),
    disk: diskInfo, loadDisk, ingest, ingestOk,
  };
})();
