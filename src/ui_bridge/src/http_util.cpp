// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "http_util.h"

#include <cctype>
#include <cstdlib>

namespace himmel::ui {

namespace {
std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
}  // namespace

bool parse_http_head(std::string_view head, HttpRequest& out) {
    out = HttpRequest{};
    size_t eol = head.find("\r\n");
    std::string_view line = head.substr(0, eol);
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return false;
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return false;
    out.method = std::string(line.substr(0, sp1));
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version = line.substr(sp2 + 1);
    if (target.empty() || target.front() != '/') return false;
    if (version.substr(0, 5) != "HTTP/") return false;
    out.keep_alive = version != "HTTP/1.0";
    const size_t q = target.find('?');
    out.path = std::string(target.substr(0, q));
    if (q != std::string_view::npos) out.query = std::string(target.substr(q + 1));

    while (eol != std::string_view::npos) {
        const size_t start = eol + 2;
        if (start >= head.size()) break;
        eol = head.find("\r\n", start);
        std::string_view h = head.substr(start, eol == std::string_view::npos ? std::string_view::npos : eol - start);
        if (h.empty()) break;
        const size_t colon = h.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string key = lower(trim(h.substr(0, colon)));
        const std::string_view val = trim(h.substr(colon + 1));
        if (key == "host") out.host = std::string(val);
        else if (key == "origin") out.origin = std::string(val);
        else if (key == "content-type") out.content_type = lower(val);
        else if (key == "content-length") {
            char* end = nullptr;
            const std::string v(val);
            const unsigned long long n = std::strtoull(v.c_str(), &end, 10);
            if (end == v.c_str() || *end) return false;
            out.content_length = static_cast<size_t>(n);
        } else if (key == "connection") {
            const std::string v = lower(val);
            if (v.find("close") != std::string::npos) out.keep_alive = false;
            if (v.find("keep-alive") != std::string::npos) out.keep_alive = true;
        }
    }
    return true;
}

std::string query_param(std::string_view query, std::string_view key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string_view::npos) amp = query.size();
        std::string_view kv = query.substr(pos, amp - pos);
        const size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) return eq == std::string_view::npos ? std::string() : std::string(kv.substr(eq + 1));
        pos = amp + 1;
    }
    return {};
}

bool safe_static_path(std::string_view path) {
    if (path.empty() || path.front() != '/') return false;
    if (path.find("..") != std::string_view::npos || path.find("//") != std::string_view::npos) return false;
    for (char c : path) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.' || c == '/';
        if (!ok) return false;
    }
    return true;
}

const char* content_type_for(std::string_view path) {
    auto ends = [&](std::string_view s) { return path.size() >= s.size() && path.substr(path.size() - s.size()) == s; };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js")) return "text/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".json")) return "application/json";
    if (ends(".ico")) return "image/x-icon";
    return "application/octet-stream";
}

bool host_is_local(std::string_view host) {
    const std::string h = lower(host);
    std::string name;
    if (!h.empty() && h.front() == '[') {
        const size_t close = h.find(']');
        name = close == std::string::npos ? h : h.substr(0, close + 1);
    } else {
        name = h.substr(0, h.find(':'));
    }
    return name == "127.0.0.1" || name == "localhost" || name == "[::1]";
}

bool origin_is_local(std::string_view origin) {
    if (origin.empty()) return true;
    const std::string o = lower(origin);
    for (const char* p : {"http://127.0.0.1", "http://localhost", "http://[::1]"}) {
        const std::string_view pre(p);
        if (o.size() >= pre.size() && std::string_view(o).substr(0, pre.size()) == pre &&
            (o.size() == pre.size() || o[pre.size()] == ':'))
            return true;
    }
    return false;
}

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '+') out.push_back(' ');
        else if (s[k] == '%' && k + 2 < s.size() && hex(s[k + 1]) >= 0 && hex(s[k + 2]) >= 0) {
            out.push_back(static_cast<char>(hex(s[k + 1]) * 16 + hex(s[k + 2])));
            k += 2;
        } else out.push_back(s[k]);
    }
    return out;
}

}  // namespace himmel::ui
