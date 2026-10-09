#pragma once

// Precompiled-header payload for the atx-vol-tests target (opt-in: ATX_USE_PCH;
// ON in the `ninja`/`dev`/`rel` presets, OFF in `hygiene`). GoogleTest is
// #included by 79 of the ~85 test TUs and is by far the dominant per-TU parse
// cost; precompiling it (plus the ubiquitous std headers and the stable
// atx-core vocabulary) parses that payload ONCE instead of once per TU. Mirrors
// atx-engine/tests/pch.hpp. The `hygiene` preset keeps ATX_USE_PCH OFF so per-TU
// include hygiene is still enforced there — nothing here may hide a missing
// include in a TU that is compiled under hygiene.
//
// Only STABLE, high-fanout headers belong here: editing a header listed below
// invalidates the PCH and forces a full test rebuild, so atx-vol's own
// still-churning headers are intentionally excluded (each TU includes those
// directly).

// --- the dominant per-TU parse cost across the suite ---
#include <gtest/gtest.h>

// --- ubiquitous std (each in >= ~10 test TUs) ---
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// --- stable atx-core vocabulary (Result/Status/types; rarely churns) ---
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
