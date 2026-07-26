// DispersionBook implementation — implied-correlation signal and vega-neutral
// straddle sizing. See dispersion.hpp for the model.

#include "atx/vol/dispersion.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp" // AmericanGreeks
#include "atx/vol/contract_projection.hpp"
#include "atx/vol/priced_surface.hpp" // PricedSurface, PricingContext
#include "atx/vol/surface_parity.hpp" // SliceContext
#include "atx/vol/types.hpp"          // Result, Side
#include "atx/vol/vol_curve.hpp"      // CurveSurface

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Diagnostic-only ATM IV resolution. This intentionally stops before the
// American pricer: implied correlation consumes forwards and IVs, not option
// marks or Greeks.
[[nodiscard]] Result<double> resolve_atm_iv(const SurfaceSet &surfaces, const DispersionMember &m,
                                            double T) {
  const SurfaceRef surf = surfaces.find(m.uid);
  if (surf == nullptr) {
    return Err(ErrorCode::NotFound,
               "dispersion: no surface registered for symbol '" + m.symbol + "'");
  }
  const double K = surf->forward_at(T);
  if (!(K > 0.0)) {
    return Err(ErrorCode::Unavailable,
               "dispersion: no ATM forward for symbol '" + m.symbol + "' at the tenor");
  }
  const double sigma = surf->iv(K, T);
  if (!std::isfinite(sigma) || sigma <= 0.0) {
    return Err(ErrorCode::Unavailable, "dispersion: ATM vol unavailable for symbol '" + m.symbol +
                                           "' (tenor outside surface domain)");
  }
  return Ok(sigma);
}

// The ATM-forward straddle of one member at tenor T: strike K = forward_at(T),
// ATM vol sigma = iv(K, T), and per-share straddle vega = call vega + put vega.
// Resolves the member's surface by uid and validates every quantity, naming the
// symbol on any failure (a member with no fittable ATM straddle sinks the book —
// the signal needs every leg).
struct ResolvedDispersionLeg {
  DispersionLeg leg;
  std::optional<FullGreekSeed> call_seed;
  std::optional<FullGreekSeed> put_seed;
};

[[nodiscard]] Status append_entry_seeds(ResolvedDispersionLeg &leg,
                                        std::vector<FullGreekSeed> &out) {
  if (leg.call_seed.has_value() != leg.put_seed.has_value()) {
    return Err(ErrorCode::Internal, "dispersion: incomplete entry seed pair");
  }
  if (leg.call_seed.has_value()) {
    out.push_back(std::move(*leg.call_seed));
    out.push_back(std::move(*leg.put_seed));
  }
  return Ok();
}

[[nodiscard]] Result<ResolvedDispersionLeg> resolve_leg(const SurfaceSet &surfaces,
                                                        const DispersionMember &m, double T,
                                                        const PriceOptions *price_options,
                                                        const StrikePolicy &strike) {
  const SurfaceRef surf = surfaces.find(m.uid);
  if (surf == nullptr) {
    return Err(ErrorCode::NotFound,
               "dispersion: no surface registered for symbol '" + m.symbol + "'");
  }
  const double forward = surf->forward_at(T);
  if (!(forward > 0.0)) {
    return Err(ErrorCode::Unavailable,
               "dispersion: no ATM forward for symbol '" + m.symbol + "' at the tenor");
  }
  // X4 STRIKE RULE (legacy synthetic-tenor path). The default assigns the forward
  // through UNCHANGED — no arithmetic — so the pinned golden's strike is bit-for-bit
  // what it was. A strangle needs two distinct strikes on one leg, which this path
  // cannot express, so it is refused rather than silently downgraded to a straddle.
  double K = forward;
  switch (strike.rule) {
  case StrikeRule::AtmForwardStraddle:
    break;
  case StrikeRule::FixedMoneyness:
    K = forward * std::exp(strike.log_moneyness);
    if (!(K > 0.0) || !std::isfinite(K)) {
      return Err(ErrorCode::InvalidArgument,
                 "dispersion: fixed-moneyness strike is degenerate for symbol '" + m.symbol + "'");
    }
    break;
  case StrikeRule::DeltaStrangle:
    return Err(ErrorCode::InvalidArgument,
               "dispersion: the delta-strangle strike rule requires the projected-maturity "
               "path (a synthetic-tenor leg carries only a single strike)");
  }
  double sigma = 0.0;
  if (price_options == nullptr) {
    sigma = surf->iv(K, T);
    if (!std::isfinite(sigma) || sigma <= 0.0) {
      return Err(ErrorCode::Unavailable, "dispersion: ATM vol unavailable for symbol '" + m.symbol +
                                             "' (tenor outside surface domain)");
    }
  }

  // C1.7: price + vega only — no full 5-solve AmericanGreeks bundle per side.
  // `fair_value()` is bit-identical to `greeks_analytic().price` (same base
  // boundary; see priced_surface.hpp/.cpp doc comments), and `vega()` is
  // bit-identical to `greeks_analytic().vega` on the AL path (american_vega_al),
  // so call_mark/put_mark/straddle_vega are unchanged from the old two-bundle
  // path while dropping the redundant delta/gamma/theta/rho/vanna/volga/charm
  // solves neither the legacy pure builder nor its caller ever read. Engine
  // entries instead produce exact full-risk seeds for sizing and risk handoff.
  double call_mark = 0.0;
  double put_mark = 0.0;
  double call_vega = 0.0;
  double put_vega = 0.0;
  std::optional<FullGreekSeed> resolved_call_seed;
  std::optional<FullGreekSeed> resolved_put_seed;
  if (price_options != nullptr) {
    ATX_TRY(FullGreekSeed call_seed,
            surf->full_greek_seed(K, T, Side::Call, price_options->analytic_greeks,
                                  price_options->query_execution));
    ATX_TRY(FullGreekSeed put_seed,
            surf->full_greek_seed(K, T, Side::Put, price_options->analytic_greeks,
                                  price_options->query_execution));
    call_mark = call_seed.greeks().price;
    put_mark = put_seed.greeks().price;
    call_vega = call_seed.greeks().vega;
    put_vega = put_seed.greeks().vega;
    sigma = call_seed.iv();
    resolved_call_seed.emplace(std::move(call_seed));
    resolved_put_seed.emplace(std::move(put_seed));
  } else {
    ATX_TRY(double resolved_call_mark, surf->fair_value(K, T, Side::Call));
    ATX_TRY(double resolved_call_vega, surf->vega(K, T, Side::Call));
    ATX_TRY(double resolved_put_mark, surf->fair_value(K, T, Side::Put));
    ATX_TRY(double resolved_put_vega, surf->vega(K, T, Side::Put));
    call_mark = resolved_call_mark;
    put_mark = resolved_put_mark;
    call_vega = resolved_call_vega;
    put_vega = resolved_put_vega;
  }
  if (!std::isfinite(sigma) || sigma <= 0.0) {
    return Err(ErrorCode::Unavailable, "dispersion: ATM vol unavailable for symbol '" + m.symbol +
                                           "' (tenor outside surface domain)");
  }
  const double straddle_vega = call_vega + put_vega;
  if (!std::isfinite(straddle_vega) || straddle_vega <= 0.0) {
    return Err(ErrorCode::Unavailable,
               "dispersion: degenerate ATM straddle vega for symbol '" + m.symbol + "'");
  }

  ResolvedDispersionLeg resolved;
  resolved.leg.symbol = m.symbol;
  resolved.leg.uid = m.uid;
  resolved.leg.K = K;
  resolved.leg.forward = forward;
  resolved.leg.T = T;
  resolved.leg.sigma = sigma;
  resolved.leg.straddle_vega = straddle_vega;
  resolved.leg.straddle_qty = 0.0; // sized by the caller
  resolved.leg.call_mark = call_mark;
  resolved.leg.put_mark = put_mark;
  resolved.call_seed = std::move(resolved_call_seed);
  resolved.put_seed = std::move(resolved_put_seed);
  return Ok(std::move(resolved));
}

// Exact surface-only projection path. `maturity` is absolute for constituent
// legs, so a calendar convention is resolved once by the index and cannot drift
// across surfaces.
[[nodiscard]] Result<ResolvedDispersionLeg>
resolve_projected_leg(const SurfaceSet &surfaces, const DispersionMember &member,
                      const ProjectedMaturitySpec &maturity, double multiplier,
                      const PriceOptions *price_options, const StrikePolicy &strike) {
  const SurfaceRef surface = surfaces.find(member.uid);
  if (surface == nullptr) {
    return Err(ErrorCode::NotFound,
               "dispersion: no surface registered for symbol '" + member.symbol + "'");
  }
  OptionProjectionConfig config;
  config.output = price_options == nullptr ? OptionProjectionOutput::FullGreeks
                                           : OptionProjectionOutput::DefinitionOnly;
  config.analytic_greeks = price_options == nullptr || price_options->analytic_greeks;
  config.query_execution =
      price_options == nullptr ? QueryExecution::Configured : price_options->query_execution;
  // X4 STRIKE RULE. The default branch constructs `atm_forward()` exactly as
  // before, so the pinned golden's projection spec is unchanged.
  const auto call_strike_spec = [&]() -> Result<ProjectedStrikeSpec> {
    switch (strike.rule) {
    case StrikeRule::AtmForwardStraddle:
      return Ok(ProjectedStrikeSpec::atm_forward());
    case StrikeRule::FixedMoneyness:
      return Ok(ProjectedStrikeSpec::log_moneyness(strike.log_moneyness));
    case StrikeRule::DeltaStrangle:
      if (!(strike.target_abs_delta > 0.0) || !(strike.target_abs_delta < 1.0)) {
        return Err(ErrorCode::InvalidArgument,
                   "dispersion: delta-strangle target_abs_delta must lie in (0, 1)");
      }
      return Ok(ProjectedStrikeSpec::delta(strike.target_abs_delta));
    }
    return Ok(ProjectedStrikeSpec::atm_forward());
  }();
  if (!call_strike_spec) {
    return Err(call_strike_spec.error());
  }
  const auto project = [&](Side side) {
    OptionProjectionSpec spec;
    spec.uid = member.uid;
    spec.maturity = maturity;
    spec.strike = *call_strike_spec;
    spec.side = side;
    spec.multiplier = multiplier;
    return project_option_contract(*surface, spec, config);
  };
  ATX_TRY(ProjectedOption call, project(Side::Call));

  // STRADDLE rules force the put onto the call's exact strike and expiry: a
  // dispersion straddle is one concrete listed-style K/expiry pair, not two
  // independently re-resolved ATM coordinates.
  //
  // The STRANGLE rule is the deliberate exception — its two legs are two
  // different strikes by definition — so the put re-resolves its own strike at
  // the same target |delta|, pinned to the call's expiry so the leg still has one
  // maturity.
  OptionProjectionSpec put_spec;
  put_spec.uid = member.uid;
  put_spec.maturity = ProjectedMaturitySpec::absolute(call.definition.expiry_ts_ns);
  put_spec.strike = strike.rule == StrikeRule::DeltaStrangle
                        ? ProjectedStrikeSpec::delta(strike.target_abs_delta)
                        : ProjectedStrikeSpec::absolute(call.definition.contract.K);
  put_spec.side = Side::Put;
  put_spec.multiplier = multiplier;
  ATX_TRY(ProjectedOption put, project_option_contract(*surface, put_spec, config));

  double resolved_sigma = call.implied_vol;
  double call_mark = call.model_mark;
  double put_mark = put.model_mark;
  double call_vega = call.greeks.vega;
  double put_vega = put.greeks.vega;
  std::optional<FullGreekSeed> resolved_call_seed;
  std::optional<FullGreekSeed> resolved_put_seed;
  if (price_options != nullptr) {
    ATX_TRY(FullGreekSeed call_seed,
            surface->full_greek_seed(call.definition.contract.K, call.definition.contract.T,
                                     Side::Call, price_options->analytic_greeks,
                                     price_options->query_execution));
    ATX_TRY(FullGreekSeed put_seed,
            surface->full_greek_seed(put.definition.contract.K, put.definition.contract.T,
                                     Side::Put, price_options->analytic_greeks,
                                     price_options->query_execution));
    call_mark = call_seed.greeks().price;
    put_mark = put_seed.greeks().price;
    call_vega = call_seed.greeks().vega;
    put_vega = put_seed.greeks().vega;
    resolved_sigma = call_seed.iv();
    resolved_call_seed.emplace(std::move(call_seed));
    resolved_put_seed.emplace(std::move(put_seed));
  }
  const double straddle_vega = call_vega + put_vega;
  if (!std::isfinite(resolved_sigma) || resolved_sigma <= 0.0) {
    return Err(ErrorCode::Unavailable,
               "dispersion: projected vol unavailable for symbol '" + member.symbol + "'");
  }
  if (!std::isfinite(straddle_vega) || straddle_vega <= 0.0) {
    return Err(ErrorCode::Unavailable,
               "dispersion: degenerate projected straddle vega for symbol '" + member.symbol + "'");
  }
  ResolvedDispersionLeg resolved;
  resolved.leg.symbol = member.symbol;
  resolved.leg.uid = member.uid;
  resolved.leg.K = call.definition.contract.K;
  resolved.leg.forward = call.forward;
  resolved.leg.T = call.definition.contract.T;
  resolved.leg.sigma = resolved_sigma;
  resolved.leg.straddle_vega = straddle_vega;
  resolved.leg.call_mark = call_mark;
  resolved.leg.put_mark = put_mark;
  resolved.leg.call_definition = call.definition;
  resolved.leg.put_definition = put.definition;
  resolved.call_seed = std::move(resolved_call_seed);
  resolved.put_seed = std::move(resolved_put_seed);
  return Ok(std::move(resolved));
}

// X4. The per-straddle risk on a scheme's matched axis, always POSITIVE for a
// usable leg. Gamma and theta ride the exact Black-Scholes identities that tie
// them to the vega already resolved, so no scheme issues an extra solve.
[[nodiscard]] double leg_matched_risk(const DispersionLeg &leg, WeightingScheme scheme) noexcept {
  switch (scheme) {
  case WeightingScheme::VegaNeutral:
  case WeightingScheme::EqualVega:
    return leg.straddle_vega;
  case WeightingScheme::GammaNeutral:
    return straddle_gamma_from_vega(leg.straddle_vega, leg.forward, leg.sigma, leg.T);
  case WeightingScheme::ThetaNeutral:
    return straddle_theta_magnitude_from_vega(leg.straddle_vega, leg.sigma, leg.T);
  }
  return leg.straddle_vega;
}

[[nodiscard]] std::string_view matched_axis_name(WeightingScheme scheme) noexcept {
  switch (scheme) {
  case WeightingScheme::VegaNeutral:
  case WeightingScheme::EqualVega:
    return "straddle vega";
  case WeightingScheme::GammaNeutral:
    return "straddle gamma";
  case WeightingScheme::ThetaNeutral:
    return "straddle theta";
  }
  return "straddle risk";
}

// Emit the two positions (Call then Put, same K/T/qty) of one sized straddle,
// appending to `out` with monotonically increasing ids from `next_id`.
void emit_straddle(const DispersionLeg &leg, double multiplier, std::uint64_t &next_id,
                   std::vector<Position> &out, std::vector<double> &marks) {
  for (const Side side : {Side::Call, Side::Put}) {
    Position p;
    p.id = next_id++;
    const ProjectedOptionDefinition &definition =
        side == Side::Call ? leg.call_definition : leg.put_definition;
    p.contract = definition.fingerprint != 0u ? definition.contract
                                              : OptionContract{leg.uid, leg.K, leg.T, side};
    p.qty = leg.straddle_qty;
    p.multiplier = multiplier;
    out.push_back(p);
    marks.push_back(side == Side::Call ? leg.call_mark : leg.put_mark);
  }
}

} // namespace

double straddle_gamma_from_vega(double vega, double forward, double sigma, double T) noexcept {
  if (!std::isfinite(vega) || !std::isfinite(forward) || !std::isfinite(sigma) ||
      !std::isfinite(T) || vega <= 0.0 || forward <= 0.0 || sigma <= 0.0 || T <= 0.0) {
    return 0.0;
  }
  const double gamma = vega / (forward * forward * sigma * T);
  return std::isfinite(gamma) ? gamma : 0.0;
}

double straddle_theta_magnitude_from_vega(double vega, double sigma, double T) noexcept {
  if (!std::isfinite(vega) || !std::isfinite(sigma) || !std::isfinite(T) || vega <= 0.0 ||
      sigma <= 0.0 || T <= 0.0) {
    return 0.0;
  }
  const double theta = 0.5 * sigma * vega / T;
  return std::isfinite(theta) ? theta : 0.0;
}

double correlation_vega(const DispersionSignal &sig, double index_signed_vega) noexcept {
  return index_signed_vega * sig.d_sigma_d_rho;
}

double correlation_gamma(const DispersionSignal &sig, double index_signed_vega, double T) noexcept {
  // volga / vega at the ATM forward == -sigma * T / 4 (d1 * d2 = -sigma^2 T / 4).
  const double volga_over_vega = -0.25 * sig.sigma_index * T;
  return index_signed_vega *
         (volga_over_vega * sig.d_sigma_d_rho * sig.d_sigma_d_rho + sig.d2_sigma_d_rho2);
}

Result<DispersionSignal> dispersion_signal(const DispersionUniverse &universe,
                                           const SurfaceSet &surfaces, double T,
                                           MissingNameSpec missing) {
  // Under Error an authored basket with < 2 names is degenerate (pre-S1-3
  // InvalidArgument). Under DropRenormalize a small/empty basket is a no-trade
  // condition, surfaced as Unavailable by the survivor guard below.
  if (missing.policy == MissingNamePolicy::Error && universe.names.size() < 2) {
    return Err(ErrorCode::InvalidArgument, "dispersion: need at least two basket names");
  }
  if (!std::isfinite(T) || T <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "dispersion: tenor T must be finite and positive");
  }
  if (missing.min_names < 2) {
    return Err(ErrorCode::InvalidArgument, "dispersion: min surviving basket size must be >= 2");
  }

  // A non-finite weight is ALWAYS an authoring bug — checked over EVERY name so a
  // NaN weight can never hide behind a drop.
  for (const DispersionMember &n : universe.names) {
    if (!std::isfinite(n.weight)) {
      return Err(ErrorCode::InvalidArgument,
                 "dispersion: non-finite weight for symbol '" + n.symbol + "'");
    }
  }
  // Under Error, the classic up-front total-weight positivity guard fires BEFORE
  // the index resolve (bit-identical ordering to pre-S1-3: a non-positive weight
  // sum is InvalidArgument even against an empty SurfaceSet). Under
  // DropRenormalize the positivity guard moves to the survivor sum after drops.
  if (missing.policy == MissingNamePolicy::Error) {
    double sum_all = 0.0;
    for (const DispersionMember &n : universe.names) {
      sum_all += n.weight;
    }
    if (!(sum_all > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "dispersion: basket weights must sum to a positive value");
    }
  }

  // The index leg is never droppable — no dispersion without an index.
  const Result<double> idx = resolve_atm_iv(surfaces, universe.index, T);
  if (!idx) {
    return Err(idx.error());
  }

  DispersionSignal sig;
  sig.T_used = T;
  sig.sigma_index = *idx;
  sig.sigma_names.reserve(universe.names.size());
  sig.used_names.reserve(universe.names.size());

  // Resolve each name in input order, collecting survivors (and, under
  // DropRenormalize, recording drops). `surv_w` is parallel to `used_names`.
  std::vector<double> surv_w;
  surv_w.reserve(universe.names.size());
  for (std::size_t i = 0; i < universe.names.size(); ++i) {
    const DispersionMember &n = universe.names[i];
    const Result<double> leg = resolve_atm_iv(surfaces, n, T);
    if (!leg) {
      // Under DropRenormalize a NotFound (surface not registered) / Unavailable
      // (unusable ATM straddle) NAME is dropped and recorded; any other code, or
      // the Error policy, propagates unchanged (never a silent drop).
      const ErrorCode ec = leg.error().code();
      if (missing.policy == MissingNamePolicy::DropRenormalize &&
          (ec == ErrorCode::NotFound || ec == ErrorCode::Unavailable)) {
        DroppedName d;
        d.symbol = n.symbol;
        d.reason =
            (ec == ErrorCode::NotFound) ? DropReason::SurfaceNotFound : DropReason::Unavailable;
        d.detail = leg.error().message();
        sig.dropped.push_back(std::move(d));
        continue;
      }
      return Err(leg.error());
    }
    sig.used_names.push_back(i);
    surv_w.push_back(n.weight);
    sig.sigma_names.push_back(*leg);
  }

  // Below the minimum surviving basket size the date has no tradeable book — the
  // "not tradeable today" contract the strategy keys off (Unavailable).
  if (sig.used_names.size() < missing.min_names) {
    return Err(ErrorCode::Unavailable, "dispersion: only " + std::to_string(sig.used_names.size()) +
                                           " of " + std::to_string(universe.names.size()) +
                                           " names survived (min " +
                                           std::to_string(missing.min_names) + ")");
  }

  // Renormalize over the SURVIVORS: w_hat_i = w_i / Σ_survivors w  =>  Σŵ = 1, so
  // rho_imp is scale-invariant and correct for the surviving partial basket.
  double sum_w = 0.0;
  for (const double w : surv_w) {
    sum_w += w;
  }
  if (!(sum_w > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "dispersion: surviving basket weights must sum to a positive value");
  }

  double sum_w_sigma = 0.0;
  double sum_w2_sigma2 = 0.0;
  for (std::size_t k = 0; k < sig.used_names.size(); ++k) {
    const double w_hat = surv_w[k] / sum_w;
    const double sigma = sig.sigma_names[k];
    sum_w_sigma += w_hat * sigma;
    sum_w2_sigma2 += w_hat * w_hat * sigma * sigma;
  }

  // Denominator == Σ_{i≠j} w_i w_j sigma_i sigma_j; positive whenever >= 2 names
  // carry positive weight+vol. Guarded per the brief.
  const double denom = sum_w_sigma * sum_w_sigma - sum_w2_sigma2;
  if (!(denom > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "dispersion: degenerate correlation denominator");
  }

  sig.sum_w_sigma = sum_w_sigma;
  sig.sum_w2_sigma2 = sum_w2_sigma2;
  sig.implied_corr = (sig.sigma_index * sig.sigma_index - sum_w2_sigma2) / denom;
  // X4 correlation-gamma primitives. sigma_idx(rho) = sqrt(A + rho * B) with
  // B == `denom` (already guarded > 0), so the two derivatives below are exact
  // and need no extra market data. Both are appended AFTER `implied_corr` so the
  // existing computation is untouched.
  if (sig.sigma_index > 0.0) {
    sig.d_sigma_d_rho = denom / (2.0 * sig.sigma_index);
    sig.d2_sigma_d_rho2 =
        -(denom * denom) / (4.0 * sig.sigma_index * sig.sigma_index * sig.sigma_index);
  }
  return Ok(std::move(sig));
}

namespace {

[[nodiscard]] Result<DispersionBook> build_dispersion_book_impl(const DispersionUniverse &universe,
                                                                const SurfaceSet &surfaces,
                                                                const DispersionConfig &cfg,
                                                                const PriceOptions *price_options) {
  if (!std::isfinite(cfg.target_T) || cfg.target_T <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "dispersion: target_T must be finite and positive");
  }
  if (!std::isfinite(cfg.target_vega) || cfg.target_vega <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "dispersion: target_vega must be finite and positive");
  }
  if (!std::isfinite(cfg.multiplier) || cfg.multiplier <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "dispersion: multiplier must be finite and positive");
  }
  if (cfg.missing.policy == MissingNamePolicy::Error && universe.names.size() < 2) {
    return Err(ErrorCode::InvalidArgument, "dispersion: need at least two basket names");
  }
  if (cfg.missing.min_names < 2) {
    return Err(ErrorCode::InvalidArgument, "dispersion: min surviving basket size must be >= 2");
  }

  double authored_weight_sum = 0.0;
  for (const DispersionMember &name : universe.names) {
    if (!std::isfinite(name.weight)) {
      return Err(ErrorCode::InvalidArgument,
                 "dispersion: non-finite weight for symbol '" + name.symbol + "'");
    }
    authored_weight_sum += name.weight;
  }
  if (cfg.missing.policy == MissingNamePolicy::Error && !(authored_weight_sum > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "dispersion: basket weights must sum to a positive value");
  }

  std::optional<ProjectedMaturitySpec> common_maturity;
  Result<ResolvedDispersionLeg> index_leg =
      resolve_leg(surfaces, universe.index, cfg.target_T, price_options, cfg.strike);
  if (cfg.projected_maturity.has_value()) {
    index_leg = resolve_projected_leg(surfaces, universe.index, *cfg.projected_maturity,
                                      cfg.multiplier, price_options, cfg.strike);
  }
  if (!index_leg) {
    return Err(index_leg.error());
  }
  if (cfg.projected_maturity.has_value()) {
    common_maturity = ProjectedMaturitySpec::absolute(index_leg->leg.call_definition.expiry_ts_ns);
  }

  std::vector<std::size_t> used_names;
  std::vector<DispersionLeg> name_legs;
  std::vector<FullGreekSeed> entry_risk_seeds;
  std::vector<DroppedName> dropped;
  used_names.reserve(universe.names.size());
  name_legs.reserve(universe.names.size());
  entry_risk_seeds.reserve(2u * (1u + universe.names.size()));
  dropped.reserve(universe.names.size());
  ATX_TRY_VOID(append_entry_seeds(*index_leg, entry_risk_seeds));
  for (std::size_t i = 0; i < universe.names.size(); ++i) {
    const DispersionMember &name = universe.names[i];
    Result<ResolvedDispersionLeg> leg =
        common_maturity.has_value()
            ? resolve_projected_leg(surfaces, name, *common_maturity, cfg.multiplier,
                                    price_options, cfg.strike)
            : resolve_leg(surfaces, name, cfg.target_T, price_options, cfg.strike);
    if (!leg) {
      const ErrorCode ec = leg.error().code();
      if (cfg.missing.policy == MissingNamePolicy::DropRenormalize &&
          (ec == ErrorCode::NotFound || ec == ErrorCode::Unavailable)) {
        dropped.push_back(DroppedName{name.symbol,
                                      ec == ErrorCode::NotFound ? DropReason::SurfaceNotFound
                                                                : DropReason::Unavailable,
                                      leg.error().message()});
        continue;
      }
      return Err(leg.error());
    }
    used_names.push_back(i);
    ATX_TRY_VOID(append_entry_seeds(*leg, entry_risk_seeds));
    name_legs.push_back(std::move(leg->leg));
  }
  if (used_names.size() < cfg.missing.min_names) {
    return Err(ErrorCode::Unavailable, "dispersion: only " + std::to_string(used_names.size()) +
                                           " of " + std::to_string(universe.names.size()) +
                                           " names survived (min " +
                                           std::to_string(cfg.missing.min_names) + ")");
  }

  double sum_w = 0.0;
  for (const std::size_t idx : used_names) {
    sum_w += universe.names[idx].weight;
  }
  if (!(sum_w > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "dispersion: surviving basket weights must sum to a positive value");
  }

  const double index_sign = cfg.side == DispersionSide::ShortIndexLongNames ? -1.0 : 1.0;
  const double name_sign = -index_sign;
  // ── E1 / AN-P1-1: `target_vega` is DOLLAR VEGA PER VOL POINT ──────────────
  //
  // CANONICAL UNIT (industry convention, and what the listed route has always
  // used): `cfg.target_vega` is dollars of index-leg gross vega per ONE VOL
  // POINT — a 0.01 move in sigma. `DispersionLeg::straddle_vega` is a per-share
  // dP/dsigma, i.e. per UNIT vol, so a contract's vega per vol point is
  //
  //     straddle_vega * multiplier * kVegaPerVolPoint
  //
  // which is textually the same expression the listed route builds as
  // `vega_per_contract_per_vol_point` (listed_dispersion_schedule.cpp). Sizing
  // off it makes both routes produce the same book from the same knob value.
  //
  // BEFORE THIS FIX the `* 0.01` was present in the listed route and ABSENT
  // here, so `build_dispersion_book` sized per UNIT vol: the same
  // `target_vega` built a projected-route book 100x SMALLER than the listed
  // one (AN-P1-1). Projected-route books therefore GROW 100x at this change —
  // see the CHANGELOG note and `ProjectedAndListedRoutesAgreeOnVegaUnit`.
  // `kVegaPerVolPoint` now lives in dispersion.hpp — `DispersionStrategy::
  // signals` needs the same conversion to recover the index leg's dollar vega
  // per UNIT vol for the correlation telemetry (FIX-E C-1).
  index_leg->leg.straddle_qty =
      index_sign * cfg.target_vega /
      contract_vega_per_vol_point(index_leg->leg.straddle_vega, cfg.multiplier);

  // ── X4 WEIGHTING POLICY ───────────────────────────────────────────────────
  //
  // VegaNeutral is evaluated by the LITERAL pre-X4 expression, not as a special
  // case of the generic branch below. The generic branch recovers the matched
  // risk as |q_index| * risk_index * multiplier, which equals `target_vega` only
  // up to a divide-then-multiply round-trip — enough to move the pinned golden
  // in the last ulp. Keeping the two paths textually separate is what makes
  // "the default is bit-identical" a property of the code rather than a hope.
  if (cfg.weighting == WeightingScheme::VegaNeutral) {
    for (std::size_t k = 0; k < used_names.size(); ++k) {
      const double normalized_weight = universe.names[used_names[k]].weight / sum_w;
      name_legs[k].straddle_qty =
          name_sign * normalized_weight * cfg.target_vega /
          contract_vega_per_vol_point(name_legs[k].straddle_vega, cfg.multiplier);
    }
  } else {
    // Every non-default scheme matches the basket to the INDEX leg on the
    // scheme's risk axis. The index leg keeps its target_vega sizing, so
    // `gross_index_vega` retains its meaning under every scheme.
    const double index_risk = leg_matched_risk(index_leg->leg, cfg.weighting);
    if (!std::isfinite(index_risk) || index_risk <= 0.0) {
      return Err(ErrorCode::Unavailable,
                 "dispersion: index leg has no usable " + std::string(matched_axis_name(cfg.weighting)) +
                     " to match the basket against");
    }
    const double target_risk = std::fabs(index_leg->leg.straddle_qty) * index_risk * cfg.multiplier;
    const double survivor_count = static_cast<double>(used_names.size());
    for (std::size_t k = 0; k < used_names.size(); ++k) {
      // EqualVega allocates uniformly across SURVIVORS (so a drop reweights the
      // rest); every other scheme allocates by renormalized index weight.
      const double allocation = cfg.weighting == WeightingScheme::EqualVega
                                    ? 1.0 / survivor_count
                                    : universe.names[used_names[k]].weight / sum_w;
      const double leg_risk = leg_matched_risk(name_legs[k], cfg.weighting);
      if (!std::isfinite(leg_risk) || leg_risk <= 0.0) {
        return Err(ErrorCode::Unavailable, "dispersion: no usable " +
                                               std::string(matched_axis_name(cfg.weighting)) +
                                               " for symbol '" + name_legs[k].symbol + "'");
      }
      name_legs[k].straddle_qty =
          name_sign * allocation * target_risk / (leg_risk * cfg.multiplier);
    }
  }

  DispersionBook book;
  book.index_leg = std::move(index_leg->leg);
  book.name_legs = std::move(name_legs);
  book.used_names = std::move(used_names);
  book.entry_risk_seeds = std::move(entry_risk_seeds);
  book.dropped = std::move(dropped);
  book.positions.reserve(2 * (1 + book.name_legs.size()));
  book.entry_marks.reserve(2 * (1 + book.name_legs.size()));

  std::uint64_t next_id = 0;
  emit_straddle(book.index_leg, cfg.multiplier, next_id, book.positions, book.entry_marks);
  for (const DispersionLeg &leg : book.name_legs) {
    emit_straddle(leg, cfg.multiplier, next_id, book.positions, book.entry_marks);
  }
  return Ok(std::move(book));
}

} // namespace

Result<DispersionBook> build_dispersion_book(const DispersionUniverse &universe,
                                             const SurfaceSet &surfaces,
                                             const DispersionConfig &cfg) {
  return build_dispersion_book_impl(universe, surfaces, cfg, nullptr);
}

Result<DispersionBook> build_dispersion_book(const DispersionUniverse &universe,
                                             const SurfaceSet &surfaces,
                                             const DispersionConfig &cfg,
                                             const PriceOptions &price_options) {
  return build_dispersion_book_impl(universe, surfaces, cfg, &price_options);
}

Result<ResolvedUniverse> resolve_universe_uids(const DispersionUniverse &universe,
                                               const SymbolUidLookup &lookup,
                                               MissingNameSpec missing) {
  // (symbol, resolved uid) already bound — small basket, so a linear scan for the
  // two "must fail loudly" duplicate checks is fine (never a hot path). `seen`
  // tracks EVERY member symbol (survivors AND drops) so a duplicate is caught even
  // when the first instance was dropped.
  std::vector<std::pair<std::string, std::uint32_t>> bound;
  bound.reserve(1u + universe.names.size());
  std::vector<std::string> seen;
  seen.reserve(1u + universe.names.size());

  // Bind one member. Returns Err on a hard failure (propagated), Ok(uid) when the
  // member resolves, or Ok(nullopt) when a NAME is droppable and absent.
  const auto bind_member = [&](const DispersionMember &m,
                               bool is_index) -> Result<std::optional<std::uint32_t>> {
    if (m.symbol.empty()) {
      return Err(ErrorCode::InvalidArgument, "dispersion: universe member has an empty symbol");
    }
    // Reject a symbol listed twice (would double-count the leg) — authoring bug.
    for (const std::string &s : seen) {
      if (s == m.symbol) {
        return Err(ErrorCode::InvalidArgument,
                   "dispersion: symbol '" + m.symbol + "' appears twice in the universe");
      }
    }
    seen.push_back(m.symbol);
    const std::optional<std::uint32_t> uid = lookup(m.symbol);
    if (!uid.has_value()) {
      // A droppable NAME absent from the directory => Ok(nullopt) (drop it). The
      // INDEX is never droppable, and under Error every absence is a hard NotFound.
      if (!is_index && missing.policy == MissingNamePolicy::DropRenormalize) {
        return Ok(std::optional<std::uint32_t>{});
      }
      return Err(ErrorCode::NotFound,
                 "dispersion: symbol '" + m.symbol + "' not present in snapshot directory");
    }
    if (*uid == 0u) {
      return Err(ErrorCode::InvalidArgument,
                 "dispersion: symbol '" + m.symbol + "' resolved to the reserved uid 0");
    }
    // Reject two distinct symbols that collapse to the same uid (would double-
    // count one surface as two legs) — authoring bug.
    for (const auto &[sym, prev] : bound) {
      if (prev == *uid) {
        return Err(ErrorCode::InvalidArgument, "dispersion: symbols '" + sym + "' and '" +
                                                   m.symbol + "' resolve to the same uid");
      }
    }
    bound.emplace_back(m.symbol, *uid);
    return Ok(std::optional<std::uint32_t>{*uid});
  };

  ResolvedUniverse out;
  out.universe.index = universe.index; // symbols + weights preserved; uid rebound below
  out.universe.names.reserve(universe.names.size());

  const Result<std::optional<std::uint32_t>> ix = bind_member(universe.index, /*is_index=*/true);
  if (!ix) {
    return Err(ix.error());
  }
  out.universe.index.uid = **ix; // index is never droppable => always resolves to a uid

  for (const DispersionMember &n : universe.names) {
    Result<std::optional<std::uint32_t>> r = bind_member(n, /*is_index=*/false);
    if (!r) {
      return Err(r.error());
    }
    if (r->has_value()) {
      DispersionMember bound_name = n; // symbol + weight preserved; uid rebound
      bound_name.uid = **r;
      out.universe.names.push_back(std::move(bound_name));
    } else {
      DroppedName d;
      d.symbol = n.symbol;
      d.reason = DropReason::NotInSnapshot;
      d.detail = "dispersion: symbol '" + n.symbol + "' not present in snapshot directory";
      out.dropped.push_back(std::move(d));
    }
  }

  return Ok(std::move(out));
}

Result<DispersionUniverse> resolve_universe_uids(const DispersionUniverse &universe,
                                                 const SymbolUidLookup &lookup) {
  // The 2-arg overload is the Error-policy resolve (S1-2 semantics): delegate to
  // the 3-arg one so the validation logic lives in exactly one place.
  Result<ResolvedUniverse> r = resolve_universe_uids(universe, lookup, MissingNameSpec{});
  if (!r) {
    return Err(r.error());
  }
  return Ok(std::move(r->universe));
}

Result<PricedSurface> with_uid(const PricedSurface &src, std::uint32_t uid) {
  CurveSurface curves = src.surface().clone();
  std::vector<SliceContext> ctx(src.context().begin(), src.context().end());
  PricingContext pc = src.pricing();
  pc.uid = uid;
  return PricedSurface::create(std::move(curves), std::move(ctx), pc);
}

} // namespace atx::vol
