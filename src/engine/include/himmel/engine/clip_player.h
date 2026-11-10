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

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace himmel::engine {

inline constexpr int kClipSlots = 8;
inline constexpr int kClipAhead = 4;  // frames decoded ahead in the play direction

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
    ClipSlot slots[kClipSlots];
    std::atomic<int64_t> wanted{0};
    std::atomic<int32_t> direction{1};
    ~LoadedClip();
};

struct ClipStats {
    std::atomic<uint64_t> decoded{0};
    std::atomic<uint32_t> decode_us_max{0};  // read + decode, since the last reset
    std::atomic<uint32_t> errors{0};
};

class ClipPlayer {
public:
    // The playlist is fixed at start-up, so asking for a clip never allocates.
    ClipPlayer(ID3D11Device* device, std::vector<std::string> playlist);
    ~ClipPlayer();
    ClipPlayer(const ClipPlayer&) = delete;
    ClipPlayer& operator=(const ClipPlayer&) = delete;

    const std::vector<std::string>& playlist() const { return playlist_; }
    // Any thread: ask the worker to load playlist[index]; the old clip plays on
    // until the new one is ready.
    void load(int index);

    // Render thread.
    LoadedClip* current();                  // swaps in a newly loaded clip; may be null
    void request(int64_t frame, int dir);   // wanted frame on the current clip
    const uint8_t* acquire(int64_t frame);  // ready data for that frame, or null
    void release();                         // after the upload

    ClipStats& stats() { return stats_; }
    std::atomic<uint32_t> load_failed{0};   // last_error is written before this is bumped
    char last_error[160] = "";
    std::atomic<int> loaded_index{-1};

private:
    void run();
    LoadedClip* open_clip(const std::string& path);
    bool fill_one(LoadedClip& c);

    ID3D11Device* device_;
    std::vector<std::string> playlist_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    void* wake_ = nullptr;  // HANDLE, auto-reset event

    std::atomic<int> request_index_{-1};
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
