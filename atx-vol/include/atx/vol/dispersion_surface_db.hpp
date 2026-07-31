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
#include <string_view>

#include "atx/vol/dispersion.hpp"          // DispersionUniverse / DispersionMember
#include "atx/vol/dispersion_backtest.hpp" // DispersionBacktestConfig
#include "atx/vol/surface_db.hpp"          // SurfaceDb (manifest symbol table)
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

/// Derive an equal-weight `DispersionUniverse` from a SurfaceDb: `index_symbol`
/// becomes the index leg, every OTHER enabled symbol in the db becomes a basket
/// name at weight 1/n, and the index member carries weight 1.0 (which the engine
/// ignores — only constituent weights enter the signal and sizing, see
/// dispersion.hpp). This is the surface-db route's answer to "which names am I
/// trading", so the operator authors a universe by building a db, not by
/// maintaining a second symbol list beside it.
///
/// THE MANIFEST'S SYMBOL TABLE IS THE UNIVERSE, NOT THE PARTITIONS. The two are
/// orthogonal namespaces (surface_db.hpp, write_partition): a partition stores
/// whatever symbols it was handed and registers NONE of them, while the symbol
/// table is where the operator states which underlyings this db is FOR and which
/// of them are switched off (`SymbolFitConfig::enabled`). Reading the table
/// therefore answers "the universe this db was built for"; reading a partition
/// would instead answer "what happened to be fitted on one particular date",
/// which would silently resize the basket whenever a single date's fit was thin.
/// A db whose table was never seeded has no universe at all and every index is
/// missing — see the error below, which names the count so that reads as the
/// wrong db rather than as a typo.
///
/// `index_symbol` is matched case-insensitively, since the manifest stores
/// canonical upper-case names, and every returned member carries that CANONICAL
/// spelling — so a lower-case config key cannot yield a universe whose symbols
/// then fail to match the snapshot directory.
///
/// ORDER IS THE MANIFEST'S AND IS ALREADY SORTED. `SurfaceDb::symbols()` is
/// strictly ascending by canonical symbol — `DbManifest::open` REJECTS a
/// manifest whose records are not — so preserving that order is enough to make
/// the basket sorted and the same db always yield the same universe in the same
/// order. Nothing here re-sorts; the invariant is the guarantee.
///
/// UIDS STAY 0. A `DispersionMember::uid` identifies a surface inside ONE
/// `MarketSnapshot`, not a symbol across time, so binding one here would be
/// wrong on every date but the one it came from. The engine rebinds them per
/// step via `resolve_universe_uids` / `MarketSnapshot::uid_of`; symbols are the
/// only durable identity a universe can carry.
///
/// The enabled filter runs BEFORE the index match, so an index that is present
/// but disabled is rejected exactly like an absent one — a switched-off symbol
/// has no surfaces being produced for it, which is precisely the state that must
/// not silently become an index leg. Either way the result is InvalidArgument
/// naming the symbol as the caller spelled it plus the manifest's symbol count.
///
/// An index whose db holds no OTHER enabled symbol yields an EMPTY basket rather
/// than an error or an infinite weight (the 1/n division is skipped): how few
/// names is too few is the caller's policy — `DispersionBacktestConfig::min_names`
/// — not this function's.
[[nodiscard]] Result<DispersionUniverse> universe_from_surface_db(const SurfaceDb &db,
                                                                  std::string_view index_symbol);

} // namespace atx::vol
