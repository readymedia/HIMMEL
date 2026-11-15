// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// A logger the frame loop may call: it formats into a fixed slot of a lock-free
// ring (no heap, no lock, no I/O); a background thread writes the lines to
// stdout (the supervisor's log). A full ring drops the line and counts it.
// One writer thread (the render thread) only.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <memory>
#include <thread>

#include "himmel/protocol/shm.h"

namespace himmel::engine {

class LogRing {
public:
    explicit LogRing(const char* tag) : tag_(tag), ring_(std::make_unique<Ring>()) {
        ring_->head = 0, ring_->tail = 0, ring_->dropped = 0;
        thread_ = std::thread([this] { run(); });
    }
    ~LogRing() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }
    LogRing(const LogRing&) = delete;
    LogRing& operator=(const LogRing&) = delete;

    void logf(const char* fmt, ...) {
        Line l;
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        int n = std::snprintf(l.text, sizeof(l.text), "%02d:%02d:%02d  %s  ", tm.tm_hour, tm.tm_min, tm.tm_sec, tag_);
        if (n < 0) n = 0;
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(l.text + n, sizeof(l.text) - static_cast<size_t>(n), fmt, ap);
        va_end(ap);
        ring_->push(l);
    }
    uint32_t dropped() const { return ring_->dropped.load(); }

private:
    struct Line {
        char text[400];
    };
    using Ring = protocol::SpscRing<Line, 256>;
    void run() {
        Line l;
        for (;;) {
            bool any = false;
            while (ring_->pop(l)) {
                std::fputs(l.text, stdout);
                std::fputc('\n', stdout);
                any = true;
            }
            if (any) std::fflush(stdout);
            if (stop_.load() && ring_->size() == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    const char* tag_;
    std::unique_ptr<Ring> ring_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace himmel::engine
