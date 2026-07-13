#include "atx/vol/prepared_fitting.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/black76.hpp"
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
  ATX_TRY(ObsSet fit_set, build_observations_european(
                              chain, inputs.S, inputs.r, inputs.F, chain.T, inputs.df, inputs.calib,
                              inputs.caches, inputs.al_opts, inputs.iv_tolerance,
                              inputs.iv_max_iterations, inputs.method, inputs.prepare_scoring));

  PreparedSlice out;
  out.expiry_index_ = inputs.expiry_index;
  out.maturity_ = chain.T;
  out.forward_ = inputs.F;
  out.n_dropped_ = fit_set.n_dropped;
  out.provenance_ = SlicePreparationProvenance{inputs.policy,
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
  PreparedSlice out;
  out.expiry_index_ = inputs.expiry_index;
  out.maturity_ = chain.T;
  out.forward_ = inputs.F;
  out.provenance_ = SlicePreparationProvenance{inputs.policy,
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
    } else {
      Result<double> market_iv = european_equiv_iv(
          chain.mids[quote_index], inputs.S, strike, chain.T, inputs.r, inputs.q_eff, side,
          inputs.method, inputs.al_opts, inputs.caches.for_side(side), inputs.iv_tolerance,
          inputs.iv_max_iterations);
      // Correctness-first serving (§8.1): under `audit_fit_inversions` every
      // fitted proposal is repriced against the cold Andersen-Lake reference. A
      // failed proposal is recomputed accurately and re-audited; a row that
      // still misses the half-spread budget is DROPPED, never fitted — the same
      // protocol build_observations_european enforces on the curve-driver path.
      // Default-off keeps the historical (unaudited-fit) path bit-identical.
      if (market_iv.has_value() && inputs.audit_fit_inversions &&
          inputs.method == AmericanMethod::AndersenLake) {
        const double audit_spread = chain.asks[quote_index] - chain.bids[quote_index];
        Result<IvRepricingAudit> audit = audit_european_equiv_iv(
            chain.mids[quote_index], audit_spread, *market_iv, inputs.S, strike, chain.T, inputs.r,
            inputs.q_eff, side, inputs.max_iv_residual_half_spreads);
        if (!audit || !audit->passed) {
          const Result<double> accurate =
              european_equiv_iv(chain.mids[quote_index], inputs.S, strike, chain.T, inputs.r,
                                inputs.q_eff, side, AmericanMethod::AndersenLake, std::nullopt,
                                nullptr);
          bool dropped = true;
          if (accurate.has_value()) {
            audit = audit_european_equiv_iv(chain.mids[quote_index], audit_spread, *accurate,
                                            inputs.S, strike, chain.T, inputs.r, inputs.q_eff,
                                            side, inputs.max_iv_residual_half_spreads);
            if (audit && audit->passed) {
              market_iv = accurate;
              dropped = false;
            }
          }
          if (dropped) {
            market_iv = Err(ErrorCode::Unavailable,
                            "prepare_legacy: fit-inversion audit rejected the row");
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
        fit.noise_sigma = (vega > 0.0) ? spread / vega : 0.0;
        fit.side = side;
        fit.source_strike_index = static_cast<std::uint32_t>(index);
        fit.score_sigma_mkt = *market_iv;
        out.fit_rows_.push_back(fit);
        if (inputs.prepare_scoring) {
          append_score_row(out, key, fit, chain);
        }
        observation.european_iv = *market_iv;
        observation.weight_w = weight;
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

Result<CanonicalPreparedExpiry> prepare_expiry(const Chain &chain,
                                               std::uint32_t expiry_index,
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

  Result<ChainForward> carry_res =
      resolve_chain_forward(chain, inputs.S, rate, inputs.cash_divs, inputs.now_ts_ns, inputs.deam);
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
          : (inputs.use_deam_cache_for_fit ? inputs.deam.caches
                                           : AmericanCorrectionCaches{});
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
  ATX_TRY(PreparedSlice prepared, PreparedSlice::create(chain, prepare_inputs));
  if (prepared.fit_observations().size() < kMinPreparedFitRows) {
    return Err(ErrorCode::NotFound, "prepare_expiry: fewer than five usable rows");
  }
  return Ok(CanonicalPreparedExpiry{std::move(prepared), rate, carry.borrow, q_eff, df});
}

} // namespace atx::vol
