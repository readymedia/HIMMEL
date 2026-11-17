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
  const H = window.H;
  const F = () => window.Flows;
  const has = b => !!(b && b.present);
  const btn = (txt, title, fn, cls) => { const b = el('button', cls || null, txt); if (title) b.title = title; if (fn) b.addEventListener('click', fn); return b; };
  const ask = (...a) => window.Panels.W.ask(...a);
  const gb = n => (n >= 1e9 ? (n / 1e9).toFixed(n >= 1e10 ? 0 : 1) + ' GB' : n >= 1e6 ? Math.round(n / 1e6) + ' MB' : Math.round((n || 0) / 1e3) + ' kB');
  const dur = s => (s < 0 ? '–' : s < 60 ? Math.round(s) + ' s' : s < 3600 ? Math.round(s / 60) + ' min' : (s / 3600).toFixed(1) + ' h');

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
      const busy = pg.done < pg.total || !!pg.file;
      // A finished batch stays on screen for 15 s ("Imported n of n"), then the bar goes away.
      if (busy) LB.doneAt = 0; else if (!LB.doneAt) LB.doneAt = performance.now();
      if (!busy && performance.now() - LB.doneAt > 15000) return null;
      return { done: pg.done, total: pg.total, eta: pg.eta_s, busy, file: pg.file, fileP: pg.file_progress, error: pg.error, v3: true };
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
      const b = btn(m.toUpperCase(), { Auto: 'AUTO: full speed with no show, gentle while a show runs (default)', Background: 'BACKGROUND: always gentle', Full: 'FULL: full speed even while a show runs (may cost frames)', Hold: 'HOLD: start nothing new; the running file pauses' }[m], async () => {
        if (m === 'Full' && !await H.confirm('FULL speed even while a show runs: the import may cost frames on the outputs. Go on?')) return;
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
      if (!await H.confirm(`Clear the import queue?\n${l?.queued || 0} waiting files are dropped. Clips that are already in the library stay.`)) return;
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
      uploadFiles([...e.dataTransfer.files]);
    });
    const ov = el('div', 'lbdropov', 'Drop videos and pictures here: they are copied into the library\'s drop folder and imported');
    page.append(ov);
    LB.built = true;
  }

  // Files dropped from the desktop (D173, LEAD 2026-11-17): each one is checked (GET /library/upload), then
  // streamed to the bridge (POST /library/upload), which writes it into <library>\\originals; the watch folder
  // imports it from there. Folders cannot be uploaded: IMPORT FOLDER… takes them by path (no copy).
  async function uploadFiles(files) {
    if (!files.length) return;
    const bad = [], done = [];
    LB.upl = { n: 0, of: files.length, name: '', pct: 0 };
    for (const f of files) {
      LB.upl.n++; LB.upl.name = f.name; LB.upl.pct = 0; window.H.invalidate();
      if (!f.size && !f.type) { bad.push(`${f.name}: a folder (use IMPORT FOLDER…)`); continue; }
      let chk;
      try { chk = await (await fetch(`/library/upload?name=${encodeURIComponent(f.name)}&size=${f.size}`)).json(); } catch (x) { chk = { ok: false, error: 'himmel_ui is not reachable' }; }
      if (!chk.ok) { bad.push(`${f.name}: ${chk.error}`); continue; }
      const r = await new Promise(res => {
        const x = new XMLHttpRequest();
        x.open('POST', `/library/upload?name=${encodeURIComponent(f.name)}`);
        x.upload.onprogress = ev => { if (ev.lengthComputable) { LB.upl.pct = ev.loaded / ev.total; window.H.invalidate(); } };
        x.onload = () => { try { res(JSON.parse(x.responseText)); } catch (y) { res({ ok: false, error: 'bad answer' }); } };
        x.onerror = () => res({ ok: false, error: 'the upload was cut off' });
        x.send(f);
      });
      if (r.ok) done.push(r.name); else bad.push(`${f.name}: ${r.error}`);
    }
    LB.upl = null; window.H.invalidate();
    if (done.length) toast(`${done.length} file${done.length === 1 ? '' : 's'} copied into the drop folder: they import by themselves now`);
    if (bad.length) fileDropHelp(bad);
  }
  function fileDropHelp(lines) {
    const names = lines;
    const md = el('div', 'modal'), box = el('div', 'mbox askbox');
    const close = () => md.remove();
    box.append(el('div', 'lbl', 'SOME DROPPED FILES WERE NOT COPIED'));
    for (const t of names.slice(0, 8)) box.append(el('div', 'bad', t));
    if (names.length > 8) box.append(el('div', 'dim', `… and ${names.length - 8} more`));
    box.append(el('div', 'dim', 'IMPORT FOLDER… / IMPORT FILES… import by path (nothing is copied, and the disk check estimates first). OPEN DROP FOLDER opens the watched folder in Explorer.'));
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
        for (const [v, t] of [[0, 'fps as shot'], [50, 'conform to 50 (≈2.5× disk)'], [60, 'conform to 60 (≈2.5× disk)']]) { const x = el('option', null, t); x.value = v; cf.append(x); }
        cf.value = o.conform_hz; cf.addEventListener('change', () => { o.conform_hz = +cf.value; });
        const est = btn('ESTIMATE DISK', 'How much space this import needs (nothing is imported)', async () => {
          if (!cur.path) return;
          info.textContent = 'Estimating…';
          LB.estimate = null; LB.estWait = true;
          const base = mode === 'files' && ticked.size ? { files: [...ticked].map(n => cur.path.replace(/[\\/]$/, '') + '\\' + n) } : { path: cur.path };
          const j = await sendImport(base, { dry_run: true });
          if (!j.ok) { info.textContent = j.error || 'refused'; return; }
          const t0 = performance.now();
          const poll = () => {
            if (!md.isConnected) return;
            const e = estimateOf(j.seq);
            if (e) { info.textContent = ''; info.append(e.node); info.classList.toggle('bad', !e.fits); return; }
            if (performance.now() - t0 < 30000) setTimeout(poll, 250);
            else info.textContent = 'No answer from the import worker in 30 s (it may be busy with a file). Try ESTIMATE again.';
          };
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
  // The worker's plan for Import `seq` (library block v3 §11.1), in words: "N clips, ~X GB, Y GB free after,
  // reserve Z GB". Null until the reply for `seq` is in.
  function planFor(seq) {
    const l = S.library, r = (l?.cmd?.replies || []).find(x => x.seq === seq);
    if (!r) return null;
    const pl = l.plan && l.plan.mine && l.plan.seq === seq ? l.plan : null;
    return { r, pl };
  }
  function planWords(pl, r) {
    if (!pl) return { text: r.message || r.status, fits: r.ok };
    const after = pl.free_bytes - pl.queue_bytes - pl.need_bytes;
    const fits = !!pl.fits;
    const clips = `${pl.clips} new clip${pl.clips === 1 ? '' : 's'}${pl.known ? ` (${pl.known} already in the library)` : ''}`;
    const queue = pl.queue_bytes ? ` (plus ${gb(pl.queue_bytes)} for the files still in the queue)` : '';
    let text;
    if (fits) text = `✓ ${clips} need about ${gb(pl.need_bytes)}${queue}. That leaves ${gb(after)} free on the disk; Himmel keeps ${gb(pl.reserve_bytes)} free.`;
    else text = `✗ ${clips} need about ${gb(pl.need_bytes)}${queue}. That would leave ${after >= 0 ? gb(after) : 'less than nothing'} free, but Himmel keeps ${gb(pl.reserve_bytes)} free` +
      ` (free ${gb(pl.reserve_bytes - after)} more, or keep less free)` + (pl.fit_clips ? `. ${pl.fit_clips} of them would fit.` : '. None of them fits now.');
    return { text, fits };
  }
  function estimateOf(seq) {
    const p = planFor(seq);
    if (!p) return null;
    const w = planWords(p.pl, p.r);
    const node = el('span', null, w.text);
    if (!w.fits) { const a = btn('KEEP FREE ON DISK…', 'Settings → Library: how much space Himmel keeps free', () => openReserve()); a.classList.add('chip2'); node.append(' ', a); }
    return { node, fits: w.fits };
  }
  function afterImport(cmd, r) {
    const plan = S.library?.plan;
    const pl = plan && plan.mine && plan.seq === r.seq ? plan : null;
    if (cmd.dry_run) { LB.estimate = { text: pl?.message || r.message, at: performance.now(), pl }; window.H.invalidate(); return; }
    if (r.status !== 'NoSpace') return;
    const md = el('div', 'modal'), box = el('div', 'mbox askbox');
    const close = () => md.remove();
    const w = planWords(pl, r);
    box.append(el('div', 'cfhead', 'Not enough free disk space: nothing was imported'),
      el('div', null, w.text),
      el('div', 'dim', `Himmel always keeps ${pl ? gb(pl.reserve_bytes) : 'some space'} free on the library's disk, so Windows and the show keep running. What you can do:`),
      el('div', null, '• Free some space on the disk (empty the recycle bin, delete or move big files), then import again.'),
      el('div', null, '• Lower "Keep free on disk" in Settings → Library (not below a few GB).'),
      el('div', null, '• Move the library to a disk with more room (MOVE LIBRARY…).'),
      el('div', null, '• Import fewer clips, or a smaller size (max 720p in the import options).'));
    const row = el('div', 'askrow');
    row.append(el('div', 'spacer'), btn('Cancel', null, close));
    if (pl && pl.fit_clips > 0) row.append(btn(`IMPORT THE ${pl.fit_clips} THAT FIT`, 'Queue the clips that fit above the reserve, in the order found', () => { close(); const c = { ...cmd }; delete c.dry_run; sendImport({}, { ...c, fit: true }); }, 'primary'));
    row.append(btn('MOVE LIBRARY…', 'Move the library to another folder or disk', () => { close(); moveDialog(); }));
    row.append(btn('KEEP FREE ON DISK…', 'How much space Himmel keeps free (Settings → Library)', () => { close(); openReserve(); }, 'primary'));
    box.append(row); md.append(box); document.body.append(md);
    md.addEventListener('click', e => { if (e.target === md) close(); });
  }
  // Library (69): move the library to another folder (v3 §11.5); the progress shows in the status line.
  async function moveDialog() {
    if (!supports('Library')) { toast('This import worker cannot move the library (needs library block v3).', 'bad'); return; }
    const r = await pathDialog('folder', 'MOVE THE LIBRARY · choose an empty folder (or a new one on another disk)');
    if (!r) return;
    const d = diskInfo();
    if (!await H.confirm(`Move the library (${gb(d.lib || 0)}) to\n${r.path}?\n\nThe import pauses while it copies. The old copy is deleted after the next restart of Himmel. Clips keep playing.`)) return;
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
      // Plain words: the chips next to it show the setting (AUTO …), this says what the worker does now.
      const how = { Full: 'full speed', Background: 'gently, a show is running', High: 'fast', Idle: 'full speed' }[l.mode] || String(l.mode).toLowerCase();
      const word = !l.alive ? 'IMPORT WORKER NOT ANSWERING' : l.mode === 'Hold' ? 'IMPORT ON HOLD' : l.disk_low ? 'IMPORT WAITS · DISK LOW' : l.paused ? 'IMPORT PAUSED · the show needs the PC' : (l.queued + l.working) ? `IMPORTING · ${how}` : 'IMPORT IDLE';
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
      const low = d.guard === 'low';   // the worker's guard decides (D180); the page has no threshold of its own
      const bar = el('span', 'lbdbar'); const used = el('i'); used.style.width = (d.total ? Math.min(100, (1 - d.free / d.total) * 100) : 0) + '%'; bar.append(used);
      de.append(el('span', 'lbl', 'disk'), bar, el('span', low ? 'bad' : 'dim', `${gb(d.free)} free${d.total ? ' of ' + gb(d.total) : ''}`),
        el('span', 'dim', `· library ${gb(d.lib || 0)}`));
      if (low) { const ch = btn('DISK LOW · the import waits', 'The import waits so the disk keeps its reserve. Click: Settings → Library (keep free on disk)', () => openReserve(), 'chip bad'); de.append(ch); }
      de.title = `${d.root}\nThe library keeps a HAP copy of every clip (often several times the original's size). The originals are never copied or touched.` + (d.reserve != null ? `\nThe import stops when less than ${gb(d.reserve)} would be left.` : '');
      de.classList.toggle('low', low);
    } else de.append(el('span', 'dim', 'disk: –'));
    s.append(de);
    for (const b of LB.acts.querySelectorAll('button')) if (b.textContent !== 'OPEN DROP FOLDER') b.disabled = !ingestOk();
    LB.acts.title = ingestOk() ? '' : 'The import worker is not running or does not answer.';
    // progress
    const p = progress(l);
    if (LB.upl) LB.reply.textContent = `Copying ${LB.upl.n} of ${LB.upl.of}: ${LB.upl.name} · ${Math.round(LB.upl.pct * 100)}%`;
    LB.prog.classList.toggle('hidden', !p);
    if (p) {
      const job = l.job || {};
      const pct = p.total ? (p.done + (p.busy ? Math.min(0.99, (p.v3 ? p.fileP : job.progress) || 0) : 0)) / p.total : 1;
      LB.progFill.style.width = (pct * 100).toFixed(1) + '%';
      LB.progTxt.textContent = '';
      LB.progTxt.append(el('b', null, p.busy ? `Importing ${Math.min(p.total, p.done + 1)} of ${p.total}` : `Imported ${p.done} of ${p.total}`));
      if (p.busy && (p.file || job.name)) LB.progTxt.append(el('span', null, ` · ${p.file || job.name} · ${job.stage || ''} ${Math.round(((p.v3 ? p.fileP : job.progress) || 0) * 100)}%`));
      if (p.busy) LB.progTxt.append(el('span', 'dim', ` · ${p.eta < 0 ? 'estimating time…' : 'about ' + dur(p.eta) + ' left'}`));
      if (l.paused) LB.progTxt.append(el('span', 'warn', ' · waiting a moment: the outputs come first'));
      if (p.error || job.last_error) LB.progTxt.append(el('span', 'bad', ' · ⚠ ' + (p.error || job.last_error)));
      if (l.disk_low) LB.progTxt.append(el('span', 'bad', ' · DISK LOW: the import waits (free space, remove clips, or CLEAR QUEUE)'));
      LB.hold.textContent = l.mode === 'Hold' ? 'RESUME' : 'PAUSE';
      LB.hold.disabled = !ingestOk();
      LB.clear.classList.toggle('hidden', !supports('ClearQueue'));
      LB.clear.disabled = !ingestOk() || !l.queued;
    }
    // the newest reply under the bar (the replies themselves are handled by pollReplies on every view)
    const cmd = l?.cmd;
    if (cmd) {
      const top = cmd.replies[0];
      if (!LB.upl) LB.reply.textContent = top ? `${top.type}: ${top.message || top.status}` : '';
      LB.reply.className = 'lbreply ' + (top && !top.ok ? 'bad' : 'dim');
    }
  }
  // Wave 8: the ingest replies are handled wherever the user is (START wizard, LIBRARY, Settings): a toast each,
  // the NoSpace / estimate follow-ups, relink mismatches. Replies that were there before this page loaded are
  // only marked seen (no old toasts after a reload).
  function pollReplies() {
    const cmd = S.library?.cmd;
    if (!cmd) return;
    if (LB.startSeq == null) { LB.startSeq = cmd.sent; LB.seenReply = Math.max(LB.seenReply, ...(cmd.replies || []).map(r => r.seq), 0); return; }
    for (const r of [...cmd.replies].reverse()) {
      if (r.seq <= LB.seenReply) continue;
      LB.seenReply = r.seq;
      const op = LB.imports[r.seq];
      if (!(op && op.dry_run)) toast(`${r.type}: ${r.message || r.status}`, r.ok ? '' : 'bad');
      if (op && r.type === 'Import') afterImport(op, r);
      const rl = LB.relink[r.seq];
      if (rl && r.status === 'Mismatch') H.confirm(`${rl.path} has other content than the clip.\nImport the clip again from this file (same name, new analysis)?`, 'IMPORT AGAIN').then(ok => ok && ingest({ ...rl, force: true }));
      F().loadRows(true);
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

  // In the order they were clicked (a Set keeps insertion order): the first one clicked goes on the first layer.
  function selectedRows() { const list = F().rows(); if (!list?.ok) return []; const byId = new Map(list.clips.map(c => [c.id, c])); return [...LB.sel].map(id => byId.get(id)).filter(Boolean); }
  function selectedIds() { return selectedRows().map(c => F().refOf(c)).filter(i => (typeof i === 'object' ? i.lib > 0 : i >= 0)); }

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
      btn('REMOVE…', 'Remove the selected clips from the library (the original files are never touched)', async () => {
        const rows = selectedRows();
        const live = rows.filter(c => F().where(F().catOf(c)).length);
        if (!await H.confirm(`Remove ${rows.length} clip${rows.length === 1 ? '' : 's'} from the library?\nThe HAP copies, stills and thumbnails are deleted; the original files stay where they are.` + (live.length ? `\n${live.length} of them are on a layer now and keep playing until the layer lets go.` : ''))) return;
        rows.forEach(c => ingest({ type: 'Remove', clip: c.id }));
        LB.sel.clear(); LB.key = '';
      }, 'bad'),
      btn('CLEAR SELECTION', null, () => { LB.sel.clear(); LB.key = ''; window.H.invalidate(); }, 'chip2'));
    for (const x of b.querySelectorAll('button')) if (/REANALYSE|RETRY|REMOVE/.test(x.textContent)) x.disabled = !ingestOk();
  }

  function render() {
    if (!LB.built) build();
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
    const a = c.analysis, cat = F().catOf(c), w = F().where(cat), ref = F().refOf(c), can = typeof ref === 'object' || ref >= 0;
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
    // Judder only matters for the show's own refresh, and only when frame smoothing is Off (ENGINE's Auto
    // smooths 24 / 25 fps on 60 Hz; LEAD wave 7).
    const smoothOff = S.outputs?.frame_smooth === 'Off';
    const jud = smoothOff ? (hz ? c.judder.filter(x => x === hz) : c.judder) : [];
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
    const load = btn('LOAD ▾', can ? 'Load to… a layer (PREVIEW A–D, PROGRAM E–H, HIT)' : 'Not playable yet (still importing)', e => { e.stopPropagation(); F().menu(e, LB.sel.has(c.id) && LB.sel.size > 1 ? selectedIds() : [ref]); }, 'primary');
    load.disabled = !can;
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
    k.addEventListener('dblclick', e => { if (e.target.closest('button')) return; if (!can) { toast('Not playable yet: still importing.', 'bad'); return; } F().load(ref, 'focus'); });
    k.addEventListener('contextmenu', e => F().menu(e, LB.sel.has(c.id) && LB.sel.size > 1 ? selectedIds() : [ref]));
    k.draggable = can;
    k.addEventListener('dragstart', e => F().setDrag(e, LB.sel.has(c.id) && LB.sel.size > 1 ? selectedIds() : [ref]));
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
    item('REMOVE FROM LIBRARY…', 'The original file is never touched', async () => {
      if (await H.confirm(`Remove "${c.name}" from the library?\nThe HAP copy, still and thumbnails are deleted; the original file stays.${cat >= 0 ? '\nA layer playing it keeps it until it lets go.' : ''}`)) ingest({ type: 'Remove', clip: c.id });
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

  // Settings → Library (LEAD wave 7): the disk, "Keep free on disk" (library command SetReserve, MEDIA; 0 = the
  // default), the library folder. Built once per visit; the numbers update in place.
  function openReserve() { window.Views.settingsPage('library'); window.setView('settings'); }
  const SL = {};
  function settings(body, fresh) {
    const l = S.library, d = diskInfo();
    loadDisk();
    if (fresh || !SL.box || !body.contains(SL.box)) {
      body.textContent = '';
      SL.box = el('div', 'setlib');
      body.append(el('h3', null, 'Library'), SL.box);
      SL.disk = el('div', 'lbdisk setdisk');
      SL.res = el('div', 'setres');
      SL.inp = el('input'); SL.inp.type = 'number'; SL.inp.min = 0; SL.inp.max = 2000; SL.inp.step = 1; SL.inp.title = 'Gigabytes Himmel always leaves free on the library\'s disk (0 = the default)';
      SL.inp.addEventListener('keydown', e => e.stopPropagation());
      SL.set = btn('SET', 'Store this reserve in the library (it survives a restart)', () => {
        const v = Math.round(+SL.inp.value);
        if (!Number.isFinite(v) || v < 0) { toast('A whole number of GB, 0 = the default', 'bad'); return; }
        ingest({ type: 'SetReserve', gb: v });
      }, 'primary');
      SL.def = btn('DEFAULT', 'Back to the default reserve', () => ingest({ type: 'SetReserve', gb: 0 }));
      SL.note = el('div', 'dim');
      const row = el('div', 'aurow'); row.append(el('span', 'lbl', 'keep free on disk'), SL.inp, el('span', 'dim', 'GB'), SL.set, SL.def);
      SL.res.append(row, SL.note);
      SL.folder = el('div', 'aurow');
      SL.box.append(SL.disk, SL.res, el('div', 'lbl', 'LIBRARY FOLDER'), SL.folder);
      SL.inp.value = d.reserve ? Math.round(d.reserve / 1e9) : '';
    }
    // disk line
    SL.disk.textContent = '';
    if (d.free != null) {
      const bar = el('span', 'lbdbar'); const used = el('i'); used.style.width = (d.total ? Math.min(100, (1 - d.free / d.total) * 100) : 0) + '%'; bar.append(used);
      SL.disk.append(el('span', 'lbl', 'disk'), bar, el('span', d.guard === 'low' ? 'bad' : null, `${gb(d.free)} free of ${gb(d.total || 0)}`),
        el('span', 'dim', `· the library uses ${gb(d.lib || 0)}` + (d.queue ? ` · the queue still needs ~${gb(d.queue)}` : '')));
      if (d.guard === 'low') SL.disk.append(el('span', 'chip bad', 'DISK LOW · the import waits'));
    } else SL.disk.append(el('span', 'dim', 'disk: – (no library in this run)'));
    const can = ingestOk() && supports('SetReserve');
    SL.inp.disabled = SL.set.disabled = SL.def.disabled = !can;
    SL.note.textContent = d.reserve != null
      ? `Himmel keeps ${gb(d.reserve)} free now. An import that would go below it is refused (or only the clips that fit are taken), and a running import waits. Windows and the show need free space too.`
      : 'The import worker does not report its reserve (older version).';
    if (!can) SL.note.textContent += supports('SetReserve') ? ' The import worker is not running.' : ' Changing it needs the newer import worker (library command SetReserve).';
    const fk = (l?.root_info?.path || d.root || '') + '|' + (l?.root_info?.move_state || '');
    if (SL.fk !== fk) {
      SL.fk = fk; SL.folder.textContent = '';
      SL.folder.append(el('span', 'mono', l?.root_info?.path || d.root || '–'), btn('MOVE LIBRARY…', 'Move the library (HAP copies, thumbnails, database) to another folder or disk', () => moveDialog()),
        btn('OPEN DROP FOLDER', 'Open the watched originals folder in Explorer', () => openOriginals()));
    }
  }

  window.Lib = {
    tick() { pollReplies(); },
    gb,
    settings,
    openReserve,
    render() { if (S.view === 'library') render(); },
    shown() { LB.key = ''; LB.dockKey = ''; F().loadRows(true); },
    importDialog: mode => { if (!LB.built) build(); importDialog(mode); },
    progress: () => progress(S.library),
    disk: diskInfo, loadDisk, ingest, ingestOk,
  };
})();
