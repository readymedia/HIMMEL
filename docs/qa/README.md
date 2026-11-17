# QA: the failure rig and the soak (M12)

Status: written 2026-11-17 (M12, branch `claude/qa-m12-rig`). The tool is
[`tools/qa_rig.py`](../../tools/qa_rig.py). Rules it checks:
[`../design/engine.md`](../design/engine.md) §3 and
[`../build-v1.md`](../build-v1.md) §4 ("a part that fails never takes down the
output").

Reports land here: `rig-<stamp>.md` (the checks), `soak-<stamp>.csv` +
`soak-<stamp>.md` (one row a minute), `stack-<stamp>.log` (everything the
supervisor and its children printed). Keep the ones that matter (a release, a
show PC) in git; delete the rest.

## 1. What the rig does

It starts its own stack in simulated mode with a **temporary show root**, so it
never opens or writes your show, library or autosave:

```
himmel.exe --sim --weapons lightpad,nuvj -- --show-root %TEMP%\himmel-qa-XXXX
```

plus a simulated MK2, Lightpad and NuVJ (the same simulators as the live
views). Then:

| Part | What it does | Passes when |
|---|---|---|
| **kills** | Kills each process in turn: `himmel_render`, `himmel_ui`, `himmel_audio`, `himmel_ingest`, `himmel_osc`, `mk2_controller`, `weapon_blocks`, `weapon_nuvj` | It is back in **< 2 s**. A killed renderer: the **curtains are black** while it is gone (sampled from the desktop), its output windows come back, and **the show is restored** (the same clips and opacities on the decks). Any other kill: the renderer **keeps running** (same epoch, frames counting) |
| **unplug** | Stops each simulator (= the USB cable pulled), then starts a new one (= plugged back) | The engine sees the MK2 go and come back; the Lightpad and NuVJ drivers let go ("offline") and reconnect; the outputs run all the time |
| **missing clip** | A copy of a test clip joins the catalog, then its file disappears and it is loaded onto a layer | The layer shows its placeholder (`MISSING` in the state), the renderer keeps running |
| **GPU device loss** | Not forced (see §4) | Reported as covered by the render-kill path |
| **soak** (`--minutes N` / `--hours N`) | A TAKE every minute; every minute it writes fps, late frames, the worst frame interval, input → present and per-process memory + handles | You read the summary (§3) |

```
python tools/qa_rig.py                       the checks, about 3 minutes
python tools/qa_rig.py --minutes 20          the checks, then a 20 minute soak
python tools/qa_rig.py --soak-only --hours 8 just the soak
```

It refuses to start while another Himmel stack runs (one stack per machine:
`tasklist`). Needs a Release build and `media/test/*.mov`
(`python tools/make_test_clips.py`). It moves the output window on top of
others while it samples the curtains; do not work on that PC meanwhile.

## 2. The owner's runs (the 0.1 release gate)

### 2.1 The 8 h soak on the dev laptop
1. Close MK2 Live, NuVJ Live and other Himmel windows. Check `tasklist | findstr himmel` is empty.
2. Plug the laptop into power, set Windows to never sleep (Settings › System › Power), close other heavy programs.
3. `python tools/qa_rig.py --minutes 20` once first: every check must say `ok`.
4. Start the long one in the evening: `python tools/qa_rig.py --soak-only --hours 8`.
5. In the morning read `docs/qa/soak-<stamp>.md`. Pass when:
   - **late frames**: the total grows by **0** (or a handful you can explain, like a Windows update toast);
   - **memory**: `himmel_render` and the drivers end within ~10 % of where they were after the first hour (no steady climb);
   - **handles**: flat (a steady climb = a leak; send the CSV);
   - **renderer restarts**: 0.
6. Commit `docs/qa/soak-<stamp>.md` + `.csv` for the release.

### 2.2 The show PC (RTX 5090), the same and more
1. Build Release on the show PC (`docs/getting-started.md`), copy `media/test`.
2. Run 2.1 there.
3. **Real hardware run:** `himmel.exe -- --outputs 3 --fullscreen` with the MK2, the Lightpad and the NuVJ plugged in and a real show. During the night:
   - pull each USB cable for ~5 s and plug it back (MK2, Lightpad, NuVJ): the outputs never stop; the weapon works again within ~2 s;
   - pull one output cable: the others never stutter, it comes back to its slot (M6);
   - kill `himmel_render.exe` in Task Manager: black for under 2 s, the show comes back;
   - unplug the audio interface and plug it back (M8): reactions glide to rest, come back.
4. Write what happened in `docs/qa/show-pc-<date>.md` (a few lines per test) and settle the release.

## 3. Reading a soak

`soak-<stamp>.csv` columns: minute, time, fps, late frames (total since
start), the worst frame interval of the last 5 s window (ms), the average CPU
time per frame (ms), input → present (ms), then memory (MB) and handles per
process. The `.md` puts the first and last minute side by side. Late frames
are counted by the renderer (a frame interval longer than 1.5 × the refresh
period).

## 4. The GPU device-loss path

A DXGI device loss (driver reset, TDR, a GPU that falls off the bus) cannot be
forced from outside the process without a driver tool: `dxcap -forcetdr` needs
the Windows "Graphics Tools" feature and admin rights, and it resets every
GPU program on the machine. So the rig does not force it. What happens is
deterministic and shared with a crash:

1. `Present()` returns `DXGI_ERROR_DEVICE_REMOVED` / `DEVICE_RESET`.
2. `himmel_render` logs "GPU device lost (0x…): exiting so the supervisor
   restarts us" and exits with code 3.
3. The supervisor's curtains show black; it restarts the renderer (≤ 0.5 s
   between tries, M6), which makes a new device and restores the show from
   the state block.

Steps 2–3 are exactly the render-kill check. To see the real thing once (on a
spare machine, as admin, with nothing else running): install Graphics Tools
(Settings › Apps › Optional features), start Himmel, run
`dxcap -forcetdr`, and check the log and the outputs.

## 5. Not covered yet
- Real USB pulls and output-cable pulls (only on hardware, §2.2).
- Audio device pulls in the rig (the simulated stack runs `himmel_audio` on the default device; its kill is covered).
- The supervisor itself dying (the outputs then close; Windows' own restart is not set up).
- Long soaks with all four weapons and the GUI open in a browser.
