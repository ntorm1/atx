// ── American / European greeks via a taped boundary tangent — WS-P P2 + P3-pre ──
//
// READ THE NAME WARNING IN adjoint_greeks.hpp FIRST (L5 T6). "Adjoint" here is
// historical. The EUROPEAN path below (`euro_reverse`) is a real hand-coded
// reverse sweep, first order only. The AMERICAN path is FORWARD-MODE tangent
// propagation (`boundary_tangent_through_iters`), both of its Jacobian-vector
// products taken by finite-differencing `al_apply_boundary_sweep`, run once per
// parameter — no λ, no Jᵀ solve, no reverse accumulation. What Christianson
// (1994) supplies is the attractive-fixed-point argument that licenses dropping
// the seed tangent ẏ⁰, and the licence to differentiate THROUGH the actual
// budget-limited iteration rather than the exact fixed point — which is what
// superseded the P2 IFT (mark-consistent only on the narrow well-converged
// subset) and widened the domain. It is not evidence that this file accumulates
// in reverse.
//
// docs/adjoint_greeks_design.md holds the full DESIGN and the primary-source
// citations (Giles-Glasserman 2006; Savine ch.9; Henrard/OpenGamma AAD+IFT 2011;
// Christianson 1994 reverse accumulation of attractive fixed points;
// Griewank-Walther cheap-gradient bound; Deussen/Naumann EuroAD-2015 American
// AAD envelope + Γ=0 trap). §4-§5 there describe the adjoint that was specified,
// NOT the kernel that shipped; both carry a marker saying so.
// Class: accuracy-improving.
//
// Pure functions (no globals/statics mutation) — live == backtest bit-for-bit.

#include "pricing/adjoint_greeks.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include "atx/core/math.hpp"
#include "atx/vol/api/pricing/american.hpp"

#include "pricing/american_boundary.hpp" // amer:: boundary + residual + price seams

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
// American adjoint via Christianson through-iterations (early-exercise puts, r > 0)
// ══════════════════════════════════════════════════════════════════════════

using amer::AlBoundary;
using amer::AlScheme;
using amer::AlWorkspace;
using amer::kAlMaxNodes;

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

using amer::AlSolveTape;

// dy*/dθ by FORWARD-MODE TANGENT PROPAGATION through the taped iterations. (The
// name in the surrounding file says "adjoint"; this function is where that name
// stops being true — see the file header. The relevant Christianson result is
// the attractive-fixed-point one that licenses ẏ⁰ = 0 below: Christianson 1994,
// "Reverse accumulation and attractive fixed points", Optim. Methods & Software
// 3:311-326. Reverse accumulation itself is NOT what runs here — cost scales
// with the parameter count, once per `param_sel`.)
//
// The budget-limited solve is y* = seed(θ) then y^k = G_k(y^{k-1}; θ) for the taped
// sweeps. Its EXACT tangent (the derivative the mark actually has, matching fd/al) is
//   ẏ^0 = ∂seed/∂θ ,   ẏ^k = ∂G_k/∂y · ẏ^{k-1} + ∂G_k/∂θ ,   dy*/dθ = ẏ^N.
// Each step needs the directional derivative ∂G_k/∂y·ẏ (a JVP) and the DIRECT partial
// ∂G_k/∂θ. Both are differenced against the SAME base point G_k(y^k;θ) — which the tape
// ALREADY holds as y^{k+1} (al_solve_put_boundary_tape and al_apply_boundary_sweep run
// the bit-identical generic sweep, so y^{k+1} == G_k(y^k;θ) to the last bit). So each
// step is a FORWARD difference reusing y^{k+1} as the unperturbed anchor: ONE sweep for
// the JVP + ONE for the θ-partial (2 apps/step) instead of the 4 a central pair costs —
// halving the tangent's sweep-application count, the dominant per-solve adjoint cost.
// Zero derivation risk (it still differentiates literally the map the pricer ran); the
// O(h) forward-vs-O(h²) central truncation is far below the mark's own ~1e-2 self-noise
// (verified: RealisticGridWideMarkExactParity / AdjointPathAccuracyVsMark stay green vs
// Richardson of the true mark). param_sel: 0 => d/dσ, 1 => d/dr. ydot is node-indexed
// (ydot[0] = 0, node 0 pinned); interior part ydot+1 feeds price_moved.
void boundary_tangent_through_iters(const AlBoundary &bnd, const AlWorkspace &ws,
                                    const AlSolveTape &tape, double sigma, double r, double q,
                                    int param_sel, double *ydot) noexcept {
  const std::uint16_t n = bnd.n;
  const double dth = (param_sel == 0) ? (1.0e-6 * std::fmax(sigma, 1.0e-3)) : 1.0e-6;
  const double sp = (param_sel == 0) ? sigma + dth : sigma;
  const double rp = (param_sel == 1) ? r + dth : r;
  // ẏ^0 = 0: the seed tangent ∂(BAW seed)/∂θ is DROPPED. Christianson's attractive-
  // fixed-point result says the tangent recursion forgets its initial value at the
  // iteration's contraction rate, so the seed tangent's contribution to ẏ^N is damped
  // by ∏M_j and negligible where the sweeps contract (the whole realistic grid: matches
  // al's cold-re-solve vega to ~1e-6 median). And the BAW seed is an iterative solve
  // converged only to ~1e-10, so an FD estimate of its derivative injects far MORE noise
  // than the signal it would add (measured: including it raised the max error 10×). The
  // weakly-contracting corners where the dropped term is non-negligible (T≳1y, low σ)
  // are caught by the self-consistency guard and handed to fd.
  for (std::uint16_t i = 0; i < n; ++i) {
    ydot[i] = 0.0;
  }
  // The JVP (y-direction) and the DIRECT θ-partial are differenced SEPARATELY with
  // independently-scaled steps: a single combined step fails when the tangent grows
  // large through a barely-contracting boundary — the y-perturbation dominates and the
  // θ-perturbation shrinks below the roundoff floor, dropping the direct ∂G/∂θ term (a
  // systematic few-% bias, observed at low-σ/deep-ITM).
  std::array<double, kAlMaxNodes> yp{}, gp{}, jvp{};
  for (std::uint16_t k = 0; k < tape.n_steps; ++k) {
    const double *yb = tape.y_iter[k].data();       // base input to sweep k (from the tape)
    const double *ynext = tape.y_iter[k + 1].data(); // = G_k(yb; θ) — the forward-diff anchor
    // ── JVP  J_k · ẏ : forward difference in the y-direction ẏ (θ held at base), step
    //    scaled so the probe is ~1e-7 regardless of ‖ẏ‖. (Skipped while ẏ≡0.)
    double ymax = 0.0;
    for (std::uint16_t i = 0; i < n; ++i) {
      ymax = std::fmax(ymax, std::fabs(ydot[i]));
    }
    if (ymax > 0.0) {
      // L5 T5: `1e-7 / fmax(ymax, 1.0)` CAPPED the step but never FLOORED it,
      // so the stated "probe is ~1e-7 regardless of ‖ẏ‖" only held for
      // ‖ẏ‖ >= 1. Below that the fmax pinned hy at 1e-7 and the actual
      // perturbation was 1e-7·ymax — at ymax ~ 1e-6 that is a 1e-13 probe
      // against boundary nodes of order K, i.e. ~6 decimal digits of
      // cancellation in `(jvp - ynext)/hy`. Scaling by ymax itself makes the
      // largest perturbed component exactly 1e-7 on BOTH sides of 1, which is
      // what the comment above always claimed. The fmax that remains is an
      // overflow guard on `hy` alone, not a step policy: |hy·ydot[i]| <= 1e-7
      // by construction for every i (ymax is the max |ydot|), so no perturbed
      // node can blow up however small ymax gets.
      const double hy = 1.0e-7 / std::fmax(ymax, 1.0e-280);
      for (std::uint16_t i = 0; i < n; ++i) {
        yp[i] = yb[i] + hy * ydot[i];
      }
      amer::al_apply_boundary_sweep(bnd, ws, yp.data(), sigma, r, q, tape.is_jn[k], jvp.data());
      for (std::uint16_t i = 0; i < n; ++i) {
        jvp[i] = (jvp[i] - ynext[i]) / hy;
      }
    } else {
      for (std::uint16_t i = 0; i < n; ++i) {
        jvp[i] = 0.0;
      }
    }
    // ── DIRECT partial ∂G_k/∂θ : forward difference in θ (y held at the tape point yb,
    //    anchored on y^{k+1} = G_k(yb;θ)).
    amer::al_apply_boundary_sweep(bnd, ws, yb, sp, rp, q, tape.is_jn[k], gp.data());
    for (std::uint16_t i = 0; i < n; ++i) {
      ydot[i] = jvp[i] + (gp[i] - ynext[i]) / dth;
    }
  }
}

// The American adjoint core. Returns nullopt on any regime it does not claim (caller
// falls back to american_greeks_fd). Genuine early-exercise PUTS only. Boundary
// sensitivities (vega/rho/vanna) come from Christianson through-iterations so they
// match the budget-limited mark on the whole domain, not the narrow well-converged
// fixed-point subset the P2 IFT claimed.
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
  AlSolveTape tape{};
  // Tape the budget-limited solve (generic kernel; agrees with production
  // al_solve_put_boundary only to the pure-hoist ~1e-9 tolerance, NOT bit-identically —
  // see al_solve_put_boundary_tape). `tape` holds seed + every swept iterate for the
  // Christianson tangent. The served mark is P0 below (this kernel's own AL price via
  // al_put_price_from_boundary), not this boundary reused as a production mark.
  if (amer::al_solve_put_boundary_tape(K, T, sigma, r, q, sch, bnd, ws, tape) !=
      amer::AlSolveStatus::Ok) {
    return std::nullopt;
  }
  const std::uint16_t n = bnd.n;
  if (n < 2) {
    return std::nullopt;
  }

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

  // ── Boundary tangents dy*/dσ, dy*/dr via Christianson through-iterations — the
  // EXACT derivative of the budget-limited solve, matching the mark (fd/al) on the
  // whole domain (no ‖R‖≤1e-7 fixed-point precondition; that guard is what capped the
  // P2 IFT to the well-converged ~14% and Amdahl-limited the portfolio speedup).
  std::array<double, kAlMaxNodes> ydot_s{}, ydot_r{};
  boundary_tangent_through_iters(bnd, ws, tape, sigma, r, q, /*d/dσ=*/0, ydot_s.data());
  boundary_tangent_through_iters(bnd, ws, tape, sigma, r, q, /*d/dr=*/1, ydot_r.data());

  // vega = dP/dσ (total) = ∂P/∂σ|_y + (∂P/∂y)·(dy*/dσ). Realized as one moving-boundary
  // central difference: move the boundary along its Christianson tangent ẏ_σ by ±h and
  // re-price at σ±h — this reproduces mark(σ±h) to O(h²), i.e. exactly al's cold-re-solve
  // vega, but from ONE taped solve instead of two extra boundary solves. ydot+1 is the
  // interior-node view price_moved consumes (node 0 is pinned, ydot[0]=0).
  const double hsig = 1.0e-4 * std::fmax(sigma, 1.0e-3);
  const double hrho = 1.0e-5;
  const double vega =
      (price_moved(bnd, ws, S, K, T, sigma + hsig, r, q, ydot_s.data() + 1, hsig) -
       price_moved(bnd, ws, S, K, T, sigma - hsig, r, q, ydot_s.data() + 1, -hsig)) /
      (2.0 * hsig);
  // rho's tangent ẏ_r is NOT independently guarded per-point (only vega is, via the
  // cold-re-solve self-consistency check below). rho rides on that: ẏ_r and ẏ_σ share
  // the same taped iteration and contraction, so a corner unstable enough to corrupt ẏ_r
  // corrupts ẏ_σ too and is caught by the vega guard → FD fallback. rho is additionally
  // gated vs Richardson-of-the-mark on the grid in AdjointPathAccuracyVsMark.
  const double rho = (price_moved(bnd, ws, S, K, T, sigma, r + hrho, q, ydot_r.data() + 1, hrho) -
                      price_moved(bnd, ws, S, K, T, sigma, r - hrho, q, ydot_r.data() + 1, -hrho)) /
                     (2.0 * hrho);

  // ── theta / charm: continuation-region Black-Scholes PDE identity (no T-boundary
  // grid derivative). θ = rV - (r-q)S·Δ - ½σ²S²·Γ ; charm = ∂θ/∂S. We are always in
  // the continuation region here: the exercise-region / straddle guard above already
  // handed any point at the intrinsic clamp to the FD bundle, so the PDE identity
  // holds unconditionally at this spot.
  const double theta = r * P0 - (r - q) * S * delta - 0.5 * sigma * sigma * S * S * gamma;
  const double charm = r * delta - (r - q) * (delta + S * gamma) -
                       0.5 * sigma * sigma * (2.0 * S * gamma + S * S * speed);

  // ── vanna = ∂delta/∂σ. Only the FIRST-ORDER boundary motion ẏ_σ enters a mixed 2nd
  // derivative; move the boundary along ẏ_σ (the Christianson tangent) and take a
  // spot-difference of delta on the σ± moved boundaries.
  //
  // L5 T5: this σ step used to be 1.0e-3·max(σ,1e-3), TEN TIMES `hsig` above,
  // with nothing recording why -- so the returned vanna was not the
  // σ-derivative of the returned vega at any consistent order. Sharing `hsig`
  // differences the whole σ direction of the bundle at one scale, and it is
  // not merely tidier: against a nested-Richardson reference over
  // `DiagnosticGaps`' 189-point grid the max |vanna| gap FELL from 1.85464e-3
  // to 1.78845e-3 (-3.6%), with vega, volga and adjoint coverage (179/189)
  // unmoved. Kept as a named local because the σ± boundary-motion scale below
  // reads it.
  const double hsv = hsig;
  const double hSv = 1.0e-3 * S;
  const double dvp =
      (price_moved(bnd, ws, S + hSv, K, T, sigma + hsv, r, q, ydot_s.data() + 1, hsv) -
       price_moved(bnd, ws, S - hSv, K, T, sigma + hsv, r, q, ydot_s.data() + 1, hsv)) /
      (2.0 * hSv);
  const double dvm =
      (price_moved(bnd, ws, S + hSv, K, T, sigma - hsv, r, q, ydot_s.data() + 1, -hsv) -
       price_moved(bnd, ws, S - hSv, K, T, sigma - hsv, r, q, ydot_s.data() + 1, -hsv)) /
      (2.0 * hSv);
  const double vanna = (dvp - dvm) / (2.0 * hsv);

  // ── K5 adjoint first-order-deferral cite (WS-K solve-wall sprint) ─────────
  // The two COLD σ± re-solves below are this adjoint bundle's volga tail; a
  // `first_order_only` adjoint tier would DEFER them. That deferral is CLEAN for
  // production: the backtest resolves FullGreeks to the ANALYTIC route
  // (american_greeks_al), never this adjoint route — RunConfig.price sets
  // analytic_greeks=true (backtest.hpp:309) while PriceOptions::adjoint_greeks
  // defaults false (portfolio_pricer.hpp:513, no production setter); dispatch at
  // portfolio_pricer.cpp:1026-1028 reaches american_greeks_adjoint only when
  // adjoint_greeks==true. The first-order win instead lands on the analytic
  // route's K4 mask (need_vega=false ⇒ σ± solves skipped). See
  // docs/seams/laned-greeks.md ("production route trace").
  // ── volga = ∂²P/∂σ²: COLD σ± boundary RE-SOLVE (captures y_σσ exactly and to
  // full tol — a warm re-solve's residual is amplified by 1/hvol² in the 2nd
  // difference), then 2nd price difference — the proven american_greeks_al route.
  // hvol = 5e-3: large enough that the cold boundary's ~1e-10 residual is not
  // amplified into volga, small enough to keep truncation modest.
  //
  // L5 T5: 5e-3 is ABSOLUTE, and the regime guard at the top of this function
  // admits σ > 1e-6 — so for every σ <= 5e-3 the DOWN-bump `sigma - hvol` was
  // non-positive. The cold re-solve then failed (or was meaningless) and the
  // whole bundle bailed to `std::nullopt`, i.e. the adjoint silently ceded the
  // entire low-σ band it claims. `american_greeks_fd` (american.cpp:3101-3102)
  // and `american_greeks_al` (:3508-3509) both already handle exactly this the
  // same way; copied verbatim rather than invented, so the three σ-bump
  // policies agree at the degenerate end. The residual 50%-relative-bump band
  // just above the switch (σ in (5e-3, 1e-2]) is american.cpp's own established
  // convention at its own step size, deliberately not diverged from here.
  double volga = 0.0;
  {
    double hvol = 5.0e-3;
    if (sigma - hvol <= 0.0) {
      hvol = 0.5 * sigma;
    }
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
      // Self-consistency guard: the Christianson through-iterations vega and this
      // INDEPENDENT cold-re-solve vega both estimate the SAME budget-limited mark
      // derivative, so they must agree closely. A gap means the boundary solver is
      // unstable at this corner (ITM / long-T / low-σ / negative-carry) where even
      // fd is unreliable — hand the point to the FD bundle. (Tolerance covers the
      // coarse hvol=5e-3 re-solve's O(hvol²·volga) truncation vs the tight tangent.)
      const double vega_resolve = (Psp - Psm) / (2.0 * hvol);
      if (std::fabs(vega - vega_resolve) >
          3.0e-2 * (std::fabs(vega) + std::fabs(vega_resolve)) + 1.0e-3) {
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
                                       double q, Side side, double *dP_dq) noexcept {
  AmericanGreeks g{};
  // Degenerate, in andersen_lake_core's OWN two-arm order (T first, then σ) so
  // this entry and the pricer collapse the same way on the same inputs.
  //
  // Arm 1 — T ~ 0 (or a nonsense spot/strike): no time left, so the value is
  // the SPOT intrinsic. That is not an approximation of arm 2, it is its
  // limit: at T <= 1e-12 both e^{-rT} and e^{(r-q)T} are 1 to well inside
  // double precision, so the discounted-forward form below degenerates to
  // exactly this. Matches `andersen_lake_core`'s T ~ 0 arm and
  // `american_greeks_fd`'s `Pput`/`Pcall` lambdas.
  if (T <= 1.0e-12 || S <= 0.0 || K <= 0.0) {
    const double intr = (side == Side::Put) ? (K - S) : (S - K);
    g.price = intr > 0.0 ? intr : 0.0;
    if (intr > 0.0) {
      g.delta = (side == Side::Put) ? -1.0 : 1.0;
    }
    if (dP_dq != nullptr) {
      *dP_dq = 0.0; // no time left: no carry sensitivity
    }
    return g;
  }
  // Arm 2 (L5 T3) — σ ~ 0 WITH time left. This used to return the bare spot
  // intrinsic with delta = ±1, which is verbatim the defect already found and
  // fixed on the American FD path: see `american_greeks_fd`'s `Pput` comment
  // ("The sigma arm used to return the bare spot intrinsic, which disagrees
  // with the pricer by the whole discounted-forward intrinsic on a
  // carry-dominant contract") and `sigma_zero_american_limit`. It also
  // disagreed with `black76_greeks`, whose own σ ~ 0 delta is ±df, not ±1.
  //
  // The EUROPEAN σ -> 0 limit is the discounted FORWARD intrinsic
  //
  //     P = df · max(sgn·(F - K), 0),   F = S·e^{(r-q)T},  df = e^{-rT},
  //     sgn = +1 (call) / -1 (put)
  //
  // with NO spot-intrinsic floor: `sigma_zero_american_limit`'s
  // max(·, immediate exercise) term is an AMERICAN early-exercise right, and
  // this entry prices the European contract (it is reached from
  // `american_greeks_adjoint` only in the regime where American == European
  // exactly). Both directions of the old error are real: the spot intrinsic
  // UNDERSTATES a carry-dominant put (r = 0, q > 0: df·(K-F) > 0 at spot
  // intrinsic 0) and OVERSTATES a spot-ITM put under positive rates (it drops
  // the discount factor entirely).
  //
  // Rewriting the ITM branch as P = sgn·(S·e^{-qT} - K·e^{-rT}) makes the
  // whole first-order row exact and elementary, so it is filled in rather than
  // left at the zeros that would now contradict the price beside them:
  //     delta  = ∂P/∂S  = sgn·e^{-qT}            (= sgn·df·e^{(r-q)T})
  //     rho    = ∂P/∂r  = sgn·K·T·e^{-rT}
  //     ∂P/∂q           = -sgn·T·S·e^{-qT}
  //     theta  = -∂P/∂T = sgn·(q·S·e^{-qT} - r·K·e^{-rT})   (calendar-time)
  //     charm  = ∂theta/∂S = sgn·q·e^{-qT} = q·delta
  // Every second-order greek stays 0: P is affine in S on each side of the
  // strike and carries no σ dependence at all, so gamma/vega/vanna/volga are
  // exactly zero rather than merely unpopulated.
  if (sigma <= 1.0e-8) {
    const double df = std::exp(-r * T);
    const double carry = std::exp((r - q) * T); // spot -> forward
    const double F = S * carry;
    const double sgn = (side == Side::Put) ? -1.0 : 1.0;
    const double fwd_intr = sgn * (F - K);
    if (fwd_intr > 0.0) {
      const double disc_spot = S * std::exp(-q * T); // = df * F
      g.price = df * fwd_intr;
      g.delta = sgn * df * carry;
      g.rho = sgn * K * T * df;
      g.theta = sgn * (q * disc_spot - r * K * df);
      g.charm = q * g.delta;
      if (dP_dq != nullptr) {
        *dP_dq = -sgn * T * disc_spot;
      }
    } else if (dP_dq != nullptr) {
      *dP_dq = 0.0; // out of the money at σ = 0: identically zero, all derivatives too
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
  // G2: expose the reverse sweep's ∂P/∂q — the carry component AmericanGreeks omits.
  if (dP_dq != nullptr) {
    *dP_dq = fo.dq;
  }
  return g;
}

Result<AmericanGreeks> american_greeks_adjoint(double S, double K, double T, double sigma,
                                               double r, double q, Side side,
                                               const std::optional<AlOpts> &opts,
                                               bool *took_adjoint_path) {
  if (took_adjoint_path != nullptr) {
    *took_adjoint_path = false;
  }
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
      if (took_adjoint_path != nullptr) {
        *took_adjoint_path = true; // the genuine IFT-adjoint path produced this result
      }
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
