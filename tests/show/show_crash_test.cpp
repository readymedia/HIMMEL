// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Kill the engine in the middle of a save -> the autosave brings everything back
// (build-v1.md M3 "done when"). The parent starts this program as a child: the
// child runs the show runtime like the render process does (a frame loop at
// ~120 Hz), builds a big show (200 scenes, so a save takes a while), saves it,
// then keeps changing the live state AND asking for saves back to back. The
// parent kills it with TerminateProcess (no clean-up at all) at a random moment,
// several times, and each time starts a fresh runtime on the same folders and
// checks: the project opens (from the saved files or the autosave), all scenes
// are there, and the live state is the child's last one (within the journal's
// 100 ms step).
#include <cmath>
#include <random>
#include <string>

#include "himmel/show/files.h"
#include "show_test_util.h"

using namespace show_test;
using namespace himmel::show;
namespace F = himmel::show::files;

namespace {

constexpr int kScenes = 200;
constexpr int kTool = static_cast<int>(Producer::Tool);

RuntimeConfig config(const fs::path& root) {
    RuntimeConfig c;
    c.projects_root = root / "Projects";
    c.app_dir = root / "App";
    return c;
}

int child(const fs::path& root) {
    Rig r(make_catalog(16));
    r.start(config(root));
    if (!r.settle()) return 10;
    const bool fresh = himmel::show::scene_count(r.show->show()) == 0;
    if (fresh) {
        // 200 scenes over 13 banks.
        for (int i = 0; i < kScenes; ++i) {
            if (i % 16 == 0) r.send(cmd(CommandType::ShowSelect, kNone, kNone, kNone, static_cast<uint8_t>(SelectWhat::Bank), i / 16));
            r.fill_deck(0, i % 13);
            r.send(cmd(CommandType::SceneSave, 0, kNone, kNone, kNone, i % 16));
        }
        r.show->set_text(kTool, "Crash");
        r.send(cmd(CommandType::Project, kNone, kNone, kNone, static_cast<uint8_t>(ProjectAction::SaveAs)));
        if (!r.settle(20000)) return 11;
    }
    std::printf("READY\n");
    std::fflush(stdout);
    // The live state counts up: layer 1 opacity = k / 4000 on deck B. Every value is printed once it
    // has been on the model for a few frames (as long as the journal needs to see it).
    for (int k = 1; k < 4000; ++k) {
        r.deck(cmd(CommandType::LayerOpacity, 1, 0, kNone, kNone, 0, k / 4000.0f));
        r.deck(cmd(CommandType::LayerClip, 1, 0, kNone, kNone, k % 16));
        for (int f = 0; f < 16; ++f) {  // 128 ms: longer than the journal step (100 ms)
            r.tick(8000);
            Sleep(8);
            if (r.show->worker_idle()) r.send(cmd(CommandType::Project, kNone, kNone, kNone, static_cast<uint8_t>(ProjectAction::Save)));
        }
        std::printf("K %d\n", k);
        std::fflush(stdout);
    }
    return 0;
}

struct Child {
    PROCESS_INFORMATION pi{};
    HANDLE out = nullptr;
    std::string buf;
    int last_k = 0;
    bool ready = false;

    bool start(const fs::path& exe, const fs::path& root) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE w = nullptr;
        if (!CreatePipe(&out, &w, &sa, 1 << 16)) return false;
        SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = w;
        si.hStdError = w;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        std::wstring cmdline = L"\"" + exe.wstring() + L"\" --child \"" + root.wstring() + L"\"";
        const BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        CloseHandle(w);
        return ok != 0;
    }
    void pump() {
        DWORD avail = 0;
        while (PeekNamedPipe(out, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            char tmp[4096];
            DWORD n = 0;
            if (!ReadFile(out, tmp, std::min<DWORD>(avail, sizeof(tmp)), &n, nullptr) || !n) break;
            buf.append(tmp, n);
        }
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            const std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.rfind("READY", 0) == 0) ready = true;
            else if (line.rfind("K ", 0) == 0) last_k = std::atoi(line.c_str() + 2);
            else if (!line.empty()) std::printf("  child: %s\n", line.c_str());
        }
    }
    void kill() {
        TerminateProcess(pi.hProcess, 99);
        WaitForSingleObject(pi.hProcess, 5000);
        pump();
        CloseHandle(pi.hProcess), CloseHandle(pi.hThread), CloseHandle(out);
    }
};

bool tmp_files_in(const fs::path& folder) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".tmp") return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--child") return child(F::from_utf8(argv[2]));

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path root = temp_root("crash");
    std::mt19937 rng(1234);
    const int kRuns = 6;
    int mid_save = 0;
    for (int run = 0; run < kRuns; ++run) {
        Child c;
        if (!c.start(self, root)) {
            std::printf("cannot start the child\n");
            return 2;
        }
        // Wait for READY (the first run builds and saves the 200-scene show).
        for (int i = 0; i < 60000 && !c.ready; ++i) {
            c.pump();
            if (WaitForSingleObject(c.pi.hProcess, 0) == WAIT_OBJECT_0) break;
            Sleep(1);
        }
        CHECK(c.ready);
        // Let it change + save for a random 0.4..1.5 s, then pull the plug.
        const int ms = 400 + static_cast<int>(rng() % 1100);
        for (int i = 0; i < ms; ++i) c.pump(), Sleep(1);
        c.kill();
        const fs::path folder = root / "Projects" / "Crash.himmel";
        const bool cut = tmp_files_in(folder);
        mid_save += cut ? 1 : 0;

        // "The engine restarts": a fresh runtime on the same folders.
        Rig r(make_catalog(16));
        r.start(config(root));
        CHECK(r.settle(20000));
        const ShowData& s = r.show->show();
        const int scenes = himmel::show::scene_count(s);
        const FilesStatus f = r.show->files();
        const float op = r.model->live().deck[1].layer[0].opacity;
        const int k = static_cast<int>(std::lround(op * 4000.0f));
        std::printf("run %d: killed after %d ms%s; reopened %d scenes, restored %u, live k = %d (child printed %d)\n", run, ms,
                    cut ? " during a save (.tmp left)" : "", scenes, f.restored, k, c.last_k);
        CHECK(scenes == kScenes);
        CHECK(std::strcmp(s.name, "Crash") == 0);
        // Nothing older than what the child had shown for a few frames; never ahead of the child's next value.
        CHECK(k >= c.last_k && k <= c.last_k + 1);
        CHECK(r.model->live().deck[1].layer[0].clip == k % 16);
        r.show->stop();
    }
    std::printf("show_crash_test: %d runs, %d killed in the middle of a write; %d checks, %d failed\n", kRuns, mid_save, g_checks,
                g_failed);
    return g_failed ? 1 : 0;
}
