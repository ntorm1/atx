#include "atx/engine/risk/garleanu_pedersen.hpp"

// atx::engine::risk — gp_aim_and_value body (S8.7 scalar-Λ GP aim + value-curvature).
// S8.8a header/source split: the closed-form body lives here so multi_horizon.hpp and
// the test no longer re-parse it. PURE refactor — byte-identical (R10): same order-fixed
// copy + the same cached-Cholesky factored (2λV)⁻¹ apply (FactorModel::apply_inverse).

#include <cmath>   // std::isnan
#include <utility> // std::move

#include "atx/core/macro.hpp" // ATX_ASSERT

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

// S4-5a [B8]: trade partway toward the AIM (not the freshly-shaped target).
// Elementwise blend: `w[i] = prev[i] + trade_rate*(aim_pos[i]-prev[i])`.
//
// trade_rate==1.0 is SPECIAL-CASED to `w[i] = aim_pos[i]` (a direct copy, no
// arithmetic) rather than relying on the algebra `prev + 1.0*(aim-prev)`
// simplifying to `aim` in floating point -- it does NOT, in general: forming
// `aim - prev` then adding `prev` back is TWO separate roundings, and when
// `prev` and `aim_pos` differ by many ULPs (e.g. a stale prior book far from
// a small aim) the round-trip can land a few ULPs off `aim_pos` (found via
// this unit's own RED test: `GpFullRateByteIdentical_RealMarkowitzTarget`
// initially failed the naive arithmetic form with `prev={10,-10}` against an
// aim near `{0.92, 0.14}` -- 0.92198581560283799 vs 0.9219858156028371, a
// real, reproducible divergence, not a hypothetical one). The special case
// makes the documented boundary pin an ACTUAL bit-identity, not a usually-true
// approximation.
std::vector<atx::f64> gp_turnover_native_step(std::span<const atx::f64> prev,
                                              std::span<const atx::f64> aim_pos,
                                              atx::f64 trade_rate) {
  ATX_ASSERT(prev.size() == aim_pos.size());
  std::vector<atx::f64> out(prev.size(), 0.0);
  if (trade_rate == 1.0) {
    for (atx::usize i = 0; i < prev.size(); ++i) {
      out[i] = aim_pos[i]; // exact copy -- see the boundary-pin note above
    }
    return out;
  }
  for (atx::usize i = 0; i < prev.size(); ++i) {
    out[i] = prev[i] + trade_rate * (aim_pos[i] - prev[i]);
  }
  return out;
}

} // namespace atx::engine::risk
