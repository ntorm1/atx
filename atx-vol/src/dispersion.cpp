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

// The ATM-forward straddle of one member at tenor T: strike K = forward_at(T),
// ATM vol sigma = iv(K, T), and per-share straddle vega = call vega + put vega.
// Resolves the member's surface by uid and validates every quantity, naming the
// symbol on any failure (a member with no fittable ATM straddle sinks the book —
// the signal needs every leg).
[[nodiscard]] Result<DispersionLeg> resolve_leg(const SurfaceSet &surfaces,
                                                const DispersionMember &m, double T) {
  const PricedSurface *surf = surfaces.find(m.uid);
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

  const Result<AmericanGreeks> call = surf->greeks(K, T, Side::Call);
  if (!call) {
    return Err(call.error());
  }
  const Result<AmericanGreeks> put = surf->greeks(K, T, Side::Put);
  if (!put) {
    return Err(put.error());
  }
  const double straddle_vega = call->vega + put->vega;
  if (!std::isfinite(straddle_vega) || straddle_vega <= 0.0) {
    return Err(ErrorCode::Unavailable,
               "dispersion: degenerate ATM straddle vega for symbol '" + m.symbol + "'");
  }

  DispersionLeg leg;
  leg.symbol = m.symbol;
  leg.uid = m.uid;
  leg.K = K;
  leg.T = T;
  leg.sigma = sigma;
  leg.straddle_vega = straddle_vega;
  leg.straddle_qty = 0.0; // sized by the caller
  return Ok(std::move(leg));
}

// Exact surface-only projection path. `maturity` is absolute for constituent
// legs, so a calendar convention is resolved once by the index and cannot drift
// across surfaces.
[[nodiscard]] Result<DispersionLeg> resolve_projected_leg(const SurfaceSet &surfaces,
                                                          const DispersionMember &member,
                                                          const ProjectedMaturitySpec &maturity,
                                                          double multiplier) {
  const PricedSurface *surface = surfaces.find(member.uid);
  if (surface == nullptr) {
    return Err(ErrorCode::NotFound,
               "dispersion: no surface registered for symbol '" + member.symbol + "'");
  }
  OptionProjectionConfig config;
  config.output = OptionProjectionOutput::FullGreeks;
  config.analytic_greeks = true;
  const auto project = [&](Side side) {
    OptionProjectionSpec spec;
    spec.uid = member.uid;
    spec.maturity = maturity;
    spec.strike = ProjectedStrikeSpec::atm_forward();
    spec.side = side;
    spec.multiplier = multiplier;
    return project_option_contract(*surface, spec, config);
  };
  ATX_TRY(ProjectedOption call, project(Side::Call));

  // Force the put onto the call's exact strike and expiry: a dispersion
  // straddle is one concrete listed-style K/expiry pair, not two independently
  // re-resolved ATM coordinates.
  OptionProjectionSpec put_spec;
  put_spec.uid = member.uid;
  put_spec.maturity = ProjectedMaturitySpec::absolute(call.definition.expiry_ts_ns);
  put_spec.strike = ProjectedStrikeSpec::absolute(call.definition.contract.K);
  put_spec.side = Side::Put;
  put_spec.multiplier = multiplier;
  ATX_TRY(ProjectedOption put, project_option_contract(*surface, put_spec, config));

  const double straddle_vega = call.greeks.vega + put.greeks.vega;
  if (!std::isfinite(straddle_vega) || straddle_vega <= 0.0) {
    return Err(ErrorCode::Unavailable,
               "dispersion: degenerate projected straddle vega for symbol '" + member.symbol + "'");
  }
  DispersionLeg leg;
  leg.symbol = member.symbol;
  leg.uid = member.uid;
  leg.K = call.definition.contract.K;
  leg.T = call.definition.contract.T;
  leg.sigma = call.implied_vol;
  leg.straddle_vega = straddle_vega;
  leg.call_definition = call.definition;
  leg.put_definition = put.definition;
  return Ok(std::move(leg));
}

// Emit the two positions (Call then Put, same K/T/qty) of one sized straddle,
// appending to `out` with monotonically increasing ids from `next_id`.
void emit_straddle(const DispersionLeg &leg, double multiplier, std::uint64_t &next_id,
                   std::vector<Position> &out) {
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
  }
}

} // namespace

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
  const Result<DispersionLeg> idx = resolve_leg(surfaces, universe.index, T);
  if (!idx) {
    return Err(idx.error());
  }

  DispersionSignal sig;
  sig.T_used = T;
  sig.sigma_index = idx->sigma;
  sig.sigma_names.reserve(universe.names.size());
  sig.used_names.reserve(universe.names.size());

  // Resolve each name in input order, collecting survivors (and, under
  // DropRenormalize, recording drops). `surv_w` is parallel to `used_names`.
  std::vector<double> surv_w;
  surv_w.reserve(universe.names.size());
  for (std::size_t i = 0; i < universe.names.size(); ++i) {
    const DispersionMember &n = universe.names[i];
    const Result<DispersionLeg> leg = resolve_leg(surfaces, n, T);
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
    sig.sigma_names.push_back(leg->sigma);
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
  return Ok(std::move(sig));
}

Result<DispersionBook> build_dispersion_book(const DispersionUniverse &universe,
                                             const SurfaceSet &surfaces,
                                             const DispersionConfig &cfg) {
  if (!std::isfinite(cfg.target_T) || cfg.target_T <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "dispersion: target_T must be finite and positive");
  }
  if (!std::isfinite(cfg.target_vega) || cfg.target_vega <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "dispersion: target_vega must be finite and positive");
  }
  if (!std::isfinite(cfg.multiplier) || cfg.multiplier <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "dispersion: multiplier must be finite and positive");
  }

  double effective_t = cfg.target_T;
  std::optional<ProjectedMaturitySpec> common_maturity;
  std::optional<DispersionLeg> projected_index;
  if (cfg.projected_maturity.has_value()) {
    ATX_TRY(DispersionLeg index, resolve_projected_leg(surfaces, universe.index,
                                                       *cfg.projected_maturity, cfg.multiplier));
    effective_t = index.T;
    common_maturity = ProjectedMaturitySpec::absolute(index.call_definition.expiry_ts_ns);
    projected_index = std::move(index);
  }

  Result<DispersionSignal> sig = dispersion_signal(universe, surfaces, effective_t, cfg.missing);
  if (!sig) {
    return Err(sig.error());
  }

  // Size over the SAME survivor set the signal used, summing the basket weights
  // over survivors only, so the sizing and the signal can never disagree about who
  // is in the basket. dispersion_signal already validated Σ_survivors w > 0, so
  // the weighted basket vega matches the index leg EXACTLY (Σ ŵ_i = 1).
  double sum_w = 0.0;
  for (const std::size_t idx : sig->used_names) {
    sum_w += universe.names[idx].weight;
  }

  // ShortIndexLongNames: index straddle short (qty < 0), names long (qty > 0).
  const double idx_sign = (cfg.side == DispersionSide::ShortIndexLongNames) ? -1.0 : 1.0;
  const double name_sign = -idx_sign;
  const double mult = cfg.multiplier;

  Result<DispersionLeg> index_leg = projected_index.has_value()
                                        ? Ok(std::move(*projected_index))
                                        : resolve_leg(surfaces, universe.index, cfg.target_T);
  if (!index_leg) {
    return Err(index_leg.error());
  }
  index_leg->straddle_qty = idx_sign * cfg.target_vega / (index_leg->straddle_vega * mult);

  DispersionBook book;
  book.dropped = sig->dropped; // copy before moving the signal
  book.signal = std::move(*sig);
  book.index_leg = std::move(*index_leg);
  book.name_legs.reserve(book.signal.used_names.size());
  book.positions.reserve(2 * (1 + book.signal.used_names.size()));

  std::uint64_t next_id = 0;
  emit_straddle(book.index_leg, mult, next_id, book.positions);

  for (const std::size_t idx : book.signal.used_names) {
    const DispersionMember &n = universe.names[idx];
    Result<DispersionLeg> leg =
        common_maturity.has_value()
            ? resolve_projected_leg(surfaces, n, *common_maturity, cfg.multiplier)
            : resolve_leg(surfaces, n, cfg.target_T);
    if (!leg) {
      return Err(leg.error());
    }
    // Normalized-weighted (over survivors) basket vega matches the index leg:
    // n_i · vega_i · mult = (w_i / Σ_survivors w) · target_vega.
    const double w_hat = n.weight / sum_w;
    leg->straddle_qty = name_sign * w_hat * cfg.target_vega / (leg->straddle_vega * mult);
    emit_straddle(*leg, mult, next_id, book.positions);
    book.name_legs.push_back(std::move(*leg));
  }

  return Ok(std::move(book));
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
