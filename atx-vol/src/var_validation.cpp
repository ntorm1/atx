#include "atx/vol/var_validation.hpp"

#include <algorithm>
#include <cmath>

#include "atx/core/error.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// count * ln(count / denominator), with the `count == 0 -> 0.0` limit taken
// before the division -- so a zero count never queries its denominator (see
// var_validation.hpp's degenerate-case policy comment). Every call site
// below only ever passes a `denominator` that is itself a sum including
// `count`, so `denominator > 0` whenever `count > 0` reaches the division.
[[nodiscard]] double xlogx_over(double count, double denominator) noexcept {
  if (count == 0.0) {
    return 0.0;
  }
  return count * std::log(count / denominator);
}

// chi-square 1-dof survival function via the closed-form erfc relation.
// `lr` is clamped to [0, +inf) first: a likelihood-ratio statistic can land
// a few ULPs below its analytic 0.0 minimum at an exact-fit point purely
// from floating-point summation order, and erfc's domain does not accept a
// negative sqrt argument.
[[nodiscard]] double chi_square_1dof_survival(double lr) noexcept {
  const double clamped = std::max(lr, 0.0);
  return std::erfc(std::sqrt(clamped / 2.0));
}

// chi-square 2-dof survival function via the closed-form exp relation.
[[nodiscard]] double chi_square_2dof_survival(double lr) noexcept {
  const double clamped = std::max(lr, 0.0);
  return std::exp(-clamped / 2.0);
}

} // namespace

Result<KupiecResult> kupiec_pof(std::size_t n_obs, std::size_t n_breaches, double var_confidence) {
  if (n_obs == 0u || n_breaches > n_obs || !std::isfinite(var_confidence) ||
      var_confidence <= 0.0 || var_confidence >= 1.0) {
    return Err(ErrorCode::InvalidArgument, "Kupiec POF test: invalid input");
  }
  const double n = static_cast<double>(n_obs);
  const double x = static_cast<double>(n_breaches);
  const double p = 1.0 - var_confidence;

  // Null hypothesis (breach probability fixed at p): p is strictly in
  // (0, 1) by the validation above, so both log terms are always finite --
  // the coefficients (n-x, x) may legitimately be zero while the log
  // argument itself stays well-defined, so no xlogx_over short-circuit is
  // needed here.
  const double ll_null = (n - x) * std::log(1.0 - p) + x * std::log(p);
  // Alternative hypothesis (breach probability at its MLE, x/n): x/n can be
  // exactly 0 or 1 when x is 0 or n respectively, so both terms route
  // through xlogx_over's count==0 short-circuit to avoid log(0).
  const double ll_alt = xlogx_over(n - x, n) + xlogx_over(x, n);
  const double lr = std::max(-2.0 * ll_null + 2.0 * ll_alt, 0.0);

  KupiecResult result;
  result.lr_pof = lr;
  result.p_value = chi_square_1dof_survival(lr);
  result.n_obs = n_obs;
  result.n_breaches = n_breaches;
  return Ok(result);
}

Result<ChristoffersenResult> christoffersen(std::span<const bool> breach_sequence,
                                            double var_confidence) {
  if (breach_sequence.size() < 2u) {
    return Err(ErrorCode::InvalidArgument,
               "Christoffersen test: breach_sequence needs at least 2 observations");
  }
  std::size_t n00 = 0u;
  std::size_t n01 = 0u;
  std::size_t n10 = 0u;
  std::size_t n11 = 0u;
  std::size_t n_breaches = breach_sequence.front() ? 1u : 0u;
  for (std::size_t i = 1u; i < breach_sequence.size(); ++i) {
    const bool from = breach_sequence[i - 1u];
    const bool to = breach_sequence[i];
    n_breaches += to ? 1u : 0u;
    if (!from && !to) {
      ++n00;
    } else if (!from && to) {
      ++n01;
    } else if (from && !to) {
      ++n10;
    } else {
      ++n11;
    }
  }

  ATX_TRY(const KupiecResult kupiec,
          kupiec_pof(breach_sequence.size(), n_breaches, var_confidence));

  const double d0 = static_cast<double>(n00 + n01); // transitions starting "no breach"
  const double d1 = static_cast<double>(n10 + n11); // transitions starting "breach"
  const double dt = d0 + d1;                        // total transitions == n_obs - 1
  const double f00 = static_cast<double>(n00);
  const double f01 = static_cast<double>(n01);
  const double f10 = static_cast<double>(n10);
  const double f11 = static_cast<double>(n11);

  // Unrestricted model: independent within-state breach rates pi01, pi11.
  const double ll_unrestricted =
      xlogx_over(f00, d0) + xlogx_over(f01, d0) + xlogx_over(f10, d1) + xlogx_over(f11, d1);
  // Restricted (null) model: a single breach rate pi shared by both rows.
  const double ll_restricted = xlogx_over(f00 + f10, dt) + xlogx_over(f01 + f11, dt);
  const double lr_independence = std::max(-2.0 * (ll_restricted - ll_unrestricted), 0.0);

  ChristoffersenResult result;
  result.lr_independence = lr_independence;
  result.p_independence = chi_square_1dof_survival(lr_independence);
  result.lr_conditional_coverage = kupiec.lr_pof + lr_independence;
  result.p_conditional_coverage = chi_square_2dof_survival(result.lr_conditional_coverage);
  return Ok(result);
}

} // namespace atx::vol
