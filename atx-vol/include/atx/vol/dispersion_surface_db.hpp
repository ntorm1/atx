#pragma once

// SurfaceDb-route dispersion backtest front end.
//
// The surface-db route runs a dispersion book straight off an already-fitted
// SurfaceDb partition set, so — unlike the OPRA route's `RunSpec` — it needs no
// corpus-build inputs at all: the only authored artifact is the STRATEGY
// configuration. That is what this header reads, in the same flat key<TAB>value
// TSV family as `read_run_spec` (dispersion_workflow.hpp) so an operator learns
// one file format for both routes.

#include <filesystem>

#include "atx/vol/dispersion_backtest.hpp"
#include "atx/vol/types.hpp" // Result / Status / ErrorCode (atx-vol's Result include)

namespace atx::vol {

/// Parse a key<TAB>value dispersion config. Every key optional; absent keys keep
/// DispersionBacktestConfig defaults. Unknown key, unparsable value, or bad enum
/// token is InvalidArgument naming the offender. Blank lines and lines starting
/// with '#' are ignored.
///
/// Keys: target_dte_days, roll_dte_days, gross_index_vega, delta_band, min_names,
///       entry_every_n, record_diagnostics (0/1), multiplier,
///       side (short_index_long_names|long_index_short_names),
///       weighting (vega_neutral|equal_vega|gamma_neutral|theta_neutral),
///       strike_rule (atm_forward_straddle|fixed_moneyness|delta_strangle),
///       log_moneyness, target_abs_delta,
///       hedge_kind (none|delta_to_zero), hedge_cadence (at_entry|daily),
///       half_spread_bps, per_contract_cost, n_threads, prefetch_depth
///
/// The last four land on NESTED fields: `half_spread_bps`/`per_contract_cost` on
/// `run.frictions`, `n_threads` on `run.price.n_threads`, `prefetch_depth` on
/// `run.prefetch_depth`.
///
/// SPREAD LANE. `FrictionModel::half_spread_bps` is read by the engine ONLY under
/// `SpreadKind::PriceBps`, and the default kind is `None` — so authoring a
/// half-spread and nothing else would otherwise be a silently ignored knob. A
/// nonzero `half_spread_bps` therefore also arms `run.frictions.spread_kind =
/// PriceBps`, which is the one lane that gives the authored number meaning; a
/// caller that wants a different lane sets it on the returned config. Zero (the
/// default) leaves `spread_kind` alone, so a frictionless config is untouched.
/// `per_contract_cost` is charged independently of the lane and arms nothing.
///
/// A missing/unreadable file is NotFound/IoError. A row that is not exactly
/// key<TAB>value — no tab, or an empty value — is InvalidArgument: a
/// present-but-empty value is an authoring error, not "use the default".
/// Repeating a key is last-one-wins.
///
/// Keys deliberately NOT exposed (they are caller/route decisions, not file
/// knobs): `project_to_calendar_expiry`, `entry`/`holding` lifecycle shape,
/// `limits`, and the rest of `RunConfig`.
[[nodiscard]] Result<DispersionBacktestConfig>
read_dispersion_backtest_config(const std::filesystem::path &path);

} // namespace atx::vol
