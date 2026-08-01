// dispersion_strangle.hpp implementation — see the header for the model.
//
// Pure config -> `StrategySpec` assembly: validate `DispersionStrangleConfig`,
// then wire up the existing declarative DSL types (LegSpec / StructureSpec /
// StrikeSelector / SizeSpec / CrossLegConstraint / LifecycleSpec). No pricing,
// no snapshot access — resolution happens later via `resolve_spec_with_policy`.

#include "atx/vol/dispersion_strangle.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "atx/vol/universe.hpp" // canonical_symbol — same rule the snapshot resolver uses

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kCalendarDaysPerYear = 365.25;

// One long strangle LegSpec on `symbol`, group "basket", TargetTheta sized.
[[nodiscard]] LegSpec make_name_leg(const std::string &symbol, double target_T,
                                    double target_abs_delta, double theta_per_name_daily,
                                    bool snap_to_sessions) {
  LegSpec leg;
  leg.symbol = symbol;
  leg.tenor.target_T = target_T;
  leg.tenor.snap_to_sessions = snap_to_sessions;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, target_abs_delta};
  leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, target_abs_delta};
  leg.size = SizeSpec{SizeSpec::Kind::TargetTheta, theta_per_name_daily, +1.0};
  leg.group = "basket";
  return leg;
}

// The short index strangle LegSpec, group "index", TargetVega sized (the
// FlatVega constraint rescales this leg's qty at resolve time).
[[nodiscard]] LegSpec make_index_leg(const std::string &symbol, double target_T,
                                     double target_abs_delta, double index_base_vega,
                                     bool snap_to_sessions) {
  LegSpec leg;
  leg.symbol = symbol;
  leg.tenor.target_T = target_T;
  leg.tenor.snap_to_sessions = snap_to_sessions;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, target_abs_delta};
  leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, target_abs_delta};
  leg.size = SizeSpec{SizeSpec::Kind::TargetVega, index_base_vega, -1.0};
  leg.group = "index";
  return leg;
}

} // namespace

Result<StrategySpec> make_dispersion_strangle_spec(const DispersionStrangleConfig &cfg) {
  if (cfg.names.empty()) {
    return Err(ErrorCode::InvalidArgument, "make_dispersion_strangle_spec: names must be non-empty");
  }
  if (cfg.index_symbol.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "make_dispersion_strangle_spec: index_symbol must be non-empty");
  }
  // Canonicalize names/index_symbol with the SAME rule the snapshot resolver uses
  // (`MarketSnapshot::uid_of` / `uid_for_symbol`, both keyed on `canonical_symbol`:
  // ASCII-upper) before comparing. A raw-string comparison would let a
  // same-symbol-different-case index/name collision ("spy" vs "SPY") or a
  // duplicate name slip through here and only surface later as a degenerate
  // self-hedged cohort or a silently double-sized theta leg.
  {
    std::vector<std::string> canon_names;
    canon_names.reserve(cfg.names.size());
    for (const std::string &name : cfg.names) {
      canon_names.push_back(canonical_symbol(name));
    }
    for (std::size_t i = 0; i < canon_names.size(); ++i) {
      for (std::size_t j = i + 1; j < canon_names.size(); ++j) {
        if (canon_names[i] == canon_names[j]) {
          return Err(ErrorCode::InvalidArgument,
                     "make_dispersion_strangle_spec: duplicate name '" + cfg.names[j] +
                         "' in names");
        }
      }
    }
    const std::string canon_index = canonical_symbol(cfg.index_symbol);
    if (std::find(canon_names.begin(), canon_names.end(), canon_index) != canon_names.end()) {
      return Err(ErrorCode::InvalidArgument,
                 "make_dispersion_strangle_spec: index_symbol '" + cfg.index_symbol +
                     "' must not also appear in names");
    }
  }
  if (!(std::isfinite(cfg.target_abs_delta) && cfg.target_abs_delta > 0.0 &&
        cfg.target_abs_delta < 1.0)) {
    return Err(ErrorCode::InvalidArgument,
               "make_dispersion_strangle_spec: target_abs_delta must lie in (0,1)");
  }
  // HoldToExpiry ignores close_dte_days for the lifecycle (cohorts age to their
  // own expiry), so only tenor_days > 0 is required there; CloseAtHorizon must
  // still have a positive residual window (tenor > close).
  if (cfg.hold_to_expiry) {
    if (!(cfg.tenor_days > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "make_dispersion_strangle_spec: tenor_days must be > 0");
    }
  } else if (!(cfg.tenor_days > cfg.close_dte_days)) {
    return Err(ErrorCode::InvalidArgument,
               "make_dispersion_strangle_spec: tenor_days must exceed close_dte_days");
  }
  if (!(cfg.close_dte_days >= 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "make_dispersion_strangle_spec: close_dte_days must be >= 0");
  }
  if (!(cfg.theta_per_name_daily > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "make_dispersion_strangle_spec: theta_per_name_daily must be > 0");
  }
  if (!(cfg.index_base_vega > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "make_dispersion_strangle_spec: index_base_vega must be > 0");
  }
  if (cfg.entry_every_n_days == 0) {
    return Err(ErrorCode::InvalidArgument,
               "make_dispersion_strangle_spec: entry_every_n_days must be >= 1");
  }
  if (cfg.missing.min_names == 0) {
    return Err(ErrorCode::InvalidArgument,
               "make_dispersion_strangle_spec: missing.min_names must be >= 1");
  }
  if (cfg.missing.min_names > cfg.names.size()) {
    return Err(ErrorCode::InvalidArgument,
               "make_dispersion_strangle_spec: missing.min_names exceeds names.size()");
  }

  const double target_T = cfg.tenor_days / kCalendarDaysPerYear;

  StrategySpec spec;
  spec.name = "mag7_dispersion_strangle";
  spec.legs.reserve(cfg.names.size() + 1);
  for (const std::string &name : cfg.names) {
    spec.legs.push_back(make_name_leg(name, target_T, cfg.target_abs_delta,
                                      cfg.theta_per_name_daily, cfg.snap_expiry_to_sessions));
  }
  spec.legs.push_back(make_index_leg(cfg.index_symbol, target_T, cfg.target_abs_delta,
                                     cfg.index_base_vega, cfg.snap_expiry_to_sessions));

  spec.constraint.kind = CrossLegConstraint::Kind::FlatVega;
  spec.constraint.group_a = "basket";
  spec.constraint.group_b = "index";

  spec.lifecycle.entry =
      (cfg.entry_every_n_days == 1) ? LifecycleSpec::Entry::EveryStep : LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.entry_every_n = cfg.entry_every_n_days;
  // HoldToExpiry: overlapping daily clips, each held to its own expiry and
  // settled by the engine at intrinsic (roll_at_T unused). CloseAtHorizon:
  // each cohort independently closed at marks once its residual T < roll_at_T.
  spec.lifecycle.holding = cfg.hold_to_expiry ? LifecycleSpec::Holding::HoldToExpiry
                                              : LifecycleSpec::Holding::CloseAtHorizon;
  spec.lifecycle.roll_at_T = cfg.close_dte_days / kCalendarDaysPerYear;

  spec.missing = cfg.missing;
  spec.hedge = cfg.hedge;

  return Ok(std::move(spec));
}

} // namespace atx::vol
