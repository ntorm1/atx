#include "fitting/american_markov_time_basis.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace atx::vol::detail {
namespace {
using atx::core::Err;
using atx::core::Ok;

// Bidiagonal R of the streaming Givens QR: diagonal[i] and upper[i] (row i,
// column i+1; the last upper entry is always zero).
struct BidiagonalFactor {
  std::vector<double> diagonal;
  std::vector<double> upper;
};

// Row admission plus the Givens updates. Ordering means no previously seen row
// touches a column beyond the current right group, so R stays bidiagonal and
// at most two rotations consume each row; the checks enforce that
// prerequisite, even for direct internal callers. Paused rows contribute none.
Status accumulate_rows(std::span<const AmericanMarkovFitTimeWeight> prefix, std::size_t groups,
                       AmericanMarkovFitTimeBasisDiagnostics &out, BidiagonalFactor &r) {
  std::size_t previous_left = 0;
  std::size_t previous_right = 0;
  double previous_time = -1.0;
  for (const auto &row : prefix) {
    if (!row.variance_active) continue;
    const auto left = row.left_group;
    const auto right = row.right_group;
    const auto weight = row.right_weight;
    if (left >= groups || right >= groups || right < left || right > left + 1 ||
        left < previous_left || right < previous_right ||
        !std::isfinite(row.variance_time) || row.variance_time < previous_time ||
        !std::isfinite(weight) || weight < 0.0 || weight > 1.0 ||
        (left == right && weight != 0.0) ||
        (left + 1 < groups && r.upper[left + 1] != 0.0)) {
      return Err(ErrorCode::InvalidArgument, "Markov fit: unordered or invalid time basis");
    }
    previous_left = left;
    previous_right = right;
    previous_time = row.variance_time;
    ++out.quoted_variance_intervals;
    const double a = left == right ? 1.0 : 1.0 - weight;
    const double b = left == right ? 0.0 : weight;
    out.column_support_counts[left] += a > 0.0 ? 1U : 0U;
    if (right != left) out.column_support_counts[right] += b > 0.0 ? 1U : 0U;
    const double radius = std::hypot(r.diagonal[left], a);
    const double cosine = radius == 0.0 ? 1.0 : r.diagonal[left] / radius;
    const double sine = radius == 0.0 ? 0.0 : a / radius;
    r.diagonal[left] = radius;
    if (left + 1 < groups) {
      const double old_upper = r.upper[left];
      r.upper[left] = cosine * old_upper + sine * b;
      const double residual = cosine * b - sine * old_upper;
      r.diagonal[left + 1] = std::hypot(r.diagonal[left + 1], residual);
    }
  }
  if (out.quoted_variance_intervals < groups) {
    return Err(ErrorCode::OutOfRange, "Markov fit: insufficient quoted-prefix time basis rows");
  }
  return Ok();
}

// Column norms, support checks and the exact ||R^-1||_inf by the bidiagonal
// back-recurrence S_i = (1 + |u_i| S_{i+1}) / d_i. R is divided by
// sqrt(quoted rows) first. A common scale cancels in the reciprocal condition
// (||cR|| * ||(cR)^-1|| = ||R|| * ||R^-1||), so that normalization is
// reporting-only: it changes rcond by rounding at most, never the admission.
Status reciprocal_condition(std::size_t groups, AmericanMarkovFitTimeBasisDiagnostics &out,
                            BidiagonalFactor &r) {
  out.common_normalization = std::sqrt(static_cast<double>(out.quoted_variance_intervals));
  double norm = 0.0;
  for (std::size_t i = 0; i < groups; ++i) {
    out.column_l2_norms[i] = std::hypot(r.diagonal[i], i == 0 ? 0.0 : r.upper[i - 1]);
    if (out.column_support_counts[i] == 0 || !(r.diagonal[i] > 0.0) ||
        !std::isfinite(out.column_l2_norms[i])) {
      return Err(ErrorCode::OutOfRange, "Markov fit: unsupported or dependent time control");
    }
    r.diagonal[i] /= out.common_normalization;
    // Column norms above need the original previous upper until the next row.
    norm = std::max(norm, r.diagonal[i] + std::abs(r.upper[i] / out.common_normalization));
  }
  double inverse_row = 0.0;
  double inverse_norm = 0.0;
  for (std::size_t i = groups; i-- > 0;) {
    inverse_row = (1.0 + std::abs(r.upper[i] / out.common_normalization) * inverse_row) /
                  r.diagonal[i];
    if (!std::isfinite(inverse_row)) {
      return Err(ErrorCode::OutOfRange, "Markov fit: time basis inverse loses numerical range");
    }
    inverse_norm = std::max(inverse_norm, inverse_row);
  }
  out.qr_reciprocal_condition_inf = (1.0 / norm) / inverse_norm;
  if (!std::isfinite(out.qr_reciprocal_condition_inf) ||
      out.qr_reciprocal_condition_inf < kAmericanMarkovFitMinTimeBasisReciprocalCondition) {
    return Err(ErrorCode::OutOfRange, "Markov fit: ill-conditioned quoted-prefix time basis");
  }
  return Ok();
}
} // namespace

Status validate_markov_time_knots(const AmericanMarkovFitConfig &config) {
  if (config.time_shape != AmericanMarkovFitTimeShape::VarianceTimeKnots) {
    if ((config.time_shape != AmericanMarkovFitTimeShape::Shared &&
         config.time_shape != AmericanMarkovFitTimeShape::PerVarianceInterval) ||
        !config.variance_time_knots.empty()) {
      return Err(ErrorCode::InvalidArgument, "Markov fit: time knots require knot time shape");
    }
    return Ok();
  }
  const auto states = std::max(std::size_t{1}, config.log_state_knots.size());
  const auto times = config.variance_time_knots.size();
  if (times < 2 || config.max_parameters > kAmericanMarkovFitMaxParameters ||
      times > config.max_parameters / states) {
    return Err(ErrorCode::OutOfRange, "Markov fit: time knots exceed parameter capacity");
  }
  double previous = -1.0;
  for (const auto time : config.variance_time_knots) {
    if (!std::isfinite(time) || time < 0.0 || !(time > previous)) {
      return Err(ErrorCode::InvalidArgument, "Markov fit: invalid variance-time knot grid");
    }
    previous = time;
  }
  return Ok();
}

Result<AmericanMarkovFitTimeWeight> markov_time_weight(double time, bool active,
                                                       std::span<const double> knots) {
  AmericanMarkovFitTimeWeight out{0, 0, 0.0, time, active};
  const auto high = std::lower_bound(knots.begin(), knots.end(), time);
  if (high == knots.begin()) return Ok(out);
  if (high == knots.end()) {
    out.left_group = out.right_group = knots.size() - 1;
    return Ok(out);
  }
  out.right_group = static_cast<std::size_t>(high - knots.begin());
  if (*high == time) {
    out.left_group = out.right_group;
    return Ok(out);
  }
  out.left_group = out.right_group - 1;
  out.right_weight = (time - *(high - 1)) / (*high - *(high - 1));
  if (!std::isfinite(out.right_weight) || !(out.right_weight > 0.0) ||
      !(out.right_weight < 1.0)) {
    return Err(ErrorCode::OutOfRange, "Markov fit: time interpolation loses numerical range");
  }
  return Ok(out);
}

Result<AmericanMarkovFitTimeBasisDiagnostics> diagnose_markov_time_basis(
    std::span<const AmericanMarkovFitTimeWeight> prefix, std::size_t groups) {
  if (groups < 2 || groups > kAmericanMarkovFitMaxParameters || prefix.empty() ||
      prefix.size() > kAmericanMarkovMaxTransitions) {
    return Err(ErrorCode::OutOfRange, "Markov fit: time basis capacity exceeded");
  }
  AmericanMarkovFitTimeBasisDiagnostics out;
  out.column_support_counts.resize(groups, 0);
  out.column_l2_norms.resize(groups, 0.0);
  BidiagonalFactor r{std::vector<double>(groups, 0.0), std::vector<double>(groups, 0.0)};
  ATX_TRY_VOID(accumulate_rows(prefix, groups, out, r));
  ATX_TRY_VOID(reciprocal_condition(groups, out, r));
  return Ok(std::move(out));
}

} // namespace atx::vol::detail
