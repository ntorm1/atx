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
  return Ok(sigma);
}

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

  // C1.7: price + vega only — no full 5-solve AmericanGreeks bundle per side.
  // `fair_value()` is bit-identical to `greeks_analytic().price` (same base
  // boundary; see priced_surface.hpp/.cpp doc comments), and `vega()` is
  // bit-identical to `greeks_analytic().vega` on the AL path (american_vega_al),
  // so call_mark/put_mark/straddle_vega are unchanged from the old two-bundle
  // path while dropping the redundant delta/gamma/theta/rho/vanna/volga/charm
  // solves neither this function nor its caller ever read.
  const Result<double> call_price = surf->fair_value(K, T, Side::Call);
  if (!call_price) {
    return Err(call_price.error());
  }
  const Result<double> call_vega = surf->vega(K, T, Side::Call);
  if (!call_vega) {
    return Err(call_vega.error());
  }
  const Result<double> put_price = surf->fair_value(K, T, Side::Put);
  if (!put_price) {
    return Err(put_price.error());
  }
  const Result<double> put_vega = surf->vega(K, T, Side::Put);
  if (!put_vega) {
    return Err(put_vega.error());
  }
  const double straddle_vega = *call_vega + *put_vega;
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
  leg.call_mark = *call_price;
  leg.put_mark = *put_price;
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
  leg.call_mark = call.model_mark;
  leg.put_mark = put.model_mark;
  leg.call_definition = call.definition;
  leg.put_definition = put.definition;
  return Ok(std::move(leg));
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
  Result<DispersionLeg> index_leg = resolve_leg(surfaces, universe.index, cfg.target_T);
  if (cfg.projected_maturity.has_value()) {
    index_leg =
        resolve_projected_leg(surfaces, universe.index, *cfg.projected_maturity, cfg.multiplier);
  }
  if (!index_leg) {
    return Err(index_leg.error());
  }
  if (cfg.projected_maturity.has_value()) {
    common_maturity = ProjectedMaturitySpec::absolute(index_leg->call_definition.expiry_ts_ns);
  }

  std::vector<std::size_t> used_names;
  std::vector<DispersionLeg> name_legs;
  std::vector<DroppedName> dropped;
  used_names.reserve(universe.names.size());
  name_legs.reserve(universe.names.size());
  dropped.reserve(universe.names.size());
  for (std::size_t i = 0; i < universe.names.size(); ++i) {
    const DispersionMember &name = universe.names[i];
    Result<DispersionLeg> leg =
        common_maturity.has_value()
            ? resolve_projected_leg(surfaces, name, *common_maturity, cfg.multiplier)
            : resolve_leg(surfaces, name, cfg.target_T);
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
    name_legs.push_back(std::move(*leg));
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
  index_leg->straddle_qty =
      index_sign * cfg.target_vega / (index_leg->straddle_vega * cfg.multiplier);
  for (std::size_t k = 0; k < used_names.size(); ++k) {
    const double normalized_weight = universe.names[used_names[k]].weight / sum_w;
    name_legs[k].straddle_qty = name_sign * normalized_weight * cfg.target_vega /
                                (name_legs[k].straddle_vega * cfg.multiplier);
  }

  DispersionBook book;
  book.index_leg = std::move(*index_leg);
  book.name_legs = std::move(name_legs);
  book.used_names = std::move(used_names);
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
