// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/audio/wasapi.h"
#include "himmel/audio/synth.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <initguid.h>  // defines the PKEY below in this file
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace himmel::audio {

namespace {

template <class T>
void release(void*& p) {
    if (p) static_cast<T*>(p)->Release();
    p = nullptr;
}

std::string utf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string friendly_name(IMMDevice* d) {
    IPropertyStore* ps = nullptr;
    std::string name;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = utf8(v.pwszVal);
        PropVariantClear(&v);
        ps->Release();
    }
    return name;
}

std::wstring device_id(IMMDevice* d) {
    LPWSTR id = nullptr;
    std::wstring s;
    if (SUCCEEDED(d->GetId(&id)) && id) {
        s = id;
        CoTaskMemFree(id);
    }
    return s;
}

EDataFlow flow_of(InputMode m) { return m == InputMode::Loopback ? eRender : eCapture; }

uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Finds an active endpoint whose name contains `part` (or the default one).
IMMDevice* find_device(IMMDeviceEnumerator* en, InputMode mode, const std::string& part, std::string* name) {
    IMMDevice* found = nullptr;
    if (part.empty()) {
        if (FAILED(en->GetDefaultAudioEndpoint(flow_of(mode), eConsole, &found))) return nullptr;
    } else {
        IMMDeviceCollection* col = nullptr;
        if (FAILED(en->EnumAudioEndpoints(flow_of(mode), DEVICE_STATE_ACTIVE, &col))) return nullptr;
        UINT n = 0;
        col->GetCount(&n);
        const std::string want = lower(part);
        for (UINT i = 0; i < n && !found; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            if (lower(friendly_name(d)).find(want) != std::string::npos) found = d;
            else d->Release();
        }
        col->Release();
    }
    if (found && name) *name = friendly_name(found);
    return found;
}

}  // namespace

std::vector<EndpointInfo> list_endpoints(InputMode mode) {
    std::vector<EndpointInfo> out;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))))
        return out;
    std::wstring def_id;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow_of(mode), eConsole, &def))) {
        def_id = device_id(def);
        def->Release();
    }
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow_of(mode), DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            EndpointInfo e;
            e.name = friendly_name(d);
            e.is_default = device_id(d) == def_id;
            IAudioClient* c = nullptr;
            if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&c)))) {
                WAVEFORMATEX* wf = nullptr;
                if (SUCCEEDED(c->GetMixFormat(&wf)) && wf) {
                    e.channels = wf->nChannels;
                    e.sample_rate = static_cast<int>(wf->nSamplesPerSec);
                    CoTaskMemFree(wf);
                }
                c->Release();
            }
            d->Release();
            out.push_back(e);
        }
        col->Release();
    }
    en->Release();
    return out;
}

bool play_drums(const std::string& device, float bpm, double seconds, float level, std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))))
        return fail("no device enumerator");
    std::string name;
    IMMDevice* dev = find_device(en, InputMode::Loopback, device, &name);  // render endpoints
    en->Release();
    if (!dev) return fail("render device not found");
    IAudioClient* c = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&c)))) {
        dev->Release();
        return fail("activate failed");
    }
    dev->Release();
    WAVEFORMATEX* wf = nullptr;
    c->GetMixFormat(&wf);
    bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        is_float = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    const int ch = wf->nChannels, bits = wf->wBitsPerSample, align = wf->nBlockAlign;
    const float sr = static_cast<float>(wf->nSamplesPerSec);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, wf, nullptr);
    CoTaskMemFree(wf);
    IAudioRenderClient* rc = nullptr;
    if (FAILED(hr) || FAILED(c->SetEventHandle(ev)) ||
        FAILED(c->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rc))) || !(is_float && bits == 32) && bits != 16) {
        if (rc) rc->Release();
        c->Release();
        CloseHandle(ev);
        return fail("cannot play to this device (format)");
    }
    UINT32 buf = 0;
    c->GetBufferSize(&buf);
    DrumSynth d;
    d.sr = sr;
    d.bpm = bpm;
    d.level = std::clamp(level, 0.0f, 1.0f);
    std::printf("playing drums at %.1f BPM to \"%s\" for %.0f s\n", bpm, name.c_str(), seconds);
    c->Start();
    const uint64_t end = now_us() + static_cast<uint64_t>(seconds * 1e6);
    while (now_us() < end) {
        if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
        UINT32 pad = 0;
        if (FAILED(c->GetCurrentPadding(&pad))) break;
        const UINT32 n = buf - pad;
        BYTE* data = nullptr;
        if (n == 0 || FAILED(rc->GetBuffer(n, &data))) continue;
        for (UINT32 i = 0; i < n; ++i) {
            const float v = d.next();
            for (int k = 0; k < ch; ++k) {
                BYTE* p = data + static_cast<size_t>(i) * align;
                if (bits == 32) std::memcpy(p + k * 4, &v, 4);
                else {
                    const int16_t s16 = static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
                    std::memcpy(p + k * 2, &s16, 2);
                }
            }
        }
        rc->ReleaseBuffer(n, 0);
    }
    c->Stop();
    rc->Release();
    c->Release();
    CloseHandle(ev);
    return true;
}

bool WasapiInput::open(const InputRequest& req, std::string* error) {
    close();
    auto fail = [&](const char* what, HRESULT hr) {
        if (error) {
            char b[160];
            std::snprintf(b, sizeof(b), "%s (0x%08lx)", what, static_cast<unsigned long>(hr));
            *error = b;
        }
        close();
        return false;
    };
    mode_ = req.mode;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  &enumerator_);
    if (FAILED(hr)) return fail("no device enumerator", hr);
    IMMDevice* dev = find_device(static_cast<IMMDeviceEnumerator*>(enumerator_), req.mode, req.device, &name_);
    if (!dev) return fail(req.device.empty() ? "no default device" : "device not found", E_FAIL);
    device_ = dev;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(hr)) return fail("activate failed", hr);
    auto* client = static_cast<IAudioClient*>(client_);
    WAVEFORMATEX* wf = nullptr;
    hr = client->GetMixFormat(&wf);
    if (FAILED(hr) || !wf) return fail("no mix format", hr);
    rate_ = static_cast<int>(wf->nSamplesPerSec);
    channels_ = wf->nChannels;
    block_align_ = wf->nBlockAlign;
    bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    int bits = wf->wBitsPerSample;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        is_float = ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        bits = wf->wBitsPerSample;
    }
    if (is_float && bits == 32) format_ = 0;
    else if (!is_float && bits == 16) format_ = 1;
    else if (!is_float && bits == 24) format_ = 2;
    else if (!is_float && bits == 32) format_ = 3;
    else {
        CoTaskMemFree(wf);
        return fail("unsupported sample format", E_FAIL);
    }
    ch_count_ = std::clamp(req.channel_count, 1, 2);
    ch_first_ = req.channel_first;
    if (ch_first_ < 0 || ch_first_ + ch_count_ > channels_) {
        ch_first_ = 0;  // out of range: fall back to channel 0 (audio-block.md §3)
        ch_count_ = std::min(ch_count_, channels_);
    }
    // Capture: event driven. Loopback: polled (the event never fires while nothing plays).
    const DWORD flags = req.mode == InputMode::Loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000 /* 20 ms */, 0, wf, nullptr);
    CoTaskMemFree(wf);
    if (FAILED(hr)) return fail("initialize failed", hr);
    if (req.mode == InputMode::Capture) {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client->SetEventHandle(static_cast<HANDLE>(event_));
        if (FAILED(hr)) return fail("event failed", hr);
    }
    hr = client->GetService(__uuidof(IAudioCaptureClient), &capture_);
    if (FAILED(hr)) return fail("no capture client", hr);
    UINT32 buf = 0;
    client->GetBufferSize(&buf);
    mono_.assign(std::max<UINT32>(buf, 4096) * 2, 0.0f);  // start-up only
    glitches_ = 0;
    hr = client->Start();
    if (FAILED(hr)) return fail("start failed", hr);
    return true;
}

void WasapiInput::close() {
    if (client_) static_cast<IAudioClient*>(client_)->Stop();
    release<IAudioCaptureClient>(capture_);
    release<IAudioClient>(client_);
    release<IMMDevice>(device_);
    release<IMMDeviceEnumerator>(enumerator_);
    if (event_) CloseHandle(static_cast<HANDLE>(event_));
    event_ = nullptr;
}

WasapiInput::Read WasapiInput::read(int timeout_ms, Sink sink, void* ctx) {
    if (!client_) return Read::Lost;
    if (event_) {
        const DWORD w = WaitForSingleObject(static_cast<HANDLE>(event_), static_cast<DWORD>(timeout_ms));
        if (w == WAIT_TIMEOUT) return Read::Timeout;
    } else {
        Sleep(static_cast<DWORD>(std::min(timeout_ms, 3)));
    }
    auto* cap = static_cast<IAudioCaptureClient*>(capture_);
    bool any = false;
    for (;;) {
        UINT32 next = 0;
        HRESULT hr = cap->GetNextPacketSize(&next);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING || FAILED(hr)) return Read::Lost;
        if (next == 0) break;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD fl = 0;
        UINT64 qpc = 0;
        hr = cap->GetBuffer(&data, &frames, &fl, nullptr, &qpc);
        if (FAILED(hr)) return Read::Lost;
        if (fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++glitches_;
        const int n = static_cast<int>(std::min<size_t>(frames, mono_.size()));
        const bool silent = (fl & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;
        const float scale = 1.0f / static_cast<float>(ch_count_);
        for (int i = 0; i < n; ++i) {
            float s = 0.0f;
            if (!silent) {
                const BYTE* f = data + static_cast<size_t>(i) * block_align_;
                for (int c = ch_first_; c < ch_first_ + ch_count_; ++c) {
                    switch (format_) {
                        case 0: {
                            float v;
                            std::memcpy(&v, f + c * 4, 4);
                            s += v;
                            break;
                        }
                        case 1: {
                            int16_t v;
                            std::memcpy(&v, f + c * 2, 2);
                            s += v / 32768.0f;
                            break;
                        }
                        case 2: {
                            const BYTE* p = f + c * 3;
                            const int32_t v = static_cast<int32_t>((p[0] << 8) | (p[1] << 16) | (p[2] << 24)) >> 8;
                            s += v / 8388608.0f;
                            break;
                        }
                        default: {
                            int32_t v;
                            std::memcpy(&v, f + c * 4, 4);
                            s += v / 2147483648.0f;
                            break;
                        }
                    }
                }
            }
            mono_[i] = s * scale;
        }
        cap->ReleaseBuffer(frames);
        // QPC position (100 ns units of the performance counter) of the first frame -> last frame, in us.
        uint64_t t_last = qpc ? qpc / 10 + static_cast<uint64_t>(frames) * 1000000ull / static_cast<uint64_t>(rate_) : now_us();
        if (t_last > now_us() + 100000) t_last = now_us();  // never trust a future time
        if (n > 0) sink(mono_.data(), n, t_last, ctx);
        any = true;
    }
    return any ? Read::Data : Read::Timeout;
}

}  // namespace himmel::audio
