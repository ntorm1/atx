#pragma once

// Precompiled-header payload for fast cold / fresh-worktree builds.
//
// OPT-IN ONLY (CMake option ATX_USE_PCH, default OFF; turned ON by the `dev`
// preset). The canonical `ninja` / CI preset keeps it OFF, so the strict build
// still compiles every TU with its own includes and catches any missing/unused
// include — the PCH never hides an include hygiene defect from CI.
//
// Contents are the HEAVY + STABLE headers shared by nearly every engine TU:
// Eigen (by far the dominant parse cost) plus the atx-core vocabulary and the
// ubiquitous std headers. Parsed once into the PCH, then reused by every TU in
// the target instead of being re-parsed ~189 times. Keep this list to headers
// that change rarely — adding a volatile header here would invalidate the PCH
// (and force a full rebuild) every time that header is touched.

// --- dominant third-party parse cost ---
#include <Eigen/Dense>

// --- ubiquitous std ---
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// --- stable atx-core vocabulary (read-only in feature trees) ---
#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"
