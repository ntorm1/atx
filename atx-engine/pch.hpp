#pragma once

// Precompiled-header payload for fast cold / fresh-worktree builds.
//
// OPT-IN ONLY as a CMake option (ATX_USE_PCH, default OFF for a bare `cmake`),
// but every preset inheriting `_base` — `ninja`, `dev`, `rel`, `rel-avx2` —
// turns it ON, so the builds you normally run DO use this PCH.
//
// The `hygiene` preset is the one that forces ATX_USE_PCH=OFF, compiling every
// TU with its own includes so a missing/unused one is caught. Nothing runs it
// for you: no CI builds this tree, so until a human runs
// `cmake --preset hygiene && cmake --build --preset hygiene`, this PCH IS
// hiding include defects. That is why its OFF-ness matters, and why the
// hygiene build is a real gate rather than a formality.
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
