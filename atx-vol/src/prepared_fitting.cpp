#include "atx/vol/prepared_fitting.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/correction.hpp" // CorrectionCache::populated/side (C1 route attribution)
#include "atx/vol/detail/deam_pass_counter.hpp" // C1 proof: fit de-Am pass tally
#include "atx/vol/surface_parity.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

std::strong_ordering operator<=>(const ObservationKey &left, const ObservationKey &right) noexcept {
  return std::tie(left.expiry_index, left.strike_index, left.side) <=>
         std::tie(right.expiry_index, right.strike_index, right.side);
}

namespace {

constexpr double kMinSpread = 1.0e-8;

[[nodiscard]] bool inputs_valid(const Chain &chain, const PreparedSliceInputs &inputs) noexcept {
  constexpr std::size_t kMaxIndexedStrikes =
      static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1u;
  if (chain.n_strikes() > kMaxIndexedStrikes) {
    return false;
  }
  if (!(inputs.S > 0.0) || !std::isfinite(inputs.S) || !(inputs.F > 0.0) ||
      !std::isfinite(inputs.F) || !(chain.T > 0.0) || !std::isfinite(chain.T) ||
      !std::isfinite(inputs.r)) {
    return false;
  }
  const double q_eff = inputs.r - std::log(inputs.F / inputs.S) / chain.T;
  const double df = std::exp(-inputs.r * chain.T);
  const auto close = [](double supplied, double derived) noexcept {
    const double scale = std::max({1.0, std::fabs(supplied), std::fabs(derived)});
    return std::fabs(supplied - derived) <= 64.0 * std::numeric_limits<double>::epsilon() * scale;
  };
  const std::size_t quote_count = 2u * chain.n_strikes();
  return std::isfinite(q_eff) && df > 0.0 && std::isfinite(df) && close(inputs.q_eff, q_eff) &&
         close(inputs.df, df) && inputs.iv_tolerance > 0.0 && std::isfinite(inputs.iv_tolerance) &&
         inputs.iv_max_iterations > 0u && chain.bids.size() >= quote_count &&
         chain.asks.size() >= quote_count && chain.mids.size() >= quote_count &&
         chain.flags.size() >= quote_count;
}

[[nodiscard]] Side preferred_side(double strike, double forward) noexcept {
  return (strike >= forward) ? Side::Call : Side::Put;
}

[[nodiscard]] ObservationKey key_for(const PreparedSliceInputs &inputs, std::size_t strike_index,
                                     Side side) noexcept {
  return ObservationKey{inputs.expiry_index, static_cast<std::uint32_t>(strike_index), side};
}

[[nodiscard]] bool quote_valid(const Chain &chain, std::size_t quote_index) noexcept {
  const double bid = chain.bids[quote_index];
  const double ask = chain.asks[quote_index];
  const double mid = chain.mids[quote_index];
  return bid > 0.0 && ask > 0.0 && ask >= bid && std::isfinite(mid) && mid > 0.0;
}

} // namespace

namespace detail {

std::vector<char> select_deam_spread(const std::vector<double> &moneyness, std::uint32_t cap) {
  const std::size_t m = moneyness.size();
  std::vector<char> selected(m, static_cast<char>(0));
  // Uncapped, or the candidate set already fits under the cap: keep every
  // candidate (all-ones). The legacy caller only invokes selection when the cap
  // strictly binds, but keeping this total-safe makes the helper self-contained.
  if (cap == 0u || m <= static_cast<std::size_t>(cap)) {
    std::fill(selected.begin(), selected.end(), static_cast<char>(1));
    return selected;
  }
  const std::size_t cap_n = static_cast<std::size_t>(cap);

  // Sort candidate positions ascending by (moneyness, original index). The
  // original-index tiebreak keeps the ordering total and deterministic even for
  // duplicate strikes/moneyness.
  std::vector<std::size_t> order(m);
  for (std::size_t i = 0; i < m; ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    if (moneyness[a] != moneyness[b]) {
      return moneyness[a] < moneyness[b];
    }
    return a < b;
  });

  // Work in sorted-position space; translate back to candidate order at the end.
  std::vector<char> keep_sorted(m, static_cast<char>(0));
  std::size_t count = 0;
  const auto take = [&](std::size_t sorted_pos) {
    if (count < cap_n && keep_sorted[sorted_pos] == 0) {
      keep_sorted[sorted_pos] = static_cast<char>(1);
      ++count;
    }
  };

  // (1) Pin both extreme wings first so the spline's outer knots stay
  //     constrained regardless of how the rest of the budget is spent.
  take(0);
  take(m - 1);

  // (2) Dense near-ATM core (contiguous in moneyness), up to ~half the budget.
  std::size_t atm = 0;
  for (std::size_t p = 1; p < m; ++p) {
    if (std::fabs(moneyness[order[p]]) < std::fabs(moneyness[order[atm]])) {
      atm = p;
    }
  }
  const std::size_t core_target = std::max<std::size_t>(1, cap_n / 2u);
  take(atm);
  std::size_t lo = atm;
  std::size_t hi = atm;
  std::size_t core_have = 1; // the ATM anchor (take() dedups if it was a wing)
  while (core_have < core_target && count < cap_n && (lo > 0 || hi < m - 1)) {
    bool take_low;
    if (lo == 0) {
      take_low = false;
    } else if (hi == m - 1) {
      take_low = true;
    } else {
      // Grow toward whichever neighbor sits nearer the money; ties pick the low
      // side for a stable, deterministic core.
      take_low = std::fabs(moneyness[order[lo - 1]]) <= std::fabs(moneyness[order[hi + 1]]);
    }
    if (take_low) {
      --lo;
      take(lo);
    } else {
      ++hi;
      take(hi);
    }
    ++core_have;
  }

  // (3) Spend the remaining budget on an even stride across the full moneyness
  //     range, thinning the intermediate strikes uniformly. Collisions with the
  //     already-kept core/wings simply keep the final count at or below `cap`.
  const std::size_t rest = cap_n - count;
  if (rest == 1) {
    take((m - 1) / 2u);
  } else if (rest >= 2) {
    for (std::size_t j = 0; j < rest && count < cap_n; ++j) {
      const double frac = static_cast<double>(j) / static_cast<double>(rest - 1);
      auto pos = static_cast<std::size_t>(std::llround(frac * static_cast<double>(m - 1)));
      if (pos >= m) {
        pos = m - 1;
      }
      take(pos);
    }
  }

  for (std::size_t p = 0; p < m; ++p) {
    if (keep_sorted[p] != 0) {
      selected[order[p]] = static_cast<char>(1);
    }
  }
  return selected;
}

struct PreparedSliceBuilder {
  static void reserve_score_rows(PreparedSlice &out, std::size_t count);
  static void append_score_row(PreparedSlice &out, ObservationKey key, const FitObs &row,
                               const Chain &chain);
  [[nodiscard]] static Result<PreparedSlice> prepare_configured(const Chain &chain,
                                                                const PreparedSliceInputs &inputs);
  [[nodiscard]] static Result<PreparedSlice> prepare_legacy(const Chain &chain,
                                                            const PreparedSliceInputs &inputs);
};

void PreparedSliceBuilder::reserve_score_rows(PreparedSlice &out, std::size_t count) {
  out.score_keys_.reserve(count);
  out.score_columns_.strike.reserve(count);
  out.score_columns_.bid.reserve(count);
  out.score_columns_.ask.reserve(count);
  out.score_columns_.mid.reserve(count);
  out.score_columns_.k_log.reserve(count);
  out.score_columns_.market_iv.reserve(count);
  out.score_columns_.side.reserve(count);
}

void PreparedSliceBuilder::append_score_row(PreparedSlice &out, ObservationKey key,
                                            const FitObs &row, const Chain &chain) {
  const std::size_t quote_index =
      chain_index(static_cast<std::uint16_t>(row.source_strike_index), row.side);
  out.score_keys_.push_back(key);
  out.score_columns_.strike.push_back(row.K);
  out.score_columns_.bid.push_back(chain.bids[quote_index]);
  out.score_columns_.ask.push_back(chain.asks[quote_index]);
  out.score_columns_.mid.push_back(chain.mids[quote_index]);
  out.score_columns_.k_log.push_back(row.k);
  out.score_columns_.market_iv.push_back(row.score_sigma_mkt);
  out.score_columns_.side.push_back(row.side);
}

Result<PreparedSlice> PreparedSliceBuilder::prepare_configured(const Chain &chain,
                                                               const PreparedSliceInputs &inputs) {
  Result<ObsSet> fit_result =
      chain.exercise_style == ExerciseStyle::European
          ? build_observations(chain, inputs.F, chain.T, inputs.df, inputs.calib)
          : build_observations_european(chain, inputs.S, inputs.r, inputs.F, chain.T, inputs.df,
                                        inputs.calib, inputs.caches, inputs.al_opts,
                                        inputs.iv_tolerance, inputs.iv_max_iterations,
                                        inputs.method, inputs.prepare_scoring);
  ATX_TRY(ObsSet fit_set, std::move(fit_result));

  PreparedSlice out;
  out.expiry_index_ = inputs.expiry_index;
  out.maturity_ = chain.T;
  out.forward_ = inputs.F;
  out.n_dropped_ = fit_set.n_dropped;
  out.provenance_ = SlicePreparationProvenance{inputs.policy,
                                               chain.exercise_style,
                                               inputs.method,
                                               inputs.al_opts,
                                               inputs.iv_tolerance,
                                               inputs.iv_max_iterations,
                                               inputs.S,
                                               inputs.r,
                                               inputs.q_eff,
                                               inputs.df,
                                               inputs.caches.call != nullptr,
                                               inputs.caches.put != nullptr,
                                               fit_set.n_score_inversions};
  out.fit_rows_ = std::move(fit_set.obs);
  out.deam_audit_ = fit_set.deam_audit;
  if (inputs.prepare_scoring) {
    reserve_score_rows(out, out.fit_rows_.size());
  }
  out.observations_.reserve(chain.n_strikes());
  out.rejections_.reserve(chain.n_strikes() - std::min(chain.n_strikes(), out.fit_rows_.size()));

  std::vector<std::optional<std::size_t>> accepted(chain.n_strikes());
  for (std::size_t row_index = 0; row_index < out.fit_rows_.size(); ++row_index) {
    const FitObs &row = out.fit_rows_[row_index];
    const std::size_t index = row.source_strike_index;
    if (index >= chain.n_strikes() || accepted[index].has_value() ||
        chain.strikes[index] != row.K || preferred_side(chain.strikes[index], row.F) != row.side) {
      return Err(ErrorCode::Internal,
                 "PreparedSlice::create: configured row carries an invalid source key");
    }
    accepted[index] = row_index;
    if (inputs.prepare_scoring) {
      append_score_row(out, key_for(inputs, index, row.side), row, chain);
    }
  }

  if (fit_set.provenance.size() != chain.n_strikes()) {
    return Err(ErrorCode::Internal,
               "PreparedSlice::create: configured provenance has the wrong size");
  }
  for (std::size_t provenance_index = 0; provenance_index < fit_set.provenance.size();
       ++provenance_index) {
    const ObsProvenance &source = fit_set.provenance[provenance_index];
    const std::size_t index = source.source_strike_index;
    if (index >= chain.n_strikes() || index != provenance_index) {
      return Err(ErrorCode::Internal,
                 "PreparedSlice::create: rejection provenance source key out of range");
    }
    const ObservationKey key = key_for(inputs, index, source.side);
    const std::size_t quote_index = chain_index(static_cast<std::uint16_t>(index), source.side);

    PreparedObservation observation;
    observation.key = key;
    observation.rejection = source.rejection;
    observation.bid = chain.bids[quote_index];
    observation.ask = chain.asks[quote_index];
    observation.raw_mid = chain.mids[quote_index];
    if (accepted[index].has_value()) {
      const FitObs &row = out.fit_rows_[*accepted[index]];
      observation.european_iv = row.sigma_mkt;
      observation.weight_w = row.weight_w;
      if (source.rejection != ObservationRejectionReason::None) {
        return Err(ErrorCode::Internal,
                   "PreparedSlice::create: accepted row marked rejected by provenance");
      }
    } else {
      out.rejections_.push_back(ObservationRejection{key, source.rejection});
    }
    out.observations_.push_back(observation);
  }
  return Ok(std::move(out));
}

Result<PreparedSlice> PreparedSliceBuilder::prepare_legacy(const Chain &chain,
                                                           const PreparedSliceInputs &inputs) {
  // C1 proof instrumentation: the Legacy/eSSVI fit's own per-slice de-Am pass.
  note_fit_deam_slice_pass();
  PreparedSlice out;
  out.expiry_index_ = inputs.expiry_index;
  out.maturity_ = chain.T;
  out.forward_ = inputs.F;
  out.provenance_ = SlicePreparationProvenance{inputs.policy,
                                               chain.exercise_style,
                                               inputs.method,
                                               inputs.al_opts,
                                               inputs.iv_tolerance,
                                               inputs.iv_max_iterations,
                                               inputs.S,
                                               inputs.r,
                                               inputs.q_eff,
                                               inputs.df,
                                               inputs.caches.call != nullptr,
                                               inputs.caches.put != nullptr,
                                               0u};
  out.observations_.reserve(chain.n_strikes());
  out.fit_rows_.reserve(chain.n_strikes());
  if (inputs.prepare_scoring) {
    reserve_score_rows(out, chain.n_strikes());
  }
  std::uint32_t n_audit_dropped = 0;

  // De-Am strike cap (latency knob; charter: legacy prep only). When set and an
  // expiry carries MORE candidate strikes than the cap, de-Americanize only a
  // deterministic moneyness-spread subset — skipping the (cold-ish) Andersen-
  // Lake inversion for the dropped strikes. The candidate set is enumerated with
  // the SAME cheap filter (strike > 0 + quote_valid) the main loop applies just
  // before the de-Am call, so the two agree exactly on which strikes are
  // eligible. `cap_dropped` stays empty (and the main loop stays bit-identical)
  // whenever the cap is unset or does not bind. The forward/borrow carry solve
  // ran upstream on the full near-ATM pair set and is untouched here.
  std::vector<char> cap_dropped;
  if (inputs.calib.max_deam_strikes_per_expiry > 0u) {
    const std::uint32_t deam_cap = inputs.calib.max_deam_strikes_per_expiry;
    std::vector<std::size_t> candidate_index;
    std::vector<double> candidate_moneyness;
    candidate_index.reserve(chain.n_strikes());
    candidate_moneyness.reserve(chain.n_strikes());
    for (std::size_t index = 0; index < chain.n_strikes(); ++index) {
      const double strike = chain.strikes[index];
      if (!(strike > 0.0)) {
        continue;
      }
      const double safe_k = std::log(strike / inputs.F);
      const Side side = otm_side(safe_k);
      const std::size_t quote_index = chain_index(static_cast<std::uint16_t>(index), side);
      if (!quote_valid(chain, quote_index)) {
        continue;
      }
      candidate_index.push_back(index);
      candidate_moneyness.push_back(safe_k);
    }
    if (candidate_index.size() > static_cast<std::size_t>(deam_cap)) {
      const std::vector<char> keep = select_deam_spread(candidate_moneyness, deam_cap);
      cap_dropped.assign(chain.n_strikes(), static_cast<char>(0));
      for (std::size_t c = 0; c < candidate_index.size(); ++c) {
        if (keep[c] == 0) {
          cap_dropped[candidate_index[c]] = static_cast<char>(1);
        }
      }
    }
  }

  // F1 (R-01p2): shared-boundary de-Am batch pre-pass. Route the Legacy/eSSVI
  // de-Am through the SAME retained sigma-boundary interpolant the Configured
  // builder (`build_observations_european`) uses — one boundary solve per
  // slice-side across strikes — instead of the per-row scalar
  // `american_implied_vol`. Seed one candidate observation per valid OTM leg using
  // the IDENTICAL `strike > 0` + `quote_valid` + cap predicate the main loop
  // applies below, so the two agree exactly on which strikes are eligible, then
  // cache each CERTIFIED European-equivalent IV by strike index. Any row the batch
  // does not certify keeps NaN and falls through to the byte-identical scalar
  // `european_equiv_iv` (the parity oracle). The helper itself honours
  // `inputs.calib.use_shared_boundary_deam` and the shared engagement guards, so
  // with the batch disabled every entry stays NaN and the main loop is
  // bit-identical to the pre-F1 per-row path.
  std::vector<double> batch_iv(chain.n_strikes(), std::numeric_limits<double>::quiet_NaN());
  {
    std::vector<FitObs> batch_rows;
    std::vector<std::size_t> batch_row_strike;
    batch_rows.reserve(chain.n_strikes());
    batch_row_strike.reserve(chain.n_strikes());
    for (std::size_t index = 0; index < chain.n_strikes(); ++index) {
      const double strike = chain.strikes[index];
      if (!(strike > 0.0)) {
        continue;
      }
      const Side side = otm_side(std::log(strike / inputs.F));
      const std::size_t quote_index = chain_index(static_cast<std::uint16_t>(index), side);
      if (!quote_valid(chain, quote_index) || (!cap_dropped.empty() && cap_dropped[index] != 0)) {
        continue;
      }
      FitObs seed;
      seed.K = strike;
      seed.F = inputs.F;
      seed.df = inputs.df;
      seed.side = side;
      seed.mid = chain.mids[quote_index];
      seed.spread = chain.asks[quote_index] - chain.bids[quote_index];
      seed.source_strike_index = static_cast<std::uint32_t>(index);
      batch_rows.push_back(seed);
      batch_row_strike.push_back(index);
    }
    if (!batch_rows.empty()) {
      // C1 (accuracy-improving): CAPTURE the shared-boundary batch's de-Am audit
      // into the slice's own `deam_audit_` instead of discarding it. The eSSVI
      // certification/diagnostics layer reuses THIS audit — the rows the fit
      // actually de-Americanized — instead of re-running a second, independent
      // Configured de-Am pass (session.cpp; finding 10). The batch records its
      // shared-lane counters + residual quantiles here; the main loop below adds
      // the row ledger (n_deam_rows/accepted) and the per-route proposal tally.
      static_cast<void>(shared_boundary_deam_batch(
          batch_rows, inputs.S, inputs.r, inputs.F, chain.T, inputs.df, inputs.calib, inputs.caches,
          inputs.al_opts, inputs.iv_tolerance, inputs.iv_max_iterations, inputs.method,
          &out.deam_audit_));
      for (std::size_t j = 0; j < batch_rows.size(); ++j) {
        batch_iv[batch_row_strike[j]] = batch_rows[j].score_sigma_mkt;
      }
    }
  }

  for (std::size_t index = 0; index < chain.n_strikes(); ++index) {
    const double strike = chain.strikes[index];
    const double safe_k = (strike > 0.0) ? std::log(strike / inputs.F) : 0.0;
    const Side side = otm_side(safe_k);
    const ObservationKey key = key_for(inputs, index, side);
    const std::size_t quote_index = chain_index(static_cast<std::uint16_t>(index), side);
    PreparedObservation observation;
    observation.key = key;
    observation.bid = chain.bids[quote_index];
    observation.ask = chain.asks[quote_index];
    observation.raw_mid = chain.mids[quote_index];

    if (!(strike > 0.0)) {
      observation.rejection = ObservationRejectionReason::InvalidStrike;
    } else if (!quote_valid(chain, quote_index)) {
      observation.rejection = ObservationRejectionReason::InvalidBidAsk;
    } else if (!cap_dropped.empty() && cap_dropped[index] != 0) {
      // Valid candidate thinned out by the de-Am strike cap: record it and skip
      // the expensive inversion. `cap_dropped` is only non-empty when the cap
      // strictly binds, so the uncapped path never reaches this branch.
      observation.rejection = ObservationRejectionReason::ObservationCap;
    } else {
      // C1: this valid candidate enters the de-Am inversion stage — count it in
      // the row ledger the reused certification consumes (guardrail: the audit
      // must describe the rows the FIT actually de-Americanized).
      ++out.deam_audit_.n_deam_rows;
      bool row_audited = false;
      // F1 (R-01p2): take the shared-boundary batch IV when this row was certified
      // above; otherwise invert it with the byte-identical per-row scalar oracle.
      // The scalar call, arguments unchanged, remains the parity oracle for every
      // uncertified row and the sole path when the batch is disabled.
      Result<double> market_iv =
          std::isfinite(batch_iv[index])
              ? Ok(batch_iv[index])
              : european_equiv_iv(chain.mids[quote_index], inputs.S, strike, chain.T, inputs.r,
                                  inputs.q_eff, side, inputs.method, inputs.al_opts,
                                  inputs.caches.for_side(side), inputs.iv_tolerance,
                                  inputs.iv_max_iterations);
      // Correctness-first serving (§8.1): under `audit_fit_inversions` every
      // fitted proposal is repriced against the cold Andersen-Lake reference. A
      // failed proposal is recomputed accurately and re-audited; a row that
      // still misses the half-spread budget is DROPPED, never fitted — the same
      // protocol build_observations_european enforces on the curve-driver path.
      // Default-off keeps the historical (unaudited-fit) path bit-identical.
      if (market_iv.has_value() && inputs.audit_fit_inversions &&
          inputs.method == AmericanMethod::AndersenLake) {
        row_audited = true; // this row is repriced against the cold reference below
        const double audit_spread = chain.asks[quote_index] - chain.bids[quote_index];
        Result<IvRepricingAudit> audit = audit_european_equiv_iv(
            chain.mids[quote_index], audit_spread, *market_iv, inputs.S, strike, chain.T, inputs.r,
            inputs.q_eff, side, inputs.max_iv_residual_half_spreads);
        if (!audit || !audit->passed) {
          const Result<double> accurate = european_equiv_iv(
              chain.mids[quote_index], inputs.S, strike, chain.T, inputs.r, inputs.q_eff, side,
              AmericanMethod::AndersenLake, std::nullopt, nullptr);
          bool dropped = true;
          if (accurate.has_value()) {
            audit = audit_european_equiv_iv(chain.mids[quote_index], audit_spread, *accurate,
                                            inputs.S, strike, chain.T, inputs.r, inputs.q_eff, side,
                                            inputs.max_iv_residual_half_spreads);
            if (audit && audit->passed) {
              market_iv = accurate;
              dropped = false;
            }
          }
          if (dropped) {
            market_iv =
                Err(ErrorCode::Unavailable, "prepare_legacy: fit-inversion audit rejected the row");
            ++n_audit_dropped;
          }
        }
      }
      if (!market_iv.has_value()) {
        observation.rejection = ObservationRejectionReason::Deamericanization;
      } else {
        const double spread = chain.asks[quote_index] - chain.bids[quote_index];
        const double vega =
            black76_value_and_vega(inputs.F, strike, chain.T, *market_iv, inputs.df, side).vega;
        const double bounded_spread = std::fmax(spread, kMinSpread);
        const double two_sigma_t = 2.0 * *market_iv * chain.T;
        double weight = 0.0;
        if (vega > 0.0 && std::isfinite(vega) && two_sigma_t > 0.0) {
          weight = (vega * vega) / (bounded_spread * bounded_spread * two_sigma_t * two_sigma_t);
        }
        if (!std::isfinite(weight) || !(weight > 0.0)) {
          weight = 1.0;
        }

        FitObs fit;
        fit.k = safe_k;
        fit.sigma_mkt = *market_iv;
        fit.w_mkt = *market_iv * *market_iv * chain.T;
        fit.weight_w = weight;
        fit.active_weight_w = weight;
        fit.K = strike;
        fit.F = inputs.F;
        fit.df = inputs.df;
        fit.mid = chain.mids[quote_index];
        fit.spread = spread;
        fit.vega = vega;
        // FT-C6: unify the vega-underflow fallback with the Configured builder
        // (calib.cpp build_one_observation): a vega <= floor row uses
        // noise_sigma = 1.0, NOT 0.0. A 0.0 noise makes the C8 spread_w
        // (2*sigma*T*max(noise_sigma,1e-7)) ~1e4x smaller than a normal row's,
        // i.e. ~1e8x the LM weight — one dead deep-wing quote could own the
        // objective. Threshold + fallback now match the configured builder.
        constexpr double kVegaFloor = 1.0e-12;
        fit.noise_sigma = (vega > kVegaFloor) ? spread / vega : 1.0;
        fit.side = side;
        fit.source_strike_index = static_cast<std::uint32_t>(index);
        fit.score_sigma_mkt = *market_iv;
        out.fit_rows_.push_back(fit);
        if (inputs.prepare_scoring) {
          append_score_row(out, key, fit, chain);
        }
        observation.european_iv = *market_iv;
        observation.weight_w = weight;

        // C1: attribute the accepted row to a proposal route so the reused
        // certification carries an honest per-route ledger (the shared-boundary
        // batch fills only its own shared-lane counters). Route mirrors
        // build_observations_european's choice: the cache route when a populated
        // same-side correction cache served the inversion, else the fast/accurate
        // cold Andersen-Lake preset. n_audited/n_reference_reprices are recorded
        // ONLY when audit_fit_inversions actually repriced the row against the
        // cold reference; otherwise the fit ran un-audited (the honest,
        // uncertifiable eSSVI default) and this route accepts more than it
        // audited — which deam_inversion_certified correctly refuses to certify.
        ++out.deam_audit_.n_deam_accepted;
        const CorrectionCache *const route_cache = inputs.caches.for_side(side);
        const bool cache_route =
            route_cache != nullptr && route_cache->populated() && route_cache->side() == side;
        InversionRouteDiagnostics &route =
            cache_route
                ? out.deam_audit_.cache
                : ((inputs.method == AmericanMethod::AndersenLake && inputs.al_opts.has_value())
                       ? out.deam_audit_.fast
                       : out.deam_audit_.accurate);
        ++route.n_proposed;
        ++route.n_accepted;
        if (row_audited) {
          ++route.n_audited;
          ++route.n_reference_reprices;
        }
      }
    }

    if (!observation.accepted()) {
      ++out.n_dropped_;
      out.rejections_.push_back(ObservationRejection{key, observation.rejection});
    }
    out.observations_.push_back(observation);
  }
  // Written BEFORE the floor check so a caller can distinguish an
  // audit-starved thin slice from a genuinely sparse one even on failure.
  if (inputs.out_legacy_fit_rows != nullptr) {
    *inputs.out_legacy_fit_rows = static_cast<std::uint32_t>(out.fit_rows_.size());
  }
  if (inputs.out_legacy_audit_dropped != nullptr) {
    *inputs.out_legacy_audit_dropped = n_audit_dropped;
  }
  if (out.fit_rows_.size() < kMinPreparedFitRows) {
    return Err(ErrorCode::NotFound,
               "PreparedSlice::create: fewer than 5 legacy eSSVI rows survived");
  }
  return Ok(std::move(out));
}

} // namespace detail

Result<PreparedSlice> PreparedSlice::create(const Chain &chain, const PreparedSliceInputs &inputs) {
  if (!inputs_valid(chain, inputs)) {
    return Err(ErrorCode::InvalidArgument,
               "PreparedSlice::create: invalid carry, solver, or chain SoA inputs");
  }
  PreparedSliceInputs normalized = inputs;
  normalized.q_eff = inputs.r - std::log(inputs.F / inputs.S) / chain.T;
  normalized.df = std::exp(-inputs.r * chain.T);
  switch (normalized.policy) {
  case PreparedObservationPolicy::Configured:
    return detail::PreparedSliceBuilder::prepare_configured(chain, normalized);
  case PreparedObservationPolicy::LegacyEssviCompatibility:
    if (chain.exercise_style == ExerciseStyle::European) {
      // The legacy policy exists to reproduce the historical American eSSVI
      // de-Am population. European contracts have no early-exercise premium;
      // reuse the raw Black-76 builder while retaining the requested policy in
      // provenance so callers can still audit how the slice was requested.
      return detail::PreparedSliceBuilder::prepare_configured(chain, normalized);
    }
    return detail::PreparedSliceBuilder::prepare_legacy(chain, normalized);
  }
  return Err(ErrorCode::Internal, "PreparedSlice::create: unknown observation policy");
}

Result<PreparedBoard> PreparedBoard::create(std::vector<PreparedSlice> slices) {
  std::stable_sort(slices.begin(), slices.end(),
                   [](const PreparedSlice &left, const PreparedSlice &right) {
                     return left.expiry_index() < right.expiry_index();
                   });
  for (std::size_t index = 1; index < slices.size(); ++index) {
    if (slices[index - 1].expiry_index() == slices[index].expiry_index()) {
      return Err(ErrorCode::InvalidArgument,
                 "PreparedBoard::create: duplicate expiry observation key");
    }
  }
  PreparedBoard board;
  board.slices_ = std::move(slices);
  return Ok(std::move(board));
}

Result<CanonicalPreparedExpiry> prepare_expiry(const Chain &chain, std::uint32_t expiry_index,
                                               const SurfaceParityInputs &inputs,
                                               PreparedObservationPolicy policy,
                                               PrepareExpiryDiagnostics *diag) {
  if (!(inputs.S > 0.0) || !std::isfinite(inputs.S) || !std::isfinite(inputs.r) ||
      !(chain.T > 0.0) || !std::isfinite(chain.T)) {
    return Err(ErrorCode::InvalidArgument, "prepare_expiry: invalid spot, rate, or maturity");
  }

  double rate = inputs.r;
  if (!inputs.expiry_rate_T.empty() || !inputs.expiry_rates.empty()) {
    if (inputs.expiry_rate_T.size() != inputs.expiry_rates.size()) {
      return Err(ErrorCode::InvalidArgument, "prepare_expiry: invalid term-rate vectors");
    }
    std::size_t rate_index = inputs.expiry_rate_T.size();
    if (expiry_index < inputs.expiry_rate_T.size() &&
        inputs.expiry_rate_T[expiry_index] == chain.T) {
      rate_index = expiry_index;
    } else {
      const auto term =
          std::find(inputs.expiry_rate_T.begin(), inputs.expiry_rate_T.end(), chain.T);
      if (term != inputs.expiry_rate_T.end()) {
        rate_index = static_cast<std::size_t>(term - inputs.expiry_rate_T.begin());
      }
    }
    if (rate_index == inputs.expiry_rate_T.size()) {
      return Err(ErrorCode::NotFound, "prepare_expiry: maturity absent from term-rate snapshot");
    }
    rate = inputs.expiry_rates[rate_index];
  }
  if (!std::isfinite(rate)) {
    return Err(ErrorCode::InvalidArgument, "prepare_expiry: non-finite expiry rate");
  }

  using StageClock = std::chrono::steady_clock;
  const bool time_stages = inputs.collect_stage_timings && diag != nullptr;
  const StageClock::time_point carry_start =
      time_stages ? StageClock::now() : StageClock::time_point{};
  Result<ChainForward> carry_res =
      resolve_chain_forward(chain, inputs.S, rate, inputs.cash_divs, inputs.now_ts_ns, inputs.deam);
  if (time_stages) {
    diag->carry_solve_ms =
        std::chrono::duration<double, std::milli>(StageClock::now() - carry_start).count();
  }
  if (!carry_res.has_value()) {
    if (diag != nullptr) {
      diag->carry_failed = true; // §5.2: the caller surfaces the carry skip
    }
    return ::tl::unexpected<::atx::core::Error>(std::move(carry_res).error());
  }
  const ChainForward carry = *std::move(carry_res);
  if (!(carry.forward > 0.0) || !std::isfinite(carry.forward)) {
    if (diag != nullptr) {
      diag->carry_failed = true;
    }
    return Err(ErrorCode::Unavailable, "prepare_expiry: forward resolution was non-positive");
  }
  if (diag != nullptr) {
    diag->carry_available = true;
    diag->carry = carry.carry;
  }

  const double q_eff = rate - std::log(carry.forward / inputs.S) / chain.T;
  const double df = std::exp(-rate * chain.T);
  // The compatibility path historically consumed the supplied correction
  // caches unconditionally. Configured preparation owns the explicit opt-in.
  const AmericanCorrectionCaches fit_caches =
      policy == PreparedObservationPolicy::LegacyEssviCompatibility
          ? inputs.deam.caches
          : (inputs.use_deam_cache_for_fit ? inputs.deam.caches : AmericanCorrectionCaches{});
  PreparedSliceInputs prepare_inputs;
  prepare_inputs.expiry_index = expiry_index;
  prepare_inputs.S = inputs.S;
  prepare_inputs.r = rate;
  prepare_inputs.F = carry.forward;
  prepare_inputs.q_eff = q_eff;
  prepare_inputs.df = df;
  prepare_inputs.calib = inputs.calib;
  prepare_inputs.caches = fit_caches;
  prepare_inputs.al_opts = inputs.deam.al_opts;
  prepare_inputs.iv_tolerance = inputs.deam.iv_tol;
  prepare_inputs.iv_max_iterations = inputs.deam.iv_max_iter;
  prepare_inputs.method = inputs.deam.method;
  prepare_inputs.policy = policy;
  // Legacy eSSVI always scored parity in the cold driver. The generic flag is
  // an optimization only for Configured preparation and must not erase the
  // historical eSSVI scoring population.
  prepare_inputs.prepare_scoring =
      policy == PreparedObservationPolicy::LegacyEssviCompatibility || inputs.score_parity;
  prepare_inputs.audit_fit_inversions = inputs.deam.audit_fit_inversions;
  prepare_inputs.max_iv_residual_half_spreads = inputs.deam.max_iv_residual_half_spreads;
  if (diag != nullptr) {
    prepare_inputs.out_legacy_fit_rows = &diag->n_fit_rows;
    prepare_inputs.out_legacy_audit_dropped = &diag->n_audit_dropped;
  }
  const StageClock::time_point observation_start =
      time_stages ? StageClock::now() : StageClock::time_point{};
  Result<PreparedSlice> prepared_result = PreparedSlice::create(chain, prepare_inputs);
  if (time_stages) {
    diag->observation_deam_ms =
        std::chrono::duration<double, std::milli>(StageClock::now() - observation_start).count();
  }
  ATX_TRY(PreparedSlice prepared, std::move(prepared_result));
  if (prepared.fit_observations().size() < kMinPreparedFitRows) {
    return Err(ErrorCode::NotFound, "prepare_expiry: fewer than five usable rows");
  }
  return Ok(CanonicalPreparedExpiry{std::move(prepared), rate, carry.borrow, q_eff, df});
}

} // namespace atx::vol
