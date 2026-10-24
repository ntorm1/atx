#pragma once

#include <cstddef>
#include <span>

namespace atx::vol::research {

struct CentralityDirection {
  std::span<double> x, s, z;
};

struct CentralityScratch {
  std::span<double> residual;
  CentralityDirection direction;
};

struct CentralityWork {
  std::size_t attempts = 0, solves = 0, accepted = 0;
  std::size_t zero_target = 0, nonfinite = 0, nonimproving = 0, solve_refused = 0;
  std::size_t invalid_target = 0;
};

// Synchronous borrowed solver for homogeneous primal/dual residuals and the
// supplied complementarity residual rc=-t. It writes only scratch directions.
// No ownership escapes. A false return discards that entire correction.
using HomogeneousSolve = bool (*)(void *, std::span<const double>, CentralityDirection);

// Private research primitive, without a production dispatch. All writable spans
// and the positive input spans must be disjoint and live for the complete call.
// Caller allocates scratch once. No allocation, factorization or clock reads
// occur here; actual solve attempts and every discarded correction are counted.
// False denotes invalid input/shape; true includes a valid unchanged direction.
// Only fully accepted directions overwrite the caller's three direction arrays.
[[nodiscard]] bool improve_centrality(std::span<const double> s,
    std::span<const double> z, double mu_target, CentralityDirection direction,
    CentralityScratch scratch, HomogeneousSolve solve, void *context,
    CentralityWork &work);

} // namespace atx::vol::research
