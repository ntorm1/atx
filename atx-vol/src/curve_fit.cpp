#include "atx/vol/curve_fit.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/calib.hpp"    // build_observations_european, ObsSet, FitObs
#include "atx/vol/deamer.hpp"   // resolve_chain_forward, european_equiv_iv, otm_side, DeAmOptions
#include "atx/vol/parity.hpp"   // chain_parity, ParityInputs, ParityReport
#include "atx/vol/universe.hpp" // Chain, chain_index

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Minimum usable strikes to attempt a slice fit (mirrors run_surface_parity).
constexpr std::size_t kMinUsableObs = 5;

// A leg's quote is invertible: strictly positive, non-crossed bid/ask, finite
// positive mid. Identical predicate to surface_parity / de_americanize_chain.
[[nodiscard]] bool leg_quote_valid(const Chain& chain, std::size_t idx) noexcept {
  const double bid = chain.bids[idx];
  const double ask = chain.asks[idx];
  const double mid = chain.mids[idx];
  return (bid > 0.0) && (ask > 0.0) && (ask >= bid) && std::isfinite(mid) &&
         (mid > 0.0);
}

// Raw American NBBO bands + de-Americanized market IV per surviving OTM leg —
// the data `chain_parity` scores the re-Americanized model against. Mirrors the
// band/market-iv half of surface_parity's build_aligned_obs (the fit obs itself
// come from build_observations_european, so this only gathers the scoring side).
struct ParityData {
  std::vector<double> strike, bid, ask, mid, k_log, market_iv;
  std::vector<Side> side;
};

[[nodiscard]] ParityData build_parity_data(const Chain& chain, double S, double r,
                                           double F, double q_eff,
                                           const DeAmOptions& deam) {
  const double T = chain.T;
  const std::size_t n = chain.n_strikes();
  ParityData p;
  for (std::size_t i = 0; i < n; ++i) {
    const double K = chain.strikes[i];
    if (!(K > 0.0)) {
      continue;
    }
    const double k = std::log(K / F);
    const Side side = otm_side(k);
    const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
    if (!leg_quote_valid(chain, idx)) {
      continue;
    }
    const Result<double> iv_res = european_equiv_iv(
        chain.mids[idx], S, K, T, r, q_eff, side, deam.method, deam.al_opts,
        deam.caches.for_side(side), deam.iv_tol, deam.iv_max_iter);
    if (!iv_res) {
      continue;
    }
    p.strike.push_back(K);
    p.bid.push_back(chain.bids[idx]);
    p.ask.push_back(chain.asks[idx]);
    p.mid.push_back(chain.mids[idx]);
    p.side.push_back(side);
    p.k_log.push_back(k);
    p.market_iv.push_back(*iv_res);
  }
  return p;
}

}  // namespace

Result<CurveSurfaceReport> fit_curve_surface(const Underlying& under,
                                             const SurfaceParityInputs& in,
                                             const CurveConfig& cfg) {
  if (!(in.S > 0.0) || !std::isfinite(in.r)) {
    return Err(ErrorCode::InvalidArgument,
               "fit_curve_surface: non-positive S or non-finite r");
  }
  if (under.chains.empty()) {
    return Err(ErrorCode::NotFound, "fit_curve_surface: underlying carries no chains");
  }

  CurveSurfaceReport out;
  out.context.reserve(under.chains.size());
  out.per_expiry.reserve(under.chains.size());
  double worst = std::numeric_limits<double>::infinity();

  for (const Chain& chain : under.chains) {
    const double T = chain.T;
    if (!(T > 0.0)) {
      continue;
    }
    // 1. Term (forward, borrow) — the borrow-only front half of the de-Am.
    const auto d_res = resolve_chain_forward(chain, in.S, in.r, in.cash_divs,
                                             in.now_ts_ns, in.deam);
    if (!d_res) {
      continue;
    }
    const double F = d_res->forward;
    if (!(F > 0.0) || !std::isfinite(F)) {
      continue;
    }
    const double q_eff = in.r - std::log(F / in.S) / T;
    const double df = std::exp(-in.r * T);

    // 2. De-Americanized fit observations (the 99.5% recipe). MUST be COLD: a
    //    near-interpolating convex fit propagates any de-Am bias straight into the
    //    served IV, so the small carry-bias of the cached-de-Am hot path (fine for
    //    the coarse eSSVI backbone) knocks the penny-tight dense fit out of band.
    //    Correctness over speed here — cold Andersen-Lake per strike.
    const auto obs = build_observations_european(chain, in.S, in.r, F, T, df,
                                                 in.calib);
    if (!obs || obs->obs.size() < kMinUsableObs) {
      continue;
    }

    // 3. Fit the configured curve kind from the European obs.
    //    Calendar floor: previous fitted slice's total variance (ascending T).
    //    Guard on the prior slice's T so a non-ascending input degrades to
    //    no-enforcement (never an inverted floor). The loader sorts ascending-T
    //    (data.cpp sort_chains_by_T), so on the standard board this is always
    //    the immediately-shorter expiry.
    std::function<double(double)> w_prev;
    if (!out.surface.empty() && out.context.back().T < T) {
      const IVolCurve* prev = out.surface.slices().back().get();
      w_prev = [prev](double k) { return prev->w(k); };
    }
    // The calendar floor inside fit_convex_slice enforces w_curr >= w_prev at the
    // fit nodes (STRICT, per the served-surface policy: calendar-arb-free by
    // construction). It adds constraint ROWS to the N-node QP, not slack variables,
    // so enforcement does not materially slow the fit. On boards with genuine
    // calendar structure this trades some price-in-band tightness for no-arb — an
    // explicit product choice (see spy_bidask_regression_test's rebaselined floor).
    auto slice_res = fit_slice_curve(cfg, obs->obs, F, T, df, w_prev);
    if (!slice_res) {
      continue;
    }
    const IVolCurve* const slice = slice_res->get();

    // 4. Score re-Americanized parity off the fitted slice's own iv(k). A parity
    //    (diagnostic) failure is non-fatal: keep the slice, push a zeroed report.
    const ParityData pd = build_parity_data(chain, in.S, in.r, F, q_eff, in.deam);
    ParityReport parity{};
    if (pd.strike.size() >= 4) {
      std::vector<double> model_iv;
      model_iv.reserve(pd.k_log.size());
      for (const double k : pd.k_log) {
        model_iv.push_back(slice->iv(k));
      }
      ParityInputs pin{};
      pin.S = in.S;
      pin.r = in.r;
      pin.q_eff = q_eff;
      pin.T = T;
      pin.method = in.deam.method;
      pin.al_opts = in.deam.al_opts;
      pin.band_k = in.band_k;
      pin.n_curve_params = 3;  // nominal chi2 dof (informational for dense fits)
      pin.caches = in.deam.caches;
      auto pr = chain_parity(pd.strike, pd.bid, pd.ask, pd.mid, pd.side, model_iv,
                             pd.market_iv, pin);
      if (pr) {
        parity = *pr;
      }
    }

    // 5. Commit the slice + its context (ascending T by construction).
    out.surface.push(std::move(*slice_res));
    out.context.push_back(SliceContext{T, F, d_res->borrow, q_eff,
                                       obs->obs.size(),
                                       static_cast<std::size_t>(obs->n_dropped)});
    out.per_expiry.push_back(parity);
    if (parity.n > 0) {
      worst = std::min(worst, parity.frac_fv_within_bidask);
    }
  }

  if (out.surface.empty()) {
    return Err(ErrorCode::NotFound,
               "fit_curve_surface: no expiry produced a usable slice");
  }
  out.n_slices = out.surface.n_slices();
  out.worst_frac_within_bidask = std::isfinite(worst) ? worst : 0.0;
  return Ok(std::move(out));
}

}  // namespace atx::vol
