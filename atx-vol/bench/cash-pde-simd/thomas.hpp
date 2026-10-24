#pragma once

// Research-only fixed-width owned layout. No production solver calls this seam.
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace atx::vol::ddiv_ref::research {

using Quad = std::array<double, 4>;
using PolicyQuad = std::array<std::uint8_t, 4>;
enum class LaneStatus : std::uint8_t { Masked, Success, InvalidInput, NonfiniteResult };
using Statuses = std::array<LaneStatus, 4>;

struct Rows {
  double lo = 0., di = 1., up = 0., bottom = 0., top = 0.;
};

struct ThomasPack {
  std::array<Rows, 4> rows{};
  std::vector<Quad> rhs, obstacle, cp, dp, work;
  std::vector<PolicyQuad> active;
};

// All six arrays have the same size (n+1, n>=1). Each enabled lane has finite
// diagonally dominant Z-matrix coefficients and finite consumed values. Invalid
// or masked lanes leave scratch untouched. Nonfinite arithmetic refuses a lane;
// its partial scratch is not a solution. Calls allocate nothing and own no state.
// FMA and two separate divisions match the frozen vol-rel scalar Thomas object.
[[nodiscard]] Statuses solve_scalar(ThomasPack &pack, std::uint8_t lanes) noexcept;
// Caller must establish AVX2+FMA availability before entering this TU.
[[nodiscard]] Statuses solve_avx2(ThomasPack &pack, std::uint8_t lanes) noexcept;

// Shared admission/accounting only; no numerical recurrence is shared.
[[nodiscard]] Statuses admit(const ThomasPack &pack, std::uint8_t lanes) noexcept;
void check_results(const ThomasPack &pack, Statuses &status) noexcept;

} // namespace atx::vol::ddiv_ref::research
