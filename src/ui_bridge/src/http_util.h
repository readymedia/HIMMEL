// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Tiny HTTP/1.1 request parsing helpers for himmel_ui (pure, unit-tested).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace himmel::ui {

struct HttpRequest {
    std::string method;
    std::string path;    // without the query
    std::string query;   // after '?', raw
    std::string host;
    std::string origin;
    std::string content_type;
    size_t content_length = 0;
    bool keep_alive = true;
};

// Parses the request line + headers (everything before the blank line).
bool parse_http_head(std::string_view head, HttpRequest& out);

// Value of `key` in a raw query string ("a=1&since=42"), or "" if absent.
std::string query_param(std::string_view query, std::string_view key);

// Percent-decoding of a query value ('+' = space); a bad escape is kept as it is.
std::string url_decode(std::string_view s);

// A static path is safe if it only has [A-Za-z0-9_.-/] and no "..".
bool safe_static_path(std::string_view path);

const char* content_type_for(std::string_view path);

// Host header must name this machine (stops DNS-rebinding pages from driving the show).
bool host_is_local(std::string_view host);
// An Origin header, if present, must be our own page.
bool origin_is_local(std::string_view origin);

}  // namespace himmel::ui
