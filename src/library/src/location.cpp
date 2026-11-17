// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/library/location.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "himmel/library/library_db.h"

namespace himmel::library {

namespace fs = std::filesystem;

namespace {

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string to_utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

nlohmann::json read_json(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    if (!in) return nlohmann::json();
    std::stringstream ss;
    ss << in.rdbuf();
    return nlohmann::json::parse(ss.str(), nullptr, false);
}

bool write_json_atomic(const fs::path& f, const nlohmann::json& j) {
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    const fs::path tmp = f.parent_path() / (f.filename().wstring() + L".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2) << "\n";
        if (!out) return false;
    }
    return MoveFileExW(tmp.wstring().c_str(), f.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

}  // namespace

fs::path location_file() {
    PWSTR docs = nullptr;
    fs::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) p = docs;
    CoTaskMemFree(docs);
    if (p.empty()) p = fs::temp_directory_path();
    return p / "Himmel" / "library-location.json";
}

fs::path read_location(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return {};
    const auto j = read_json(file);
    if (!j.is_object() || j.value("format", std::string()) != kLocationFormat) return {};
    const fs::path root = from_utf8(j.value("root", std::string()));
    return root.is_absolute() ? root : fs::path();
}

bool save_location(const fs::path& file, const fs::path& root) {
    return write_json_atomic(file, {{"format", kLocationFormat}, {"root", to_utf8(root)}});
}

fs::path resolve_library_root(fs::path root) {
    std::error_code ec;
    for (int hop = 0; hop < 8; ++hop) {
        if (fs::exists(root / kDbFile, ec) || !fs::exists(root / kMovedFile, ec)) break;
        const auto j = read_json(root / kMovedFile);
        const fs::path to = j.is_object() ? from_utf8(j.value("to", std::string())) : fs::path();
        if (!to.is_absolute() || to == root) break;
        root = to;
    }
    return root;
}

bool write_moved_redirect(const fs::path& old_root, const fs::path& to) {
    return write_json_atomic(old_root / kMovedFile, {{"to", to_utf8(to)}});
}

}  // namespace himmel::library
