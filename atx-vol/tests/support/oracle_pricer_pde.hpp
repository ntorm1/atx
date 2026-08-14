#pragma once

// Crank-Nicolson finite-difference American option oracle (test-only).
//
// Ported from the C ats-vol test support (tests/src/support/oracle_pricer_pde.c)
// into C++. Slow but correct — the regression oracle the fast Andersen-Lake /
// BAW pricers are checked against. Solves the Black-Scholes-Merton PDE with
// continuous-dividend yield q on a uniform log-S grid, projecting the
// early-exercise constraint max(V, payoff) at every time step (Thomas-solved
// tridiagonal Crank-Nicolson). Working buffers are std::vector — no malloc.
//
// Convergence: O(dt² + dx²); the defaults (n_t = 2000, n_x = 4000) give ~6-7
// sig figs. A few ms per call.

#include "atx/vol/api/core/types.hpp"

namespace atx::vol::test {

struct OraclePdeOpts {
  int n_t = 2000;             // time steps
  int n_x = 4000;             // spatial nodes
  double s_min_mult = 0.05;   // lower boundary as an S/K multiple
  double s_max_mult = 20.0;   // upper boundary as an S/K multiple
};

// American price via Crank-Nicolson. Returns NaN on degenerate input or when the
// target spot falls outside the grid.
[[nodiscard]] double oracle_pde_american(double S, double K, double T,
                                         double sigma, double r, double q,
                                         Side side, const OraclePdeOpts& opts = {});

}  // namespace atx::vol::test
