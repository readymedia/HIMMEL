// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// HAP clip player: a worker thread reads + decodes frames ahead of the render
// thread; the render thread picks finished frames without locks and uploads
// them to the clip's GPU texture.
//
// Threads:
//   worker  - opens clips (MOV parse, buffers, GPU texture: all allocation is
//             here), then keeps the frames around the wanted one decoded.
//   render  - per frame: current() (swaps in a newly loaded clip), request()
//             the wanted frame, acquire() it if it is ready, upload, release().
//
// Warm frames (M2, clips-and-playback.md §10): the render thread names up to
// kWarmSlots frames (the layer's cue points / slices) with set_warm(); the
// worker keeps them decoded in their own slots once the play window is full,
// so a jump to one of them is uploaded in the same frame (no decode wait).
//
// A slot is Free -> Writing (worker) -> Ready -> Reading (render) -> Ready.
// Only compare-and-swap moves a slot between threads, so neither side ever
// waits for the other. A missed frame just keeps the last one on screen.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "himmel/media/hap.h"
#include "himmel/media/mov.h"

namespace himmel::library {
struct ClipTable;
}

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace himmel::engine {

inline constexpr int kClipSlots = 8;
inline constexpr int kClipAhead = 4;  // frames decoded ahead in the play direction
// Cue points / slices kept decoded per clip. GUESS: RAM 16 x 1-2 MB per 1080p clip and layer
// (9 layers: up to ~290 MB with Hap Q); fine on the show PC, watch it with big 4K libraries.
inline constexpr int kWarmSlots = 16;

struct ClipSlot {
    enum : uint32_t { Free, Writing, Ready, Reading };
    std::atomic<uint32_t> state{Free};
    std::atomic<int64_t> frame{-1};
    std::vector<uint8_t> data;  // one texture, sized at load
};

// One loaded clip. Created by the worker, owned by the render thread once
// current, handed back to the worker to delete.
struct LoadedClip {
    std::string path, name;
    media::MovVideoTrack track;
    media::HapTexture texture = media::HapTexture::Unknown;
    size_t tex_bytes = 0, row_pitch = 0;
    int tex_w = 0, tex_h = 0;  // rounded up to 4
    void* file = nullptr;      // HANDLE
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    // Wave 7 frame blend: a second texture, so the two nearest frames are both on the GPU
    // (the renderer uploads each clip frame once and alternates between the two).
    ID3D11Texture2D* tex_b = nullptr;
    ID3D11ShaderResourceView* srv_b = nullptr;
    ID3D11Texture2D* gpu_tex(int k) const { return k ? tex_b : tex; }
    ID3D11ShaderResourceView* view(int k) const { return k ? srv_b : srv; }
    ClipSlot slots[kClipSlots];
    ClipSlot warm[kWarmSlots];                   // data sized by the worker on first use
    std::atomic<int64_t> warm_want[kWarmSlots];  // -1 = none; written by the render thread
    std::atomic<int64_t> wanted{0};
    std::atomic<int32_t> direction{1};
    ~LoadedClip();
};

struct ClipStats {
    std::atomic<uint64_t> decoded{0};
    std::atomic<uint32_t> decode_us_max{0};  // read + decode, since the last reset
    std::atomic<uint32_t> errors{0};
    std::atomic<uint64_t> warm_decoded{0};
};

class ClipPlayer {
public:
    // The playlist is fixed at start-up, so asking for a clip never allocates.
    ClipPlayer(ID3D11Device* device, std::vector<std::string> playlist);
    // MEDIA (M7): the playlist is the shared, append-only clip table (library clips join while
    // the show runs; library/clip_table.h). It must outlive the player.
    ClipPlayer(ID3D11Device* device, const library::ClipTable* table);
    ~ClipPlayer();
    ClipPlayer(const ClipPlayer&) = delete;
    ClipPlayer& operator=(const ClipPlayer&) = delete;

    const std::vector<std::string>& playlist() const { return playlist_; }
    // Any thread: ask the worker to load playlist[index]; the old clip plays on
    // until the new one is ready. Wave 7 pre-roll: the worker decodes `start_frame` (and
    // the frames after it in `dir`) before it hands the clip over, so the first frame is
    // on screen in the frame the clip swaps in (no black, no placeholder flash).
    void load(int index, int64_t start_frame = 0, int dir = 1);

    // Render thread.
    LoadedClip* current();                  // swaps in a newly loaded clip; may be null
    void request(int64_t frame, int dir);   // wanted frame on the current clip
    const uint8_t* acquire(int64_t frame);  // ready data for that frame (play window or warm), or null
    // The frames to keep warm on the current clip (-1 = none); cheap when unchanged.
    void set_warm(const int64_t (&frames)[kWarmSlots]);
    void release();                         // after the upload

    ClipStats& stats() { return stats_; }
    std::atomic<uint32_t> load_failed{0};   // last_error is written before this is bumped
    char last_error[160] = "";
    std::atomic<int> loaded_index{-1};

private:
    void run();
    LoadedClip* open_clip(const std::string& path);
    bool fill_one(LoadedClip& c);
    bool fill_warm(LoadedClip& c);
    bool decode_into(LoadedClip& c, ClipSlot& slot, int64_t f);

    ID3D11Device* device_;
    std::vector<std::string> playlist_;
    const library::ClipTable* table_ = nullptr;  // M7: when set, the playlist (append-only)
    int playlist_size() const;
    const std::string& playlist_path(int index) const;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    void* wake_ = nullptr;  // HANDLE, auto-reset event

    std::atomic<int> request_index_{-1};
    std::atomic<int64_t> request_frame_{0};
    std::atomic<int32_t> request_dir_{1};
    std::atomic<uint32_t> request_seq_{0};
    std::atomic<LoadedClip*> pending_{nullptr};       // worker -> render
    std::atomic<LoadedClip*> retired_[4] = {};        // render -> worker (delete)
    LoadedClip* current_ = nullptr;                   // render thread only
    LoadedClip* newest_ = nullptr;                    // worker thread only
    ClipSlot* reading_ = nullptr;                     // render thread only
    std::vector<uint8_t> read_buf_;                   // worker thread only
    ClipStats stats_;
};

}  // namespace himmel::engine
