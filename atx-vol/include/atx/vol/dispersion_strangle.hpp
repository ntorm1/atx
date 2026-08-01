#pragma once

// Dispersion-strangle strategy spec builder — pure config -> `StrategySpec`
// assembly for the "long equal-theta single-name strangles vs a short
// vega-flat index strangle" trade, expressed entirely through the existing
// declarative DSL (strategy.hpp). No pricing happens here: this module only
// validates a `DispersionStrangleConfig` and wires up `LegSpec`s, a
// `CrossLegConstraint`, and a `LifecycleSpec`. Resolution/sizing/pricing is
// `resolve_spec_with_policy` + the backtest engine's job, unchanged.

#include <string>
#include <vector>

#include "atx/vol/dispersion.hpp" // MissingNameSpec, MissingNamePolicy
#include "atx/vol/strategy.hpp"   // StrategySpec, LegSpec, HedgeSpec, ...
#include "atx/vol/types.hpp"      // Result

namespace atx::vol {

// Long equal-theta single-name strangles vs a short vega-flat index strangle,
// one cohort per entry tick, each cohort closed at close_dte_days to expiry.
// Pricing is projection-path only (synthetic strikes/expiries off the fitted
// surfaces); expiry = entry ts + tenor_days calendar days.
struct DispersionStrangleConfig {
  std::vector<std::string> names;              // long single names (>= 1)
  std::string index_symbol{"SPY"};             // short hedge leg
  double target_abs_delta{0.40};               // both strangle legs, in (0,1)
  double tenor_days{90.0};                     // calendar days to synthetic expiry
  double close_dte_days{10.0};                 // close cohort below this residual
  unsigned entry_every_n_days{1};              // 1 = every trading day (EveryStep)
  double theta_per_name_daily{10.0};           // $/calendar-day theta per name
  double index_base_vega{10000.0};             // pre-constraint index sizing seed
  MissingNameSpec missing{MissingNamePolicy::DropRenormalize, 4};
  HedgeSpec hedge{};                           // default: no delta hedge
  // Lifecycle: hold each daily cohort to its OWN expiry (LifecycleSpec::
  // HoldToExpiry) rather than closing it early at `close_dte_days`
  // (CloseAtHorizon). The vega-flat dispersion PnL-track deliverable (WS-D D4)
  // holds to expiry: overlapping clips accumulate, each aged to its expiry and
  // settled by the engine at intrinsic once residual T reaches 0. When true,
  // `close_dte_days` is IGNORED for the lifecycle (only `tenor_days > 0` is
  // required). Default false preserves the CloseAtHorizon behaviour exactly, so
  // existing callers/tests are bit-identical.
  bool hold_to_expiry{false};
  // Set `tenor.snap_to_sessions` on EVERY leg (names and index), so each
  // cohort's synthetic expiry lands on a session the run actually observes
  // rather than on a weekend/holiday a hold-to-expiry settlement can never
  // reach. The builder is corpus-agnostic, so it leaves `spec.session_ts`
  // EMPTY: the caller fills it from its `Clock` refs after building the spec.
  // Default false is bit-identical to the pre-existing raw-offset expiry.
  bool snap_expiry_to_sessions{false};
};

// Validated assembly into the declarative DSL:
//  - one LegSpec per name: Strangle{Delta d call, Delta d put}, tenor
//    tenor_days/365.25, SizeSpec{TargetTheta, theta_per_name_daily, +1},
//    group "basket";
//  - one index LegSpec: same structure/tenor, SizeSpec{TargetVega,
//    index_base_vega, -1}, group "index";
//  - constraint FlatVega{group_a="basket", group_b="index"} (scales the index
//    leg so gross index vega == gross basket vega; opposite signs net ~0);
//  - lifecycle: EveryStep when entry_every_n_days==1 else EveryNDays with
//    entry_every_n; Holding::HoldToExpiry when cfg.hold_to_expiry else
//    Holding::CloseAtHorizon with roll_at_T = close_dte_days/365.25;
//  - spec.missing = cfg.missing, spec.hedge = cfg.hedge,
//    spec.name = "mag7_dispersion_strangle" (or names.size()-agnostic label).
// InvalidArgument when: names empty; index_symbol empty; names contains a
// duplicate or index_symbol also appears in names (both compared
// case-insensitively, the same canonicalization the snapshot resolver uses);
// target_abs_delta outside (0,1); tenor_days <= close_dte_days;
// close_dte_days < 0; theta_per_name_daily <= 0; index_base_vega <= 0;
// entry_every_n_days == 0; missing.min_names == 0 or > names.size().
[[nodiscard]] Result<StrategySpec>
make_dispersion_strangle_spec(const DispersionStrangleConfig &cfg);

} // namespace atx::vol
