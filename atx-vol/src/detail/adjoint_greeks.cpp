// ── Adjoint (AAD) American / European greeks — WS-P P2 ────────────────────
//
// Hand-coded adjoint algorithmic differentiation of the Andersen-Lake American
// pricer with implicit-function-theorem (IFT) differentiation THROUGH the
// early-exercise boundary. See docs/adjoint_greeks_design.md for the full design
// and the primary-source citations (Giles-Glasserman 2006; Savine ch.9; Henrard/
// OpenGamma AAD+IFT 2011; Griewank-Walther cheap-gradient bound; Deussen/Naumann
// EuroAD-2015 American AAD envelope + Γ=0 trap). Class: accuracy-improving.
//
// Pure functions (no globals/statics mutation) — live == backtest bit-for-bit.

#include "atx/vol/detail/adjoint_greeks.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include "atx/core/math.hpp"
#include "atx/vol/american.hpp"

#include "../american_boundary.hpp" // amer:: boundary + residual + price seams

namespace atx::vol::detail {

using atx::core::Err;
using atx::core::Ok;

namespace {

using atx::core::norm_cdf;
using atx::core::norm_pdf;

// ══════════════════════════════════════════════════════════════════════════
// European (Black-Scholes-Merton spot form, continuous yield q) adjoint
// ══════════════════════════════════════════════════════════════════════════
//
// The exact American price in the no-early-exercise regime (American == European)
// AND the TDD rung-1 validation of the adjoint architecture. First order is a
// hand-coded REVERSE sweep of the price graph; the raw chain rule reproduces the
// simplified closed forms (F·φ(d1) = K·φ(d2) cancellation happens numerically).
//
// price(put)  = df·(K·Φ(-d2) - F·Φ(-d1))     F = S·e^{(r-q)T}, df = e^{-rT}
// price(call) = df·(F·Φ(d1)  - K·Φ(d2))       d1 = ln(F/K)/v + v/2, d2 = d1 - v
// with v = σ√T. Reference: Giles-Glasserman "Smoking Adjoints" (RISK 2006) — the
// reverse sweep gives the whole input gradient in one pass.

struct EuroFirstOrder {
  double price;
  double dS, dK, dT, dsig, dr, dq; // full direct-input gradient (Jacobian row)
};

[[nodiscard]] EuroFirstOrder euro_reverse(double S, double K, double T, double sigma, double r,
                                          double q, Side side) noexcept {
  // ── forward ──
  const double sqrtT = std::sqrt(T);
  const double v = sigma * sqrtT;
  const double EE = std::exp((r - q) * T); // spot→forward carry
  const double F = S * EE;
  const double df = std::exp(-r * T);
  const double lnFK = std::log(F / K);
  const double d1 = lnFK / v + 0.5 * v;
  const double d2 = d1 - v;
  const double phi1 = norm_pdf(d1);
  const double phi2 = norm_pdf(d2);

  double price = 0.0;
  double dfbar = 0.0, Fbar = 0.0, Kbar = 0.0, d1bar = 0.0, d2bar = 0.0;
  if (side == Side::Put) {
    const double Nm1 = norm_cdf(-d1);
    const double Nm2 = norm_cdf(-d2);
    price = df * (K * Nm2 - F * Nm1);
    // reverse of price = df*(K*Nm2 - F*Nm1)
    dfbar = K * Nm2 - F * Nm1;
    Kbar = df * Nm2;
    Fbar = -df * Nm1;
    const double Nm2bar = df * K;
    const double Nm1bar = -df * F;
    d1bar = Nm1bar * (-phi1); // dΦ(-d1)/dd1 = -φ(d1)
    d2bar = Nm2bar * (-phi2);
  } else {
    const double Np1 = norm_cdf(d1);
    const double Np2 = norm_cdf(d2);
    price = df * (F * Np1 - K * Np2);
    dfbar = F * Np1 - K * Np2;
    Fbar = df * Np1;
    Kbar = -df * Np2;
    const double Np1bar = df * F;
    const double Np2bar = -df * K;
    d1bar = Np1bar * phi1; // dΦ(d1)/dd1 = φ(d1)
    d2bar = Np2bar * phi2;
  }

  // ── common reverse tail ──
  // d2 = d1 - v
  d1bar += d2bar;
  double vbar = -d2bar;
  // d1 = lnFK/v + 0.5*v
  const double lnFKbar = d1bar / v;
  vbar += d1bar * (-lnFK / (v * v) + 0.5);
  // lnFK = log(F/K)
  Fbar += lnFKbar / F;
  Kbar += lnFKbar * (-1.0 / K);
  // v = sigma*sqrtT
  const double dsig = vbar * sqrtT;
  double sqrtTbar = vbar * sigma;
  // sqrtT = sqrt(T)
  double Tbar = sqrtTbar * 0.5 / sqrtT;
  // df = exp(-r*T)
  double rbar = dfbar * df * (-T);
  Tbar += dfbar * df * (-r);
  // F = S*exp((r-q)*T)
  const double dS = Fbar * EE;
  rbar += Fbar * F * T;
  const double dq = Fbar * (-F * T);
  Tbar += Fbar * F * (r - q);

  return EuroFirstOrder{price, dS, Kbar, Tbar, dsig, rbar, dq};
}

// Exact BSM second-order closed forms (continuous q). Reference: Haug,
// "Complete Guide to Option Pricing Formulas" (2nd ed.), gamma/vanna/vomma/charm.
struct EuroSecondOrder {
  double gamma, vanna, volga, charm;
};

[[nodiscard]] EuroSecondOrder euro_second_order(double S, double K, double T, double sigma,
                                                double r, double q, Side side) noexcept {
  const double sqrtT = std::sqrt(T);
  const double v = sigma * sqrtT;
  const double F = S * std::exp((r - q) * T);
  const double lnFK = std::log(F / K);
  const double d1 = lnFK / v + 0.5 * v;
  const double d2 = d1 - v;
  const double eqT = std::exp(-q * T);
  const double phi1 = norm_pdf(d1);

  const double gamma = eqT * phi1 / (S * v);
  const double vanna = -eqT * phi1 * d2 / sigma;
  const double vega = S * eqT * phi1 * sqrtT;
  const double volga = vega * d1 * d2 / sigma;
  // charm = ∂θ/∂S = -∂²P/∂S∂T (calendar-time "delta decay"); matches the codebase
  // convention used by american_greeks_fd/al.
  const double common = eqT * phi1 * (2.0 * (r - q) * T - d2 * v) / (2.0 * T * v);
  double charm;
  if (side == Side::Call) {
    charm = q * eqT * norm_cdf(d1) - common;
  } else {
    charm = -q * eqT * norm_cdf(-d1) - common;
  }
  return EuroSecondOrder{gamma, vanna, volga, charm};
}

// ══════════════════════════════════════════════════════════════════════════
// American IFT-adjoint (genuine early-exercise puts, r > 0)
// ══════════════════════════════════════════════════════════════════════════

using amer::AlBoundary;
using amer::AlScheme;
using amer::AlWorkspace;
using amer::kAlMaxNodes;

// Dense partial-pivot LU (Doolittle), row-major stride `n`, in place. A small
// well-conditioned system (m ≤ 31; J = I - ∂G/∂y with the fixed-point map G a
// contraction ⇒ eigenvalues clustered near 1). Own LU (not the file-static one in
// american.cpp) so this TU is self-contained. Returns false on a zero pivot.
[[nodiscard]] bool lu_factor(double *A, int n, int *piv) noexcept {
  for (int i = 0; i < n; ++i) {
    piv[i] = i;
  }
  for (int k = 0; k < n; ++k) {
    int p = k;
    double amax = std::fabs(A[k * n + k]);
    for (int i = k + 1; i < n; ++i) {
      const double v = std::fabs(A[i * n + k]);
      if (v > amax) {
        amax = v;
        p = i;
      }
    }
    if (!(amax > 0.0)) {
      return false;
    }
    if (p != k) {
      for (int j = 0; j < n; ++j) {
        std::swap(A[k * n + j], A[p * n + j]);
      }
      std::swap(piv[k], piv[p]);
    }
    const double akk = A[k * n + k];
    for (int i = k + 1; i < n; ++i) {
      const double f = A[i * n + k] / akk;
      A[i * n + k] = f;
      for (int j = k + 1; j < n; ++j) {
        A[i * n + j] -= f * A[k * n + j];
      }
    }
  }
  return true;
}

// Solve A x = b from a factorization (LU + piv), b overwritten with x.
void lu_solve(const double *LU, const int *piv, double *b, int n) noexcept {
  std::array<double, kAlMaxNodes> y{};
  for (int i = 0; i < n; ++i) {
    y[i] = b[piv[i]];
  }
  for (int i = 0; i < n; ++i) { // forward (unit lower)
    double s = y[i];
    for (int j = 0; j < i; ++j) {
      s -= LU[i * n + j] * y[j];
    }
    y[i] = s;
  }
  for (int i = n - 1; i >= 0; --i) { // back (upper)
    double s = y[i];
    for (int j = i + 1; j < n; ++j) {
      s -= LU[i * n + j] * y[j];
    }
    y[i] = s / LU[i * n + i];
  }
  for (int i = 0; i < n; ++i) {
    b[i] = y[i];
  }
}

// Price from a boundary whose interior nodes are y0 + scale*dir (node 0 pinned).
[[nodiscard]] double price_moved(const AlBoundary &base, const AlWorkspace &ws, double S, double K,
                                 double T, double sigma, double r, double q, const double *dir,
                                 double scale) noexcept {
  AlBoundary scr = base;
  const std::uint16_t n = base.n;
  for (std::uint16_t i = 1; i < n; ++i) {
    scr.y[i] = base.y[i] + scale * dir[i - 1]; // dir indexed over interior nodes
  }
  return amer::al_put_price_from_boundary(scr, ws, S, K, T, sigma, r, q);
}

// The American IFT-adjoint core. Returns nullopt on any regime it does not claim
// (caller falls back to american_greeks_fd). Genuine early-exercise PUTS only.
[[nodiscard]] std::optional<AmericanGreeks>
american_put_adjoint(double S, double K, double T, double sigma, double r, double q,
                     const std::optional<AlOpts> &opts) noexcept {
  // Regime: single-boundary American only (r > 0), non-degenerate.
  if (!(r > 0.0) || T <= 1.0e-6 || sigma <= 1.0e-6) {
    return std::nullopt;
  }
  const AlScheme sch = amer::scheme_from_opts(opts);
  AlBoundary bnd{};
  AlWorkspace ws{};
  if (amer::al_solve_put_boundary(K, T, sigma, r, q, sch, bnd, ws) != amer::AlSolveStatus::Ok) {
    return std::nullopt;
  }
  const std::uint16_t n = bnd.n;
  if (n < 2) {
    return std::nullopt;
  }
  const int m = n - 1; // interior collocation nodes (node 0 pinned)

  auto price_at = [&](double s, double sig, double rr) noexcept {
    return amer::al_put_price_from_boundary(bnd, ws, s, K, T, sig, rr, q);
  };
  const double P0 = price_at(S, sigma, r);

  // ── delta / gamma / speed: frozen (spot-independent) base-boundary stencils.
  // ∂R/∂S = 0 ⇒ no boundary term; exact, and NOT the MC-LSM Γ=0 trap (the boundary
  // genuinely does not depend on S). Bit-identical to american_greeks_al/fd.
  const double hS = 1.0e-3 * S;
  const double vSp = price_at(S + hS, sigma, r);
  const double vSm = price_at(S - hS, sigma, r);
  const double vS2p = price_at(S + 2.0 * hS, sigma, r);
  const double vS2m = price_at(S - 2.0 * hS, sigma, r);
  const double delta = (vSp - vSm) / (2.0 * hS);
  const double gamma = (vSp - 2.0 * P0 + vSm) / (hS * hS);
  const double speed = (vS2p - 2.0 * vSp + 2.0 * vSm - vS2m) / (2.0 * hS * hS * hS);

  // Exercise-region / boundary-straddle guard. If any spot in the delta/gamma/
  // speed stencil is at its intrinsic clamp (i.e. in the S < b exercise region),
  // the stencil straddles the C¹ smooth-pasting boundary — the spot second/third
  // differences (hence the PDE θ/charm built from them) are FD-corrupted there.
  // Hand those razor's-edge points to the FD bundle, which resolves them via its
  // own 17-solve stencils. The adjoint claims strict-continuation puts only.
  auto clamped = [&](double s, double px) noexcept {
    const double intr = K - s;
    return intr > 0.0 && px <= intr + 1.0e-9 * K;
  };
  if (clamped(S, P0) || clamped(S + hS, vSp) || clamped(S - hS, vSm) ||
      clamped(S + 2.0 * hS, vS2p) || clamped(S - 2.0 * hS, vS2m)) {
    return std::nullopt;
  }

  // IFT precondition: the boundary must be a CONVERGED fixed point R(y0) ≈ 0. The
  // scheme early-exits at tol=1e-10 but caps its sweep budget, so a nasty corner
  // (ITM / long-T / low-σ / negative-carry) can return a boundary that still
  // carries residual. Differentiating a non-converged fixed point via IFT is
  // invalid — the derivative would omit the truncated-iteration term. Verify
  // convergence; hand unconverged points to the FD bundle (which re-solves each
  // bumped boundary directly and needs no IFT precondition).
  {
    std::array<double, kAlMaxNodes> R0{};
    amer::al_put_boundary_residual(bnd, ws, bnd.y.data(), sigma, r, q, R0.data());
    double rmax = 0.0;
    for (std::uint16_t i = 1; i < n; ++i) {
      rmax = std::fmax(rmax, std::fabs(R0[i]));
    }
    if (!(rmax <= 1.0e-7)) {
      return std::nullopt;
    }
  }

  // ── Jacobian J = ∂R/∂y (m×m) and ∂P/∂y (m-vector), via central FD of the pure
  // residual seam and the price-from-boundary. Node grid / xmax / K held fixed.
  std::array<double, kAlMaxNodes> y0{};
  for (std::uint16_t i = 0; i < n; ++i) {
    y0[i] = bnd.y[i];
  }
  std::array<double, kAlMaxNodes * kAlMaxNodes> J{}; // row-major stride m
  std::array<double, kAlMaxNodes> Rp{};
  std::array<double, kAlMaxNodes> Rm{};
  std::array<double, kAlMaxNodes> ywork{};
  std::array<double, kAlMaxNodes> Py{};
  for (std::uint16_t i = 0; i < n; ++i) {
    ywork[i] = y0[i];
  }
  for (int jc = 0; jc < m; ++jc) {
    const int jn = jc + 1;
    const double yj = y0[jn];
    const double h = 1.0e-6 * std::fmax(std::fabs(yj), 1.0e-3);
    ywork[jn] = yj + h;
    amer::al_put_boundary_residual(bnd, ws, ywork.data(), sigma, r, q, Rp.data());
    const double Pp_y = [&] {
      AlBoundary scr = bnd;
      scr.y[jn] = yj + h;
      return amer::al_put_price_from_boundary(scr, ws, S, K, T, sigma, r, q);
    }();
    ywork[jn] = yj - h;
    amer::al_put_boundary_residual(bnd, ws, ywork.data(), sigma, r, q, Rm.data());
    const double Pm_y = [&] {
      AlBoundary scr = bnd;
      scr.y[jn] = yj - h;
      return amer::al_put_price_from_boundary(scr, ws, S, K, T, sigma, r, q);
    }();
    ywork[jn] = yj;
    for (int ir = 0; ir < m; ++ir) {
      J[ir * m + jc] = (Rp[ir + 1] - Rm[ir + 1]) / (2.0 * h);
    }
    Py[jc] = (Pp_y - Pm_y) / (2.0 * h);
  }

  // ── R_sigma, R_r (interior m-vectors): central FD of the pure residual.
  const double hs = 1.0e-5 * std::fmax(sigma, 1.0e-3);
  const double hr = 1.0e-6;
  std::array<double, kAlMaxNodes> Rsp{}, Rsm{}, Rrp{}, Rrm{};
  amer::al_put_boundary_residual(bnd, ws, y0.data(), sigma + hs, r, q, Rsp.data());
  amer::al_put_boundary_residual(bnd, ws, y0.data(), sigma - hs, r, q, Rsm.data());
  amer::al_put_boundary_residual(bnd, ws, y0.data(), sigma, r + hr, q, Rrp.data());
  amer::al_put_boundary_residual(bnd, ws, y0.data(), sigma, r - hr, q, Rrm.data());
  std::array<double, kAlMaxNodes> Rsig{}, Rrho{};
  for (int k = 0; k < m; ++k) {
    Rsig[k] = (Rsp[k + 1] - Rsm[k + 1]) / (2.0 * hs);
    Rrho[k] = (Rrp[k + 1] - Rrm[k + 1]) / (2.0 * hr);
  }

  // ── Adjoint boundary multiplier: J^T λ = ∂P/∂y  (ONE transposed solve). Then
  // vega = ∂P/∂σ|_y - λ^T R_σ, rho = ∂P/∂r|_y - λ^T R_r.  Reverse-mode IFT
  // (Henrard/OpenGamma 2011; Giles-Glasserman 2006): the boundary adjoint is
  // solved once regardless of the number of upstream parameters.
  std::array<double, kAlMaxNodes * kAlMaxNodes> Jt{};
  for (int i = 0; i < m; ++i) {
    for (int j = 0; j < m; ++j) {
      Jt[i * m + j] = J[j * m + i];
    }
  }
  std::array<int, kAlMaxNodes> piv{};
  std::array<double, kAlMaxNodes> lam{};
  for (int i = 0; i < m; ++i) {
    lam[i] = Py[i];
  }
  if (!lu_factor(Jt.data(), m, piv.data())) {
    return std::nullopt;
  }
  // Ill-conditioning guard. J = I - ∂G/∂y with G the boundary fixed-point map; a
  // U-pivot ratio near 0 means an eigenvalue of ∂G/∂y is near 1 (the iteration
  // barely contracts — a near-degenerate boundary, e.g. the ITM / long-T / low-σ
  // negative-carry corner). There the IFT amplifies the FD-estimated J/R_σ/∂P/∂y
  // by ~1/pivot, so vega/rho/vanna/volga become unreliable. Hand those points to
  // the robust FD bundle (which re-solves each bumped boundary directly).
  {
    double pmin = std::fabs(Jt[0]);
    double pmax = pmin;
    for (int i = 1; i < m; ++i) {
      const double d = std::fabs(Jt[i * m + i]);
      pmin = std::fmin(pmin, d);
      pmax = std::fmax(pmax, d);
    }
    if (!(pmin > 1.0e-10 * pmax)) {
      return std::nullopt;
    }
  }
  lu_solve(Jt.data(), piv.data(), lam.data(), m);

  // Frozen-boundary direct σ/r partials (boundary y0 held; premium+euro re-evaluated).
  const double hsig_p = 1.0e-4 * std::fmax(sigma, 1.0e-3);
  const double hr_p = 1.0e-5;
  const double dPsig_frozen = (price_at(S, sigma + hsig_p, r) - price_at(S, sigma - hsig_p, r)) /
                              (2.0 * hsig_p);
  const double dPr_frozen = (price_at(S, sigma, r + hr_p) - price_at(S, sigma, r - hr_p)) /
                            (2.0 * hr_p);
  double lam_dot_Rsig = 0.0, lam_dot_Rrho = 0.0;
  for (int k = 0; k < m; ++k) {
    lam_dot_Rsig += lam[k] * Rsig[k];
    lam_dot_Rrho += lam[k] * Rrho[k];
  }
  const double vega = dPsig_frozen - lam_dot_Rsig;
  const double rho = dPr_frozen - lam_dot_Rrho;

  // ── theta / charm: continuation-region Black-Scholes PDE identity (no T-boundary
  // grid derivative). θ = rV - (r-q)S·Δ - ½σ²S²·Γ ; charm = ∂θ/∂S. In the
  // exercised region (V ≤ intrinsic) θ = charm = 0.
  const double intr0 = K - S;
  const bool exercised = (P0 <= intr0 + 1.0e-9 * K) && (intr0 > 0.0);
  double theta = 0.0, charm = 0.0;
  if (!exercised) {
    theta = r * P0 - (r - q) * S * delta - 0.5 * sigma * sigma * S * S * gamma;
    charm = r * delta - (r - q) * (delta + S * gamma) -
            0.5 * sigma * sigma * (2.0 * S * gamma + S * S * speed);
  }

  // ── vanna = ∂delta/∂σ via the FIRST-ORDER boundary tangent y_σ (forward IFT:
  // J y_σ = -R_σ). Only first-order boundary motion enters a mixed 2nd derivative.
  std::array<double, kAlMaxNodes * kAlMaxNodes> Jf = J;
  std::array<int, kAlMaxNodes> pivf{};
  std::array<double, kAlMaxNodes> ysig{};
  for (int k = 0; k < m; ++k) {
    ysig[k] = -Rsig[k];
  }
  double vanna = 0.0;
  if (lu_factor(Jf.data(), m, pivf.data())) {
    lu_solve(Jf.data(), pivf.data(), ysig.data(), m);
    const double hsv = 1.0e-3 * std::fmax(sigma, 1.0e-3);
    const double hSv = 1.0e-3 * S;
    // delta on the σ+ / σ- moved boundaries.
    const double dvp = (price_moved(bnd, ws, S + hSv, K, T, sigma + hsv, r, q, ysig.data(), hsv) -
                        price_moved(bnd, ws, S - hSv, K, T, sigma + hsv, r, q, ysig.data(), hsv)) /
                       (2.0 * hSv);
    const double dvm = (price_moved(bnd, ws, S + hSv, K, T, sigma - hsv, r, q, ysig.data(), -hsv) -
                        price_moved(bnd, ws, S - hSv, K, T, sigma - hsv, r, q, ysig.data(), -hsv)) /
                       (2.0 * hSv);
    vanna = (dvp - dvm) / (2.0 * hsv);
  }

  // ── volga = ∂²P/∂σ²: COLD σ± boundary RE-SOLVE (captures y_σσ exactly and to
  // full tol — a warm re-solve's residual is amplified by 1/hvol² in the 2nd
  // difference), then 2nd price difference — the proven american_greeks_al route.
  // hvol = 5e-3: large enough that the cold boundary's ~1e-10 residual is not
  // amplified into volga, small enough to keep truncation modest.
  double volga = 0.0;
  {
    const double hvol = 5.0e-3;
    AlBoundary bsp{}, bsm{};
    AlWorkspace wsp{}, wsm{};
    const bool okp =
        amer::al_solve_put_boundary(K, T, sigma + hvol, r, q, sch, bsp, wsp) ==
        amer::AlSolveStatus::Ok;
    const bool okm =
        amer::al_solve_put_boundary(K, T, sigma - hvol, r, q, sch, bsm, wsm) ==
        amer::AlSolveStatus::Ok;
    if (okp && okm) {
      const double Psp = amer::al_put_price_from_boundary(bsp, wsp, S, K, T, sigma + hvol, r, q);
      const double Psm = amer::al_put_price_from_boundary(bsm, wsm, S, K, T, sigma - hvol, r, q);
      volga = (Psp - 2.0 * P0 + Psm) / (hvol * hvol);
      // Self-consistency guard: the IFT vega and this INDEPENDENT cold-re-solve
      // vega must agree. A disagreement means the boundary solver is unstable at
      // this corner (the ITM / long-T / low-σ negative-carry region), so neither
      // the IFT nor the re-solve is trustworthy — hand the point to the FD bundle.
      const double vega_resolve = (Psp - Psm) / (2.0 * hvol);
      if (std::fabs(vega - vega_resolve) >
          2.0e-2 * (std::fabs(vega) + std::fabs(vega_resolve) + 1.0)) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }

  AmericanGreeks g{};
  g.price = P0;
  g.delta = delta;
  g.gamma = gamma;
  g.vega = vega;
  g.theta = theta;
  g.rho = rho;
  g.vanna = vanna;
  g.volga = volga;
  g.charm = charm;
  return g;
}

} // namespace

AmericanGreeks european_greeks_adjoint(double S, double K, double T, double sigma, double r,
                                       double q, Side side) noexcept {
  AmericanGreeks g{};
  // Degenerate: collapse to intrinsic to avoid a v=0 division. Matches the
  // pricer's T~0/σ~0 intrinsic policy.
  if (T <= 1.0e-12 || sigma <= 1.0e-8 || S <= 0.0 || K <= 0.0) {
    const double intr = (side == Side::Put) ? (K - S) : (S - K);
    g.price = intr > 0.0 ? intr : 0.0;
    if (intr > 0.0) {
      g.delta = (side == Side::Put) ? -1.0 : 1.0;
    }
    return g;
  }
  const EuroFirstOrder fo = euro_reverse(S, K, T, sigma, r, q, side);
  const EuroSecondOrder so = euro_second_order(S, K, T, sigma, r, q, side);
  g.price = fo.price;
  g.delta = fo.dS;
  g.vega = fo.dsig;
  g.rho = fo.dr;
  g.theta = -fo.dT; // calendar-time
  g.gamma = so.gamma;
  g.vanna = so.vanna;
  g.volga = so.volga;
  g.charm = so.charm;
  return g;
}

Result<AmericanGreeks> american_greeks_adjoint(double S, double K, double T, double sigma,
                                               double r, double q, Side side,
                                               const std::optional<AlOpts> &opts) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_greeks_adjoint: S, K, T, sigma must be > 0");
  }
  // The IFT-adjoint claims genuine early-exercise PUTS in the single-boundary
  // American regime (rate = r > 0). Every other case — calls, the European-exact
  // regime (American == European), degenerate T~0/σ~0, the negative-carry /
  // double-continuation corners, and any boundary that fails to solve — routes to
  // the untouched american_greeks_fd reference (Trap 2: keep the FD path intact).
  if (side == Side::Put &&
      classify_regime(/*rate=*/r, /*yield=*/q) == ExerciseRegime::American) {
    if (std::optional<AmericanGreeks> g = american_put_adjoint(S, K, T, sigma, r, q, opts)) {
      return Ok(*g);
    }
  } else if (side == Side::Put && classify_regime(r, q) == ExerciseRegime::European) {
    // American == European exactly: the closed-form adjoint IS the American mark.
    return Ok(european_greeks_adjoint(S, K, T, sigma, r, q, side));
  }
  return american_greeks_fd(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake, opts,
                            /*warm_start=*/true);
}

} // namespace atx::vol::detail
