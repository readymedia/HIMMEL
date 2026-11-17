// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the library (M7): hash, analysis v1 on synthetic proxies,
// sidecars, the SQLite index, the watch-folder scan, throttling, ffmpeg
// command lines and the engine's library feed. No hardware, no ffmpeg needed
// (one round trip through ffmpeg.exe runs only when it is on PATH).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

#include "himmel/ingest/ffmpeg.h"
#include "himmel/ingest/guard.h"
#include "himmel/ingest/library_move.h"
#include "himmel/ingest/ingest_ops.h"
#include "himmel/ingest/scan.h"
#include "himmel/ingest/selftest.h"
#include "himmel/ingest/svg.h"
#include "himmel/ingest/throttle.h"
#include "himmel/library/analysis.h"
#include "himmel/library/clip_table.h"
#include "himmel/library/feed.h"
#include "himmel/library/hash.h"
#include "himmel/library/library_db.h"
#include "himmel/library/location.h"
#include "himmel/library/sidecar.h"

using namespace himmel;
using namespace himmel::library;
namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static fs::path temp_dir(const char* name) {
    const fs::path p = fs::temp_directory_path() / "himmel_library_tests" / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

static void write_file(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::binary) << data;
}

// ---- synthetic proxies ----
struct Rgb { uint8_t r, g, b; };
static ProxyClip make_clip(int frames, int w, int h, float fps, double hap_fps, const std::function<Rgb(int f, int x, int y)>& px) {
    ProxyClip c;
    c.w = w, c.h = h, c.fps = fps, c.hap_fps = hap_fps;
    c.hap_frames = static_cast<int64_t>(std::llround(frames * hap_fps / fps));
    c.rgb.resize(static_cast<size_t>(frames) * w * h * 3);
    for (int f = 0; f < frames; ++f)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const Rgb v = px(f, x, y);
                uint8_t* p = &c.rgb[((static_cast<size_t>(f) * h + y) * w + x) * 3];
                p[0] = v.r, p[1] = v.g, p[2] = v.b;
            }
    return c;
}

static uint32_t chan(uint32_t rgb, int k) { return (rgb >> (16 - 8 * k)) & 255; }

static void test_hash() {
    const fs::path d = temp_dir("hash");
    std::string big(5 << 20, 'x');
    for (size_t i = 0; i < big.size(); i += 4096) big[i] = static_cast<char>(i / 4096);
    write_file(d / "a.bin", big);
    write_file(d / "renamed.bin", big);
    big[(5 << 20) - 1] = 'y';
    write_file(d / "c.bin", big);
    write_file(d / "small.bin", "hello");
    const std::string a = hash_file(d / "a.bin");
    CHECK(a.size() == 16);
    CHECK(a == hash_file(d / "renamed.bin"));  // stable across renames
    CHECK(a != hash_file(d / "c.bin"));        // the end sample changed
    CHECK(hash_file(d / "small.bin").size() == 16);
    CHECK(hash_file(d / "missing.bin").empty());
    CHECK(hex16(0x1234) == "0000000000001234");
    const std::vector<fs::path> seq = {d / "a.bin", d / "c.bin"};
    CHECK(hash_sequence(seq).size() == 16);
    CHECK(hash_sequence(seq) != hash_sequence({d / "a.bin", d / "renamed.bin"}));
}

static void test_judder() {
    CHECK(judder_for(25).size() == 1 && judder_for(25)[0] == 60);
    CHECK(judder_for(30).size() == 1 && judder_for(30)[0] == 50);
    CHECK(judder_for(50).size() == 1 && judder_for(50)[0] == 60);
    CHECK(judder_for(60).size() == 1 && judder_for(60)[0] == 50);
    CHECK(judder_for(24).size() == 2);
    CHECK(judder_for(0).empty());  // stills
    CHECK(judder_for(12.5).size() == 1 && judder_for(12.5)[0] == 60);
}

static void test_analysis() {
    // Half red, half blue, still: palette, loop, slices, flicker, motion.
    {
        auto c = make_clip(50, 64, 36, 12.5f, 25.0, [](int, int x, int) { return x < 32 ? Rgb{220, 20, 20} : Rgb{20, 30, 220}; });
        const Analysis a = analyse(c);
        CHECK(a.version == kAnalysisVersion);
        CHECK(a.palette.size() >= 2);
        if (a.palette.size() >= 2) {
            CHECK(std::fabs(a.palette[0].weight - 0.5f) < 0.05f);
            const uint32_t p0 = a.palette[0].rgb, p1 = a.palette[1].rgb;
            const bool red_blue = (chan(p0, 0) > 180 && chan(p1, 2) > 180) || (chan(p1, 0) > 180 && chan(p0, 2) > 180);
            CHECK(red_blue);
        }
        CHECK(a.motion < 0.01f);
        CHECK(a.cuts.empty());
        CHECK(a.loop_quality > 0.99f);
        CHECK(a.loop_style == "loopable");
        CHECK(!a.needs_crossfade);
        CHECK(a.slices.size() == 16);
        CHECK(a.slices.front() == 0);
        CHECK(a.flicker == "ok");
        CHECK(a.black_frames == 0);
        CHECK(a.saturation > 0.7f);
        CHECK(a.energy >= 1 && a.energy <= 5);
        CHECK(a.mood_chill > a.mood_crazy);
        CHECK(a.natural_beats == 8);  // 4 s at 120 BPM
        CHECK(a.bars_at_120 == 2);
        CHECK(static_cast<int>(a.motion_curve.size()) == 50);
    }
    // Warm vs cool.
    {
        auto warm = make_clip(4, 32, 18, 12.5f, 25.0, [](int, int, int) { return Rgb{240, 150, 30}; });
        auto cool = make_clip(4, 32, 18, 12.5f, 25.0, [](int, int, int) { return Rgb{30, 90, 230}; });
        CHECK(analyse(warm).warmth > 0.3f);
        CHECK(analyse(cool).warmth < -0.3f);
        const Analysis w = analyse(warm);
        CHECK(std::abs(static_cast<int>(chan(w.avg_rgb, 0)) - 240) <= 1);
    }
    // One hard cut at proxy frame 20 (hap frame 40), a few noisy frames on each side.
    {
        auto c = make_clip(40, 64, 36, 12.5f, 25.0, [](int f, int x, int y) {
            const uint8_t n = static_cast<uint8_t>((x * 7 + y * 13 + f * 3) % 20);
            return f < 20 ? Rgb{static_cast<uint8_t>(200 + n), 40, 40} : Rgb{30, static_cast<uint8_t>(180 + n), 60};
        });
        const Analysis a = analyse(c);
        CHECK(a.cuts.size() == 1);
        if (!a.cuts.empty()) CHECK(a.cuts[0] == 40);
        CHECK(a.cuts_per_min > 0);
        CHECK(a.loop_quality < 0.5f);  // first and last frames differ
        CHECK(a.loop_style == "one-shot");
    }
    // Five cuts -> slices from the cuts.
    {
        auto c = make_clip(60, 32, 18, 12.5f, 25.0, [](int f, int, int) {
            const int s = f / 10;
            return Rgb{static_cast<uint8_t>(s * 50), static_cast<uint8_t>(250 - s * 50), static_cast<uint8_t>((s % 2) * 200)};
        });
        const Analysis a = analyse(c);
        CHECK(a.cuts.size() == 5);
        CHECK(a.slices.size() == 6);
        if (a.slices.size() == 6) CHECK(a.slices[1] == 20 && a.slices[5] == 100);
    }
    // A pattern moving right 1 px per frame: motion, direction ~0 degrees.
    {
        auto c = make_clip(30, 64, 36, 12.5f, 25.0, [](int f, int x, int y) {
            const int v = ((x - f) & 7) < 4 ? 230 : 20;
            return Rgb{static_cast<uint8_t>(v), static_cast<uint8_t>((y * 5) & 255), static_cast<uint8_t>(v)};
        });
        const Analysis a = analyse(c);
        CHECK(a.motion > 0.2f);
        CHECK(a.motion_dir < 20.0f || a.motion_dir > 340.0f);
        CHECK(a.cuts.empty());
        CHECK(a.steadiness > 0.5f);
    }
    // Black / white every frame at 12.5 fps: way over 3 flashes / s.
    {
        auto c = make_clip(25, 16, 9, 12.5f, 25.0, [](int f, int, int) { return f % 2 ? Rgb{255, 255, 255} : Rgb{0, 0, 0}; });
        const Analysis a = analyse(c);
        CHECK(a.flicker == "bad");
        CHECK(a.black_frames == 13);
        CHECK(a.mood_crazy > a.mood_chill);
    }
    // A slow blink (one change a second) is fine.
    {
        auto c = make_clip(50, 16, 9, 12.5f, 25.0, [](int f, int, int) { return (f / 13) % 2 ? Rgb{200, 200, 200} : Rgb{60, 60, 60}; });
        CHECK(analyse(c).flicker == "ok");
    }
    // A still (1 frame).
    {
        auto c = make_clip(1, 32, 18, 12.5f, 25.0, [](int, int x, int) { return Rgb{static_cast<uint8_t>(x * 8), 100, 100}; });
        c.hap_frames = 1;
        const Analysis a = analyse(c);
        CHECK(a.loop_quality == 1.0f);
        CHECK(a.slices.size() == 1 && a.slices[0] == 0);
        CHECK(a.motion == 0.0f);
        CHECK(a.natural_beats == 1);
        CHECK(a.bars_at_120 == 0);
    }
    // Pieces.
    {
        std::vector<float> a(64 * 36, 0.5f), b(64 * 36, 0.5f);
        CHECK(std::fabs(ssim(a, b, 64, 36) - 1.0f) < 1e-4f);
        for (size_t i = 0; i < b.size(); ++i) b[i] = (i % 3) ? 0.9f : 0.1f;
        CHECK(ssim(a, b, 64, 36) < 0.5f);
        CHECK(pow2_round(15) == 16 && pow2_round(0.3) == 1 && pow2_round(5) == 4 && pow2_round(100) == 128);
    }
    // Empty input does not crash.
    {
        ProxyClip c;
        CHECK(analyse(c).version == kAnalysisVersion);
    }
}

static ClipRecord sample_record(const std::string& hash, const std::string& name) {
    ClipRecord r;
    r.hash = hash;
    r.kind = Kind::Video;
    r.name = name;
    r.source = "C:\\clips\\" + name + ".mp4";
    r.state = State::Ready;
    r.stage_done = 15;
    r.play_path = "hap/" + hash + ".mov";
    r.thumb_path = "thumbs/" + hash + ".jpg";
    r.probe = {"h264", "yuv420p", 1920, 1080, 25.0, 750, 30.0, false};
    r.codec = "hap";
    r.width = 1920, r.height = 1080, r.chunks = 8, r.fps = 25.0, r.frames = 750, r.seconds = 30.0;
    r.judder = {60};
    auto c = make_clip(20, 32, 18, 12.5f, 25.0, [](int f, int x, int) { return Rgb{static_cast<uint8_t>(x * 8), static_cast<uint8_t>(f * 10), 90}; });
    r.analysis = analyse(c);
    r.added_unix = 1794931331;
    r.ready_unix = 1794931360;
    r.stars = 3;
    r.user_tags = {"sunset", "warm"};
    return r;
}

static void test_sidecar() {
    const ClipRecord r = sample_record("9f2c0a1b33d4e5f6", "Sunset");
    const std::string text = sidecar_to_json(r);
    CHECK(text.find("\"himmel-clip/1\"") != std::string::npos);
    ClipRecord b;
    std::string err;
    CHECK(sidecar_from_json(text, b, &err));
    CHECK(b.hash == r.hash && b.name == r.name && b.kind == r.kind && b.source == r.source);
    CHECK(b.play_path == r.play_path && b.codec == "hap" && b.width == 1920 && b.frames == 750);
    CHECK(b.probe.codec == "h264" && b.probe.fps == 25.0);
    CHECK(b.judder == r.judder);
    CHECK(b.analysis.version == r.analysis.version);
    CHECK(b.analysis.palette.size() == r.analysis.palette.size());
    if (!b.analysis.palette.empty()) CHECK(b.analysis.palette[0].rgb == r.analysis.palette[0].rgb);
    CHECK(b.analysis.motion_curve.size() == r.analysis.motion_curve.size());
    CHECK(b.analysis.slices == r.analysis.slices);
    CHECK(b.analysis.loop_style == r.analysis.loop_style);
    CHECK(b.analysis.avg_rgb == r.analysis.avg_rgb);
    CHECK(b.added_unix == r.added_unix && b.ready_unix == r.ready_unix);
    CHECK(b.stars == 3 && b.user_tags.size() == 2);
    CHECK(!sidecar_from_json("{\"format\":\"other\"}", b, &err));
    CHECK(!sidecar_from_json("not json", b, &err));
    CHECK(rgb_hex(0xd0703a) == "#d0703a" && hex_rgb("#d0703a") == 0xd0703a);
    CHECK(parse_iso_utc(iso_utc(1794931331)) == 1794931331);
    const fs::path d = temp_dir("sidecar");
    CHECK(write_sidecar(d / "meta" / "x.json", r));
    CHECK(read_sidecar(d / "meta" / "x.json", b) && b.name == "Sunset");
    CHECK(!fs::exists(d / "meta" / "x.json.tmp"));
}

static void test_db() {
    const fs::path d = temp_dir("db");
    LibraryDb db;
    std::string err;
    CHECK(db.open(d / kDbFile, false, &err));
    ClipRecord a = sample_record("aaaaaaaaaaaaaaaa", "Sunset");
    a.state = State::Queued;
    CHECK(db.upsert(a) && a.id > 0);
    ClipRecord b = sample_record("bbbbbbbbbbbbbbbb", "sunset");  // same name, other case
    CHECK(db.upsert(b));
    CHECK(b.name == "sunset (2)");
    CHECK(db.count() == 2);
    CHECK(db.count(static_cast<int>(State::Ready)) == 1);
    ClipRecord g;
    CHECK(db.find_by_hash("aaaaaaaaaaaaaaaa", g) && g.id == a.id && g.state == State::Queued);
    CHECK(g.analysis.motion_curve.size() == a.analysis.motion_curve.size());  // through the stored sidecar
    CHECK(g.analysis.avg_rgb == a.analysis.avg_rgb);
    a.state = State::Ready;
    a.stars = 5;
    CHECK(db.upsert(a));
    CHECK(db.get(a.id, g) && g.state == State::Ready && g.stars == 5 && g.name == "Sunset");
    const auto ready = db.list(static_cast<int>(State::Ready));
    CHECK(ready.size() == 2 && ready[0].id < ready[1].id);
    CHECK(db.list(static_cast<int>(State::Ready), a.id).size() == 1);
    CHECK(db.ready_seq() == 0);
    db.bump_ready_seq();
    db.bump_ready_seq();
    CHECK(db.ready_seq() == 2);
    std::string h;
    CHECK(!db.cached_hash("C:\\x.mp4", 10, 20, h));
    db.cache_hash("C:\\x.mp4", 10, 20, "cccccccccccccccc");
    CHECK(db.cached_hash("C:\\x.mp4", 10, 20, h) && h == "cccccccccccccccc");
    CHECK(!db.cached_hash("C:\\x.mp4", 11, 20, h));  // the file changed

    // A reader sees the writer's rows (WAL), and cannot write.
    {
        LibraryDb ro;
        CHECK(ro.open(d / kDbFile, true, &err));
        CHECK(ro.count(static_cast<int>(State::Ready)) == 2);
        ClipRecord x = sample_record("dddddddddddddddd", "X");
        CHECK(!ro.upsert(x));
    }
    {
        LibraryDb missing;
        CHECK(!missing.open(d / "nope.db", true, &err));
    }

    // Rebuild from the sidecars: only clips whose play file exists.
    write_sidecar(d / "meta" / (a.hash + ".json"), a);
    write_sidecar(d / "meta" / (b.hash + ".json"), b);
    write_file(d / a.play_path, "x");  // b's play file is missing
    CHECK(db.rebuild_from_sidecars(d) == 1);
    CHECK(db.count() == 1);
    CHECK(db.find_by_hash(a.hash, g) && g.state == State::Ready && g.stars == 5);
}

static void test_scan() {
    const fs::path d = temp_dir("scan");
    write_file(d / "clip one.mp4", "v");
    write_file(d / "logo.PNG", "p");
    write_file(d / "art.svg", "<svg/>");
    write_file(d / "loop.gif", "g");
    write_file(d / "notes.txt", "t");
    write_file(d / "~temp.mov", "t");
    for (int i = 1; i <= 5; ++i) {
        char n[32];
        std::snprintf(n, sizeof(n), "shot_%04d.png", i);
        write_file(d / "Smoke" / n, "s");
    }
    write_file(d / "Smoke" / "cover.jpg", "c");
    write_file(d / "skipme" / "x.mp4", "x");
    const auto c = ingest::scan_folder(d, {d / "skipme"});
    int videos = 0, stills = 0, vectors = 0, anims = 0, seqs = 0;
    for (const auto& x : c) {
        switch (x.kind) {
            case Kind::Video: ++videos; break;
            case Kind::Still: ++stills; break;
            case Kind::Vector: ++vectors; break;
            case Kind::Animation: ++anims; break;
            case Kind::Sequence:
                ++seqs;
                CHECK(x.name == "Smoke");
                CHECK(x.seq_pattern == "shot_%04d.png");
                CHECK(x.seq_start == 1);
                CHECK(x.seq_files.size() == 5);
                CHECK(x.size == 5);
                break;
        }
    }
    CHECK(videos == 1 && stills == 2 && vectors == 1 && anims == 1 && seqs == 1);  // logo.PNG + Smoke/cover.jpg
    for (const auto& x : c)
        if (x.kind == Kind::Video) CHECK(x.name == "clip one");

    // Unpadded numbers; the longest run wins; a single numbered file is no sequence.
    ingest::Candidate s;
    CHECK(ingest::find_sequence({"f/a1.png", "f/a2.png", "f/a10.png", "f/b1.jpg"}, s));
    CHECK(s.seq_pattern == "a%d.png" && s.seq_start == 1 && s.seq_files.size() == 3 && s.seq_files.back() == fs::path("f/a10.png"));
    CHECK(!ingest::find_sequence({"f/a1.png", "f/b.png"}, s));

    Kind k;
    CHECK(ingest::kind_for_extension(".mov", k) && k == Kind::Video);
    CHECK(ingest::kind_for_extension(".webp", k) && k == Kind::Still);
    CHECK(!ingest::kind_for_extension(".txt", k));

    ingest::SettleTracker t(2000);
    ingest::Candidate f;
    f.path = "x.mp4", f.size = 10, f.mtime = 5;
    CHECK(!t.settled(f, 0));
    CHECK(!t.settled(f, 1500));
    CHECK(t.settled(f, 2100));
    f.size = 20;  // still being copied
    CHECK(!t.settled(f, 2200));
    CHECK(t.settled(f, 4300));
}

static void test_throttle() {
    ingest::Throttle t;
    ingest::EngineSample s;
    t.update(s, 0);
    CHECK(!t.show_running() && !t.paused());
    s.present = true, s.heartbeat_age_ms = 5000;  // a dead renderer is no show
    t.update(s, 100);
    CHECK(!t.show_running());
    s.heartbeat_age_ms = 10, s.frames = 100, s.late_frames = 3;
    t.update(s, 200);
    CHECK(t.show_running() && !t.paused());  // old late frames do not count
    s.frames = 106;
    t.update(s, 300);
    CHECK(!t.paused());
    s.frames = 112, s.late_frames = 4;
    t.update(s, 400);
    CHECK(t.paused() && t.pauses() == 1);
    s.frames = 118;
    t.update(s, 1500);
    CHECK(t.paused());
    t.update(s, 2401);
    CHECK(!t.paused());
    s.frames = 5, s.late_frames = 0;  // a new renderer
    t.update(s, 2500);
    CHECK(!t.paused());
}

static void test_ffmpeg_args() {
    auto has = [](const std::vector<std::string>& v, const std::string& a, const std::string& b) {
        for (size_t i = 0; i + 1 < v.size(); ++i)
            if (v[i] == a && v[i + 1] == b) return true;
        return false;
    };
    auto index = [](const std::vector<std::string>& v, const std::string& a) {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == a) return static_cast<int>(i);
        return -1;
    };
    ingest::ConvertSpec s;
    s.input = "in.mp4", s.output = "out.mov", s.format = "hap_alpha", s.chunks = 8, s.threads = 2;
    auto a = ingest::convert_args(s);
    CHECK(has(a, "-c:v", "hap") && has(a, "-format", "hap_alpha") && has(a, "-chunks", "8") && has(a, "-threads", "2"));
    CHECK(has(a, "-an", "-sn") && has(a, "-fps_mode", "cfr") && a.back() == "out.mov");
    CHECK(a[static_cast<size_t>(index(a, "-vf") + 1)].find("trunc(iw/4)*4") != std::string::npos);
    s.kind = Kind::Still;
    a = ingest::convert_args(s);
    CHECK(has(a, "-frames:v", "1") && index(a, "-fps_mode") < 0);
    CHECK(a[static_cast<size_t>(index(a, "-vf") + 1)].find("min(iw,3840)") != std::string::npos);
    s.kind = Kind::Sequence, s.fps = 30, s.seq_start = 7, s.input = "Smoke/shot_%04d.png";
    a = ingest::convert_args(s);
    CHECK(has(a, "-framerate", "30") && has(a, "-start_number", "7") && index(a, "-framerate") < index(a, "-i"));
    s.kind = Kind::Animation, s.fps = 10;
    a = ingest::convert_args(s);
    CHECK(has(a, "-r", "10"));
    a = ingest::proxy_args("x.mov", 64, 36, 12.5, 1);
    CHECK(has(a, "-pix_fmt", "rgb24") && has(a, "-f", "rawvideo") && a.back() == "-");
    CHECK(has(a, "-vf", "fps=12.5,scale=64:36:flags=area"));
    a = ingest::strip_args("x.mov", "s.jpg", 800);
    CHECK(a[static_cast<size_t>(index(a, "-vf") + 1)].find("mod(n\\,100)") != std::string::npos);

    CHECK(std::fabs(ingest::parse_rate("30000/1001") - 29.97) < 0.01);
    CHECK(ingest::parse_rate("0/0") == 0);
    CHECK(ingest::pix_fmt_has_alpha("yuva420p") && ingest::pix_fmt_has_alpha("rgba") && !ingest::pix_fmt_has_alpha("yuv420p"));
    Probe p;
    std::string err;
    const char* json = R"({"streams":[{"codec_name":"h264","width":1920,"height":1080,"pix_fmt":"yuv420p",
        "avg_frame_rate":"25/1","r_frame_rate":"25/1","nb_frames":"750","duration":"30.000000"}],"format":{"duration":"30.0"}})";
    CHECK(ingest::parse_probe(json, p, &err));
    CHECK(p.codec == "h264" && p.width == 1920 && p.fps == 25.0 && p.frames == 750 && !p.alpha);
    const char* gif = R"({"streams":[{"codec_name":"gif","width":200,"height":100,"pix_fmt":"bgra",
        "avg_frame_rate":"100/9","r_frame_rate":"100/9"}],"format":{"duration":"2.0"}})";
    CHECK(ingest::parse_probe(gif, p, &err));
    CHECK(p.alpha && std::fabs(p.fps - 11.11) < 0.01 && p.frames == 22);
    CHECK(!ingest::parse_probe(R"({"streams":[]})", p, &err));
}

static void test_feed() {
    const fs::path d = temp_dir("feed");
    {
        LibraryDb db;
        CHECK(db.open(d / kDbFile, false));
        ClipRecord a = sample_record("aaaaaaaaaaaaaaaa", "Intro");
        ClipRecord b = sample_record("bbbbbbbbbbbbbbbb", "Smoke");
        b.codec = "hap_alpha";
        ClipRecord q = sample_record("cccccccccccccccc", "Half");
        q.state = State::Converting;  // never offered
        ClipRecord gone = sample_record("eeeeeeeeeeeeeeee", "Gone");  // ready, but its file is missing
        db.upsert(a), db.upsert(b), db.upsert(q), db.upsert(gone);
        write_file(d / a.play_path, "x");
        write_file(d / b.play_path, "x");
    }
    auto table = std::make_unique<ClipTable>();
    protocol::ClipInfo media{};
    std::snprintf(media.name, sizeof(media.name), "intro");  // a media/ clip with the same name
    CHECK(table->append("C:/media/intro.mov", media) == 0);
    LibraryFeed feed;
    CHECK(feed.load_initial(d, *table) == 2);
    CHECK(table->size() == 3);
    CHECK(std::string(table->info[1].name) == "Intro (lib)");
    CHECK(std::string(table->info[2].name) == "Smoke");
    CHECK(table->info[1].codec == 0 && table->info[2].codec == 1);
    CHECK(table->info[1].width == 1920 && std::fabs(table->info[1].fps - 25.0f) < 1e-4f);
    CHECK(table->library_id[1] > 0 && table->library_id[0] == 0);
    CHECK(table->path[2].find("bbbbbbbbbbbbbbbb.mov") != std::string::npos);
    CHECK(feed.skipped.load() == 1);

    // The worker picks up a clip that turns ready later.
    feed.start(100);
    {
        LibraryDb db;
        CHECK(db.open(d / kDbFile, false));
        ClipRecord q;
        CHECK(db.find_by_hash("cccccccccccccccc", q));
        write_file(d / q.play_path, "x");
        q.state = State::Ready;
        db.upsert(q);
    }
    for (int i = 0; i < 50 && table->size() < 4; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    feed.stop();
    CHECK(table->size() == 4);
    CHECK(std::string(table->info[3].name) == "Half");
    CHECK(feed.added.load() == 1);

    // A full table refuses politely.
    auto full = std::make_unique<ClipTable>();
    for (int i = 0; i < ClipTable::kCapacity; ++i) full->append("x", media);
    CHECK(full->append("y", media) == -1);
    CHECK(full->size() == static_cast<uint32_t>(ClipTable::kCapacity));
    LibraryFeed feed2;
    CHECK(feed2.load_initial(d, *full) == 0);
    CHECK(feed2.catalog_full.load() == 3);  // the 3 ready clips with files stay in the library
}

// Optional: one real round trip through ffmpeg.exe (skipped without it).
static void test_ffmpeg_round_trip() {
    ingest::Tools tools;
    if (!tools.find()) {
        std::printf("  (ffmpeg not on PATH: round trip skipped)\n");
        return;
    }
    const fs::path d = temp_dir("ffmpeg");
    const std::string src = ingest::u8path(d / "src.mp4"), hap = ingest::u8path(d / "out.mov");
    ingest::RunOptions o;
    auto r = ingest::run_tool(tools.ffmpeg,
                              {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=130x74:rate=25:duration=1",
                               "-pix_fmt", "yuv420p", src},
                              o);
    CHECK(r.ok());
    std::string out;
    o.on_stdout = [&](const uint8_t* p, size_t n) { out.append(reinterpret_cast<const char*>(p), n); };
    r = ingest::run_tool(tools.ffprobe, ingest::probe_args(src), o);
    Probe p;
    CHECK(r.ok() && ingest::parse_probe(out, p) && p.width == 130 && p.height == 74);
    ingest::ConvertSpec s;
    s.input = src, s.output = hap, s.chunks = 2;
    r = ingest::run_tool(tools.ffmpeg, ingest::convert_args(s), {});
    CHECK(r.ok());
    out.clear();
    o.on_stdout = [&](const uint8_t* q, size_t n) { out.append(reinterpret_cast<const char*>(q), n); };
    r = ingest::run_tool(tools.ffprobe, ingest::probe_args(hap), o);
    CHECK(r.ok() && ingest::parse_probe(out, p) && p.codec == "hap" && p.width == 128 && p.height == 72);
    // The proxy stream is exactly frames * w * h * 3 bytes.
    ProxyClip px;
    px.w = 16, px.h = 9, px.fps = 12.5f;
    o.on_stdout = [&](const uint8_t* q, size_t n) { px.rgb.insert(px.rgb.end(), q, q + n); };
    r = ingest::run_tool(tools.ffmpeg, ingest::proxy_args(hap, 16, 9, 12.5, 1), o);
    CHECK(r.ok() && px.frames() >= 12 && px.frames() <= 14 && px.rgb.size() % (16 * 9 * 3) == 0);
    // Pause / resume: a suspended ffmpeg still finishes.
    int polls = 0;
    o.on_stdout = nullptr;
    o.should_pause = [&] { return ++polls < 6; };
    r = ingest::run_tool(tools.ffmpeg, ingest::thumb_args(hap, ingest::u8path(d / "t.jpg"), 0.1), o);
    CHECK(r.ok() && r.paused_ms > 100 && fs::exists(d / "t.jpg"));
    // A failing input reports stderr.
    r = ingest::run_tool(tools.ffmpeg, {"-hide_banner", "-loglevel", "error", "-i", ingest::u8path(d / "nope.mp4"), "-f", "null", "-"}, {});
    CHECK(!r.ok() && !r.err_tail.empty());

    // SVG through Direct2D.
    write_file(d / "art.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 100"><rect x="10" y="10" width="180" height="80" fill="#f80"/></svg>)");
    std::string err;
    int w = 0, h = 0;
    CHECK(ingest::rasterize_svg(d / "art.svg", d / "art.png", 216, 4000, &err, &w, &h));
    CHECK(w == 432 && h == 216);
    CHECK(fs::exists(d / "art.png") && fs::file_size(d / "art.png") > 100);
    if (!err.empty()) std::printf("  svg: %s\n", err.c_str());
}

// Ingest commands 60..67 (library-db.md §10): every command through the real transport.
static void test_ingest_commands() {
    const fs::path d = temp_dir("commands");
    int lines = 0, fails = 0;
    const int f = ingest::run_command_selftest(d / "lib", [&](const std::string& line) {
        ++lines;
        if (line.rfind("  FAIL", 0) == 0) ++fails, std::printf("%s\n", line.c_str());
    });
    g_checks += lines;
    g_failed += f;
    CHECK(f == fails && lines > 50);
    CHECK(!fs::exists(d / "lib"));  // the self-test cleans up

    // v2 DB rules: ready_order is kept through re-analysis / removal; "removed" round-trips.
    LibraryDb db;
    CHECK(db.open(d / "x.db", false));
    ClipRecord r;
    r.hash = "0123456789abcdef", r.name = "r", r.source = "C:\\r.mp4", r.state = State::Ready, r.added_unix = 1;
    CHECK(db.upsert(r) && r.ready_order == 1);
    r.state = State::Analysing;
    CHECK(db.upsert(r) && r.ready_order == 1);
    r.state = State::Removed;
    r.source_missing = true;
    CHECK(db.upsert(r));
    ClipRecord g;
    CHECK(db.get(r.id, g) && g.state == State::Removed && g.source_missing && g.ready_order == 1);
    CHECK(db.list_ready_after(0).empty());
    CHECK(std::string(state_name(State::Removed)) == "removed");
    CHECK(db.meta_set("watch", "[\"C:\\a\"]") && db.meta_get("watch") == "[\"C:\\a\"]");
    // The block layout the GUI builds on.
    static_assert(sizeof(IngestCommand) == 1024 && sizeof(IngestReply) == 128 && kLibraryVersion == 3);
    IngestCommand c;
    std::string long_path(1200, 'a');
    CHECK(!make_ingest(c, IngestCommandType::Import, 0, long_path.c_str()));
    CHECK(make_ingest(c, IngestCommandType::Remove, 7) && c.type == 62 && c.clip == 7 && c.path[0] == 0);
}

// himmel_ingest's command line (wave 6): an unknown / incomplete argument or --help prints
// the usage and exits 2, at once, and never creates a library (also not the default one).
static int run_ingest(const std::string& args, std::string& out) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path exe = fs::path(self).parent_path() / "himmel_ingest.exe";
    out.clear();
    const std::string cmd = "\"\"" + ingest::u8path(exe) + "\" " + args + " 2>&1\"";
    FILE* p = _popen(cmd.c_str(), "r");
    if (!p) return -1;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    return _pclose(p);
}

static void test_ingest_cli() {
    const fs::path d = temp_dir("cli");
    const std::string lib = "\"" + ingest::u8path(d / "lib") + "\"";
    // A worker that started would never exit by itself, so "returns in < 10 s with code 2" proves
    // nothing was started; the temp --library proves nothing was created. (The default library is
    // not compared: the owner's own himmel_ingest may be writing it while the tests run.)
    std::error_code ec;
    const bool def_existed = fs::exists(default_library_root(), ec);
    std::string out;
    for (const std::string& args : std::vector<std::string>{"--bogus", "--help", "--library", "--library " + lib + " --bogus",
                                   "--library " + lib + " --mode turbo", "--library " + lib + " --canvas 12",
                                   "--library " + lib + " --quality best", "--library " + lib + " --watch",
                                   "--library --once", "-x " + lib}) {
        const auto t0 = std::chrono::steady_clock::now();
        const int rc = run_ingest(args, out);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));  // nothing started
        if (rc != 2 || out.find("usage: himmel_ingest") == std::string::npos)
            std::printf("  himmel_ingest %s -> %d\n%s", args.c_str(), rc, out.c_str());
        CHECK(rc == 2);
        CHECK(out.find("usage: himmel_ingest") != std::string::npos);
        CHECK(!fs::exists(d / "lib", ec));  // no library created
    }
    CHECK(run_ingest("--bogus", out) == 2 && out.find("unknown argument --bogus") != std::string::npos);
    CHECK(fs::exists(default_library_root(), ec) == def_existed);  // never created by a bad command line
    // --send with no verb prints its own usage and exits 2 too (it never starts a worker).
    CHECK(run_ingest("--send", out) == 2 && out.find("usage") != std::string::npos);
}

// ---- v3 (wave 7): disk guard maths, options, progress, plain errors, location, move ----
static void test_v3_guard() {
    using namespace himmel::ingest;
    // The reserve: max(10 GB, 10 % of the disk), or the override.
    CHECK(reserve_for(100 * kGB) == 10 * kGB);
    CHECK(reserve_for(2000 * kGB) == 200 * kGB);
    CHECK(reserve_for(2000 * kGB, 5 * kGB) == 5 * kGB);
    // The owner's case (2026-11-17): 7.3 GB free, 17 GB needed -> refused.
    CHECK(!fits(static_cast<uint64_t>(7.3 * kGB), 0, 17 * kGB, 10 * kGB));
    CHECK(!fits(5 * kGB, 0, 0, 10 * kGB));  // already below the reserve
    CHECK(fits(30 * kGB, 5 * kGB, 15 * kGB, 10 * kGB));
    CHECK(!fits(30 * kGB, 5 * kGB, 15 * kGB + 1, 10 * kGB));

    // Output shape and estimate: 1080p30, 313 frames (one of the owner's clips).
    Probe pr;
    pr.width = 1920, pr.height = 1080, pr.fps = 30, pr.frames = 313, pr.seconds = 313 / 30.0;
    ImportOptions o;
    auto s = output_shape(Kind::Video, pr, o, "hap", 1920, 1080);
    CHECK(s.w == 1920 && s.h == 1080 && s.frames == 313 && s.codec == "hap" && !s.conform);
    const int64_t hap = estimate_bytes(s, Kind::Video);
    CHECK(hap == static_cast<int64_t>(1920.0 * 1080 * 313 * 0.5) + kSideBytes);  // ~324 MB (measured: ~214 MB)
    o.codec = "hap_q";
    CHECK(estimate_bytes(output_shape(Kind::Video, pr, o, "hap", 1920, 1080), Kind::Video) > hap * 19 / 10);
    o = {};
    o.max_h = 720;  // "1080p -> 720p"
    s = output_shape(Kind::Video, pr, o, "hap", 1920, 1080);
    CHECK(s.w == 1280 && s.h == 720);
    CHECK(estimate_bytes(s, Kind::Video) < hap / 2);
    o.max_h = 2160;  // never up
    s = output_shape(Kind::Video, pr, o, "hap", 1920, 1080);
    CHECK(s.w == 1920 && s.h == 1080);
    pr.width = 1918, pr.height = 1078;  // rounded down to a multiple of 4
    s = output_shape(Kind::Video, pr, {}, "hap", 1920, 1080);
    CHECK(s.w == 1916 && s.h == 1076);
    pr.width = 1920, pr.height = 1080;
    pr.alpha = true;
    CHECK(output_shape(Kind::Video, pr, {}, "hap", 1920, 1080).codec == "hap_alpha");
    pr.alpha = false;
    // Stills fit in 2 x the canvas, one frame, plus the PNG.
    Probe big;
    big.width = 8000, big.height = 4000;
    s = output_shape(Kind::Still, big, {}, "hap", 1920, 1080);
    CHECK(s.w == 3840 && s.h == 1920 && s.frames == 1);

    // Frame-rate conform (D181): 30 on 60 and 25 on 50 are already smooth.
    CHECK(conform_fps(30, 60) == 0);
    CHECK(conform_fps(25, 50) == 0);
    CHECK(conform_fps(60, 60) == 0);
    CHECK(conform_fps(120, 60) == 0);
    CHECK(conform_fps(24, 60) == 60);
    CHECK(conform_fps(25, 60) == 60);
    CHECK(conform_fps(30, 50) == 50);
    CHECK(conform_fps(24, 0) == 0);
    CHECK(conform_fps(0, 60) == 0);
    o = {};
    o.conform_hz = 60;
    pr.fps = 24, pr.frames = 240;
    s = output_shape(Kind::Video, pr, o, "hap", 1920, 1080);
    CHECK(s.conform && s.fps == 60 && s.frames == 600);
    pr.fps = 30, pr.frames = 300;
    s = output_shape(Kind::Video, pr, o, "hap", 1920, 1080);
    CHECK(!s.conform && s.fps == 30 && s.frames == 300);

    CHECK(human_bytes(17200000000ull) == "17.2 GB");
    CHECK(human_bytes(640000000ull) == "640 MB");

    // Plain words.
    CHECK(plain_error("converting", "ffmpeg: Error writing trailer: No space left on device") == "The disk is full.");
    CHECK(is_disk_full("av_interleaved_write_frame(): No space left on device"));
    CHECK(!is_disk_full("Invalid data found when processing input"));
    CHECK(plain_error("probing", "probe: x.mp4: Invalid data found when processing input").find("damaged") != std::string::npos);
    CHECK(plain_error("probing", "the original is gone").find("gone") != std::string::npos);
    CHECK(plain_error("converting", "something odd") == "The import failed while converting.");

    // Batch progress and ETA (work in estimated bytes).
    BatchProgress b;
    b.add(3);
    CHECK(b.eta_s(1000) == -1 && b.total() == 3 && b.done() == 0);
    b.file_done(100, 10.0, 80);  // 10 bytes per second
    CHECK(b.done() == 1 && b.written() == 80);
    CHECK(b.eta_s(200) == 20);
    CHECK(b.eta_s(0) == 0);
    b.file_done(0, 0, 0);  // a cleared clip: counts as done, not as speed
    CHECK(b.done() == 2 && b.eta_s(200) == 20);
    b.reset();
    CHECK(b.done() == 0 && b.total() == 0 && b.eta_s(1) == -1);
    CHECK(file_progress("probing", 1.0f) > 0.019f && file_progress("probing", 1.0f) < 0.021f);
    CHECK(std::fabs(file_progress("converting", 0.5f) - 0.42f) < 1e-4f);
    CHECK(file_progress("analysing", 1.0f) == 1.0f && file_progress("ready", 0) == 1.0f);

    // ffmpeg's -progress lines.
    ProgressParser pp;
    const std::string chunk1 = "frame=12\nfps=30.0\ntotal_size=4", chunk2 = "096\nprogress=continue\nframe=40\r\n";
    pp.feed(reinterpret_cast<const uint8_t*>(chunk1.data()), chunk1.size());
    CHECK(pp.frame == 12 && pp.total_size == 0);
    pp.feed(reinterpret_cast<const uint8_t*>(chunk2.data()), chunk2.size());
    CHECK(pp.frame == 40 && pp.total_size == 4096);

    // The convert command line with the v3 options.
    ConvertSpec cs;
    cs.input = "in.mp4", cs.output = "out.mov";
    cs.out_max_h = 720, cs.conform_fps = 60, cs.progress = true;
    const auto a = convert_args(cs);
    std::string joined;
    for (const auto& x : a) joined += x + " ";
    CHECK(joined.find("min(ih,720)") != std::string::npos);
    CHECK(joined.find("framerate=fps=60") != std::string::npos);
    CHECK(joined.find("-progress pipe:1") != std::string::npos);
    ConvertSpec plain;
    plain.input = "in.mp4", plain.output = "out.mov";
    std::string j2;
    for (const auto& x : convert_args(plain)) j2 += x + " ";
    CHECK(j2.find("framerate") == std::string::npos && j2.find("-progress") == std::string::npos && j2.find("min(ih") == std::string::npos);

    // The real disk API answers for the temp folder (and for a folder not made yet).
    const auto d = disk_space(fs::temp_directory_path() / "himmel_no_such_folder" / "x");
    CHECK(d.known && d.total > 0 && d.free <= d.total);

    // Import options survive the sidecar (and so the DB's sidecar column).
    ClipRecord r;
    r.hash = "0123456789abcdef", r.name = "opt", r.source = "C:\\x.mp4";
    r.import.max_h = 720, r.import.codec = "hap_q", r.import.conform_hz = 60, r.import.est_bytes = 123456789;
    ClipRecord back;
    CHECK(sidecar_from_json(sidecar_to_json(r), back));
    CHECK(back.import.max_h == 720 && back.import.codec == "hap_q" && back.import.conform_hz == 60 && back.import.est_bytes == 123456789);
    ClipRecord none;
    none.hash = r.hash, none.name = "n", none.source = r.source;
    CHECK(sidecar_to_json(none).find("\"import\"") == std::string::npos);  // v2 sidecars stay as they were
}

static void test_v3_location_and_move() {
    using namespace himmel::ingest;
    const fs::path t = temp_dir("v3_move");
    std::error_code ec;
    // The saved location (a temp file here; the real one is Documents\Himmel\library-location.json).
    const fs::path loc = t / "settings" / "library-location.json";
    CHECK(read_location(loc).empty());
    CHECK(save_location(loc, t / "E-drive" / "Himmel Library"));
    CHECK(read_location(loc) == t / "E-drive" / "Himmel Library");
    write_file(loc, "{\"format\": \"something-else\", \"root\": \"C:\\\\x\"}");
    CHECK(read_location(loc).empty());
    CHECK(location_file().filename() == "library-location.json");

    // A library to move: derived files, the DB, originals (never copied).
    const fs::path a = t / "A", b = t / "B";
    {
        LibraryDb db;
        CHECK(db.open(a / kDbFile, false));
        CHECK(!db.meta_get("uid").empty());
        db.checkpoint();
    }
    write_file(a / "hap" / "1111111111111111.mov", std::string(300000, 'h'));
    write_file(a / "thumbs" / "1111111111111111.jpg", std::string(2000, 't'));
    write_file(a / "meta" / "1111111111111111.json", "{}");
    write_file(a / "originals" / "mine.mp4", std::string(5000, 'o'));
    write_file(a / "work" / "junk.mov", "x");
    CHECK(library_bytes(a) >= 302000);
    const auto plan = plan_move(a);
    bool has_orig = false, has_db = false, has_work = false;
    for (const auto& f : plan.files) {
        has_orig |= f.string().find("originals") != std::string::npos;
        has_work |= f.string().find("work") != std::string::npos;
        has_db |= f == fs::path(kDbFile);
    }
    CHECK(!has_orig && !has_work && has_db && plan.files.size() == 4);
    CHECK(empty_or_missing(b) && !empty_or_missing(a));
    // A cancelled copy leaves nothing behind and the old library untouched.
    std::string err;
    CHECK(!copy_library(a, b, plan, [](uint64_t done, uint64_t) { return done == 0; }, err));
    CHECK(err.find("cancelled") != std::string::npos);
    CHECK(!fs::exists(b / "hap" / "1111111111111111.mov", ec) && fs::exists(a / "hap" / "1111111111111111.mov", ec));
    // A full copy with progress.
    uint64_t last = 0, calls = 0;
    CHECK(copy_library(a, b, plan, [&](uint64_t done, uint64_t total) { last = done, ++calls; return total == plan.bytes; }, err));
    CHECK(last == plan.bytes && calls >= plan.files.size());
    CHECK(fs::file_size(b / "hap" / "1111111111111111.mov", ec) == 300000);
    {
        LibraryDb moved, orig;
        CHECK(moved.open(b / kDbFile, true) && orig.open(a / kDbFile, true));
        CHECK(moved.meta_get("uid") == orig.meta_get("uid"));  // the feed keeps its cursor after a move
    }
    // The redirect in the old folder: found once the old DB is gone.
    CHECK(write_moved_redirect(a, b));
    CHECK(resolve_library_root(a) == a);  // the old DB is still there (renderer not restarted yet)
    CHECK(delete_old_library(a) == 0);
    CHECK(!fs::exists(a / kDbFile, ec) && !fs::exists(a / "hap", ec) && !fs::exists(a / "work", ec));
    CHECK(fs::exists(a / "originals" / "mine.mp4", ec));  // originals are never deleted
    CHECK(resolve_library_root(a) == b);
    CHECK(resolve_library_root(b) == b);
    // A copy into a folder that already has the file fails and undoes itself.
    CHECK(!copy_library(a / ".." / "B", b, plan_move(b), nullptr, err));
    CHECK(fs::exists(b / "hap" / "1111111111111111.mov", ec));  // the target's own files are kept
    fs::remove_all(t, ec);
}

int main() {
    test_hash();
    test_judder();
    test_analysis();
    test_sidecar();
    test_db();
    test_scan();
    test_throttle();
    test_ffmpeg_args();
    test_feed();
    test_ingest_commands();
    test_ingest_cli();
    test_v3_guard();
    test_v3_location_and_move();
    test_ffmpeg_round_trip();
    std::printf("library_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
