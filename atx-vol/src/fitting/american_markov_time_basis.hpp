#pragma once

// Private VarianceTimeKnots helpers shared by the fitter and the policy
// catalog (bodies in american_markov_time_basis.cpp). The focused fit test
// drives diagnose_markov_time_basis directly through its private src/ include:
// no public fitter input can produce the unordered rows its refusals guard.

#include <cstddef>
#include <span>

#include "atx/vol/api/fitting/american_markov_fit.hpp"

namespace atx::vol::detail {

// Shape/knot-grid admission: knots only with the knot shape, >=2 finite
// nonnegative strictly increasing times, and knots x state knots within
// max_parameters.
[[nodiscard]] Status validate_markov_time_knots(const AmericanMarkovFitConfig &config);

// Requires the validated knot vector. No clipping or endpoint nudging: a lost
// interior coefficient refuses the represented interpolation geometry.
[[nodiscard]] Result<AmericanMarkovFitTimeWeight> markov_time_weight(
    double time, bool active, std::span<const double> knots);

// Gram-free streaming QR of chronologically ordered adjacent-support rows,
// with column support/norm checks and the reciprocal infinity condition of the
// normalized R (admitted at kAmericanMarkovFitMinTimeBasisReciprocalCondition).
[[nodiscard]] Result<AmericanMarkovFitTimeBasisDiagnostics> diagnose_markov_time_basis(
    std::span<const AmericanMarkovFitTimeWeight> prefix, std::size_t groups);

} // namespace atx::vol::detail
