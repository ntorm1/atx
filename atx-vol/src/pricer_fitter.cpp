#include "atx/vol/pricer_fitter.hpp"

#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american_iv.hpp"  // american_implied_vol
#include "atx/vol/correction.hpp"   // AmericanCorrectionCaches (cached inversion hot path)
#include "atx/vol/parallel_for.hpp" // parallel_for (shared block-partition fan-out)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// Ordered fallback rungs for a primary curve family that failed to build. A
// profile is a latency prior, not permission to drop an underlier: every
// auto-routed board descends toward the minimally identified direct-variance
// curve, and the direct curve itself falls back to the parsimonious eSSVI
// backbone (a board too degenerate for market nodes may still admit a smooth
// five-parameter slice). Switching on the enum with no `default:` lets
// -Wswitch -Werror reject a new kind that forgets to declare its rungs.
std::span<const VolCurveKind> fallback_curve_rungs(VolCurveKind primary) noexcept {
  static constexpr VolCurveKind kFromC8[]{VolCurveKind::Essvi, VolCurveKind::LinearVariance};
  static constexpr VolCurveKind kFromEssvi[]{VolCurveKind::Svi, VolCurveKind::LinearVariance};
  static constexpr VolCurveKind kFromSvi[]{VolCurveKind::LinearVariance};
  static constexpr VolCurveKind kFromConvex[]{VolCurveKind::LinearVariance};
  static constexpr VolCurveKind kFromLinear[]{VolCurveKind::Essvi};
  // SplineVol is not in default_selector_candidates() v1 (task-3 constraint),
  // so this rung is not exercised by the auto-routed path today; it still
  // needs a progressing ladder for any caller that pins SplineVol explicitly.
  static constexpr VolCurveKind kFromSpline[]{VolCurveKind::LinearVariance};
  switch (primary) {
  case VolCurveKind::C8:
    return kFromC8;
  case VolCurveKind::Essvi:
    return kFromEssvi;
  case VolCurveKind::Svi:
    return kFromSvi;
  case VolCurveKind::ConvexDense:
    return kFromConvex;
  case VolCurveKind::LinearVariance:
    return kFromLinear;
  case VolCurveKind::SplineVol:
    return kFromSpline;
  }
  return {};
}

std::optional<std::size_t> ChainValuation::row_of(OptionId id) const {
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == id) {
      return i;
    }
  }
  return std::nullopt;
}

Status PricerFitter::fit(const OptionChain &chain) {
  selection_.reset();
  decision_.reset();

  FitPreset effective_preset = cfg_.preset;
  const bool pinned_hft = !cfg_.curve.has_value() && cfg_.preset == FitPreset::Hft;
  if (!cfg_.curve.has_value() && !pinned_hft) {
    FitDecision d =
        select_fit_policy(chain.underlying(), chain.underlying().ticker, cfg_.context, cfg_.policy);
    effective_preset = d.needs_cross_validation ? cfg_.preset : d.preset;
    decision_ = std::move(d);
  }

  SessionInputs in =
      make_session_inputs(effective_preset, chain.spot(), chain.rate(), chain.now_ns());
  if (chain.env().yield.size() > 0u) {
    in.expiry_rate_T.reserve(chain.underlying().chains.size());
    in.expiry_rates.reserve(chain.underlying().chains.size());
    for (const Chain &expiry : chain.underlying().chains) {
      in.expiry_rate_T.push_back(expiry.T);
      in.expiry_rates.push_back(chain.env().rate_at(expiry.T));
    }
  }
  // Apply the selected profile's existing quote/calibration policy through the
  // same SessionInputs consumed by every curve family. Reapply the preset after
  // copying the profile so latency/fidelity controls (HFT knot cap, shortcut,
  // de-Am options) remain authoritative.
  if (decision_.has_value() && effective_preset != FitPreset::Hft) {
    const auto profile = profile_lookup(decision_->profile.kind);
    if (profile.has_value()) {
      in.calib = profile.value()->calib;
      apply_fit_preset(in, effective_preset);
    }
  }
  // Dividends: the chain's MarketEnv supplies the schedule; a non-empty config
  // value overrides it.
  in.cash_divs = cfg_.cash_divs.empty() ? chain.env().cash_divs : cfg_.cash_divs;
  if (cfg_.use_correction_cache.has_value()) {
    in.use_correction_cache = *cfg_.use_correction_cache;
  }
  if (cfg_.score_parity.has_value()) {
    in.score_parity = *cfg_.score_parity;
  }
  if (cfg_.enforce_calendar_floor.has_value()) {
    in.enforce_calendar_floor = *cfg_.enforce_calendar_floor;
  }
  if (cfg_.use_deam_cache_for_fit.has_value()) {
    in.use_deam_cache_for_fit = *cfg_.use_deam_cache_for_fit;
  }
  if (cfg_.max_obs_per_slice.has_value()) {
    in.calib.max_obs_per_slice = *cfg_.max_obs_per_slice;
  }
  if (cfg_.max_otm_shortcut_premium_spread_frac.has_value()) {
    in.calib.max_otm_shortcut_premium_spread_frac = *cfg_.max_otm_shortcut_premium_spread_frac;
  }
  if (!in.expiry_rates.empty()) {
    // CorrectionCache is built at one scalar (r, q) pair. A term-rate board
    // must stay on the cold pricer until the cache itself becomes term-aware.
    in.use_correction_cache = false;
    in.use_deam_cache_for_fit = false;
  }

  // Curve config: pinned, profile-direct, or held-out selected for this board.
  if (cfg_.curve.has_value()) {
    in.curve = *cfg_.curve;
  } else if (pinned_hft) {
    // Hft's preset-pinned direct market curve avoids both selector candidate
    // fits and the per-expiry dense QP on penny-dense index boards.
    in.curve.kind = VolCurveKind::LinearVariance;
  } else if (decision_.has_value() && !decision_->needs_cross_validation) {
    // The adaptive knot budget is a policy default, so an explicit
    // cfg_.max_obs_per_slice (already applied above) must win -- its documented
    // contract is "nullopt => use the preset default". Apply the cap before
    // mirroring calib into the curve so the two never disagree.
    if (decision_->curve.kind == VolCurveKind::LinearVariance &&
        !cfg_.max_obs_per_slice.has_value() && cfg_.policy.dense_node_cap > 0) {
      in.calib.max_obs_per_slice = cfg_.policy.dense_node_cap;
    }
    decision_->curve.parametric = in.calib;
    in.curve = decision_->curve;
  } else {
    SurfaceParityInputs sp;
    sp.S = in.S;
    sp.r = in.r;
    sp.expiry_rate_T = in.expiry_rate_T;
    sp.expiry_rates = in.expiry_rates;
    sp.cash_divs = in.cash_divs;
    sp.now_ts_ns = in.now_ts_ns;
    sp.deam = in.deam;
    sp.calib = in.calib;
    sp.band_k = in.band_k;
    sp.repair = in.calendar_repair;
    sp.score_parity = in.score_parity;
    sp.enforce_calendar_floor = in.enforce_calendar_floor;
    sp.use_deam_cache_for_fit = in.use_deam_cache_for_fit;
    ATX_TRY(SelectorResult chosen, select_curve(chain.underlying(), sp, cfg_.selector));
    // Parametric candidates inherit the selected profile's calibration policy;
    // the held-out selector chooses family/curve-local knobs, not a second quote
    // filtering policy.
    chosen.chosen.parametric = in.calib;
    in.curve = chosen.chosen;
    if (decision_.has_value()) {
      decision_->curve = chosen.chosen;
      decision_->preset = effective_preset;
    }
    selection_ = std::move(chosen);
  }

  Result<VolaSession> built = VolaSession::build(chain.underlying(), in);
  // A profile is a fast prior, not permission to drop an underlier: walk the
  // fallback ladder for anything the policy routed, including a board whose curve
  // came from the held-out selector. A curve the CALLER pinned (cfg_.curve, or the
  // preset-pinned Hft dense route) is an explicit instruction and is never
  // silently substituted.
  const bool auto_routed = decision_.has_value() && !cfg_.curve.has_value() && !pinned_hft;
  if (!built.has_value() && auto_routed) {
    const CurveConfig primary_curve = in.curve;
    for (const VolCurveKind rung : fallback_curve_rungs(primary_curve.kind)) {
      in.curve.kind = rung;
      Result<VolaSession> retry = VolaSession::build(chain.underlying(), in);
      if (!retry.has_value()) {
        continue;
      }
      decision_->primary_curve = primary_curve;
      decision_->curve = in.curve;
      decision_->used_fallback = true;
      built = std::move(retry);
      break;
    }
  }
  if (!built.has_value()) {
    // `built` still holds the PRIMARY failure: a rung only overwrites it on
    // success, so an exhausted ladder reports the informative first error.
    return Err(std::move(built).error());
  }
  VolaSession sess = std::move(*built);
  // FittedSurface's ctor is private (friend PricerFitter), so make_unique cannot
  // reach it — construct explicitly.
  surface_.reset(new FittedSurface(std::move(sess)));
  return Ok();
}

Result<ChainValuation> PricerFitter::value_chain(const OptionChain &chain, OutputField fields,
                                                 unsigned n_threads) const {
  if (surface_ == nullptr) {
    return Err(ErrorCode::Unavailable,
               "PricerFitter::value_chain: no fitted surface; call fit() first");
  }
  const VolaSession &sess = surface_->session();
  const double S = chain.spot();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  ChainValuation val;
  ChainSnapshot snap = chain.snapshot();
  val.ids = std::move(snap.ids);
  val.filled = fields;
  const std::size_t n = val.ids.size();

  if (has(fields, OutputField::ModelPrice)) {
    val.model_price.assign(n, nan);
  }
  if (has(fields, OutputField::ModelIV)) {
    val.model_iv.assign(n, nan);
  }
  if (has(fields, OutputField::BidIV)) {
    val.bid_iv.assign(n, nan);
  }
  if (has(fields, OutputField::AskIV)) {
    val.ask_iv.assign(n, nan);
  }
  if (has(fields, OutputField::MidIV)) {
    val.mid_iv.assign(n, nan);
  }
  if (has(fields, OutputField::Greeks)) {
    val.greeks.assign(n, AmericanGreeks{});
  }

  const unsigned nt = n_threads ? n_threads : cfg_.n_threads;
  const bool want_bands = has(fields, OutputField::BidIV) || has(fields, OutputField::AskIV) ||
                          has(fields, OutputField::MidIV);

  // The per-side correction caches the fit built. Routing the bid/ask/mid IV
  // inversions through them replaces the cold per-residual Andersen-Lake solve
  // (12 BAW root-finds + sweeps + quadrature + cold polish) with the cached hot
  // path (Black-76 + one Chebyshev evaluation) — the SOTA American-IV method (a
  // fast surrogate in the root-find, not a pricer). A null cache for a side
  // transparently falls back to the cold path (bit-identical, just slower).
  const AmericanCorrectionCaches caches = sess.correction_caches();

  const auto eval = [&](std::size_t i) {
    const double K = snap.strike[i];
    const double T = snap.T[i];
    const Side side = snap.side[i];
    if (!(K > 0.0) || !(T > 0.0)) {
      return; // decode failed or degenerate expiry — leave the row NaN
    }
    const double q = sess.q_eff_at(T);
    const double rate = sess.rate_at(T);
    if (has(fields, OutputField::ModelIV)) {
      val.model_iv[i] = sess.iv(K, T);
    }
    if (has(fields, OutputField::ModelPrice)) {
      const auto fv = sess.fair_value(K, T, side);
      val.model_price[i] = fv.has_value() ? *fv : nan;
    }
    if (has(fields, OutputField::Greeks)) {
      const auto g = sess.greeks(K, T, side);
      if (g.has_value()) {
        val.greeks[i] = *g;
      } else {
        val.greeks[i].price = nan;
      }
    }
    if (!want_bands) {
      return;
    }
    // Parallel American-IV band inversions through the cached hot path. The
    // surface's own IV at (K, T) seeds all three (bid/ask/mid vols sit within a
    // spread's width of it), so each is 1-2 Newton steps.
    const CorrectionCache *cc = caches.for_side(side);
    const double miv = sess.iv(K, T);
    const double ws = (std::isfinite(miv) && miv > 0.0) ? miv : 0.0;
    const auto invert = [&](double px) {
      return american_implied_vol(px, S, K, T, rate, q, side, AmericanMethod::AndersenLake, 1.0e-7,
                                  64, std::nullopt, cc, ws);
    };
    if (has(fields, OutputField::BidIV) && snap.bid[i] > 0.0) {
      const auto iv = invert(snap.bid[i]);
      val.bid_iv[i] = iv.has_value() ? *iv : nan;
    }
    if (has(fields, OutputField::AskIV) && snap.ask[i] > 0.0) {
      const auto iv = invert(snap.ask[i]);
      val.ask_iv[i] = iv.has_value() ? *iv : nan;
    }
    if (has(fields, OutputField::MidIV) && snap.mid[i] > 0.0) {
      const auto iv = invert(snap.mid[i]);
      val.mid_iv[i] = iv.has_value() ? *iv : nan;
    }
  };

  parallel_for(n, nt, eval);
  return Ok(std::move(val));
}

} // namespace atx::vol
