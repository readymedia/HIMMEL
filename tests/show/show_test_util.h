// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Shared helpers for the SHOW tests: a fake catalog, a model + runtime pair, CHECK.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "himmel/engine/deck_model.h"
#include "himmel/engine/fx_catalog.h"
#include "himmel/show/runtime.h"

namespace show_test {

using namespace himmel::protocol;
namespace fs = std::filesystem;

inline int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++show_test::g_checks;                                            \
        if (!(cond)) {                                                    \
            ++show_test::g_failed;                                        \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

inline std::unique_ptr<Catalog> make_catalog(int clips, int skip_clip = -1) {
    auto c = std::make_unique<Catalog>();
    std::memset(c.get(), 0, sizeof(Catalog));
    int n = 0;
    for (int i = 0; i < clips; ++i) {
        if (i == skip_clip) continue;  // "the clip is missing on this PC"
        ClipInfo& ci = c->clip[n++];
        std::snprintf(ci.name, kNameLen, "clip%02d.mov", i);
        ci.width = 1920, ci.height = 1080, ci.fps = 30.0f, ci.seconds = 10.0f;
        ci.thumb_rgb = 0x102030u * static_cast<uint32_t>(i + 1);
    }
    c->clip_count = static_cast<uint32_t>(n);
    himmel::engine::fx::fill_catalog(*c);
    return c;
}

inline uint64_t now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

inline uint32_t g_seq = 0;
inline Command cmd(CommandType t, uint8_t deck = kNone, uint8_t layer = kNone, uint8_t slot = kNone, uint8_t param = kNone,
                   int32_t i = 0, float f0 = 0.0f, uint16_t flags = 0) {
    Command c = make_fx(t, deck, layer, slot, param, i, f0, flags);
    c.seq = ++g_seq;
    return c;
}

// Engine stand-in: the model + the runtime, ticked like the frame loop.
struct Rig {
    std::unique_ptr<Catalog> cat;
    std::unique_ptr<himmel::engine::DeckModel> model;
    std::unique_ptr<ShowBlock> block = std::make_unique<ShowBlock>();
    std::unique_ptr<himmel::show::ShowRuntime> show = std::make_unique<himmel::show::ShowRuntime>();
    uint64_t t = 1000000;

    explicit Rig(std::unique_ptr<Catalog> c) : cat(std::move(c)) { model = std::make_unique<himmel::engine::DeckModel>(*cat); }
    void start(const himmel::show::RuntimeConfig& cfg) { show->start(cfg, *cat, block.get()); }
    Reject deck(Command c) { return model->submit(static_cast<int>(Producer::Tool), c, t); }
    Reject send(Command c) { return show->submit(static_cast<int>(Producer::Tool), c, t, *model); }
    void tick(uint64_t dt_us = 16667) {
        t += dt_us;
        model->tick(t);
        show->tick(*model, t);
    }
    // Tick until the worker is idle (and nothing waits to be handed over). Real time passes.
    bool settle(int max_ms = 5000) {
        for (int i = 0; i < max_ms; ++i) {
            tick(1000);
            if (show->worker_idle()) {
                tick(1000);
                if (show->worker_idle()) {
                    if (GetEnvironmentVariableA("SHOW_DEBUG", nullptr, 0)) {
                        const FilesStatus f = show->files();
                        std::printf("    [settle] msg \"%s\" err \"%s\" path \"%s\" restored %u\n", show->status().message,
                                    f.error, f.path, f.restored);
                    }
                    return true;
                }
            }
            Sleep(1);
        }
        return false;
    }
    // A full 4-layer look on deck d: clips, opacity, blend, speed, 2 FX per layer, deck FX, transport.
    void fill_deck(int d, int base) {
        for (int l = 0; l < kLayersPerDeck; ++l) {
            const auto D = static_cast<uint8_t>(d), L = static_cast<uint8_t>(l);
            deck(cmd(CommandType::LayerClip, D, L, kNone, kNone, (base + l) % static_cast<int>(cat->clip_count)));
            deck(cmd(CommandType::LayerOpacity, D, L, kNone, kNone, 0, 0.3f + 0.1f * l + 0.05f * base));
            deck(cmd(CommandType::LayerBlend, D, L, kNone, kNone, (l + base) % 3));
            deck(cmd(CommandType::LayerSpeed, D, L, kNone, kNone, 0, 0.5f + 0.25f * l));
            for (int s = 0; s < 2; ++s) {
                const auto S = static_cast<uint8_t>(s);
                const int fx = (base + l * 2 + s) % static_cast<int>(cat->effect_count);
                deck(cmd(CommandType::FxSet, D, L, S, kNone, fx));
                for (int p = 0; p < 3 && p < cat->effect[fx].param_count; ++p)
                    deck(cmd(CommandType::FxParam, D, L, S, static_cast<uint8_t>(p), 0, 0.1f * (p + 1) + 0.03f * base));
                deck(cmd(CommandType::FxMix, D, L, S, kNone, 0, 0.8f));
            }
            Command m = cmd(CommandType::LayerSet, D, L, kNone, static_cast<uint8_t>(LayerParam::Mode), (base + l) % 3);
            deck(m);
            deck(cmd(CommandType::LayerSet, D, L, kNone, static_cast<uint8_t>(LayerParam::In), 0, 0.1f));
            deck(cmd(CommandType::LayerSet, D, L, kNone, static_cast<uint8_t>(LayerParam::Out), 0, 0.9f - 0.01f * base));
        }
        deck(cmd(CommandType::FxSet, static_cast<uint8_t>(d), kNone, 0, kNone, (base + 5) % static_cast<int>(cat->effect_count)));
        tick();
    }
};

inline fs::path temp_root(const char* name) {
    fs::path p = fs::temp_directory_path() / "himmel_show_tests" / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

}  // namespace show_test
