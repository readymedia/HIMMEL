// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// `himmel_ingest --selftest` (library-db.md §10.5), also run by library_tests.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace himmel::ingest {

// Runs every ingest command against a temporary library in `tmp` (created and deleted;
// never under Documents). Returns the number of failed checks; `say` gets one line each.
int run_command_selftest(const std::filesystem::path& tmp, const std::function<void(const std::string&)>& say);

}  // namespace himmel::ingest
