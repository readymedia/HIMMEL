// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/library/hash.h"

#include <cstdio>
#include <fstream>

namespace himmel::library {

std::string hex16(uint64_t h) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::string hash_file(const std::filesystem::path& file) {
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(file, ec);
    if (ec) return {};
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    uint8_t sz[8];
    for (int i = 0; i < 8; ++i) sz[i] = static_cast<uint8_t>(size >> (8 * i));
    uint64_t h = fnv1a(sz, 8);
    std::vector<char> buf(kHashSample);
    auto sample = [&](uint64_t off, uint64_t n) {
        in.clear();
        in.seekg(static_cast<std::streamoff>(off));
        in.read(buf.data(), static_cast<std::streamsize>(n));
        const auto got = static_cast<size_t>(in.gcount());
        h = fnv1a(buf.data(), got, h);
        return got == n;
    };
    bool ok = true;
    if (size <= 3 * kHashSample) {
        for (uint64_t off = 0; off < size && ok; off += kHashSample) ok = sample(off, std::min<uint64_t>(kHashSample, size - off));
    } else {
        ok = sample(0, kHashSample) && sample((size - kHashSample) / 2, kHashSample) && sample(size - kHashSample, kHashSample);
    }
    return ok ? hex16(h) : std::string();
}

std::string hash_sequence(const std::vector<std::filesystem::path>& files) {
    if (files.empty()) return {};
    uint64_t h = fnv1a("seq", 3);
    std::error_code ec;
    for (const auto& f : files) {
        const auto name = f.filename().u8string();
        h = fnv1a(name.data(), name.size(), h);
        const uint64_t size = std::filesystem::file_size(f, ec);
        h = fnv1a(&size, sizeof(size), h);
    }
    const std::string a = hash_file(files.front()), b = hash_file(files.back());
    if (a.empty() || b.empty()) return {};
    h = fnv1a(a.data(), a.size(), h);
    h = fnv1a(b.data(), b.size(), h);
    return hex16(h);
}

}  // namespace himmel::library
