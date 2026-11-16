// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/fx_catalog.h"

#include "himmel/fx/fx.h"

namespace himmel::engine::fx {

void fill_catalog(protocol::Catalog& c) { himmel::fx::builtin().fill_catalog(c); }

}  // namespace himmel::engine::fx
