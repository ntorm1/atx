#pragma once

#include "stock_pwl.hpp"

#include <cstddef>
#include <span>

namespace atx::vol::cash_gaussian {
struct SemigroupWork { std::size_t completed_steps = 0, grid_nodes = 0; };

// Consumes the initial PWL and performs exactly `steps` constant-dt propagations.
// Every step visits every grid node and every input interval. No cash/exercise,
// cutoff, reference price or unchanged-function shortcut. stock must be an exact
// grid anchor. The caller-owned Budget includes construction of the input PWL;
// work resets on entry and both diagnostics retain partial work on refusal.
[[nodiscard]] Result<double> repeat_expectation(StockPwl value,
    std::span<const double> grid, double stock, double dt, std::size_t steps,
    double sigma, double rate, double yield, Budget &, SemigroupWork &);
} // namespace atx::vol::cash_gaussian
