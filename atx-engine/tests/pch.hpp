#pragma once

// Precompiled-header payload for the atx-engine-tests target (opt-in:
// ATX_USE_PCH, default OFF; ON in the `dev` preset). See atx-engine/pch.hpp for
// the rationale — this adds GoogleTest (included by all 145 test TUs) on top of
// the engine's heavy/stable header set. The canonical `ninja` / CI preset keeps
// ATX_USE_PCH OFF, so per-TU include hygiene is still enforced there.

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
