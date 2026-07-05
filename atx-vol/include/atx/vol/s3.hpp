#pragma once

// S3 / SSVI three-parameter volatility shape curve in normalized-strike form
// (Klassen, "Pricing Vanilla Options with a Volatility Surface", Vola Dynamics,
// 2017). This is the deliberately SIMPLE baseline against which the richer
// C8 (c8.hpp) and CStar (cstar.hpp) families are compared apples-to-apples,
// and a closed-form reference for the Roper butterfly density.
//
// ── Normalized strike (NS) coordinate ─────────────────────────────────────
// With F the forward and sigma0 = sigma(T, K=F) the AT-THE-FORWARD (ATF) vol,
// define the ATF total-vol / standard deviation sigma_hat0 = sigma0*sqrt(T)
// and the normalized strike
//     z = log(K/F) / sigma_hat0 = k / sigma_hat0,   k = log(K/F).
// The whole shape is written in z; the z<->k coupling depends ONLY on the ATF
// sigma0 (a global scale), never on the local sigma(z).
//
// ── S3 / SSVI shape (3 params: sigma0, s2, c2 with c2 >= 0) ───────────────
//   sigma^2(z) = sigma0^2 * f(z),
//   f(z) = 0.5*(1 + s2*z) + sqrt( 0.25*(1 + s2*z)^2 + 0.5*c2*z^2 ).
// s2 is the dimensionless skew, c2 the dimensionless curvature. Near z = 0,
//   f(z) = 1 + s2*z + 0.5*c2*z^2 + O(z^3),
// so f(0) = 1 (sigma(0) = sigma0 by construction), f'(0) = s2, f''(0) = c2.
// The c2 -> 0 limit collapses to the kinked "takeover-for-cash" shape
//   f(z) = max(1 + s2*z, 0).
//
// Total variance is w(k) = T*sigma^2 = sigma_hat0^2 * f(z), with z = k/sigma_hat0.
//
// ── Wings ─────────────────────────────────────────────────────────────────
// The asymptotic slope of sigma^2/sigma0^2 = f(z) in |z| is C_pm:
//   C_plus  = sqrt(0.25*s2^2 + 0.5*c2) + 0.5*s2   (z -> +inf),
//   C_minus = sqrt(0.25*s2^2 + 0.5*c2) - 0.5*s2   (z -> -inf).
// Lee's moment / wing bound w(y) <= 2|y| becomes sigma_hat0 * C_pm <= 2.
//
// ── Roper butterfly density g ─────────────────────────────────────────────
// With w(y) = T*sigma(y)^2, y = log(K/F), the total-variance Roper density is
//   g(y) = (1 - y*w'/(2w))^2 - 0.25*(1/w + 0.25)*w'^2 + 0.5*w''
// (identical to arb.hpp / arb_check_butterfly). Butterfly-arbitrage-free
// <=> g >= 0 everywhere (Black-Scholes gives g == 1). s3_density_g evaluates
// this in the z coordinate using ANALYTIC first/second derivatives of the S3
// shape (chain rule through z = k/sigma_hat0). At the forward it reduces to
//   g(0) = 1 + 0.5*c2 - 0.25*s2^2*(1 + 0.25*sigma_hat0^2),
// so g(0) >= 0 <=> s2^2 <= (4 + 2*c2)/(1 + 0.25*sigma_hat0^2) (the ATF bound).
//
// Thread-safety: every free function is a pure function of its arguments (no
// globals, no allocation) — safe to call concurrently.

#include <span>

#include "atx/vol/types.hpp"  // Result, ErrorCode

namespace atx::vol {

// ── Parameters ────────────────────────────────────────────────────────────
//
// Aggregate; trivially copyable; every member value-initialized. sigma0 is the
// ATF vol (> 0 on any meaningful slice); c2 >= 0 is the curvature.
struct S3Params {
  double sigma0{};  // ATF vol sigma(T, K=F), > 0
  double s2{};      // dimensionless skew   (= f'(0))
  double c2{};      // dimensionless curvature, >= 0  (= f''(0))
};

// Asymptotic wing slopes of sigma^2/sigma0^2 = f(z) in |z|.
struct S3Wings {
  double c_minus{};  // z -> -inf
  double c_plus{};   // z -> +inf
};

// sigma^2(z) = sigma0^2 * f(z) as a function of the normalized strike z. Bare
// evaluator (no domain checks); NaN in => NaN out.
[[nodiscard]] double s3_sigma2_of_z(double z, const S3Params& p) noexcept;

// Total variance w = T*sigma^2 at log-moneyness k = log(K/F) and maturity T.
// Converts k -> z with the ATF sigma0 internally (z = k / (sigma0*sqrt(T))).
// Returns NaN when sigma0*sqrt(T) is not strictly positive.
[[nodiscard]] double s3_total_var(double k_log, double T,
                                  const S3Params& p) noexcept;

// Implied vol sigma = sqrt(w(k, T) / T). NaN when T <= 0 or w is non-positive.
[[nodiscard]] double s3_iv(double k_log, double T, const S3Params& p) noexcept;

// Asymptotic wing slopes {C_minus, C_plus}. Pure algebra; always finite for
// finite (s2, c2 >= 0).
[[nodiscard]] S3Wings s3_wings(const S3Params& p) noexcept;

// Roper butterfly density g at normalized strike z and maturity T, using the
// ANALYTIC first/second derivatives of the S3 shape (see file header). Matches
// arb.hpp's definition exactly. Returns NaN when sigma_hat0 or w is non-positive.
[[nodiscard]] double s3_density_g(double z, double T,
                                  const S3Params& p) noexcept;

// ATF no-arbitrage skew test: true iff g(0) >= 0, i.e.
//   s2^2 <= (4 + 2*c2) / (1 + 0.25*sigma_hat0^2),  sigma_hat0 = sigma0*sqrt(T).
[[nodiscard]] bool s3_atf_arb_free(const S3Params& p, double T) noexcept;

// Closed-form maximum admissible |s2| at the forward for a given curvature and
// ATF stdev: sqrt((4 + 2*c2) / (1 + 0.25*sigma_hat0^2)). At |s2| == this value
// s3_density_g(0, ...) == 0; above it g(0) < 0.
[[nodiscard]] double s3_atf_max_skew(double c2, double sigma_hat0) noexcept;

// Robust least-squares seed of (sigma0, s2, c2) from clean (k_log, iv) samples
// at maturity T. Uses the exact identity f^2 - f = s2*(f*z) + 0.5*c2*z^2 (a
// linear system in (s2, c2) once the ATF stdev sigma_hat0 is fixed) inside a
// 1-D golden-section search over sigma_hat0 that minimizes the total-variance
// residual — no Taylor truncation, so clean samples recover the injected
// parameters to near machine precision.
// @return InvalidArgument if the spans differ in length, carry fewer than 3
//         samples, contain a non-finite / non-positive iv, have no spread in k,
//         or T <= 0; Internal if the search fails to produce a finite fit.
[[nodiscard]] Result<S3Params> s3_seed_from_ivs(std::span<const double> k_log,
                                                std::span<const double> iv,
                                                double T) noexcept;

}  // namespace atx::vol
