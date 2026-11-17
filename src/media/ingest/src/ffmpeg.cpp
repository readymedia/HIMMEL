// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/ingest/ffmpeg.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

namespace himmel::ingest {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string u8path(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool Tools::find(const fs::path& dir) {
    auto look = [&](const wchar_t* name) -> fs::path {
        std::error_code ec;
        if (!dir.empty()) {
            const auto p = dir / name;
            return fs::exists(p, ec) ? p : fs::path();
        }
        wchar_t buf[MAX_PATH];
        const DWORD n = SearchPathW(nullptr, name, nullptr, MAX_PATH, buf, nullptr);
        return n > 0 && n < MAX_PATH ? fs::path(buf) : fs::path();
    };
    ffmpeg = look(L"ffmpeg.exe");
    ffprobe = look(L"ffprobe.exe");
    return !ffmpeg.empty() && !ffprobe.empty();
}

// ---- command lines ----

namespace {
std::vector<std::string> head() { return {"-hide_banner", "-nostdin", "-loglevel", "error", "-y"}; }
std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}
// Round down to a multiple of 4 (HAP), never below 4.
const char* kRound4 = "scale=w='max(4,trunc(iw/4)*4)':h='max(4,trunc(ih/4)*4)'";
std::string fit(int w, int h) {
    return "scale=w='min(iw," + std::to_string(w) + ")':h='min(ih," + std::to_string(h) +
           ")':force_original_aspect_ratio=decrease";
}
}  // namespace

std::vector<std::string> probe_args(const std::string& input, bool sequence, double seq_fps, int seq_start) {
    std::vector<std::string> a = {"-v", "error", "-print_format", "json", "-show_streams", "-show_format", "-select_streams", "v:0"};
    if (sequence) a.insert(a.end(), {"-framerate", num(seq_fps), "-start_number", std::to_string(seq_start)});
    a.insert(a.end(), {"-i", input});
    return a;
}

std::vector<std::string> convert_args(const ConvertSpec& s) {
    using library::Kind;
    auto a = head();
    if (s.kind == Kind::Sequence)
        a.insert(a.end(), {"-framerate", num(s.fps > 0 ? s.fps : 25.0), "-start_number", std::to_string(s.seq_start)});
    a.insert(a.end(), {"-i", s.input, "-map", "0:v:0", "-an", "-sn", "-dn"});
    const bool still = s.kind == Kind::Still || s.kind == Kind::Vector;
    std::string vf;
    if (still) vf = fit(s.max_w, s.out_max_h > 0 ? std::min(s.max_h, s.out_max_h) : s.max_h) + ",";
    else if (s.out_max_h > 0) vf = fit(16384, s.out_max_h) + ",";
    // v3: frame-rate conform with blended in-between frames (D181, PROPOSED).
    if (!still && s.conform_fps > 0) vf += "framerate=fps=" + num(s.conform_fps) + ",";
    vf += kRound4;
    a.insert(a.end(), {"-vf", vf});
    if (still) a.insert(a.end(), {"-frames:v", "1"});
    if (s.kind == Kind::Animation && s.fps > 0) a.insert(a.end(), {"-r", num(s.fps)});
    if (!still) a.insert(a.end(), {"-fps_mode", "cfr"});
    a.insert(a.end(), {"-c:v", "hap", "-format", s.format});
    if (s.chunks > 1) a.insert(a.end(), {"-chunks", std::to_string(s.chunks)});
    if (s.threads > 0) a.insert(a.end(), {"-threads", std::to_string(s.threads)});
    if (s.progress) a.insert(a.end(), {"-progress", "pipe:1", "-nostats"});
    a.insert(a.end(), {"-f", "mov", s.output});
    return a;
}

void ProgressParser::feed(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const char ch = static_cast<char>(p[i]);
        if (ch != '\n' && ch != '\r') {
            if (line_.size() < 256) line_.push_back(ch);
            continue;
        }
        if (line_.rfind("frame=", 0) == 0) frame = std::atoll(line_.c_str() + 6);
        else if (line_.rfind("total_size=", 0) == 0) total_size = std::atoll(line_.c_str() + 11);
        line_.clear();
    }
}

std::vector<std::string> still_png_args(const std::string& input, const std::string& out, int max_w, int max_h) {
    auto a = head();
    a.insert(a.end(), {"-i", input, "-frames:v", "1", "-vf", fit(max_w, max_h), "-f", "image2", "-c:v", "png", out});
    return a;
}

std::vector<std::string> thumb_args(const std::string& hap, const std::string& out, double at) {
    auto a = head();
    a.insert(a.end(), {"-ss", num(std::max(0.0, at)), "-i", hap, "-frames:v", "1", "-vf", "scale=320:-2,format=yuvj420p", "-q:v", "4",
                       "-f", "image2", out});
    return a;
}

std::vector<std::string> strip_args(const std::string& hap, const std::string& out, int64_t frames) {
    const int64_t k = std::max<int64_t>(1, frames / 8);
    auto a = head();
    a.insert(a.end(), {"-i", hap, "-vf", "select='not(mod(n\\," + std::to_string(k) + "))',scale=160:-2,tile=8x1,format=yuvj420p",
                       "-fps_mode", "passthrough", "-frames:v", "1", "-q:v", "5", "-f", "image2", out});
    return a;
}

std::vector<std::string> proxy_args(const std::string& hap, int w, int h, double fps, int threads) {
    auto a = head();
    if (threads > 0) a.insert(a.end(), {"-threads", std::to_string(threads)});
    a.insert(a.end(), {"-i", hap, "-vf",
                       "fps=" + num(fps) + ",scale=" + std::to_string(w) + ":" + std::to_string(h) + ":flags=area",
                       "-pix_fmt", "rgb24", "-f", "rawvideo", "-"});
    return a;
}

// ---- ffprobe JSON ----

double parse_rate(const std::string& r) {
    double n = 0, d = 1;
    if (sscanf_s(r.c_str(), "%lf/%lf", &n, &d) == 2) return d != 0 ? n / d : 0;
    return std::atof(r.c_str());
}

bool pix_fmt_has_alpha(const std::string& p) {
    if (p.empty()) return false;
    if (p == "pal8") return true;  // GIF / PNG palettes may hold transparency
    return p.find("rgba") != std::string::npos || p.find("bgra") != std::string::npos ||
           p.find("argb") != std::string::npos || p.find("abgr") != std::string::npos ||
           p.rfind("yuva", 0) == 0 || p.rfind("ya", 0) == 0 || p.rfind("gbrap", 0) == 0;
}

bool parse_probe(const std::string& text, library::Probe& out, std::string* err) {
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        if (err) *err = "ffprobe output is not JSON";
        return false;
    }
    const auto streams = j.find("streams");
    if (streams == j.end() || !streams->is_array() || streams->empty()) {
        if (err) *err = "no video stream";
        return false;
    }
    const auto& v = (*streams)[0];
    auto str = [](const nlohmann::json& o, const char* k) {
        const auto it = o.find(k);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    out.codec = str(v, "codec_name");
    out.pix_fmt = str(v, "pix_fmt");
    out.width = v.value("width", 0);
    out.height = v.value("height", 0);
    double fps = parse_rate(str(v, "avg_frame_rate"));
    if (fps <= 0 || fps > 240) fps = parse_rate(str(v, "r_frame_rate"));
    out.fps = fps > 0 && fps <= 240 ? fps : 0;
    out.frames = std::atoll(str(v, "nb_frames").c_str());
    out.seconds = std::atof(str(v, "duration").c_str());
    if (out.seconds <= 0)
        if (const auto f = j.find("format"); f != j.end() && f->is_object()) out.seconds = std::atof(str(*f, "duration").c_str());
    if (out.frames <= 0 && out.seconds > 0 && out.fps > 0) out.frames = std::llround(out.seconds * out.fps);
    out.alpha = pix_fmt_has_alpha(out.pix_fmt);
    if (out.width <= 0 || out.height <= 0) {
        if (err) *err = "no picture size";
        return false;
    }
    return true;
}

// ---- running ----

namespace {

using NtProc = LONG(NTAPI*)(HANDLE);
using NtSetInfo = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);

struct Nt {
    NtProc suspend = nullptr, resume = nullptr;
    NtSetInfo set_info = nullptr;
    Nt() {
        if (HMODULE m = GetModuleHandleW(L"ntdll.dll")) {
            suspend = reinterpret_cast<NtProc>(GetProcAddress(m, "NtSuspendProcess"));
            resume = reinterpret_cast<NtProc>(GetProcAddress(m, "NtResumeProcess"));
            set_info = reinterpret_cast<NtSetInfo>(GetProcAddress(m, "NtSetInformationProcess"));
        }
    }
};
const Nt& nt() {
    static const Nt n;
    return n;
}

std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++bs;
        } else if (c == L'"') {
            q.append(bs * 2 + 1, L'\\');
            q += c;
            bs = 0;
        } else {
            q.append(bs, L'\\');
            q += c;
            bs = 0;
        }
    }
    q.append(bs * 2, L'\\');
    return q + L"\"";
}

HANDLE kill_job() {  // every tool dies with himmel_ingest
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (j) SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof(li));
        return j;
    }();
    return job;
}

}  // namespace

RunResult run_tool(const fs::path& exe, const std::vector<std::string>& args, const RunOptions& opt) {
    RunResult res;
    std::wstring cmd = quote(exe.wstring());
    for (const auto& a : args) cmd += L" " + quote(widen(a));

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 20) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
        res.err_tail = "cannot create pipes";
        return res;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (opt.background ? IDLE_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS);
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        CloseHandle(out_r);
        CloseHandle(err_r);
        res.err_tail = "cannot start " + u8path(exe.filename());
        return res;
    }
    res.started = true;
    if (HANDLE job = kill_job()) AssignProcessToJobObject(job, pi.hProcess);
    if (opt.background && nt().set_info) {
        ULONG io = 0;  // IoPriorityVeryLow
        nt().set_info(pi.hProcess, 33 /*ProcessIoPriority*/, &io, sizeof(io));
        ULONG page = 1;  // MEMORY_PRIORITY_VERY_LOW
        nt().set_info(pi.hProcess, 39 /*ProcessPagePriority*/, &page, sizeof(page));
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::thread out_t([&] {
        std::vector<uint8_t> buf(1 << 16);
        DWORD got = 0;
        while (ReadFile(out_r, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0)
            if (opt.on_stdout) opt.on_stdout(buf.data(), got);
    });
    std::thread err_t([&] {
        char buf[1024];
        DWORD got = 0;
        while (ReadFile(err_r, buf, sizeof(buf), &got, nullptr) && got > 0) {
            res.err_tail.append(buf, got);
            if (res.err_tail.size() > 1024) res.err_tail.erase(0, res.err_tail.size() - 1024);
        }
    });

    bool suspended = false;
    const ULONGLONG t0 = GetTickCount64();
    ULONGLONG paused_since = 0, paused_total = 0;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
        if (opt.should_stop && opt.should_stop()) {
            TerminateProcess(pi.hProcess, 1);
            res.stopped = true;
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
        const bool want = opt.should_pause && opt.should_pause();
        if (want != suspended && nt().suspend && nt().resume) {
            if (want) nt().suspend(pi.hProcess), paused_since = GetTickCount64();
            else nt().resume(pi.hProcess), paused_total += GetTickCount64() - paused_since;
            suspended = want;
        }
        const ULONGLONG ran = GetTickCount64() - t0 - paused_total - (suspended ? GetTickCount64() - paused_since : 0);
        if (opt.timeout_ms > 0 && ran > static_cast<ULONGLONG>(opt.timeout_ms)) {
            if (suspended) nt().resume(pi.hProcess);
            TerminateProcess(pi.hProcess, 1);
            res.timed_out = true;
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }
    if (suspended) paused_total += GetTickCount64() - paused_since;
    res.paused_ms = static_cast<double>(paused_total);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exit_code = static_cast<int>(code);
    out_t.join();
    err_t.join();
    CloseHandle(out_r);
    CloseHandle(err_r);
    CloseHandle(pi.hProcess);
    while (!res.err_tail.empty() && (res.err_tail.back() == '\n' || res.err_tail.back() == '\r')) res.err_tail.pop_back();
    return res;
}

}  // namespace himmel::ingest
