#pragma once

// Batched (SoA) second-order Taylor P&L-explain — the portfolio pnl-explain
// hot path, vectorized.
//
// Given a book's per-position base Greeks and the per-position state moves
// (dS/dSigma/dt/dr taken over base -> shifted), this decomposes each position's
// P&L into the eight analytic Taylor components atx-vol uses in
// PortfolioPricer::pnl_explain (portfolio_pricer.cpp, scatter_pnl_rows):
//
//   pnl_delta = delta·dS            pnl_theta = theta·dt
//   pnl_gamma = ½·gamma·dS²         pnl_rho   = rho·dr
//   pnl_vega  = vega·dSigma         pnl_vanna = vanna·dS·dSigma
//   pnl_volga = ½·volga·dSigma²     pnl_charm = charm·dS·dt
//   total     = Σ of the eight components         (the "explained" P&L)
//
// Each component (and the total) is scaled by the position weight
// w = qty·multiplier — supplied as the single `qty` column (already the product
// of contracts and contract multiplier), or 1.0 when `qty == nullptr`.
//
// Units mirror atx-vol exactly: dS is the ABSOLUTE forward move (not a relative
// return), dSigma the absolute lognormal-vol change (sigma_shifted - sigma_base),
// dt the calendar-time move that theta/charm are taken over, dr the absolute rate
// move. `total` is the Taylor-explained sum only; the FULL pnl_explain forms
// `unexplained = (reprice PnL) - total` at the call site, which owns the reprice.
//
// Layout: structure-of-arrays, all columns length n, contiguous doubles, and the
// output columns must not alias the inputs. Dispatches to a 4-lane AVX2 kernel
// when the host supports it (atx::vol::simd::have_avx2()) and to a scalar loop —
// the numerical source of truth — otherwise; the AVX2 path evaluates the identical
// association tree and reproduces the scalar loop BIT-FOR-BIT, so a position's P&L
// depends on neither the dispatched route nor its index within the batch. Passing
// n == 0 is a no-op.
//
// noexcept and allocation-free — safe to call concurrently from any threads (no
// shared mutable state; the CPUID cache is init-once).

#include <cstddef>

namespace atx::vol::simd {

// Non-owning SoA input columns for a Taylor P&L-explain batch. Every Greek and
// shock column has length n and is a contiguous array of doubles. `qty` is the
// per-position weight (contracts · multiplier); pass nullptr to weight every
// position by 1.0. Columns must not overlap the PnlExplainOutputs arrays.
struct PnlExplainInputs {
  const double* delta;  // ∂P/∂F   base Greeks (per share)
  const double* gamma;  // ∂²P/∂F²
  const double* vega;   // ∂P/∂sigma
  const double* volga;  // ∂²P/∂sigma²
  const double* vanna;  // ∂²P/∂F∂sigma
  const double* theta;  // ∂P/∂t (calendar)
  const double* rho;    // ∂P/∂r
  const double* charm;  // ∂²P/∂F∂t
  const double* qty;    // position weight = qty·multiplier; nullptr => 1.0
  const double* dS;     // absolute forward move (base -> shifted), per share
  const double* dSigma; // absolute vol change (sigma_shifted - sigma_base)
  const double* dt;     // calendar-time move theta/charm are taken over
  const double* dr;     // absolute rate move
};

// Non-owning SoA output columns, each length n and distinct from the inputs.
// The eight component columns sum (per position) to `total`; `total` is the
// Taylor-explained P&L (position-weighted). `total` is formed as the left-to-right
// sum of the SAME eight per-share products the columns carry, so with qty ==
// nullptr (weight exactly 1.0) the identity is BIT-EXACT on either route; with a
// weight it holds to floating-point tolerance only, because w·Σpₖ and Σ(w·pₖ) are
// different roundings of the same quantity.
struct PnlExplainOutputs {
  double* delta_pnl;
  double* gamma_pnl;
  double* vega_pnl;
  double* volga_pnl;
  double* vanna_pnl;
  double* theta_pnl;
  double* rho_pnl;
  double* charm_pnl;
  double* total;
};

// Decompose each position's second-order Taylor P&L into the eight component
// columns and their total. out[k] = w·(component k), total = w·Σ components,
// with w = in.qty[i] (or 1.0 when in.qty == nullptr). n == 0 is a no-op.
void pnl_taylor_explain_batch(const PnlExplainInputs& in,
                              const PnlExplainOutputs& out,
                              std::size_t n) noexcept;

} // namespace atx::vol::simd
