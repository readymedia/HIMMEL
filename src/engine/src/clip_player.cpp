#include "himmel/engine/clip_player.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <chrono>
#include <cstring>
#include <filesystem>

namespace himmel::engine {

namespace {

// Paths travel as UTF-8 std::string; std::filesystem wants char8_t for that in C++20.
std::filesystem::path utf8_path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
std::string utf8_of(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

int64_t wrap(int64_t f, int64_t n) { return n > 0 ? ((f % n) + n) % n : 0; }

bool read_at(HANDLE h, uint64_t offset, void* dst, size_t size) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    return size <= 0xFFFFFFFFu && ReadFile(h, dst, static_cast<DWORD>(size), &got, &ov) && got == size;
}

DXGI_FORMAT dxgi_format(media::HapTexture t) {
    switch (t) {
        case media::HapTexture::DXT1: return DXGI_FORMAT_BC1_UNORM;
        case media::HapTexture::DXT5:
        case media::HapTexture::YCoCgDXT5: return DXGI_FORMAT_BC3_UNORM;
        case media::HapTexture::RGTC1: return DXGI_FORMAT_BC4_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

}  // namespace

LoadedClip::~LoadedClip() {
    if (srv) srv->Release();
    if (tex) tex->Release();
    if (file) CloseHandle(static_cast<HANDLE>(file));
}

ClipPlayer::ClipPlayer(ID3D11Device* device, std::vector<std::string> playlist)
    : device_(device), playlist_(std::move(playlist)) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker_ = std::thread([this] { run(); });
}

ClipPlayer::~ClipPlayer() {
    stop_.store(true);
    SetEvent(static_cast<HANDLE>(wake_));
    if (worker_.joinable()) worker_.join();
    LoadedClip* pending = pending_.exchange(nullptr);
    for (auto& r : retired_) delete r.exchange(nullptr);
    if (newest_ != current_ && newest_ != pending) delete newest_;  // newest is pending or current
    delete pending;
    delete current_;
    CloseHandle(static_cast<HANDLE>(wake_));
}

void ClipPlayer::load(int index) {
    if (index < 0 || index >= static_cast<int>(playlist_.size())) return;
    request_index_.store(index);
    request_seq_.fetch_add(1);
    SetEvent(static_cast<HANDLE>(wake_));
}

// ---------------------------------------------------------------- render thread
LoadedClip* ClipPlayer::current() {
    if (pending_.load(std::memory_order_acquire)) {
        // Swap only if there is a free retire slot for the old clip (else try next frame).
        for (auto& r : retired_) {
            LoadedClip* expected = nullptr;
            if (!current_ || r.compare_exchange_strong(expected, current_)) {
                current_ = pending_.exchange(nullptr);
                SetEvent(static_cast<HANDLE>(wake_));
                break;
            }
        }
    }
    return current_;
}

void ClipPlayer::request(int64_t frame, int dir) {
    if (!current_) return;
    const int64_t f = wrap(frame, static_cast<int64_t>(current_->track.frames()));
    const int32_t d = dir < 0 ? -1 : 1;
    if (current_->wanted.exchange(f) != f || current_->direction.exchange(d) != d) SetEvent(static_cast<HANDLE>(wake_));
}

const uint8_t* ClipPlayer::acquire(int64_t frame) {
    if (!current_) return nullptr;
    for (auto& s : current_->slots) {
        if (s.frame.load(std::memory_order_acquire) != frame) continue;
        uint32_t expected = ClipSlot::Ready;
        if (!s.state.compare_exchange_strong(expected, ClipSlot::Reading)) continue;
        if (s.frame.load(std::memory_order_acquire) != frame) {  // rewritten in between
            s.state.store(ClipSlot::Ready);
            continue;
        }
        reading_ = &s;
        return s.data.data();
    }
    return nullptr;
}

void ClipPlayer::release() {
    if (reading_) reading_->state.store(ClipSlot::Ready, std::memory_order_release);
    reading_ = nullptr;
}

// ---------------------------------------------------------------- worker thread
LoadedClip* ClipPlayer::open_clip(const std::string& path) {
    auto fail = [&](const std::string& m) -> LoadedClip* {
        strncpy_s(last_error, sizeof(last_error), (utf8_of(utf8_path(path).filename()) + ": " + m).c_str(),
                  _TRUNCATE);
        load_failed.fetch_add(1);
        return nullptr;
    };
    const std::wstring wpath = utf8_path(path).wstring();
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("cannot open the file");
    auto c = std::make_unique<LoadedClip>();
    c->file = h;
    c->path = path;
    c->name = utf8_of(utf8_path(path).stem());
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    media::ByteSource src;
    src.size = static_cast<uint64_t>(size.QuadPart);
    src.read = [h](uint64_t off, void* dst, size_t n) { return read_at(h, off, dst, n); };
    std::string err;
    auto track = media::read_mov(src, &err);
    if (!track) return fail(err);
    c->track = std::move(*track);

    read_buf_.resize(std::max<size_t>(read_buf_.size(), c->track.max_frame_size));
    if (!read_at(h, c->track.offsets[0], read_buf_.data(), c->track.sizes[0])) return fail("read error");
    c->texture = media::hap_peek_texture(read_buf_.data(), c->track.sizes[0]);
    if (c->texture == media::HapTexture::Unknown) return fail("not a supported HAP clip (" + c->track.codec() + ")");
    c->tex_w = (c->track.width + 3) & ~3;
    c->tex_h = (c->track.height + 3) & ~3;
    c->tex_bytes = media::hap_texture_bytes(c->texture, c->track.width, c->track.height);
    c->row_pitch = media::hap_row_pitch(c->texture, c->track.width);
    for (auto& s : c->slots) s.data.resize(c->tex_bytes);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(c->tex_w);
    td.Height = static_cast<UINT>(c->tex_h);
    td.MipLevels = td.ArraySize = 1;
    td.Format = dxgi_format(c->texture);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &c->tex)) ||
        FAILED(device_->CreateShaderResourceView(c->tex, nullptr, &c->srv)))
        return fail("could not create the GPU texture");
    return c.release();
}

bool ClipPlayer::fill_one(LoadedClip& c) {
    const int64_t n = static_cast<int64_t>(c.track.frames());
    const int64_t wanted = c.wanted.load();
    const int32_t dir = c.direction.load();
    auto in_window = [&](int64_t f) {
        const int64_t d = dir > 0 ? wrap(f - wanted, n) : wrap(wanted - f, n);
        return d <= kClipAhead;
    };
    for (int k = 0; k <= kClipAhead && k < n; ++k) {
        const int64_t f = wrap(wanted + k * dir, n);
        bool have = false;
        for (auto& s : c.slots) {
            const uint32_t st = s.state.load();
            if (s.frame.load() == f && (st == ClipSlot::Ready || st == ClipSlot::Reading)) have = true;
        }
        if (have) continue;
        // A victim: a free slot, or a ready one outside the window.
        ClipSlot* slot = nullptr;
        for (auto& s : c.slots) {
            uint32_t expected = ClipSlot::Free;
            if (s.state.compare_exchange_strong(expected, ClipSlot::Writing)) {
                slot = &s;
                break;
            }
        }
        if (!slot)
            for (auto& s : c.slots) {
                if (in_window(s.frame.load())) continue;
                uint32_t expected = ClipSlot::Ready;
                if (s.state.compare_exchange_strong(expected, ClipSlot::Writing)) {
                    slot = &s;
                    break;
                }
            }
        if (!slot) return false;  // everything is in use; wait for the renderer

        const auto t0 = std::chrono::steady_clock::now();
        slot->frame.store(f);
        const size_t i = static_cast<size_t>(f);
        bool ok = read_at(static_cast<HANDLE>(c.file), c.track.offsets[i], read_buf_.data(), c.track.sizes[i]);
        if (ok) {
            const auto r = media::hap_decode(read_buf_.data(), c.track.sizes[i], slot->data.data(), slot->data.size());
            ok = r.ok && r.bytes == c.tex_bytes;
        }
        if (!ok) {  // a broken frame shows black instead of stopping the clip
            std::memset(slot->data.data(), 0, slot->data.size());
            stats_.errors.fetch_add(1);
        }
        slot->state.store(ClipSlot::Ready, std::memory_order_release);
        const auto us = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        uint32_t prev = stats_.decode_us_max.load();
        while (us > prev && !stats_.decode_us_max.compare_exchange_weak(prev, us)) {
        }
        stats_.decoded.fetch_add(1);
        return true;
    }
    return false;
}

void ClipPlayer::run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    uint32_t seen_seq = 0;
    while (!stop_.load()) {
        const uint32_t seq = request_seq_.load();
        if (seq != seen_seq) {
            seen_seq = seq;
            const int index = request_index_.load();
            if (LoadedClip* c = open_clip(playlist_[static_cast<size_t>(index)])) {
                delete pending_.exchange(c);  // an older one the renderer never took
                newest_ = c;
                loaded_index.store(index);
            }
        }
        for (auto& r : retired_) delete r.exchange(nullptr);
        if (newest_ && fill_one(*newest_)) continue;
        WaitForSingleObject(static_cast<HANDLE>(wake_), 5);
    }
}

}  // namespace himmel::engine
