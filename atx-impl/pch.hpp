#pragma once

// Stable parse cache for the application library, private and ATX_USE_PCH-only.
// Keep all engine, pipeline, configuration and provenance headers out: their
// ordinary edits must not rebuild the PCH. Hygiene builds disable this payload.
// Translation units must still include their own directly used headers.
#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
