// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// One library clip, as the sidecar (meta/<hash>.json) and the DB row hold it
// (docs/interfaces/library-db.md §3, §5). Plain data, no I/O.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace himmel::library {

inline constexpr int kAnalysisVersion = 1;
inline constexpr int kPaletteSize = 5;
inline constexpr int kMaxSlices = 16;

enum class Kind : uint8_t { Video, Still, Vector, Animation, Sequence };
enum class State : uint8_t { Queued, Probing, Converting, Analysing, Ready, Failed };

// Resume bits (stage_done).
inline constexpr uint32_t kStageProbe = 1, kStageConvert = 2, kStageAnalyse = 4, kStageThumbs = 8;

const char* kind_name(Kind k);
const char* state_name(State s);
bool kind_from(const std::string& s, Kind& out);
bool state_from(const std::string& s, State& out);

struct Probe {
    std::string codec;      // ffprobe codec_name of the source
    std::string pix_fmt;
    int width = 0, height = 0;
    double fps = 0;
    int64_t frames = 0;
    double seconds = 0;
    bool alpha = false;
};

struct PaletteEntry {
    uint32_t rgb = 0;       // 0xRRGGBB
    float weight = 0;       // 0..1, the weights add up to 1
};

struct Analysis {
    int version = 0;        // 0 = not analysed
    uint32_t avg_rgb = 0x404040;
    float brightness = 0, contrast = 0, saturation = 0;
    float warmth = 0;       // -1 cool .. +1 warm
    std::vector<PaletteEntry> palette;
    float motion = 0;       // 0..1
    float motion_dir = 0;   // degrees, 0 = right, 90 = up
    float steadiness = 1;   // 0..1
    std::vector<float> motion_curve;   // one per proxy frame
    std::vector<int64_t> cuts;         // frames of the HAP file
    float cuts_per_min = 0;
    int energy = 1;         // 1..5
    float mood_chill = 0, mood_groove = 0, mood_techno = 0, mood_crazy = 0;
    float loop_quality = 0; // SSIM last vs first frame
    int64_t loop_in = 0, loop_out = 0;
    float loop_score = 0;
    std::string loop_style = "loopable";   // loopable | ping-pong | one-shot
    bool needs_crossfade = false;
    std::vector<int64_t> slices;       // up to 16 frames
    int natural_beats = 0;  // 1..64
    int bars_at_120 = 0;    // 0 = free
    std::string flicker = "ok";        // ok | warn | bad
    int black_frames = 0;
    int proxy_w = 0, proxy_h = 0, proxy_frames = 0;
    float proxy_fps = 0;
};

struct ClipRecord {
    int64_t id = 0;         // DB id (0 = not in the DB yet); not in the sidecar
    int64_t ready_order = 0;  // DB only: 1, 2, 3 ... in the order clips turned ready (the feed's cursor)
    std::string hash;       // 16 hex
    Kind kind = Kind::Video;
    std::string name;
    std::string source;     // absolute UTF-8 path of the original
    State state = State::Queued;
    uint32_t stage_done = 0;
    std::string error;
    std::string play_path, still_path, thumb_path;  // relative to the library root
    Probe probe;
    std::string codec;      // hap | hap_alpha | hap_q
    int width = 0, height = 0, chunks = 0;  // of the HAP file
    double fps = 0;
    int64_t frames = 0;
    double seconds = 0;
    std::vector<int> judder;                // refresh rates it judders on (50, 60)
    Analysis analysis;
    int64_t added_unix = 0, ready_unix = 0;
    int stars = 0;
    bool favourite = false;
    std::vector<std::string> user_tags;
};

// Refresh rates (of 50 and 60 Hz) on which a clip of this fps judders:
// the refresh / fps ratio is not close to a whole number (operations.md §3).
std::vector<int> judder_for(double fps);

}  // namespace himmel::library
