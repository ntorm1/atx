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
#include "atx/vol/calib.hpp"        // build_observations_european, ObsSet, FitObs
#include "atx/vol/deamer.hpp"       // resolve_chain_forward, european_equiv_iv, otm_side, DeAmOptions
#include "atx/vol/parallel_for.hpp" // parallel_for (block-partition fan-out)
#include "atx/vol/parity.hpp"       // chain_parity, ParityInputs, ParityReport
#include "atx/vol/universe.hpp"     // Chain, chain_index

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

// Per-chain output slot for the parallel de-Am pre-pass (phase 1). `usable`
// mirrors EXACTLY the set of `continue` gates the old sequential loop applied
// (T<=0, forward resolve failed, F non-finite/non-positive, obs < kMinUsableObs)
// so phase 2 skips precisely the chains the pre-S0-1 code skipped. Written by
// AT MOST one worker (its own chain index) and read only after every worker has
// joined (parallel_for's scope-exit barrier) — no cross-thread reduction, pure
// const reads of `under`/`in`, disjoint writes into `slot[i]`. Bit-identical for
// any worker count (the value_chain / calibrate_pool determinism pattern).
struct ChainPrepass {
  bool usable = false;
  double T = 0.0;
  double F = 0.0;
  double borrow = 0.0;
  double q_eff = 0.0;
  double df = 0.0;
  ObsSet obs;  // meaningful only when `usable`
};

// Phase 1: the cold, per-chain de-Am (resolve_chain_forward + the European
// observation build) fanned out over `n_threads` workers. Pure per-chain work,
// disjoint output slots — see `ChainPrepass` above.
[[nodiscard]] std::vector<ChainPrepass> run_deam_prepass(const Underlying& under,
                                                         const SurfaceParityInputs& in,
                                                         unsigned n_threads) {
  std::vector<ChainPrepass> prepass(under.chains.size());
  parallel_for(under.chains.size(), n_threads, [&](std::size_t i) {
    const Chain& chain = under.chains[i];
    ChainPrepass& slot = prepass[i];
    const double T = chain.T;
    if (!(T > 0.0)) {
      return;
    }
    const auto d_res = resolve_chain_forward(chain, in.S, in.r, in.cash_divs,
                                             in.now_ts_ns, in.deam);
    if (!d_res) {
      return;
    }
    const double F = d_res->forward;
    if (!(F > 0.0) || !std::isfinite(F)) {
      return;
    }
    const double q_eff = in.r - std::log(F / in.S) / T;
    const double df = std::exp(-in.r * T);

    // COLD per-strike de-Am (no correction cache) at the caller's Andersen-Lake
    // accuracy: see the HARD CONSTRAINT note on the (now sequential-only)
    // fit/parity walk below — this pre-pass must stay cold for the same reason.
    auto obs = build_observations_european(chain, in.S, in.r, F, T, df, in.calib,
                                           AmericanCorrectionCaches{}, in.deam.al_opts,
                                           in.deam.iv_tol, in.deam.iv_max_iter);
    if (!obs || obs->obs.size() < kMinUsableObs) {
      return;
    }

    slot.T = T;
    slot.F = F;
    slot.borrow = d_res->borrow;
    slot.q_eff = q_eff;
    slot.df = df;
    slot.obs = std::move(*obs);
    slot.usable = true;
  });
  return prepass;
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

  // Phase 1 (PARALLEL): the cold per-chain de-Am — resolve_chain_forward (term
  // forward/borrow) + build_observations_european (the 99.5% recipe's European
  // fit observations) — is independent per chain, so it fans out over
  // `in.fit_workers` disjoint output slots (0 => hardware_concurrency; 1 =>
  // serial, bit-identical to the pre-S0-1 path). MUST stay COLD (no correction
  // cache): a near-interpolating convex fit propagates any de-Am bias straight
  // into the served IV, so the small carry-bias of the cached-de-Am hot path
  // (fine for the coarse eSSVI backbone) knocks the penny-tight dense fit out
  // of band. Correctness over speed here — cold Andersen-Lake per strike, just
  // run concurrently across chains.
  const std::vector<ChainPrepass> prepass = run_deam_prepass(under, in, in.fit_workers);

  // Phase 2 (SEQUENTIAL): the fit is order-dependent — each fitted slice's w(k)
  // becomes the calendar floor for the next (ascending-T) slice — so this walk
  // stays single-threaded, unchanged from the pre-S0-1 logic. It only reads the
  // phase-1 pre-pass results (skipping EXACTLY the chains phase 1 flagged) and
  // re-derives nothing the pre-pass already computed.
  for (std::size_t ci = 0; ci < under.chains.size(); ++ci) {
    const ChainPrepass& pre = prepass[ci];
    if (!pre.usable) {
      continue;
    }
    const Chain& chain = under.chains[ci];
    const double T = pre.T;
    const double F = pre.F;
    const double q_eff = pre.q_eff;
    const double df = pre.df;

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
    auto slice_res = fit_slice_curve(cfg, pre.obs.obs, F, T, df, w_prev);
    if (!slice_res) {
      continue;
    }
    const IVolCurve* const slice = slice_res->get();

    // 4. Score re-Americanized parity off the fitted slice's own iv(k). A parity
    //    (diagnostic) failure is non-fatal: keep the slice, push a zeroed report.
    //    NOTE (S0-2 follow-up): this is the SECOND cold de-Am pass over the
    //    chain (build_parity_data re-inverts every OTM leg); left as-is here.
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
    out.context.push_back(SliceContext{T, F, pre.borrow, q_eff,
                                       pre.obs.obs.size(),
                                       static_cast<std::size_t>(pre.obs.n_dropped)});
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
