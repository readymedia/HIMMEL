// Maschine MK2 controller: the control map (maschine_mk2_map.json).
// Spec section 4. The verified layout is also baked in (default_map) so the
// controller still works if the file is missing or broken (show-safe).
#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include "himmel/mk2/types.h"

namespace himmel::mk2 {

struct BitPos {
    int byte = -1;  // index into the controls payload, -1 = not mapped
    int bit = 0;
};

struct KnobPos {
    int byte = -1;
    bool word = true;  // true: uint16 LE at byte, byte+1; false: 4-bit nibble
    bool low_nibble = true;
};

struct Map {
    std::array<BitPos, kButtons> buttons{};
    std::array<KnobPos, kKnobs> knobs{};
    KnobPos encoder{};
    std::array<int, kPads> pad_hw_id{};       // pad number - 1 -> hardware id 0..15
    std::array<int, kPads> pad_of_hw_id{};    // hardware id -> pad number - 1
    std::array<int, kPads> noise_floor{};     // pad number - 1 -> resting raw pressure
    bool verified = false;
};

// The layout verified on the owner's MK2 on 2026-11-09.
Map default_map();

// Loads the map file. On error returns nullopt and sets *error.
std::optional<Map> load_map(const std::filesystem::path& path, std::string* error);

}  // namespace himmel::mk2
