#include "himmel/engine/camera_capture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace himmel::engine {

namespace {

constexpr uint32_t kNew = 4;

uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string narrow(const wchar_t* w) {
    char buf[256];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

const char* subtype_name(const GUID& g) {
    if (g == MFVideoFormat_NV12) return "NV12";
    if (g == MFVideoFormat_YUY2) return "YUY2";
    if (g == MFVideoFormat_MJPG) return "MJPG";
    if (g == MFVideoFormat_RGB32) return "RGB32";
    if (g == MFVideoFormat_I420) return "I420";
    return "other";
}

int subtype_rank(const GUID& g) {
    if (g == MFVideoFormat_NV12) return 3;
    if (g == MFVideoFormat_YUY2) return 2;
    if (g == MFVideoFormat_MJPG) return 1;
    return 0;
}

}  // namespace

CameraCapture::CameraCapture(std::string name_hint, int index) : hint_(std::move(name_hint)), index_(index) {
    for (auto& f : frames_) {  // all memory up front: no allocation while capturing
        f.y.resize(static_cast<size_t>(kCamMaxW) * kCamMaxH);
        f.uv.resize(static_cast<size_t>(kCamMaxW) * kCamMaxH / 2);
    }
    worker_ = std::thread([this] { run(); });
}

CameraCapture::~CameraCapture() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
}

const CameraFrame* CameraCapture::latest() {
    if (ready_.load(std::memory_order_acquire) & kNew) front_ = ready_.exchange(front_, std::memory_order_acq_rel) & 3;
    return frames_[front_].seq ? &frames_[front_] : nullptr;
}

bool CameraCapture::pop_luma(LumaSample& out) {
    const uint32_t t = luma_tail_.load(std::memory_order_relaxed);
    if (t == luma_head_.load(std::memory_order_acquire)) return false;
    out = luma_[t % kLumaRing];
    luma_tail_.store(t + 1, std::memory_order_release);
    return true;
}

void CameraCapture::run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    while (!stop_.load()) {
        capture_session();
        running.store(false);
        for (int i = 0; i < 20 && !stop_.load(); ++i) Sleep(100);  // retry every 2 s
    }
    MFShutdown();
    CoUninitialize();
}

bool CameraCapture::capture_session() {
    auto fail = [&](const char* m, HRESULT hr) {
        std::snprintf(error, sizeof(error), "%s (0x%08lx)", m, static_cast<unsigned long>(hr));
        return false;
    };
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 1);
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attr.Get(), &devices, &count);
    if (FAILED(hr) || count == 0) {
        if (devices) CoTaskMemFree(devices);
        return fail("no camera found", hr);
    }
    // Pick: index if given, else the name hint, else the first non-IR camera.
    int pick = -1;
    std::vector<std::wstring> names(count);
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t* n = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &n, &len))) {
            names[i] = n;
            CoTaskMemFree(n);
        }
    }
    const std::wstring hint = lower(std::wstring(hint_.begin(), hint_.end()));
    if (index_ >= 0 && index_ < static_cast<int>(count)) pick = index_;
    for (UINT32 i = 0; i < count && pick < 0; ++i)
        if (!hint.empty() && lower(names[i]).find(hint) != std::wstring::npos) pick = static_cast<int>(i);
    for (UINT32 i = 0; i < count && pick < 0; ++i)
        if (lower(names[i]).find(L" ir ") == std::wstring::npos && lower(names[i]).find(L"infrared") == std::wstring::npos &&
            !(lower(names[i]).size() >= 3 && lower(names[i]).find(L"ir camera") != std::wstring::npos))
            pick = static_cast<int>(i);
    if (pick < 0) pick = 0;
    strncpy_s(name, sizeof(name), narrow(names[static_cast<size_t>(pick)].c_str()).c_str(), _TRUNCATE);

    ComPtr<IMFMediaSource> source;
    hr = devices[pick]->ActivateObject(IID_PPV_ARGS(&source));
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);
    if (FAILED(hr)) return fail("could not open the camera (in use by another app?)", hr);

    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 3);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);  // MJPG/YUY2 -> NV12
    ra->SetUINT32(MF_LOW_LATENCY, TRUE);
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), ra.Get(), &reader);
    if (FAILED(hr)) {
        source->Shutdown();
        return fail("source reader failed", hr);
    }
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    // Best native format.
    ComPtr<IMFMediaType> best;
    double best_score = -1;
    UINT32 bw = 0, bh = 0;
    double bfps = 0;
    GUID bsub{};
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(stream, i, &t))) break;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT32 w = 0, h = 0, num = 0, den = 1;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        const double fps = den ? static_cast<double>(num) / den : 0;
        if (w > kCamMaxW || h > kCamMaxH || w == 0 || subtype_rank(sub) == 0) continue;
        const double score = (fps >= 29 ? 1e9 : 0) + std::min(fps, 60.0) * 1e7 + static_cast<double>(w) * h * 10 + subtype_rank(sub);
        if (score > best_score) best_score = score, best = t, bw = w, bh = h, bfps = fps, bsub = sub;
    }
    if (!best) {
        source->Shutdown();
        return fail("the camera has no usable format", E_FAIL);
    }
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(stream, TRUE);
    hr = reader->SetCurrentMediaType(stream, nullptr, best.Get());
    if (FAILED(hr)) {
        source->Shutdown();
        return fail("could not set the camera format", hr);
    }
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, bw, bh);
    hr = reader->SetCurrentMediaType(stream, nullptr, out.Get());
    if (FAILED(hr)) {
        source->Shutdown();
        return fail("could not get NV12 from the camera", hr);
    }
    ComPtr<IMFMediaType> cur;
    reader->GetCurrentMediaType(stream, &cur);
    UINT32 w = bw, h = bh;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
    w = std::min<UINT32>(w, kCamMaxW) & ~1u;
    h = std::min<UINT32>(h, kCamMaxH) & ~1u;
    UINT32 stride_attr = 0;
    LONG default_stride = static_cast<LONG>(w);
    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_attr))) default_stride = static_cast<LONG>(stride_attr);
    std::snprintf(format, sizeof(format), "%ux%u %.0f FPS %s", w, h, bfps, subtype_name(bsub));
    error[0] = 0;
    opens.fetch_add(1);

    uint64_t seq = 0, t_rate = now_us(), n_rate = 0;
    while (!stop_.load()) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(stream, 0, &idx, &flags, &ts, &sample);
        const uint64_t t = now_us();
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            fail("the camera stopped delivering", FAILED(hr) ? hr : E_FAIL);
            break;
        }
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
        BYTE* p = nullptr;
        LONG pitch = default_stride;
        ComPtr<IMF2DBuffer> b2;
        bool locked2d = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch));
        DWORD len = 0;
        if (!locked2d && FAILED(buf->Lock(&p, nullptr, &len))) continue;
        if (pitch <= 0) pitch = static_cast<LONG>(w);  // bottom-up NV12 does not exist in practice

        CameraFrame& f = frames_[back_];
        for (UINT32 r = 0; r < h; ++r) std::memcpy(f.y.data() + static_cast<size_t>(r) * w, p + static_cast<size_t>(r) * pitch, w);
        const BYTE* uvp = p + static_cast<size_t>(pitch) * h;
        for (UINT32 r = 0; r < h / 2; ++r)
            std::memcpy(f.uv.data() + static_cast<size_t>(r) * w, uvp + static_cast<size_t>(r) * pitch, w);
        if (locked2d) b2->Unlock2D();
        else buf->Unlock();
        f.width = static_cast<int>(w);
        f.height = static_cast<int>(h);
        f.t_us = t;
        f.seq = ++seq;

        // Centre-third brightness for the latency test (every 4th pixel).
        uint64_t sum = 0, n = 0;
        for (UINT32 y = h / 3; y < 2 * h / 3; y += 4)
            for (UINT32 x = w / 3; x < 2 * w / 3; x += 4) sum += f.y[static_cast<size_t>(y) * w + x], ++n;
        const uint32_t head = luma_head_.load(std::memory_order_relaxed);
        if (head - luma_tail_.load(std::memory_order_acquire) < kLumaRing) {
            luma_[head % kLumaRing] = {t, n ? static_cast<float>(sum) / n : 0.0f};
            luma_head_.store(head + 1, std::memory_order_release);
        }

        back_ = ready_.exchange(back_ | kNew, std::memory_order_acq_rel) & 3;
        running.store(true);
        ++n_rate;
        if (t - t_rate >= 2000000) {
            fps_x100.store(static_cast<uint32_t>(n_rate * 100000000.0 / (t - t_rate)));
            t_rate = t;
            n_rate = 0;
        }
    }
    source->Shutdown();
    return true;
}

}  // namespace himmel::engine
