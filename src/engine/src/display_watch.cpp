// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Display watcher (M6). See display_watch.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "himmel/engine/display_watch.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>

namespace himmel::engine {

namespace {

// The EDID from HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\<model>\<instance>\Device Parameters,
// found from the monitor device path \\?\DISPLAY#<model>#<instance>#{guid}.
bool read_edid(const wchar_t* device_path, uint8_t* edid, DWORD& size) {
    std::wstring p = device_path;
    if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    const size_t a = p.find(L'#'), b = a == std::wstring::npos ? a : p.find(L'#', a + 1),
                 c = b == std::wstring::npos ? b : p.find(L'#', b + 1);
    if (c == std::wstring::npos) return false;
    const std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\" + p.substr(0, a) + L"\\" + p.substr(a + 1, b - a - 1) +
                             L"\\" + p.substr(b + 1, c - b - 1) + L"\\Device Parameters";
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    const LONG r = RegQueryValueExW(k, L"EDID", nullptr, nullptr, edid, &size);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && size >= 128;
}

void parse_edid(const uint8_t* e, protocol::DisplayId& id) {
    const uint16_t m = static_cast<uint16_t>(e[8] << 8 | e[9]);
    id.maker[0] = static_cast<char>('A' - 1 + ((m >> 10) & 31));
    id.maker[1] = static_cast<char>('A' - 1 + ((m >> 5) & 31));
    id.maker[2] = static_cast<char>('A' - 1 + (m & 31));
    id.maker[3] = 0;
    id.product = static_cast<uint16_t>(e[10] | e[11] << 8);
    id.serial = static_cast<uint32_t>(e[12] | e[13] << 8 | e[14] << 16 | static_cast<uint32_t>(e[15]) << 24);
    for (int d = 54; d <= 108; d += 18) {
        if (e[d] || e[d + 1] || e[d + 3] != 0xFF) continue;  // display descriptor 0xFF = serial text
        int n = 0;
        for (int i = 5; i < 18 && n < 15; ++i) {
            const char ch = static_cast<char>(e[d + i]);
            if (ch == '\n' || ch == 0) break;
            id.serial_text[n++] = ch;
        }
        while (n > 0 && id.serial_text[n - 1] == ' ') --n;
        id.serial_text[n] = 0;
    }
}

}  // namespace

void DisplayWatch::scan(DisplayList& out) {
    out.count = 0;
    UINT32 np = 0, nm = 0;
    DISPLAYCONFIG_PATH_INFO paths[32];
    DISPLAYCONFIG_MODE_INFO modes[64];
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return;
    np = std::min<UINT32>(np, 32), nm = std::min<UINT32>(nm, 64);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, nullptr) != ERROR_SUCCESS) return;
    for (UINT32 i = 0; i < np && out.count < kMaxDisplays; ++i) {
        const auto& p = paths[i];
        DisplayInfo di{};
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = p.sourceInfo.adapterId;
        sn.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;
        wcsncpy_s(di.gdi, sn.viewGdiDeviceName, _TRUNCATE);
        // Desktop rect from the source mode (same as the monitor rect).
        const UINT32 smi = p.sourceInfo.modeInfoIdx;
        if (smi < nm && modes[smi].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const auto& sm = modes[smi].sourceMode;
            di.rect[0] = sm.position.x, di.rect[1] = sm.position.y;
            di.rect[2] = sm.position.x + static_cast<int32_t>(sm.width);
            di.rect[3] = sm.position.y + static_cast<int32_t>(sm.height);
            di.primary = sm.position.x == 0 && sm.position.y == 0;
        } else {
            continue;
        }
        const auto& rr = p.targetInfo.refreshRate;
        di.hz = rr.Denominator ? static_cast<float>(rr.Numerator) / rr.Denominator : 0.0f;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = p.targetInfo.adapterId;
        tn.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS) {
            WideCharToMultiByte(CP_UTF8, 0, tn.monitorFriendlyDeviceName, -1, di.name, sizeof(di.name), nullptr, nullptr);
            di.id.connector = tn.connectorInstance;
            di.id.technology = static_cast<uint32_t>(tn.outputTechnology);
            uint8_t edid[512];
            DWORD size = sizeof(edid);
            if (read_edid(tn.monitorDevicePath, edid, size)) {
                parse_edid(edid, di.id);
            } else if (tn.flags.edidIdsValid) {  // no EDID in the registry: the ids Windows gives us
                const uint16_t m = _byteswap_ushort(tn.edidManufactureId);
                di.id.maker[0] = static_cast<char>('A' - 1 + ((m >> 10) & 31));
                di.id.maker[1] = static_cast<char>('A' - 1 + ((m >> 5) & 31));
                di.id.maker[2] = static_cast<char>('A' - 1 + (m & 31));
                di.id.product = tn.edidProductCodeId;
            }
        }
        if (!di.name[0]) strncpy_s(di.name, "Display", _TRUNCATE);
        // Duplicate source (clone mode): one entry per desktop rect is enough.
        bool dup = false;
        for (uint32_t k = 0; k < out.count; ++k) dup |= std::wcscmp(out.d[k].gdi, di.gdi) == 0;
        if (!dup) out.d[out.count++] = di;
    }
    for (uint32_t k = 0; k < out.count; ++k) out.order[k] = static_cast<int>(k);
    std::sort(out.order, out.order + out.count, [&](int a, int b) {
        const auto &ra = out.d[a].rect, &rb = out.d[b].rect;
        return ra[0] != rb[0] ? ra[0] < rb[0] : ra[1] < rb[1];
    });
}

void DisplayWatch::start() {
    if (thread_.joinable()) return;
    DisplayList first;
    scan(first);
    seq_.fetch_add(1);
    pub_ = first;
    seq_.fetch_add(1);
    gen_.store(1);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
}

void DisplayWatch::stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    if (wake_) SetEvent(wake_);
    thread_.join();
    if (wake_) CloseHandle(wake_), wake_ = nullptr;
}

void DisplayWatch::poke() {
    if (wake_) SetEvent(wake_);
}

bool DisplayWatch::latest(DisplayList& out, uint32_t& gen) const {
    const uint32_t g = gen_.load(std::memory_order_acquire);
    if (g == gen) return false;
    const uint32_t s1 = seq_.load(std::memory_order_acquire);
    if (s1 & 1) return false;
    std::memcpy(&out, &pub_, sizeof(DisplayList));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq_.load(std::memory_order_acquire) != s1) return false;
    gen = g;
    return true;
}

void DisplayWatch::want_refresh(const DisplayList& on, const int* indexes, int n, float hz) {
    RefreshReq r = req_;
    r.hz = hz;
    r.n = 0;
    for (int i = 0; i < n && r.n < protocol::kMaxOutputs; ++i)
        if (indexes[i] >= 0 && indexes[i] < static_cast<int>(on.count))
            wcsncpy_s(r.gdi[r.n++], on.d[indexes[i]].gdi, _TRUNCATE);
    ++r.serial;
    req_seq_.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_release);
    req_ = r;
    std::atomic_thread_fence(std::memory_order_release);
    req_seq_.fetch_add(1, std::memory_order_acq_rel);
    poke();
}

// Worker: switch each asked display to the asked refresh at its current size.
void DisplayWatch::apply_refresh() {
    RefreshReq r;
    for (int attempt = 0;; ++attempt) {
        const uint32_t s1 = req_seq_.load(std::memory_order_acquire);
        if (s1 & 1) {
            if (attempt > 100) return;
            continue;
        }
        r = req_;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (req_seq_.load(std::memory_order_acquire) == s1) break;
    }
    if (r.serial == req_done_) return;
    req_done_ = r.serial;
    if (r.hz <= 0) return;
    const DWORD want = static_cast<DWORD>(std::lround(r.hz < 59.95f && r.hz > 59.9f ? 59.0f : r.hz));
    for (int i = 0; i < r.n; ++i) {
        DEVMODEW cur{};
        cur.dmSize = sizeof(cur);
        if (!EnumDisplaySettingsW(r.gdi[i], ENUM_CURRENT_SETTINGS, &cur)) continue;
        if (cur.dmDisplayFrequency == want) continue;
        DEVMODEW m{};
        m.dmSize = sizeof(m);
        bool found = false;
        for (DWORD k = 0; EnumDisplaySettingsW(r.gdi[i], k, &m); ++k)
            if (m.dmPelsWidth == cur.dmPelsWidth && m.dmPelsHeight == cur.dmPelsHeight && m.dmDisplayFrequency == want &&
                m.dmBitsPerPel == cur.dmBitsPerPel) {
                found = true;
                break;
            }
        if (!found) {
            mode_failures_.fetch_add(1);
            continue;
        }
        m.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
        // CDS_FULLSCREEN: temporary - Windows restores the user's mode when we exit.
        if (ChangeDisplaySettingsExW(r.gdi[i], &m, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL)
            mode_changes_.fetch_add(1);
        else
            mode_failures_.fetch_add(1);
    }
}

void DisplayWatch::run() {
    SetThreadDescription(GetCurrentThread(), L"himmel display watch");
    DisplayList cur;
    while (!stop_.load()) {
        WaitForSingleObject(static_cast<HANDLE>(wake_), 500);
        if (stop_.load()) break;
        apply_refresh();
        scan(cur);
        // Publish only when something changed (rects, ids, Hz).
        bool same = cur.count == pub_.count;
        for (uint32_t k = 0; same && k < cur.count; ++k)
            same = std::memcmp(&cur.d[k], &pub_.d[k], sizeof(DisplayInfo)) == 0;
        if (same) continue;
        seq_.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);
        pub_ = cur;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.fetch_add(1, std::memory_order_acq_rel);
        gen_.fetch_add(1, std::memory_order_acq_rel);
    }
}

}  // namespace himmel::engine
