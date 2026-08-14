// atx-vol strategy DSL interpreter (Phase B1) — see strategy.hpp for the model.
//
// Resolution: strike-from-delta (deterministic bisection on log-moneyness), the
// StrikeSelector -> K map, LegSpec -> ResolvedLeg expansion, per-leg sizing, and
// the cross-leg vega constraint. Plus the DeclarativeStrategy lifecycle (entry
// cadence, HoldToExpiry overlapping cohorts / RollAtHorizon single-cohort roll).

#include "atx/vol/api/backtest/strategy.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/pricing/american.hpp"         // AmericanGreeks
#include "atx/vol/api/pricing/pricing_executor.hpp" // WS-P P4: batched-basket strike resolve fan-out

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// ── Strike-from-delta solver ────────────────────────────────────────────────

namespace {

constexpr double kLegacyDeltaTolerance = 1.0e-4;
constexpr unsigned kMaxAdaptiveRefineIterations = 16u;
constexpr double kInt64ExclusiveUpper = 0x1p63;

struct CanonicalTenor {
  std::int64_t expiry_ts_ns{0};
  double T{0.0};
};

[[nodiscard]] double timestamp_delta_ns(std::int64_t lhs, std::int64_t rhs) noexcept {
  if (lhs >= rhs) {
    return static_cast<double>(static_cast<std::uint64_t>(lhs) - static_cast<std::uint64_t>(rhs));
  }
  return -static_cast<double>(static_cast<std::uint64_t>(rhs) - static_cast<std::uint64_t>(lhs));
}

// The synthetic expiry anchor for `requested_T`, optionally snapped onto the
// run's session grid (see TenorSpec::snap_to_sessions). `sessions` must be
// sorted ascending; it is read, never copied.
[[nodiscard]] Result<CanonicalTenor> canonicalize_tenor(std::int64_t valuation_ts_ns,
                                                        double requested_T, bool snap_to_sessions,
                                                        std::span<const std::int64_t> sessions) {
  if (!(std::isfinite(requested_T) && requested_T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "expand_leg: tenor T must be finite and positive");
  }
  const double tenor_ns = requested_T * kNsPerYear;
  if (!std::isfinite(tenor_ns)) {
    return Err(ErrorCode::InvalidArgument, "expand_leg: tenor expiry is out of range");
  }
  const double rounded_ns = std::round(tenor_ns);
  // The upper bound is exclusive because double(INT64_MAX) rounds to 2^63;
  // checking against INT64_MAX after conversion would itself risk UB.
  if (!(rounded_ns >= 1.0 && rounded_ns < kInt64ExclusiveUpper)) {
    return Err(ErrorCode::InvalidArgument, "expand_leg: tenor expiry is out of range");
  }
  const std::int64_t delta_ns = static_cast<std::int64_t>(rounded_ns);
  if (valuation_ts_ns > std::numeric_limits<std::int64_t>::max() - delta_ns) {
    return Err(ErrorCode::InvalidArgument, "expand_leg: tenor expiry overflows timestamp");
  }
  const std::int64_t expiry_ts_ns = valuation_ts_ns + delta_ns;
  const CanonicalTenor raw{expiry_ts_ns, static_cast<double>(delta_ns) / kNsPerYear};
  if (!snap_to_sessions) {
    return Ok(raw);
  }
  if (sessions.empty()) {
    return Err(ErrorCode::InvalidArgument, "expand_leg: snap_to_sessions requires session_ts");
  }
  if (expiry_ts_ns > sessions.back()) {
    // The cohort out-lives the corpus: there is no session to snap onto and none
    // is needed — it never reaches settlement inside the run and is
    // liquidation-marked at run end. Intentionally left on the raw anchor.
    return Ok(raw);
  }
  const auto after = std::upper_bound(sessions.begin(), sessions.end(), expiry_ts_ns);
  if (after == sessions.begin()) {
    return Err(ErrorCode::InvalidArgument, "expand_leg: snapped expiry is not after valuation");
  }
  const std::int64_t snapped_ts_ns = *(after - 1); // greatest session <= raw expiry
  if (snapped_ts_ns <= valuation_ts_ns) {
    return Err(ErrorCode::InvalidArgument, "expand_leg: snapped expiry is not after valuation");
  }
  // T is re-derived from the SNAPPED anchor so the contract key the engine books
  // and the greeks the leg is sized on describe the same expiry.
  return Ok(CanonicalTenor{snapped_ts_ns,
                           timestamp_delta_ns(snapped_ts_ns, valuation_ts_ns) / kNsPerYear});
}

[[nodiscard]] Result<double> resolve_strike_by_delta_routed(const SurfaceRef &s, double T,
                                                            Side side, double target_abs_delta,
                                                            QueryExecution execution,
                                                            double final_tolerance) {
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
  // asymptotic |delta| (-> 1 deep ITM, -> 0 deep OTM). The terminal trial must itself
  // be exact, so a sentinel-only "straddle" cannot pass as a hit.
  struct GVal {
    double value{0.0};
    bool exact{false};
  };
  const auto gof = [&](double k) -> GVal {
    const double K = F * std::exp(k);
    // The bisection consumes ONLY |delta|: the delta-only fast path (~1-2 boundary
    // solves) replaces the full-greeks reprice (7 unique boundaries / 17 stencils,
    // measured 7.07x) per candidate strike, at a bit-identical delta.
    const Result<double> gr = s.delta(K, T, side, execution);
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
  GVal terminal{};
  int retained = 0; // Illinois: +1 => hi retained last step, -1 => lo retained
  for (int it = 0; it < 128; ++it) {
    double x = 0.5 * (lo + hi);
    if (glo.exact && ghi.exact && glo.value != ghi.value) {
      const double xs = (glo.value * hi - ghi.value * lo) / (glo.value - ghi.value);
      const double margin = 1.0e-6 * (hi - lo);
      if (xs > lo + margin && xs < hi - margin) {
        x = xs; // interior secant guess
      }
    }
    kroot = x;
    const GVal gm = gof(x);
    terminal = gm;
    const double solve_tolerance = std::min(1.0e-7, 0.25 * final_tolerance);
    if (gm.exact && std::fabs(gm.value) <= solve_tolerance) {
      break;
    }
    if ((hi - lo) <= 1.0e-10) {
      break;
    }
    if ((gm.value <= 0.0) == (glo.value <= 0.0)) {
      lo = x;
      glo = gm;
      if (retained == -1 && ghi.exact) {
        ghi.value *= 0.5; // Illinois: hi retained twice -> down-weight it
      }
      retained = -1;
    } else {
      hi = x;
      ghi = gm;
      if (retained == +1 && glo.exact) {
        glo.value *= 0.5; // Illinois: lo retained twice -> down-weight it
      }
      retained = +1;
    }
  }

  // The terminal trial is already the exact delta evaluation at Kroot on every exit:
  // residual convergence, bracket-width exhaustion, or iteration-cap exhaustion.
  // Reuse it for validation; recomputing the identical expression costs one cold delta.
  const double Kroot = F * std::exp(kroot);
  if (!terminal.exact || !std::isfinite(terminal.value) ||
      std::fabs(terminal.value) > final_tolerance) {
    return Err(ErrorCode::InvalidArgument, "resolve_strike_by_delta: delta target unreachable");
  }
  return Ok(Kroot);
}

// ── StrikeSelector -> absolute K ────────────────────────────────────────────

[[nodiscard]] Status validate_resolution_options(const ResolutionOptions &options) {
  if (!(std::isfinite(options.cold_delta_tolerance) && options.cold_delta_tolerance > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "resolve_strike_by_delta: cold delta tolerance must be finite and positive");
  }
  if (!(std::isfinite(options.max_log_strike_step) && options.max_log_strike_step > 0.0 &&
        options.max_log_strike_step <= 1.0)) {
    return Err(ErrorCode::InvalidArgument,
               "resolve_strike_by_delta: max log-strike step must lie in (0,1]");
  }
  if (options.max_refine_iterations > kMaxAdaptiveRefineIterations) {
    return Err(ErrorCode::InvalidArgument,
               "resolve_strike_by_delta: max refine iterations exceeds 16");
  }
  return Ok();
}

struct DeltaPoint {
  double k{0.0};
  double residual{0.0};
};

[[nodiscard]] std::optional<DeltaPoint> delta_point(const SurfaceRef &surface, double forward,
                                                    double T, Side side, double target_abs_delta,
                                                    double k, QueryExecution execution) {
  const double K = forward * std::exp(k);
  if (!(std::isfinite(K) && K > 0.0)) {
    return std::nullopt;
  }
  const Result<double> delta = surface.delta(K, T, side, execution);
  if (!delta || !std::isfinite(*delta)) {
    return std::nullopt;
  }
  return DeltaPoint{k, std::fabs(*delta) - target_abs_delta};
}

[[nodiscard]] Result<double> resolve_strike_by_delta_adaptive(const SurfaceRef &s, double T,
                                                              Side side, double target_abs_delta,
                                                              const ResolutionOptions &options) {
  const Status valid_options = validate_resolution_options(options);
  if (!valid_options) {
    return Err(valid_options.error());
  }
  const double F = s.forward_at(T);
  if (!(F > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "resolve_strike_by_delta: no forward at tenor");
  }

  const auto cold_fallback = [&]() {
    return resolve_strike_by_delta_routed(
        s, T, side, target_abs_delta, QueryExecution::ColdReference, options.cold_delta_tolerance);
  };
  const QueryPricingTier tier = s.query_pricing_tier();
  if (tier != QueryPricingTier::RepresentativeFast && tier != QueryPricingTier::CarryBank) {
    return cold_fallback();
  }

  const Result<double> screen = resolve_strike_by_delta_routed(
      s, T, side, target_abs_delta, QueryExecution::Configured, kLegacyDeltaTolerance);
  if (!screen) {
    return cold_fallback();
  }
  const double screen_k = std::log(*screen / F);
  std::optional<DeltaPoint> current =
      delta_point(s, F, T, side, target_abs_delta, screen_k, QueryExecution::ColdReference);
  if (!current) {
    return cold_fallback();
  }
  if (std::fabs(current->residual) <= options.cold_delta_tolerance) {
    return Ok(*screen);
  }

  std::optional<DeltaPoint> previous;
  for (unsigned iteration = 0; iteration < options.max_refine_iterations; ++iteration) {
    double slope = 0.0;
    if (previous && current->k != previous->k) {
      slope = (current->residual - previous->residual) / (current->k - previous->k);
    }
    if (!(std::isfinite(slope) && std::fabs(slope) > 1.0e-12)) {
      const double h = std::min(1.0e-3, 0.25 * options.max_log_strike_step);
      const auto left =
          delta_point(s, F, T, side, target_abs_delta, current->k - h, QueryExecution::Configured);
      const auto right =
          delta_point(s, F, T, side, target_abs_delta, current->k + h, QueryExecution::Configured);
      if (!left || !right) {
        break;
      }
      slope = (right->residual - left->residual) / (2.0 * h);
    }
    if (!(std::isfinite(slope) && std::fabs(slope) > 1.0e-12)) {
      break;
    }
    const double raw_step = -current->residual / slope;
    const double step =
        std::clamp(raw_step, -options.max_log_strike_step, options.max_log_strike_step);
    if (!(std::isfinite(step) && std::fabs(step) > 1.0e-12)) {
      break;
    }
    const auto next = delta_point(s, F, T, side, target_abs_delta, current->k + step,
                                  QueryExecution::ColdReference);
    if (!next) {
      break;
    }
    if (std::fabs(next->residual) <= options.cold_delta_tolerance) {
      return Ok(F * std::exp(next->k));
    }
    previous = current;
    current = next;
  }
  return cold_fallback();
}

} // namespace

Result<double> resolve_strike_by_delta(const SurfaceRef &s, double T, Side side,
                                       double target_abs_delta) {
  return resolve_strike_by_delta_routed(s, T, side, target_abs_delta, QueryExecution::Configured,
                                        kLegacyDeltaTolerance);
}

Result<double> resolve_strike_by_delta(const SurfaceRef &s, double T, Side side,
                                       double target_abs_delta, const ResolutionOptions &options) {
  if (!options.fast_screen_cold_confirm) {
    return resolve_strike_by_delta(s, T, side, target_abs_delta);
  }
  return resolve_strike_by_delta_adaptive(s, T, side, target_abs_delta, options);
}

namespace {

[[nodiscard]] Status validate_model_tenor(const TenorSpec &tenor) {
  if (!tenor.snap_to_listed) {
    return Ok();
  }
  return Err(ErrorCode::NotImplemented,
             "TenorSpec::snap_to_listed is unavailable in the model-on-model declarative "
             "strategy; use the listed OPRA workflow in listed_opra.hpp (see "
             "spy_strangle_tradeable)");
}

} // namespace

Result<double> resolve_strike(const SurfaceRef &s, const TenorSpec &tenor, Side side,
                              const StrikeSelector &sel) {
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

Result<double> resolve_strike(const SurfaceRef &s, const TenorSpec &tenor, Side side,
                              const StrikeSelector &sel, const ResolutionOptions &options) {
  return resolve_strike(s, tenor, side, sel, options, PriceOptions{});
}

Result<double> resolve_strike(const SurfaceRef &s, const TenorSpec &tenor, Side side,
                              const StrikeSelector &sel, const ResolutionOptions &options,
                              const PriceOptions &price_options) {
  const Status tenor_status = validate_model_tenor(tenor);
  if (!tenor_status) {
    return Err(tenor_status.error());
  }
  if (sel.kind != StrikeSelector::Kind::Delta) {
    return resolve_strike(s, tenor, side, sel);
  }
  if (options.fast_screen_cold_confirm) {
    return resolve_strike_by_delta(s, tenor.target_T, side, sel.value, options);
  }
  return resolve_strike_by_delta_routed(s, tenor.target_T, side, sel.value,
                                        price_options.query_execution, kLegacyDeltaTolerance);
}

// ── LegSpec -> ResolvedLeg(s) ───────────────────────────────────────────────

namespace {

// The `expand_leg` body, plus the run's session grid for `snap_to_sessions` legs
// (`sessions` is borrowed from the enclosing StrategySpec — never copied, and
// empty for the spec-free public overloads).
[[nodiscard]] Result<std::vector<ResolvedLeg>>
expand_leg_impl(const MarketSnapshot &snap, const LegSpec &leg, const ResolutionOptions &options,
                const PriceOptions &price_options, std::span<const std::int64_t> sessions) {
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
  const SurfaceRef surf = snap.find(uid);
  if (surf == nullptr) {
    return Err(ErrorCode::NotFound, "expand_leg: no surface for leg's uid");
  }
  const Result<CanonicalTenor> canonical =
      canonicalize_tenor(snap.ts_ns(), leg.tenor.target_T, leg.tenor.snap_to_sessions, sessions);
  if (!canonical) {
    return Err(canonical.error());
  }
  const double T = canonical->T;
  TenorSpec canonical_tenor = leg.tenor;
  canonical_tenor.target_T = T;
  const QueryExecution sizing_execution = options.fast_screen_cold_confirm
                                              ? QueryExecution::ColdReference
                                              : price_options.query_execution;
  const auto sizing_seed = [&](double K, Side side) -> Result<FullGreekSeed> {
    return surf->full_greek_seed(K, T, side, price_options.analytic_greeks, sizing_execution);
  };

  // Resolve one (side, selector) into a ResolvedLeg with per-share vega + sigma.
  const auto make_one = [&](Side side, const StrikeSelector &sel) -> Result<ResolvedLeg> {
    const Result<double> K =
        resolve_strike(*surf, canonical_tenor, side, sel, options, price_options);
    if (!K) {
      return Err(K.error());
    }
    Result<FullGreekSeed> seed = sizing_seed(*K, side);
    if (!seed) {
      return Err(seed.error());
    }
    const AmericanGreeks &gr = seed->greeks();
    ResolvedLeg rl;
    rl.uid = uid;
    rl.K = *K;
    rl.T = T;
    rl.sigma = seed->iv();
    rl.model_price = gr.price;
    rl.vega = gr.vega; // signed greek vega (> 0 for both call and put)
    rl.theta = gr.theta;
    rl.gamma = gr.gamma;
    rl.side = side;
    rl.group = leg.group;
    rl.expiry_ts_ns = canonical->expiry_ts_ns;
    rl.full_greek_seed.emplace(std::move(*seed));
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
    const Result<double> K =
        resolve_strike(*surf, canonical_tenor, Side::Call, leg.strike, options, price_options);
    if (!K) {
      return Err(K.error());
    }
    for (const Side side : {Side::Call, Side::Put}) {
      Result<FullGreekSeed> seed = sizing_seed(*K, side);
      if (!seed) {
        return Err(seed.error());
      }
      const AmericanGreeks &gr = seed->greeks();
      ResolvedLeg rl;
      rl.uid = uid;
      rl.K = *K;
      rl.T = T;
      rl.sigma = seed->iv();
      rl.model_price = gr.price;
      rl.vega = gr.vega;
      rl.theta = gr.theta;
      rl.gamma = gr.gamma;
      rl.side = side;
      rl.group = leg.group;
      rl.expiry_ts_ns = canonical->expiry_ts_ns;
      rl.full_greek_seed.emplace(std::move(*seed));
      out.push_back(std::move(rl));
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

} // namespace

Result<std::vector<ResolvedLeg>> expand_leg(const MarketSnapshot &snap, const LegSpec &leg,
                                            const ResolutionOptions &options,
                                            const PriceOptions &price_options) {
  // No enclosing spec here, so no session grid: a leg that asks to snap is
  // rejected by canonicalize_tenor rather than silently left unsnapped.
  return expand_leg_impl(snap, leg, options, price_options, {});
}

Result<std::vector<ResolvedLeg>> expand_leg(const MarketSnapshot &snap, const LegSpec &leg,
                                            const ResolutionOptions &options) {
  return expand_leg(snap, leg, options, PriceOptions{});
}

Result<std::vector<ResolvedLeg>> expand_leg(const MarketSnapshot &snap, const LegSpec &leg) {
  return expand_leg(snap, leg, ResolutionOptions{});
}

// ── Sizing + cross-leg constraint ───────────────────────────────────────────

namespace {

// Gross position vega Σ|qty*vega*mult| over the sized legs tagged `group`.
[[nodiscard]] double group_gross_vega(const std::vector<SizedLeg> &sized,
                                      const std::string &group) {
  double g = 0.0;
  for (const SizedLeg &sl : sized) {
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
[[nodiscard]] Result<std::vector<SizedLeg>>
expand_and_size_leg(const MarketSnapshot &snap, const LegSpec &ls, const ResolutionOptions &options,
                    const PriceOptions &price_options, std::span<const std::int64_t> sessions) {
  constexpr double kMult = 100.0;

  Result<std::vector<ResolvedLeg>> exp = expand_leg_impl(snap, ls, options, price_options, sessions);
  if (!exp) {
    return Err(exp.error());
  }
  const std::vector<ResolvedLeg> &opts = *exp;

  // Per-leg base sizing: one signed qty applied to every option of the structure.
  double qty = 0.0;
  switch (ls.size.kind) {
  case SizeSpec::Kind::FixedContracts:
  case SizeSpec::Kind::Weight:
    qty = ls.size.sign * ls.size.value; // Weight: unitless, pre-constraint
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
    constexpr double kCalendarDaysPerYear = 365.25; // matches kNsPerYear
    const auto pick = [&](const ResolvedLeg &o) -> double {
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
    for (const ResolvedLeg &o : opts) {
      structure_greek += pick(o);
    }
    if (!(std::isfinite(structure_greek) && std::fabs(structure_greek) > 0.0)) {
      return Err(ErrorCode::Unavailable,
                 "resolve_spec: degenerate structure greek for target sizing");
    }
    const double target = (ls.size.kind == SizeSpec::Kind::TargetTheta)
                              ? ls.size.value * kCalendarDaysPerYear // $/day -> $/yr
                              : ls.size.value;
    qty = ls.size.sign * target / (std::fabs(structure_greek) * kMult);
    break;
  }
  }
  std::vector<SizedLeg> out;
  out.reserve(opts.size());
  for (const ResolvedLeg &o : opts) {
    out.push_back(SizedLeg{o, qty, kMult});
  }
  return Ok(std::move(out));
}

// Shared resolution body for `resolve_spec` / `resolve_spec_with_policy`. Under
// `missing.policy == Error` this reproduces `resolve_spec`'s pre-S1-3/T2 inline
// behavior exactly (first leg failure propagates unchanged; no drop bookkeeping,
// no min_names floor). Under `DropRenormalize`, see `resolve_spec_with_policy`'s
// doc comment (strategy.hpp) for the full contract.
[[nodiscard]] Result<std::vector<SizedLeg>> resolve_spec_impl(const MarketSnapshot &snap,
                                                              const StrategySpec &spec,
                                                              const MissingNameSpec &missing,
                                                              std::vector<ResolveDrop> *dropped,
                                                              const PriceOptions &price_options) {
  if (dropped != nullptr) {
    dropped->clear();
  }
  if (spec.resolution.fast_screen_cold_confirm) {
    const Status resolution_status = validate_resolution_options(spec.resolution);
    if (!resolution_status) {
      return Err(resolution_status.error());
    }
  }
  // This is a configuration/capability error, never missing market data. Reject
  // the whole spec before DropRenormalize can turn an explicitly requested
  // listed contract into a silent model-contract substitution or name drop.
  bool any_snaps_to_sessions = false;
  for (const LegSpec &leg : spec.legs) {
    const Status tenor_status = validate_model_tenor(leg.tenor);
    if (!tenor_status) {
      return Err(tenor_status.error());
    }
    any_snaps_to_sessions = any_snaps_to_sessions || leg.tenor.snap_to_sessions;
  }
  // The session grid is consumed by a binary search, which on an unsorted range
  // still RETURNS an anchor — a wrong expiry, a wrong T and therefore a wrong
  // strike, reported as success. That is the only silent-wrong-answer path in a
  // feature whose whole point is fail-closed calendar handling, so an out-of-order
  // grid is rejected here, in the same configuration pre-pass and with the same
  // never-droppable status as `snap_to_listed`: `InvalidArgument` is outside
  // DropRenormalize's {NotFound, Unavailable} set, so a misordered calendar can
  // never be laundered into a name drop. Checked only when some leg actually reads
  // the grid (the flag is gathered by the loop above at zero extra cost), so a
  // spec that carries `session_ts` it never consumes pays nothing.
  if (any_snaps_to_sessions && !std::is_sorted(spec.session_ts.begin(), spec.session_ts.end())) {
    return Err(ErrorCode::InvalidArgument,
               "resolve_spec: session_ts must be sorted ascending for snap_to_sessions legs");
  }
  const bool drop_policy = missing.policy == MissingNamePolicy::DropRenormalize;

  const CrossLegConstraint &c = spec.constraint;
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

  // L3 (AL-solve-wall sprint, fewer-solves entry wall): fan the per-leg entry
  // resolution across the shared pricing_executor pool. Each leg's work — the
  // delta-strike Illinois iterations + the full_greek_seed American solve, the
  // entry-day wall of finding 3 (~0.75-4.5 s single-threaded on the 51-name config)
  // — is POOL-FREE SCALAR work (resolve_strike_by_delta_routed and
  // full_greek_seed->evaluate never dispatch to the pool), so fanning at LEG
  // granularity nests no pool dispatch and batches BOTH the strike solve AND the
  // per-leg seed in one pass at name granularity (the parallelism that matters for
  // a per-name universe). expand_and_size_leg is a pure function of (const snap, ls,
  // options) with no shared mutable state, and run_blocks writes each slot exactly
  // once (disjoint per-leg writes), so `per_leg` — and therefore the in-order
  // assembly below — is BIT-IDENTICAL to the old serial resolve for any worker
  // count (same drop/constraint bookkeeping, same error propagation order).
  std::vector<Result<std::vector<SizedLeg>>> per_leg;
  per_leg.reserve(spec.legs.size());
  for (std::size_t i = 0; i < spec.legs.size(); ++i) {
    per_leg.emplace_back(Err(ErrorCode::Internal, "resolve_spec: leg not resolved"));
  }
  if (!spec.legs.empty()) {
    // Borrowed by every lane: the grid is read-only during the fan-out, so no
    // per-leg copy of the run's session timestamps is made.
    const std::span<const std::int64_t> sessions{spec.session_ts};
    pricing_executor().run_blocks(
        spec.legs.size(), price_options.n_threads, [&](std::size_t i) {
          per_leg[i] =
              expand_and_size_leg(snap, spec.legs[i], spec.resolution, price_options, sessions);
        });
  }

  std::vector<SizedLeg> sized;
  std::size_t group_a_survivors = 0;
  for (std::size_t leg_ix = 0; leg_ix < spec.legs.size(); ++leg_ix) {
    const LegSpec &ls = spec.legs[leg_ix];
    Result<std::vector<SizedLeg>> &leg_sized = per_leg[leg_ix];
    if (!leg_sized) {
      const ErrorCode code = leg_sized.error().code();
      // SP100 task 2: an opted-in ENTRY SKIP when the scaled hedge group's symbol
      // (the dispersion index) is simply not in this snapshot. Checked BEFORE the
      // policy branch below because it is independent of `missing.policy`: it is
      // about the index leg, not about dropping basket names. Keyed strictly on
      // NotFound — "no such symbol here" — so a degenerate fit (Unavailable) or a
      // configuration error on the same leg still propagates verbatim.
      //
      // The skip is expressed as an EMPTY sized book, the channel
      // `DeclarativeStrategy::prepare_cohort` already reads as "open nothing this
      // step", with the reason recorded in `dropped` so it is never silent.
      if (spec.skip_entry_on_missing_index && has_constraint && ls.group == gb &&
          code == ErrorCode::NotFound) {
        if (dropped != nullptr) {
          dropped->push_back(ResolveDrop{ls.symbol, "index leg absent from this snapshot; entry "
                                                    "skipped: " +
                                                        leg_sized.error().message()});
        }
        return Ok(std::vector<SizedLeg>{});
      }
      const bool droppable_market_failure =
          code == ErrorCode::NotFound || code == ErrorCode::Unavailable;
      if (!drop_policy || !droppable_market_failure) {
        return Err(leg_sized.error()); // hard/configuration failure: never a name drop
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
    for (SizedLeg &sl : *leg_sized) {
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
      return Err(ErrorCode::Unavailable,
                 "resolve_spec: constraint target group has zero gross vega");
    }
    const double scale = gross_a / gross_b; // one factor => intra-group ratios preserved
    for (SizedLeg &sl : sized) {
      if (sl.leg.group == gb) {
        sl.qty *= scale;
      }
    }
  }

  return Ok(std::move(sized));
}

} // namespace

Result<std::vector<SizedLeg>> resolve_spec(const MarketSnapshot &snap, const StrategySpec &spec) {
  return resolve_spec(snap, spec, PriceOptions{});
}

Result<std::vector<SizedLeg>> resolve_spec(const MarketSnapshot &snap, const StrategySpec &spec,
                                           const PriceOptions &price_options) {
  return resolve_spec_impl(snap, spec, MissingNameSpec{MissingNamePolicy::Error, 2}, nullptr,
                           price_options);
}

Result<std::vector<SizedLeg>> resolve_spec_with_policy(const MarketSnapshot &snap,
                                                       const StrategySpec &spec,
                                                       std::vector<ResolveDrop> *dropped) {
  return resolve_spec_with_policy(snap, spec, PriceOptions{}, dropped);
}

Result<std::vector<SizedLeg>> resolve_spec_with_policy(const MarketSnapshot &snap,
                                                       const StrategySpec &spec,
                                                       const PriceOptions &price_options,
                                                       std::vector<ResolveDrop> *dropped) {
  return resolve_spec_impl(snap, spec, spec.missing, dropped, price_options);
}

// ── Lifecycle decision ──────────────────────────────────────────────────────

LifecycleDecision lifecycle_decide(const LifecycleSpec &lifecycle, std::size_t step_index,
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
    return LifecycleDecision{tick, false}; // overlapping cohorts: never clear
  }

  // RollAtHorizon: a single cohort, rolled at the horizon.
  bool need = book_empty;
  if (!need && have_front) {
    const double residual_T = timestamp_delta_ns(front_expiry, base_ts) / kNsPerYear;
    if (residual_T < lifecycle.roll_at_T) {
      need = true;
    }
  }
  return LifecycleDecision{need, need && !book_empty};
}

std::int64_t select_fixed_cycle_expiry(std::span<const std::int64_t> sessions,
                                       std::int64_t base_ts, std::int64_t tenor_ns) noexcept {
  if (sessions.empty()) {
    return 0;
  }
  const std::int64_t anchor = (base_ts > std::numeric_limits<std::int64_t>::max() - tenor_ns)
                                  ? std::numeric_limits<std::int64_t>::max()
                                  : base_ts + tenor_ns;
  const auto at_or_after = std::lower_bound(sessions.begin(), sessions.end(), anchor);
  // Past the end of the grid there is no session to hold the full tenor. Take
  // the LAST one instead: a short final cycle that the run still observes
  // settling beats an expiry the corpus never reaches.
  const std::int64_t expiry = (at_or_after != sessions.end()) ? *at_or_after : sessions.back();
  return expiry > base_ts ? expiry : 0;
}

// ── DeclarativeStrategy ─────────────────────────────────────────────────────

namespace {

// FixedExpiryRestrike / swap-lane configuration validation, run once on the
// first on_step (the Status channel a constructor does not have). Everything
// here is a CONFIGURATION error — fatal, `InvalidArgument`/`NotImplemented` —
// as opposed to the soft, counted, data-driven failures the restrike step
// handles. Returns the legs' common tenor in ns (the cycle tenor).
[[nodiscard]] Result<std::int64_t> validate_restrike_spec(const StrategySpec &spec) {
  if (!spec.swap_legs.empty() &&
      spec.lifecycle.holding != LifecycleSpec::Holding::FixedExpiryRestrike) {
    return Err(ErrorCode::NotImplemented,
               "DeclarativeStrategy: swap_legs require the FixedExpiryRestrike lifecycle — a "
               "swap lot cannot be erased, so no other holding mode can carry one");
  }
  if (spec.legs.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "DeclarativeStrategy: FixedExpiryRestrike requires at least one option leg");
  }
  if (spec.session_ts.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "DeclarativeStrategy: FixedExpiryRestrike requires session_ts to fix a cycle "
               "expiry");
  }
  // A binary search over an unsorted grid still RETURNS an anchor — a wrong
  // expiry reported as success — so an out-of-order calendar fails closed here.
  if (!std::is_sorted(spec.session_ts.begin(), spec.session_ts.end())) {
    return Err(ErrorCode::InvalidArgument,
               "DeclarativeStrategy: session_ts must be sorted ascending");
  }
  if (spec.constraint.kind != CrossLegConstraint::Kind::None) {
    return Err(ErrorCode::InvalidArgument,
               "DeclarativeStrategy: cross-leg constraints are not part of the restrike mode; "
               "size each leg directly");
  }
  if (spec.missing.policy != MissingNamePolicy::Error) {
    return Err(ErrorCode::InvalidArgument,
               "DeclarativeStrategy: the restrike mode has its own keep-strikes policy; "
               "missing.policy must stay Error");
  }

  const double cycle_T = spec.legs.front().tenor.target_T;
  for (std::size_t i = 0; i < spec.legs.size(); ++i) {
    const LegSpec &leg = spec.legs[i];
    if (!(std::isfinite(leg.tenor.target_T) && leg.tenor.target_T > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "DeclarativeStrategy: restrike leg " + std::to_string(i) +
                     " needs a finite positive tenor.target_T");
    }
    if (leg.tenor.target_T != cycle_T) {
      return Err(ErrorCode::InvalidArgument,
                 "DeclarativeStrategy: restrike legs must share ONE tenor (the cycle's); leg " +
                     std::to_string(i) + " differs from leg 0");
    }
    if (leg.tenor.snap_to_listed || leg.tenor.snap_to_sessions) {
      return Err(ErrorCode::InvalidArgument,
                 "DeclarativeStrategy: the FixedExpiryRestrike lifecycle owns expiry snapping; "
                 "leg " + std::to_string(i) + " must not set a snap flag");
    }
    if (leg.size.kind == SizeSpec::Kind::Weight) {
      return Err(ErrorCode::InvalidArgument,
                 "DeclarativeStrategy: Weight sizing needs a cross-leg constraint, which the "
                 "restrike mode does not carry; size leg " + std::to_string(i) + " directly");
    }
    if (leg.structure.kind == StructureSpec::Kind::RiskReversal) {
      // Rejected HERE, not left to the resolver: the step treats resolution
      // failures as soft keep-strikes data gaps, and a config error must never
      // hide in a counter.
      return Err(ErrorCode::InvalidArgument,
                 "DeclarativeStrategy: RiskReversal is not in the restrike mode's scope (leg " +
                     std::to_string(i) + ")");
    }
    // Selector values are validated up front for the same reason: after this,
    // a strike-resolution failure at step time can only be data-driven.
    const auto valid_selector = [](const StrikeSelector &sel) noexcept {
      if (!std::isfinite(sel.value)) {
        return false;
      }
      return sel.kind != StrikeSelector::Kind::Delta || (sel.value > 0.0 && sel.value < 1.0);
    };
    const bool strangle = leg.structure.kind == StructureSpec::Kind::Strangle;
    const bool selectors_ok = strangle ? valid_selector(leg.structure.call_leg) &&
                                             valid_selector(leg.structure.put_leg)
                                       : valid_selector(leg.strike);
    if (!selectors_ok) {
      return Err(ErrorCode::InvalidArgument,
                 "DeclarativeStrategy: restrike leg " + std::to_string(i) +
                     " has a non-finite or out-of-range strike selector");
    }
  }
  const double tenor_ns = std::round(cycle_T * kNsPerYear);
  if (!(tenor_ns >= 1.0 && tenor_ns < kInt64ExclusiveUpper)) {
    return Err(ErrorCode::InvalidArgument,
               "DeclarativeStrategy: the cycle tenor is out of range");
  }

  for (std::size_t i = 0; i < spec.swap_legs.size(); ++i) {
    const SwapLegSpec &leg = spec.swap_legs[i];
    const std::string tag = "DeclarativeStrategy: swap leg " + std::to_string(i);
    if (leg.symbol.empty() && leg.uid == 0) {
      return Err(ErrorCode::InvalidArgument, tag + " needs a symbol or a nonzero uid");
    }
    if (!(std::isfinite(leg.notional) && leg.notional > 0.0)) {
      return Err(ErrorCode::InvalidArgument, tag + " needs a finite positive notional");
    }
    if (!(std::isfinite(leg.annualization) && leg.annualization > 0.0)) {
      return Err(ErrorCode::InvalidArgument, tag + " needs a finite positive annualization");
    }
    const bool capped =
        leg.kind == DerivKind::CappedVarSwap || leg.kind == DerivKind::CappedVolSwap;
    if (capped && !(std::isfinite(leg.cap_dec) && leg.cap_dec > 0.0)) {
      return Err(ErrorCode::InvalidArgument, tag + " is a capped kind and needs cap_dec > 0");
    }
    if (!capped && leg.cap_dec != 0.0) {
      return Err(ErrorCode::InvalidArgument, tag + " is uncapped and must keep cap_dec == 0");
    }
    switch (leg.size.kind) {
    case SwapSizeSpec::Kind::FixedQty:
      if (!(std::isfinite(leg.size.value) && leg.size.value != 0.0)) {
        return Err(ErrorCode::InvalidArgument, tag + " FixedQty needs a finite nonzero value");
      }
      break;
    case SwapSizeSpec::Kind::TargetVega:
      if (!(std::isfinite(leg.size.value) && leg.size.value > 0.0)) {
        return Err(ErrorCode::InvalidArgument, tag + " TargetVega needs a finite positive value");
      }
      if (!(std::isfinite(leg.size.sign) && leg.size.sign != 0.0)) {
        return Err(ErrorCode::InvalidArgument, tag + " TargetVega needs a finite nonzero sign");
      }
      break;
    case SwapSizeSpec::Kind::MatchGroupVega:
      if (!leg.size.group.empty()) {
        const bool known = std::any_of(spec.legs.begin(), spec.legs.end(),
                                       [&leg](const LegSpec &option_leg) {
                                         return option_leg.group == leg.size.group;
                                       });
        if (!known) {
          return Err(ErrorCode::InvalidArgument,
                     tag + " MatchGroupVega names group '" + leg.size.group +
                         "', which no option leg carries");
        }
      }
      break;
    }
  }
  return Ok(static_cast<std::int64_t>(tenor_ns));
}

} // namespace

Result<std::optional<DeclarativeStrategy::PendingCohort>>
DeclarativeStrategy::prepare_cohort(const MarketSnapshot &base, std::uint64_t first_lot_id,
                                    const PriceOptions &price_options) {
  Result<std::vector<SizedLeg>> sized =
      resolve_spec_with_policy(base, spec_, price_options, &last_dropped_);
  if (!sized) {
    // NO-TRADE CONTRACT (mirrors DispersionStrategy, dispersion_strategy.cpp):
    // under DropRenormalize, Unavailable means the entry can't be built today
    // (too few surviving names, or the hedge leg is missing) — a flat / no-trade
    // step. Leave the book untouched and continue; any other error is fatal.
    if (spec_.missing.policy == MissingNamePolicy::DropRenormalize &&
        sized.error().code() == ErrorCode::Unavailable) {
      return Ok(std::optional<PendingCohort>{});
    }
    return Err(sized.error());
  }
  if (sized->empty()) {
    return Ok(std::optional<PendingCohort>{});
  }
  for (const SizedLeg &sl : *sized) {
    if (!(std::isfinite(sl.leg.model_price) && sl.leg.model_price >= 0.0)) {
      return Err(ErrorCode::Unavailable, "DeclarativeStrategy: sized leg has no model mark");
    }
    if (sl.leg.expiry_ts_ns <= base.ts_ns()) {
      return Err(ErrorCode::Unavailable, "DeclarativeStrategy: sized leg has no future expiry");
    }
    if (!sl.leg.full_greek_seed.has_value()) {
      return Err(ErrorCode::Unavailable, "DeclarativeStrategy: sized leg has no full-risk seed");
    }
  }

  PendingCohort pending;
  pending.lots.reserve(sized->size());
  pending.seeds.reserve(sized->size());
  std::uint64_t lot_id = first_lot_id;
  bool have_front = false;
  for (SizedLeg &sl : *sized) {
    const std::int64_t expiry = sl.leg.expiry_ts_ns;
    Lot lot;
    lot.id = lot_id++;
    lot.contract = OptionContract{sl.leg.uid, sl.leg.K, sl.leg.T, sl.leg.side};
    lot.qty = sl.qty;
    lot.multiplier = sl.multiplier;
    lot.expiry_ts_ns = expiry;
    lot.cohort = cohort_counter_;
    // The final sizing-Greeks query already produced this exact mark. Reuse it
    // instead of paying for a second American solve per entry leg.
    lot.entry_price = sl.leg.model_price; // fill at model mid
    pending.lots.push_back(lot);
    pending.seeds.push_back(std::move(*sl.leg.full_greek_seed));
    if (!have_front || expiry < pending.front_expiry) {
      pending.front_expiry = expiry;
      have_front = true;
    }
  }
  return Ok(std::optional<PendingCohort>{std::move(pending)});
}

Status DeclarativeStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                    PortfolioState &book, std::uint64_t &next_lot_id) {
  return on_step(base, step_index, book, next_lot_id, PriceOptions{});
}

Status DeclarativeStrategy::step_restrike(const MarketSnapshot &base, std::size_t step_index,
                                          PortfolioState &book, std::uint64_t &next_lot_id,
                                          const PriceOptions &price_options) {
  constexpr double kRestrikeNaN = std::numeric_limits<double>::quiet_NaN();
  last_entry_seeds_.clear();
  // Nothing measured YET this step. Every path that commits a fresh book
  // overwrites this; every path that does not leaves the step reporting "not
  // measured" rather than the previous step's vega.
  last_options_vega_ = kRestrikeNaN;
  last_step_ts_ns_ = base.ts_ns();

  const std::int64_t base_ts = base.ts_ns();
  bool cycle_opened = false;
  if (cycle_expiry_ts_ns_ <= base_ts) {
    // Inception, or the engine settled the cycle before calling us (it erases
    // expired lots strictly before on_step). Fix the next cycle's expiry.
    cycle_expiry_ts_ns_ = select_fixed_cycle_expiry(spec_.session_ts, base_ts, cycle_tenor_ns_);
    if (cycle_expiry_ts_ns_ <= base_ts) {
      return Ok(); // the session grid is exhausted: no cycle left to open
    }
    ++cohort_counter_; // `Lot::cohort` counts CYCLES, not the clips inside one
    cycle_opened = true;
  }

  // Restrike cadence: an off-tick step holds the book — that is the cadence
  // speaking, not a failure, so no counter moves. A cycle-open step always
  // opens regardless of the tick (its expiry came off the calendar).
  if (!cycle_opened && spec_.lifecycle.entry == LifecycleSpec::Entry::EveryNDays) {
    const unsigned n = (spec_.lifecycle.entry_every_n == 0) ? 1u : spec_.lifecycle.entry_every_n;
    if (step_index % n != 0) {
      return Ok();
    }
  }

  // Resolve every option leg PINNED to the cycle expiry: the tenor handed to
  // the resolver is re-derived from the exact anchor, and the committed lots
  // carry the exact int64 expiry (settlement is an exact-match, and a
  // re-rounded anchor a few ns off would never settle).
  const double cycle_T =
      static_cast<double>(static_cast<std::uint64_t>(cycle_expiry_ts_ns_) -
                          static_cast<std::uint64_t>(base_ts)) /
      kNsPerYear;
  std::vector<SizedLeg> fresh;
  bool resolved = true;
  for (const LegSpec &ls : spec_.legs) {
    LegSpec pinned = ls;
    pinned.tenor = TenorSpec{cycle_T, false, false};
    Result<std::vector<SizedLeg>> sized =
        expand_and_size_leg(base, pinned, spec_.resolution, price_options, {});
    if (!sized) {
      // KEEP THE LIVE STRIKES. Selector/config errors were validated up front,
      // so a failure here is data-driven (a dark board, an unreachable delta,
      // an unpriceable wing) — and the alternative to keeping is either
      // flattening real exposure for a data gap or booking a strike the
      // surface never priced. Which counter this is depends on whether there
      // is anything TO keep (see below).
      resolved = false;
      break;
    }
    for (SizedLeg &sl : *sized) {
      if (!(std::isfinite(sl.leg.model_price) && sl.leg.model_price >= 0.0) ||
          !sl.leg.full_greek_seed.has_value()) {
        resolved = false; // a lot the engine boundary would reject; never a 0.0 fill
        break;
      }
      fresh.push_back(std::move(sl));
    }
    if (!resolved) {
      break;
    }
  }
  if (!resolved) {
    // `book.lots` holds the survivors the engine did not settle, so an empty
    // book here means inception or a cycle roll: no strikes were held, and
    // saying otherwise would put a fictitious hold on the record.
    if (book.lots.empty()) {
      ++unopened_entry_steps_;
    } else {
      ++skipped_restrikes_;
    }
    if (cycle_opened && !spec_.swap_legs.empty()) {
      // The cycle is fixed regardless (its expiry comes off the calendar, not
      // the surface) but there is no entry vega to size against, and a swap
      // leg is a per-CYCLE instrument — a later session getting its board back
      // does not retro-open it. This cycle runs one-legged, and says so.
      skipped_swap_cycles_ += spec_.swap_legs.size();
    }
    return Ok();
  }

  // Close-and-reopen: the engine diffs the pre-step book against the post-step
  // book and books the departed lots as a roll-close at today's marks (the
  // expiry settle path ran earlier in its loop, so nothing here can reach it).
  book.lots.clear();
  book.lots.reserve(fresh.size());
  std::vector<FullGreekSeed> seeds;
  seeds.reserve(fresh.size());
  double options_vega = 0.0;
  // Per-group entry dollar vega, for MatchGroupVega swap sizing. A tiny linear
  // map: specs carry a handful of groups at most.
  std::vector<std::pair<std::string, double>> group_vega;
  for (SizedLeg &sl : fresh) {
    Lot lot;
    lot.id = next_lot_id++;
    lot.contract = OptionContract{sl.leg.uid, sl.leg.K, sl.leg.T, sl.leg.side};
    lot.qty = sl.qty;
    lot.multiplier = sl.multiplier;
    lot.expiry_ts_ns = cycle_expiry_ts_ns_; // the cycle's exact expiry, always
    lot.cohort = cohort_counter_;
    lot.entry_price = sl.leg.model_price; // fill at the model mid, priced once
    book.lots.push_back(lot);
    seeds.push_back(std::move(*sl.leg.full_greek_seed));
    // Per-share American vega x qty x multiplier — the very scaling the
    // portfolio pricer applies, and the number a MatchGroupVega swap leg is
    // sized against. ONE definition, or the equal-vega claim is unfalsifiable.
    const double leg_vega = sl.leg.vega * sl.qty * sl.multiplier;
    options_vega += leg_vega;
    const auto found =
        std::find_if(group_vega.begin(), group_vega.end(),
                     [&sl](const auto &entry) { return entry.first == sl.leg.group; });
    if (found == group_vega.end()) {
      group_vega.emplace_back(sl.leg.group, leg_vega);
    } else {
      found->second += leg_vega;
    }
  }
  last_options_vega_ = options_vega;
  last_entry_seeds_ = std::move(seeds);

  // The swap lane opens ONCE, on the step that FIXES the cycle — never on a
  // restrike, because the engine's swap-lot lane is append-only and held to
  // expiry (there is no unwind price for an OTC swap here), so a per-step
  // reopen would pile up one stale leg per session instead of expressing
  // today's view. Every refusal is one skipped cycle, never a fabricated leg.
  if (cycle_opened && !spec_.swap_legs.empty()) {
    for (const SwapLegSpec &leg : spec_.swap_legs) {
      const std::uint32_t uid = [&]() -> std::uint32_t {
        if (leg.uid != 0) {
          return leg.uid;
        }
        const std::optional<std::uint32_t> resolved = base.uid_of(leg.symbol);
        return resolved.value_or(0);
      }();
      const SurfaceRef surface = (uid != 0) ? base.find(uid) : nullptr;
      if (surface == nullptr) {
        ++skipped_swap_cycles_; // the leg's own board is dark this session
        continue;
      }

      double target_vega = 0.0;
      switch (leg.size.kind) {
      case SwapSizeSpec::Kind::MatchGroupVega: {
        if (leg.size.group.empty()) {
          target_vega = options_vega;
        } else {
          const auto found = std::find_if(
              group_vega.begin(), group_vega.end(),
              [&leg](const auto &entry) { return entry.first == leg.size.group; });
          // The group exists (validated); its vega can still be degenerate,
          // which the solve refuses below.
          target_vega = (found != group_vega.end()) ? found->second
                                                    : std::numeric_limits<double>::quiet_NaN();
        }
        break;
      }
      case SwapSizeSpec::Kind::TargetVega:
        target_vega = leg.size.sign * leg.size.value;
        break;
      case SwapSizeSpec::Kind::FixedQty:
        // The solve's vega contract is irrelevant here — the fair strike and
        // every schedule/finiteness check are qty-independent — so it runs
        // with a unit target and the requested qty is stamped on afterwards.
        target_vega = 1.0;
        break;
      }

      CycleSwapRequest req;
      req.uid = uid;
      req.kind = leg.kind;
      req.cap_dec = leg.cap_dec;
      req.notional = leg.notional;
      req.annualization = leg.annualization;
      req.open_ts_ns = base_ts;
      req.expiry_ts_ns = cycle_expiry_ts_ns_;
      req.session_ts = spec_.session_ts;
      req.deriv_cfg = leg.deriv_cfg;

      Result<SwapLot> lot = solve_cycle_swap(surface, req, target_vega);
      if (!lot) {
        ++skipped_swap_cycles_;
        continue;
      }
      if (leg.size.kind == SwapSizeSpec::Kind::FixedQty) {
        lot->qty = leg.size.value;
      }
      lot->id = next_lot_id++; // the watermark moves only once the lot is real
      book.swap_lots.push_back(*lot);
    }
  }
  return Ok();
}

std::vector<std::pair<std::string, double>>
DeclarativeStrategy::signals(const MarketSnapshot &base) const {
  if (spec_.swap_legs.empty()) {
    return {}; // no swap lane, no columns: existing specs keep their default
  }
  // The probe enforces the as-of discipline for its five columns itself;
  // `options_vega` is guarded by the same condition — the cached vega is as-of
  // ONE snapshot, and handed any other this row reports "not measured".
  const bool as_of = probe_.stepped() && base.ts_ns() == last_step_ts_ns_;
  std::vector<std::pair<std::string, double>> out;
  out.reserve(8);
  probe_.append_swap_greek_signals(base, out);
  out.emplace_back("options_vega",
                   as_of ? last_options_vega_ : std::numeric_limits<double>::quiet_NaN());
  out.emplace_back("skipped_restrikes", static_cast<double>(skipped_restrikes_));
  out.emplace_back("skipped_swaps", static_cast<double>(skipped_swap_cycles_));
  return out;
}

Status DeclarativeStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                    PortfolioState &book, std::uint64_t &next_lot_id,
                                    const PriceOptions &price_options) {
  // The restrike mode (and any spec carrying swap legs, which requires it) has
  // its own per-step decision and never reaches lifecycle_decide below.
  if (spec_.lifecycle.holding == LifecycleSpec::Holding::FixedExpiryRestrike ||
      !spec_.swap_legs.empty()) {
    if (!restrike_validated_) {
      const Result<std::int64_t> tenor = validate_restrike_spec(spec_);
      if (!tenor) {
        return Err(tenor.error());
      }
      cycle_tenor_ns_ = *tenor;
      restrike_validated_ = true;
    }
    if (spec_.swap_legs.empty()) {
      return step_restrike(base, step_index, book, next_lot_id, price_options);
    }
    // The probe brackets the step: ids captured BEFORE the strategy touches
    // the book, mirrors refreshed only on SUCCESS (an errored step aborts the
    // run, and refreshing off a half-built book would be state nobody can use).
    probe_.capture_pre_step(book);
    const Status stepped = step_restrike(base, step_index, book, next_lot_id, price_options);
    if (!stepped) {
      return stepped;
    }
    probe_.refresh(base, book);
    return Ok();
  }

  last_entry_seeds_.clear();
  const bool close_at_horizon = spec_.lifecycle.holding == LifecycleSpec::Holding::CloseAtHorizon;
  const std::int64_t base_ts = base.ts_ns();
  const double roll_at_T = spec_.lifecycle.roll_at_T;
  const auto closes_at_horizon = [base_ts, roll_at_T](const Lot &lot) noexcept {
    return timestamp_delta_ns(lot.expiry_ts_ns, base_ts) < roll_at_T * kNsPerYear;
  };
  bool effective_book_empty = book.lots.empty();
  if (close_at_horizon) {
    // Identify every horizon close without mutating the live book. If this step
    // also enters, the closes commit only after the new cohort is fully prepared;
    // if the entry side finds nothing to open they still commit (see the no-trade
    // branch below) — the close is an unconditional risk rule, not a leg of the
    // entry. The engine's before/after
    // `book.lots` diff (src/backtest.cpp `execute`) books these as roll-closes
    // at today's marks — never settlement (the engine's own expiry settle runs
    // earlier in its loop, strictly before on_step, so a lot closed here never
    // reaches that path).
    effective_book_empty =
        std::none_of(book.lots.begin(), book.lots.end(),
                     [&closes_at_horizon](const Lot &lot) { return !closes_at_horizon(lot); });
  }

  const LifecycleDecision d = lifecycle_decide(spec_.lifecycle, step_index, effective_book_empty,
                                               base.ts_ns(), front_expiry_, have_front_);
  if (!d.open) {
    if (close_at_horizon) {
      std::erase_if(book.lots, closes_at_horizon);
    }
    return Ok();
  }
  Result<std::optional<PendingCohort>> prepared = prepare_cohort(base, next_lot_id, price_options);
  if (!prepared) {
    // FATAL entry error: leave the book exactly as the step found it. A non-Ok
    // on_step aborts the whole run (`run_backtest`, src/backtest.cpp), so no lot
    // can be carried past its horizon through this path, and an untouched book is
    // the one the failing step is diagnosed against.
    return Err(prepared.error());
  }
  if (!prepared->has_value()) {
    // NO-TRADE is not NO-CLOSE. The entry side found nothing to open today (a
    // DropRenormalize/Unavailable step, or an empty sizing) but the run CONTINUES,
    // so a lot left here rides past its close horizon to expiry and settles at
    // intrinsic — precisely the economics CloseAtHorizon exists to avoid. Commit
    // the staged closes; `d.clear` is unreachable in this mode (lifecycle_decide
    // never returns clear=true for CloseAtHorizon), so this is the whole close.
    //
    // CONSEQUENCE, by design: a committed close is a roll-close in the engine's
    // before/after diff, and `execute` fails closed when a roll-close lot has no
    // surface on this step ("run_backtest: no surface for roll-close lot", see
    // src/backtest.cpp). The no-trade branch is reached exactly when names are
    // dropping out of the board, so "cannot build the entry AND cannot price a
    // held lot" is a correlated step, and such a run now returns Err where it
    // used to complete (visible only under UnpricedLotPolicy::ExcludeAndReport —
    // the Error policy already aborts on the unpriced held lot earlier in the
    // same iteration). That is NOT a new class of failure: the !d.open branch
    // above commits the same closes and has always reached the same engine
    // contract on such a step. Suppressing it here — closing only lots that
    // happen to be priceable today — would re-create the ride-to-settlement
    // defect for precisely the lots whose data is deteriorating, and would make
    // the close depend on whether the entry side happened to succeed. Pinned by
    // Strategy.CloseAtHorizonClosingALotWithNoSurfaceFailsClosedInTheEngine,
    // which asserts the no-trade and off-tick branches fail identically.
    //
    // A2 fix: RollAtHorizon reaches this same branch with `d.clear == true`
    // whenever the single live cohort is AT its horizon and today's re-entry
    // could not be built — before this fix `d.clear` was silently dropped here
    // (only the success path below ever consumed it), so the aged cohort rode
    // past its own roll horizon, all the way to expiry on an unbuildable
    // corpus. The close is an unconditional risk rule exactly as it is for
    // CloseAtHorizon above; only the RE-ENTRY is conditional on prepare_cohort
    // succeeding. CloseAtHorizon itself is untouched: lifecycle_decide never
    // sets `d.clear` in that mode, so this is a no-op there.
    if (close_at_horizon) {
      std::erase_if(book.lots, closes_at_horizon);
    }
    if (d.clear) {
      book.lots.clear(); // RollAtHorizon: close the aged cohort even though no replacement opened
    }
    ++n_steps_entry_skipped_;
    return Ok();
  }
  PendingCohort &pending = **prepared;

  std::size_t retained = d.clear ? 0u : book.lots.size();
  if (close_at_horizon && !d.clear) {
    retained = static_cast<std::size_t>(
        std::count_if(book.lots.begin(), book.lots.end(),
                      [&closes_at_horizon](const Lot &lot) { return !closes_at_horizon(lot); }));
  }
  book.lots.reserve(retained + pending.lots.size());
  if (close_at_horizon) {
    std::erase_if(book.lots, closes_at_horizon);
  }
  if (d.clear) {
    book.lots.clear(); // RollAtHorizon: close the front cohort before reopening
  }
  for (Lot &lot : pending.lots) {
    book.lots.push_back(std::move(lot));
  }
  next_lot_id += static_cast<std::uint64_t>(pending.lots.size());
  ++cohort_counter_;
  front_expiry_ = pending.front_expiry; // earliest-expiring leg drives the roll
  have_front_ = true;
  last_entry_seeds_ = std::move(pending.seeds);
  return Ok();
}

} // namespace atx::vol
