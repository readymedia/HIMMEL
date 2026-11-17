# Preview block (`Local\HimmelPreview`)

Status: **version 1, written by the LEAD 2026-11-15** for the playable slice
(D90). Header: [`src/protocol/include/himmel/protocol/preview.h`](../../src/protocol/include/himmel/protocol/preview.h).
Writer: `himmel_render` only. Reader: the GUI bridge (and tools).

## 1. What is in it

Six streams of small BGRA8 pictures (top-down rows, stride = width × 4, at most
480 × 270): `DeckA`, `DeckB`, `Main`, `Output1..3`. Each stream has `active`,
`width` / `height` (the canvas aspect inside 480 × 270; a 3-screen span gives MAIN 480 × 90, and the GUI letterboxes it with `object-fit: contain`), `fps_target`
(15–30), and 3 buffers with their own seqlock. ~9.3 MB in all.

## 2. Writing it without hurting the frame (ENGINE)

- One downsample pass per active stream into a small render target, then
  `CopyResource` into one of **3 staging textures per stream**.
- The copy from 2–3 frames ago is mapped with `D3D11_MAP_FLAG_DO_NOT_WAIT`.
  If the GPU is not done (`DXGI_ERROR_WAS_STILL_DRAWING`), skip it this frame.
  **Never wait.**
- The writer fills the buffer after `latest` (round-robin), with
  `seq_write_bytes`, sets `frame`, then stores `latest` and increments
  `published`.
- Rate: at most `fps_target` per stream; off-air decks already render at
  thumbnail size (design/engine.md §2), so their preview is nearly free.
- Budget: the whole preview path must stay under 0.3 ms GPU and 0.3 ms CPU
  per frame on the GTX 1070 (measure and note it in `docs/measurements/`).

## 3. Reading it (GUI bridge)

- Poll `published`; when it changed, copy `buffer[latest]` with
  `seq_read_bytes`. A failed copy is skipped, never shown torn.
- The bridge decides how pictures go to the browser (raw / PNG / JPEG over
  HTTP or WebSocket). That is internal to the GUI track; a JPEG encoder is a
  new dependency and needs a `THIRD_PARTY_NOTICES.md` row (small public-domain
  / MIT ones are fine, anything bigger is a PROPOSED row first).
- A GUI that dies or lags never affects the engine: the engine does not know
  or care whether anyone reads.
