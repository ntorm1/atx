#include "atx/engine/risk/garleanu_pedersen.hpp"

// atx::engine::risk — gp_aim_and_value body (S8.7 scalar-Λ GP aim + value-curvature).
// S8.8a header/source split: the closed-form body lives here so multi_horizon.hpp and
// the test no longer re-parse it. PURE refactor — byte-identical (R10): same order-fixed
// copy + the same cached-Cholesky factored (2λV)⁻¹ apply (FactorModel::apply_inverse).

#include <cmath>   // std::isnan
#include <utility> // std::move

#include "atx/engine/risk/factor_model.hpp" // FactorModel (factored V apply path, R4)

namespace atx::engine::risk {

atx::core::Result<GpAimValue> gp_aim_and_value(std::span<const atx::f64> alpha_bar,
                                               const FactorModel &V, atx::f64 lambda) {
  namespace co = atx::core;
  const atx::usize m = V.n_instruments();
  if (alpha_bar.size() != m) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "gp_aim_and_value: alpha_bar length must equal V.n_instruments()");
  }
  if (lambda < 0.0) {
    return co::Err(co::ErrorCode::InvalidArgument, "gp_aim_and_value: lambda must be >= 0");
  }

  GpAimValue out;
  out.alpha_bar.assign(alpha_bar.begin(), alpha_bar.end());
  out.aim_pos.assign(m, 0.0);

  // λ == 0 ⇒ no risk curvature to invert; the position aim IS the pure-alpha direction
  // (NaN preserved). This branch never feeds a curvature fold (P = 0 at λ = 0).
  if (lambda == 0.0) {
    out.aim_pos.assign(alpha_bar.begin(), alpha_bar.end());
    return co::Ok(std::move(out));
  }

  // aim_pos = (2λV)⁻¹ ᾱ = (1/2λ)·V⁻¹·ᾱ. A no-opinion (NaN) ᾱ name is a 0-weight name in
  // the V⁻¹ apply (carries no return tilt); the FactorModel apply has no NaN-exclusion
  // path, so we zero NaN cells into the apply input (mirrors the QP's q = −ᾱ NaN→0 rule).
  std::vector<atx::f64> rhs(m, 0.0);
  for (atx::usize i = 0; i < m; ++i) {
    rhs[i] = std::isnan(alpha_bar[i]) ? 0.0 : alpha_bar[i];
  }
  std::vector<atx::f64> vinv(m, 0.0);
  V.apply_inverse(std::span<const atx::f64>(rhs), std::span<atx::f64>(vinv)); // V⁻¹ ᾱ
  const atx::f64 inv = 1.0 / (2.0 * lambda);
  for (atx::usize i = 0; i < m; ++i) {
    out.aim_pos[i] = inv * vinv[i]; // (1/2λ)·V⁻¹ ᾱ
  }
  return co::Ok(std::move(out));
}

} // namespace atx::engine::risk
