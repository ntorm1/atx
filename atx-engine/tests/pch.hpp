#pragma once

// Precompiled-header payload shared by the atx-engine-<group>-tests targets
// (opt-in: ATX_USE_PCH; ON in the `ninja` and `dev` presets). The
// configured consumers reuse a minimal stable carrier, so selecting a different
// group order does not rebuild their PCH. Data has a separate compatible carrier
// for its miniz include path. See atx-engine/pch.hpp for the
// rationale. The `hygiene` preset keeps ATX_USE_PCH OFF so per-TU include hygiene
// is still enforced there.

// --- the two dominant per-TU parse costs across the test suite ---
#include <gtest/gtest.h>
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

// --- stable atx-core vocabulary ---
#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"
