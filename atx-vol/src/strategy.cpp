// atx-vol strategy DSL interpreter (Phase B1) — see strategy.hpp for the model.
//
// Resolution: strike-from-delta (deterministic bisection on log-moneyness), the
// StrikeSelector -> K map, LegSpec -> ResolvedLeg expansion, per-leg sizing, and
// the cross-leg vega constraint. Plus the DeclarativeStrategy lifecycle (entry
// cadence, HoldToExpiry overlapping cohorts / RollAtHorizon single-cohort roll).

#include "atx/vol/strategy.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"  // AmericanGreeks

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// ── Strike-from-delta solver ────────────────────────────────────────────────

Result<double> resolve_strike_by_delta(const PricedSurface& s, double T, Side side,
                                       double target_abs_delta) {
  if (!(std::isfinite(target_abs_delta) && target_abs_delta > 0.0 && target_abs_delta < 1.0)) {
    return Err(ErrorCode::InvalidArgument,
               "resolve_strike_by_delta: target |delta| must lie in (0,1)");
  }
  const double F = s.forward_at(T);
  if (!(F > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "resolve_strike_by_delta: no forward at tenor");
  }

  // g(k) = |delta(F*e^k)| - target. `exact=false` marks a ±sentinel used only for
  // bracketing: when greeks() is unavailable at an extreme k, we impute the
  // asymptotic |delta| (-> 1 deep ITM, -> 0 deep OTM). The final root is validated
  // against a real reprice, so a sentinel-only "straddle" cannot pass as a hit.
  struct GVal {
    double value{0.0};
    bool exact{false};
  };
  const auto gof = [&](double k) -> GVal {
    const double K = F * std::exp(k);
    // The bisection consumes ONLY |delta|: the delta-only fast path (~1-2 boundary
    // solves) replaces the full-greeks reprice (17) per candidate strike, at a
    // bit-identical delta.
    const Result<double> gr = s.delta(K, T, side);
    if (gr) {
      const double d = std::fabs(*gr);
      if (std::isfinite(d)) {
        return GVal{d - target_abs_delta, true};
      }
    }
    const bool itm = (side == Side::Call) ? (k < 0.0) : (k > 0.0);
    const double asym = itm ? (1.0 - target_abs_delta) : (0.0 - target_abs_delta);
    return GVal{asym, false};
  };

  // Bracket: the first width whose endpoints straddle the target.
  double lo = 0.0;
  double hi = 0.0;
  GVal glo{};
  GVal ghi{};
  bool bracketed = false;
  for (const double w : {1.5, 3.0, 5.0}) {
    lo = -w;
    hi = w;
    glo = gof(lo);
    ghi = gof(hi);
    if ((glo.value <= 0.0) != (ghi.value <= 0.0)) {
      bracketed = true;
      break;
    }
  }
  if (!bracketed) {
    return Err(ErrorCode::InvalidArgument, "resolve_strike_by_delta: delta target unreachable");
  }

  // Root-find on the [lo,hi] sign-change bracket, fixed iteration cap
  // (deterministic; monotone |delta| per side). The trial abscissa is an
  // Illinois-weighted false-position (secant) step rather than the midpoint: for the
  // smooth, monotone g(k) = |delta|(k) - target this converges SUPERLINEARLY
  // (~6-8 evals vs bisection's ~23 to the same 1e-7 tol), which is the dominant cost
  // of the daily-restrike hot path. The bracket invariant is preserved, so the
  // method is as robust as bisection and lands on the same root. Interpolation is
  // used ONLY when both bracket residuals are EXACT repriced deltas and the guess
  // stays strictly interior; a sentinel endpoint (asymptotic bracket imputation) or
  // an escaping guess falls back to the midpoint. The Illinois down-weight of a
  // retained endpoint breaks the classic false-position stall. Accuracy is
  // unchanged: the convergence test and the post-loop validation use the true
  // residual, never the down-weighted one.
  double kroot = 0.5 * (lo + hi);
  int retained = 0;  // Illinois: +1 => hi retained last step, -1 => lo retained
  for (int it = 0; it < 128; ++it) {
    double x = 0.5 * (lo + hi);
    if (glo.exact && ghi.exact && glo.value != ghi.value) {
      const double xs = (glo.value * hi - ghi.value * lo) / (glo.value - ghi.value);
      const double margin = 1.0e-6 * (hi - lo);
      if (xs > lo + margin && xs < hi - margin) {
        x = xs;  // interior secant guess
      }
    }
    kroot = x;
    const GVal gm = gof(x);
    if (gm.exact && std::fabs(gm.value) <= 1.0e-7) {
      break;
    }
    if ((hi - lo) <= 1.0e-10) {
      break;
    }
    if ((gm.value <= 0.0) == (glo.value <= 0.0)) {
      lo = x;
      glo = gm;
      if (retained == -1 && ghi.exact) {
        ghi.value *= 0.5;  // Illinois: hi retained twice -> down-weight it
      }
      retained = -1;
    } else {
      hi = x;
      ghi = gm;
      if (retained == +1 && glo.exact) {
        glo.value *= 0.5;  // Illinois: lo retained twice -> down-weight it
      }
      retained = +1;
    }
  }

  // Validate: the root must actually reprice to the target (guards unreachable
  // targets that straddled only through the asymptotic sentinel).
  const double Kroot = F * std::exp(kroot);
  const Result<double> gr = s.delta(Kroot, T, side);
  if (!gr || !std::isfinite(*gr) ||
      std::fabs(std::fabs(*gr) - target_abs_delta) > 1.0e-4) {
    return Err(ErrorCode::InvalidArgument, "resolve_strike_by_delta: delta target unreachable");
  }
  return Ok(Kroot);
}

// ── StrikeSelector -> absolute K ────────────────────────────────────────────

namespace {

[[nodiscard]] Status validate_model_tenor(const TenorSpec& tenor) {
  if (!tenor.snap_to_listed) {
    return Ok();
  }
  return Err(ErrorCode::NotImplemented,
             "TenorSpec::snap_to_listed is unavailable in the model-on-model declarative "
             "strategy; use the listed OPRA workflow in listed_opra.hpp (see "
             "spy_strangle_tradeable)");
}

}  // namespace

Result<double> resolve_strike(const PricedSurface& s, const TenorSpec& tenor, Side side,
                              const StrikeSelector& sel) {
  const Status tenor_status = validate_model_tenor(tenor);
  if (!tenor_status) {
    return Err(tenor_status.error());
  }
  const double T = tenor.target_T;
  switch (sel.kind) {
    case StrikeSelector::Kind::AtmForward: {
      const double F = s.forward_at(T);
      if (!(F > 0.0)) {
        return Err(ErrorCode::InvalidArgument, "resolve_strike: no forward at tenor");
      }
      return Ok(F);
    }
    case StrikeSelector::Kind::Delta:
      return resolve_strike_by_delta(s, T, side, sel.value);
    case StrikeSelector::Kind::Moneyness: {
      const double F = s.forward_at(T);
      if (!(F > 0.0)) {
        return Err(ErrorCode::InvalidArgument, "resolve_strike: no forward at tenor");
      }
      return Ok(F * std::exp(sel.value));
    }
    case StrikeSelector::Kind::AbsStrike:
      if (!(std::isfinite(sel.value) && sel.value > 0.0)) {
        return Err(ErrorCode::InvalidArgument, "resolve_strike: AbsStrike must be positive");
      }
      return Ok(sel.value);
  }
  return Err(ErrorCode::InvalidArgument, "resolve_strike: unknown selector kind");
}

// ── LegSpec -> ResolvedLeg(s) ───────────────────────────────────────────────

Result<std::vector<ResolvedLeg>> expand_leg(const MarketSnapshot& snap, const LegSpec& leg) {
  const Status tenor_status = validate_model_tenor(leg.tenor);
  if (!tenor_status) {
    return Err(tenor_status.error());
  }
  std::uint32_t uid = leg.uid;
  if (uid == 0) {
    const std::optional<std::uint32_t> u = snap.uid_of(leg.symbol);
    if (!u) {
      return Err(ErrorCode::NotFound, "expand_leg: symbol '" + leg.symbol + "' not in snapshot");
    }
    uid = *u;
  }
  const PricedSurface* surf = snap.find(uid);
  if (surf == nullptr) {
    return Err(ErrorCode::NotFound, "expand_leg: no surface for leg's uid");
  }
  const double T = leg.tenor.target_T;
  if (!(std::isfinite(T) && T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "expand_leg: tenor T must be finite and positive");
  }

  // Resolve one (side, selector) into a ResolvedLeg with per-share vega + sigma.
  const auto make_one = [&](Side side, const StrikeSelector& sel) -> Result<ResolvedLeg> {
    const Result<double> K = resolve_strike(*surf, leg.tenor, side, sel);
    if (!K) {
      return Err(K.error());
    }
    const Result<AmericanGreeks> gr = surf->greeks(*K, T, side);
    if (!gr) {
      return Err(gr.error());
    }
    ResolvedLeg rl;
    rl.uid = uid;
    rl.K = *K;
    rl.T = T;
    rl.sigma = surf->iv(*K, T);
    rl.vega = gr->vega;  // signed greek vega (> 0 for both call and put)
    rl.theta = gr->theta;
    rl.gamma = gr->gamma;
    rl.side = side;
    rl.group = leg.group;
    return Ok(rl);
  };

  std::vector<ResolvedLeg> out;
  switch (leg.structure.kind) {
    case StructureSpec::Kind::Single: {
      Result<ResolvedLeg> rl = make_one(leg.structure.single_side, leg.strike);
      if (!rl) {
        return Err(rl.error());
      }
      out.push_back(std::move(*rl));
      break;
    }
    case StructureSpec::Kind::Straddle: {
      // Call + Put at the SAME strike (the leg's selector; default AtmForward).
      const Result<double> K = resolve_strike(*surf, leg.tenor, Side::Call, leg.strike);
      if (!K) {
        return Err(K.error());
      }
      const double sigma = surf->iv(*K, T);
      for (const Side side : {Side::Call, Side::Put}) {
        const Result<AmericanGreeks> gr = surf->greeks(*K, T, side);
        if (!gr) {
          return Err(gr.error());
        }
        ResolvedLeg rl;
        rl.uid = uid;
        rl.K = *K;
        rl.T = T;
        rl.sigma = sigma;
        rl.vega = gr->vega;
        rl.theta = gr->theta;
        rl.gamma = gr->gamma;
        rl.side = side;
        rl.group = leg.group;
        out.push_back(rl);
      }
      break;
    }
    case StructureSpec::Kind::Strangle: {
      Result<ResolvedLeg> rc = make_one(Side::Call, leg.structure.call_leg);
      if (!rc) {
        return Err(rc.error());
      }
      Result<ResolvedLeg> rp = make_one(Side::Put, leg.structure.put_leg);
      if (!rp) {
        return Err(rp.error());
      }
      out.push_back(std::move(*rc));
      out.push_back(std::move(*rp));
      break;
    }
    case StructureSpec::Kind::RiskReversal:
      return Err(ErrorCode::InvalidArgument, "expand_leg: RiskReversal not in B1");
  }
  return Ok(std::move(out));
}

// ── Sizing + cross-leg constraint ───────────────────────────────────────────

namespace {

// Gross position vega Σ|qty*vega*mult| over the sized legs tagged `group`.
[[nodiscard]] double group_gross_vega(const std::vector<SizedLeg>& sized, const std::string& group) {
  double g = 0.0;
  for (const SizedLeg& sl : sized) {
    if (sl.leg.group == group) {
      g += std::fabs(sl.qty * sl.leg.vega * sl.multiplier);
    }
  }
  return g;
}

// Expand + per-leg base sizing for ONE `LegSpec` (FixedContracts/TargetVega/
// Weight/TargetTheta/TargetGamma, multiplier 100). The exact per-leg body
// `resolve_spec` used inline pre-S1-3/T2, factored out so both `resolve_spec`
// and `resolve_spec_with_policy` share one implementation (errors — code AND
// message — are bit-identical to the pre-refactor inline form).
[[nodiscard]] Result<std::vector<SizedLeg>> expand_and_size_leg(const MarketSnapshot& snap,
                                                                 const LegSpec& ls) {
  constexpr double kMult = 100.0;

  Result<std::vector<ResolvedLeg>> exp = expand_leg(snap, ls);
  if (!exp) {
    return Err(exp.error());
  }
  const std::vector<ResolvedLeg>& opts = *exp;

  // Per-leg base sizing: one signed qty applied to every option of the structure.
  double qty = 0.0;
  switch (ls.size.kind) {
    case SizeSpec::Kind::FixedContracts:
    case SizeSpec::Kind::Weight:
      qty = ls.size.sign * ls.size.value;  // Weight: unitless, pre-constraint
      break;
    case SizeSpec::Kind::TargetVega:
    case SizeSpec::Kind::TargetTheta:
    case SizeSpec::Kind::TargetGamma: {
      // Size to a book GREEK: qty = sign * target / (|Σ leg greek| * mult), so
      // `value` is the target |book greek| and `sign` picks long/short. |Σ| makes
      // the target axis-agnostic (theta < 0 for a long option), so a short strangle
      // sized TargetTheta holds +value book theta. TargetVega is bit-identical to
      // the old form (structure vega is already > 0, so |.| is a no-op).
      //
      // THETA CONVENTION: the American greek theta is dP/dt with t in YEARS (per
      // kNsPerYear), so it is an ANNUALIZED $ theta. TargetTheta's `value` is a
      // per-CALENDAR-DAY theta (how traders quote it), converted to the annualized
      // book theta the greek carries by * 365.25 (the kNsPerYear day count).
      constexpr double kCalendarDaysPerYear = 365.25;  // matches kNsPerYear
      const auto pick = [&](const ResolvedLeg& o) -> double {
        switch (ls.size.kind) {
          case SizeSpec::Kind::TargetTheta:
            return o.theta;
          case SizeSpec::Kind::TargetGamma:
            return o.gamma;
          default:
            return o.vega;
        }
      };
      double structure_greek = 0.0;
      for (const ResolvedLeg& o : opts) {
        structure_greek += pick(o);
      }
      if (!(std::isfinite(structure_greek) && std::fabs(structure_greek) > 0.0)) {
        return Err(ErrorCode::Unavailable,
                   "resolve_spec: degenerate structure greek for target sizing");
      }
      const double target = (ls.size.kind == SizeSpec::Kind::TargetTheta)
                                ? ls.size.value * kCalendarDaysPerYear  // $/day -> $/yr
                                : ls.size.value;
      qty = ls.size.sign * target / (std::fabs(structure_greek) * kMult);
      break;
    }
  }
  std::vector<SizedLeg> out;
  out.reserve(opts.size());
  for (const ResolvedLeg& o : opts) {
    out.push_back(SizedLeg{o, qty, kMult});
  }
  return Ok(std::move(out));
}

// Shared resolution body for `resolve_spec` / `resolve_spec_with_policy`. Under
// `missing.policy == Error` this reproduces `resolve_spec`'s pre-S1-3/T2 inline
// behavior exactly (first leg failure propagates unchanged; no drop bookkeeping,
// no min_names floor). Under `DropRenormalize`, see `resolve_spec_with_policy`'s
// doc comment (strategy.hpp) for the full contract.
[[nodiscard]] Result<std::vector<SizedLeg>>
resolve_spec_impl(const MarketSnapshot& snap, const StrategySpec& spec, const MissingNameSpec& missing,
                  std::vector<ResolveDrop>* dropped) {
  if (dropped != nullptr) {
    dropped->clear();
  }
  // This is a configuration/capability error, never missing market data. Reject
  // the whole spec before DropRenormalize can turn an explicitly requested
  // listed contract into a silent model-contract substitution or name drop.
  for (const LegSpec& leg : spec.legs) {
    const Status tenor_status = validate_model_tenor(leg.tenor);
    if (!tenor_status) {
      return Err(tenor_status.error());
    }
  }
  const bool drop_policy = missing.policy == MissingNamePolicy::DropRenormalize;

  const CrossLegConstraint& c = spec.constraint;
  std::string ga = c.group_a;
  std::string gb = c.group_b;
  if (c.kind == CrossLegConstraint::Kind::VegaNeutralBasket) {
    if (ga.empty()) {
      ga = "index";
    }
    if (gb.empty()) {
      gb = "basket";
    }
  }
  const bool has_constraint = c.kind != CrossLegConstraint::Kind::None;

  std::vector<SizedLeg> sized;
  std::size_t group_a_survivors = 0;
  for (const LegSpec& ls : spec.legs) {
    Result<std::vector<SizedLeg>> leg_sized = expand_and_size_leg(snap, ls);
    if (!leg_sized) {
      if (!drop_policy) {
        return Err(leg_sized.error());  // Error policy: hard fail, unchanged
      }
      if (has_constraint && ls.group == gb) {
        // The scaled hedge group is never droppable: without it the entry can't
        // be built at all.
        return Err(ErrorCode::Unavailable, "resolve_spec_with_policy: hedge leg '" + ls.symbol +
                                               "' (group '" + gb +
                                               "') unavailable: " + leg_sized.error().message());
      }
      if (dropped != nullptr) {
        dropped->push_back(ResolveDrop{ls.symbol, leg_sized.error().message()});
      }
      continue;
    }
    const bool counts_toward_a = !has_constraint || ls.group == ga;
    if (counts_toward_a) {
      ++group_a_survivors;
    }
    for (SizedLeg& sl : *leg_sized) {
      sized.push_back(std::move(sl));
    }
  }

  if (drop_policy && group_a_survivors < missing.min_names) {
    return Err(ErrorCode::Unavailable,
               "resolve_spec_with_policy: only " + std::to_string(group_a_survivors) +
                   " surviving name(s), need >= " + std::to_string(missing.min_names));
  }

  // Cross-leg constraint: scale one group's gross vega onto another's, over the
  // SURVIVORS only — a dropped leg was never added to `sized`, so FlatVega's
  // ratio renormalizes automatically.
  if (c.kind == CrossLegConstraint::Kind::FlatVega ||
      c.kind == CrossLegConstraint::Kind::VegaNeutralBasket) {
    const double gross_a = group_gross_vega(sized, ga);
    const double gross_b = group_gross_vega(sized, gb);
    if (!(gross_b > 0.0)) {
      return Err(ErrorCode::Unavailable, "resolve_spec: constraint target group has zero gross vega");
    }
    const double scale = gross_a / gross_b;  // one factor => intra-group ratios preserved
    for (SizedLeg& sl : sized) {
      if (sl.leg.group == gb) {
        sl.qty *= scale;
      }
    }
  }

  return Ok(std::move(sized));
}

}  // namespace

Result<std::vector<SizedLeg>> resolve_spec(const MarketSnapshot& snap, const StrategySpec& spec) {
  return resolve_spec_impl(snap, spec, MissingNameSpec{MissingNamePolicy::Error, 2}, nullptr);
}

Result<std::vector<SizedLeg>> resolve_spec_with_policy(const MarketSnapshot& snap,
                                                       const StrategySpec& spec,
                                                       std::vector<ResolveDrop>* dropped) {
  return resolve_spec_impl(snap, spec, spec.missing, dropped);
}

// ── Lifecycle decision ──────────────────────────────────────────────────────

LifecycleDecision lifecycle_decide(const LifecycleSpec& lifecycle, std::size_t step_index,
                                   bool book_empty, std::int64_t base_ts, std::int64_t front_expiry,
                                   bool have_front) {
  if (lifecycle.holding == LifecycleSpec::Holding::HoldToExpiry ||
      lifecycle.holding == LifecycleSpec::Holding::CloseAtHorizon) {
    // Same entry-tick rule for both: overlapping cohorts, never clear (each
    // cohort is closed independently — HoldToExpiry via engine settlement at
    // T<=0, CloseAtHorizon via the strategy's own close pass in on_step).
    bool tick = true;
    if (lifecycle.entry == LifecycleSpec::Entry::EveryNDays) {
      const unsigned n = (lifecycle.entry_every_n == 0) ? 1u : lifecycle.entry_every_n;
      tick = (step_index % n) == 0;
    }
    return LifecycleDecision{tick, false};  // overlapping cohorts: never clear
  }

  // RollAtHorizon: a single cohort, rolled at the horizon.
  bool need = book_empty;
  if (!need && have_front) {
    const double residual_T =
        (static_cast<double>(front_expiry) - static_cast<double>(base_ts)) / kNsPerYear;
    if (residual_T < lifecycle.roll_at_T) {
      need = true;
    }
  }
  return LifecycleDecision{need, need && !book_empty};
}

// ── DeclarativeStrategy ─────────────────────────────────────────────────────

Status DeclarativeStrategy::open_cohort(const MarketSnapshot& base, PortfolioState& book,
                                        std::uint64_t& next_lot_id) {
  Result<std::vector<SizedLeg>> sized = resolve_spec_with_policy(base, spec_, &last_dropped_);
  if (!sized) {
    // NO-TRADE CONTRACT (mirrors DispersionStrategy, dispersion_strategy.cpp):
    // under DropRenormalize, Unavailable means the entry can't be built today
    // (too few surviving names, or the hedge leg is missing) — a flat / no-trade
    // step. Leave the book untouched and continue; any other error is fatal.
    if (spec_.missing.policy == MissingNamePolicy::DropRenormalize &&
        sized.error().code() == ErrorCode::Unavailable) {
      return Ok();
    }
    return Err(sized.error());
  }
  const std::uint32_t cohort = cohort_counter_++;
  std::int64_t front_expiry = 0;
  bool have_front = false;
  for (const SizedLeg& sl : *sized) {
    const PricedSurface* surf = base.find(sl.leg.uid);
    if (surf == nullptr) {
      return Err(ErrorCode::NotFound, "DeclarativeStrategy: no surface for sized leg");
    }
    const Result<double> mark = surf->fair_value(sl.leg.K, sl.leg.T, sl.leg.side);
    if (!mark) {
      return Err(mark.error());
    }
    const std::int64_t expiry =
        base.ts_ns() + std::llround(sl.leg.T * kNsPerYear);
    Lot lot;
    lot.id = next_lot_id++;
    lot.contract = OptionContract{sl.leg.uid, sl.leg.K, sl.leg.T, sl.leg.side};
    lot.qty = sl.qty;
    lot.multiplier = sl.multiplier;
    lot.expiry_ts_ns = expiry;
    lot.cohort = cohort;
    lot.entry_price = *mark;  // fill at mid
    book.lots.push_back(lot);
    if (!have_front || expiry < front_expiry) {
      front_expiry = expiry;
      have_front = true;
    }
  }
  if (have_front) {
    front_expiry_ = front_expiry;  // earliest-expiring leg drives the roll
    have_front_ = true;
  }
  return Ok();
}

Status DeclarativeStrategy::on_step(const MarketSnapshot& base, std::size_t step_index,
                                    PortfolioState& book, std::uint64_t& next_lot_id) {
  if (spec_.lifecycle.holding == LifecycleSpec::Holding::CloseAtHorizon) {
    // Close pass FIRST, before any entry this step: erase every lot whose
    // residual maturity has fallen below roll_at_T. The engine's before/after
    // `book.lots` diff (src/backtest.cpp `execute`) books these as roll-closes
    // at today's marks — never settlement (the engine's own expiry settle runs
    // earlier in its loop, strictly before on_step, so a lot closed here never
    // reaches that path).
    const std::int64_t base_ts = base.ts_ns();
    const double roll_at_T = spec_.lifecycle.roll_at_T;
    std::erase_if(book.lots, [base_ts, roll_at_T](const Lot& lot) {
      const double residual_ns = static_cast<double>(lot.expiry_ts_ns - base_ts);
      return residual_ns < roll_at_T * kNsPerYear;
    });
  }

  const LifecycleDecision d = lifecycle_decide(spec_.lifecycle, step_index, book.lots.empty(),
                                               base.ts_ns(), front_expiry_, have_front_);
  if (!d.open) {
    return Ok();
  }
  if (d.clear) {
    book.lots.clear();  // RollAtHorizon: close the front cohort before reopening
    have_front_ = false;
  }
  return open_cohort(base, book, next_lot_id);
}

}  // namespace atx::vol
