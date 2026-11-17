// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Background throttling (docs/interfaces/library-db.md §7). Pure logic: the
// ingest worker samples Local\HimmelEngine every ~100 ms and feeds it here.
#pragma once

#include <cstdint>

namespace himmel::ingest {

struct EngineSample {
    bool present = false;        // the block is mapped and valid
    uint64_t heartbeat_age_ms = 0;
    uint32_t late_frames = 0;    // total since the renderer started
    uint64_t frames = 0;
    uint32_t fps_x100 = 0;
    uint32_t pid = 0;            // v3: the renderer's process id (a restarted renderer has another)
};

class Throttle {
public:
    int64_t resume_after_ms = 2000;
    int64_t alive_ms = 1000;      // heartbeat younger than this = a show is running

    void update(const EngineSample& s, int64_t now_ms) {
        show_ = s.present && s.heartbeat_age_ms < static_cast<uint64_t>(alive_ms);
        if (!show_) {
            paused_ = false;
            have_ = false;
            return;
        }
        bool trigger = false;
        if (have_) {
            if (s.frames < last_frames_) have_ = false;          // a new renderer: start over
            else if (s.late_frames > last_late_) trigger = true;  // a late frame since the last sample
        }
        last_late_ = s.late_frames;
        last_frames_ = s.frames;
        have_ = true;
        if (trigger) {
            if (!paused_) ++pauses_;
            paused_ = true;
            last_trigger_ms_ = now_ms;
        } else if (paused_ && now_ms - last_trigger_ms_ >= resume_after_ms) {
            paused_ = false;
        }
    }
    bool show_running() const { return show_; }
    bool paused() const { return paused_; }
    uint32_t pauses() const { return pauses_; }

private:
    bool show_ = false, paused_ = false, have_ = false;
    uint32_t last_late_ = 0, pauses_ = 0;
    uint64_t last_frames_ = 0;
    int64_t last_trigger_ms_ = 0;
};

}  // namespace himmel::ingest
