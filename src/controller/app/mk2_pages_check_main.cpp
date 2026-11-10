// mk2_pages_check.exe - renders every screen page with sample content to PNG,
// so layouts can be checked without the MK2.
//
//   mk2_pages_check [out_dir]      (default: tools/_screens/cpp, gitignored)
//
// Each PNG shows both screens 3x, white-on-blue like the device, with a gap
// for the bezel and small marks under the physical knob centres.
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "himmel/mk2/pages.h"
#include "page_samples.h"
#include "png_write.h"

using namespace himmel::mk2;

namespace {

bool save(const std::filesystem::path& dir, const char* name, const ui::Screen& c) {
    constexpr int S = 3, gap = 24, mark = 12;
    const int w = 2 * kScreenW * S + gap, h = kScreenH * S + mark;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 30);
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = &rgb[(static_cast<size_t>(y) * w + x) * 3];
        p[0] = r, p[1] = g, p[2] = b;
    };
    for (int y = 0; y < kScreenH; ++y)
        for (int x = 0; x < 2 * kScreenW; ++x) {
            const bool on = c.get(x, y);
            const int ox = x * S + (x >= kScreenW ? gap : 0);
            for (int dy = 0; dy < S; ++dy)
                for (int dx = 0; dx < S; ++dx) {
                    if (on) put(ox + dx, y * S + dy, 235, 242, 255);
                    else put(ox + dx, y * S + dy, 18, 40, 110);
                }
        }
    for (int k = 0; k < kKnobs; ++k) {  // knob centre marks below the screens
        const int x = ui::kKnobX[k] * S + (ui::kKnobX[k] >= kScreenW ? gap : 0) + 1;
        for (int y = kScreenH * S + 2; y < h; ++y)
            for (int dx = -2; dx <= 2; ++dx) put(x + dx, y, 200, 120, 40);
    }
    const auto path = (dir / name).string();
    const bool ok = himmel::png::write_rgb(path, w, h, rgb);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", path.c_str());
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path dir = argc > 1 ? argv[1] : "tools/_screens/cpp";
    std::filesystem::create_directories(dir);
    ui::Screen c;
    bool ok = true;

    const std::array<float, kKnobs> v1 = {0.82f, 0.62f, 0.0f, 0.25f, 0.1f, 0.0f, 0.4f, 1.0f};
    ui::draw_perform(c, samples::perform(0, 0, v1, 128, true, 1, 2.3f, false));
    ok &= save(dir, "01_perform.png", c);

    const std::array<float, kKnobs> v2 = {0.3f, 0.85f, 0.6f, 0.5f, 0.0f, 0.0f, 0.3f, 0.9f};
    auto m2 = samples::perform(5, 1, v2, 124, false, -1, -1.0f, true);
    m2.knobs[2].automated = true;
    ui::draw_perform(c, m2);
    ok &= save(dir, "02_perform_shift.png", c);

    const std::array<float, kKnobs> v3 = {0.5f, 0.75f, 0.2f, 0.5f, 0.3f, 0.5f, 0.5f, 0.5f};
    ui::draw_perform(c, samples::perform(1, 2, v3, 140, true, 6, 0.1f, false));
    ok &= save(dir, "03_perform_motion.png", c);

    std::array<uint8_t, 96 * 54> thumb{};
    samples::thumbnail(3, thumb);
    c.clear();
    ui::draw_list(c, 0, samples::browser(11));
    ui::draw_detail(c, 1, samples::detail(11, &thumb));
    ok &= save(dir, "04_browse.png", c);

    ui::AudioModel a;
    a.head[0].left.set("AUDIO  WASAPI LOOPBACK");
    a.head[1].left.set("SPECTRUM");
    a.head[1].right.set("32");
    a.level[0] = 0.72f, a.peak[0] = 0.86f, a.level[1] = 0.64f, a.peak[1] = 0.80f;
    for (int i = 0; i < 32; ++i) a.bands[i] = 0.15f + 0.8f * std::exp(-i / 9.0f) * (0.7f + 0.3f * std::sin(i * 1.7f));
    a.bpm = 127.9f, a.beat_phase = 1.5f, a.beat_lock = true;
    ui::draw_audio(c, a);
    ok &= save(dir, "05_audio.png", c);

    ui::draw_perform(c, samples::perform(0, 0, v1, 128, true, -1, 3.0f, false));
    ui::draw_message(c, 1, "TEMPO", "128.0 BPM", "TURN = CHANGE  PUSH = TAP");
    ok &= save(dir, "06_message_tempo.png", c);

    const std::array<std::string_view, 8> menu = {"PERFORM", "BROWSE", "AUDIO", "KNOB ALIGN", "SCREEN TEST", "LED TEST", "ABOUT", ""};
    ui::draw_perform(c, samples::perform(0, 0, v1, 128, true, -1, -1.0f, true));
    ui::draw_list(c, 0, samples::system_menu(menu, 7, 3));
    ok &= save(dir, "07_menu.png", c);

    ui::draw_knob_align(c, 5);
    ok &= save(dir, "08_knob_align.png", c);

    return ok ? 0 : 1;
}
