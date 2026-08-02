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
#include <optional>
#include <string>
#include <string_view>

#include "atx/vol/backtest_driver.hpp"     // RunOutcome (result + tearsheet + EngineRunStats)
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
///       half_spread_bps, per_contract_cost, n_threads, prefetch_depth,
///       unpriced (error|exclude_and_report)
///
/// The last five land on NESTED fields: `half_spread_bps`/`per_contract_cost` on
/// `run.frictions`, `n_threads` on `run.price.n_threads`, `prefetch_depth` on
/// `run.prefetch_depth`, `unpriced` on `run.unpriced`.
///
/// UNPRICED-LOT POLICY. `run.unpriced` defaults to `UnpricedLotPolicy::Error`
/// (backtest.hpp:555-561) — a step on which a HELD lot has no surface aborts the
/// run rather than let NAV silently truncate — and this reader does NOT change
/// that: a config that never names the key runs exactly as it did before the key
/// existed. It is exposed because a real surface db DOES contain such sessions —
/// the sp100-2026 corpus loses basket names on 118 of its 140 sessions, so a run
/// entered on 2026-01-02 aborts on the very next one (2026-01-05, 8 held lots
/// across CMCSA/MO/QCOM/WMT); see the sprint's Task 6 report — and an operator's
/// only alternatives were to shorten the window or edit code.
/// `exclude_and_report` is the documented opt-in to the lenient arithmetic,
/// and it is not free: the excluded step's P&L is never recovered when the surface
/// reappears, so NAV permanently diverges from liquidation value and the only
/// record of it is the per-row `BacktestResult::n_unpriced_lots` count. Author it
/// deliberately, and read that column.
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

/// Everything the surface-db route needs to run: WHICH db, WHICH window, WHICH
/// index, optionally WHICH basket, and the strategy config. There is deliberately
/// no corpus-build input — the db IS the corpus — and no output path, because
/// persistence is the caller's (see examples/surface_db_dispersion_backtest.cpp).
struct SurfaceDbDispersionSpec {
  std::string db_root;   // required; SurfaceDb::open's root
  std::string date_lo;   // required; ISO "YYYY-MM-DD", INCLUSIVE
  std::string date_hi;   // required; ISO "YYYY-MM-DD", INCLUSIVE
  std::string index_symbol{"SPY"};
  /// Absent (the default) => the EQUAL-WEIGHT route: the basket is derived from
  /// the db manifest by `universe_from_surface_db` and frozen for the whole run.
  /// Present => the POINT-IN-TIME route: a `UniverseRow` TSV read by
  /// `read_universe`, re-resolved per step, so a mid-window reconstitution (or a
  /// removal) is honoured. The two routes build DIFFERENT books; which one runs
  /// is decided by this field alone and never by a fallback.
  std::optional<std::filesystem::path> universe_path;
  DispersionBacktestConfig config{}; // from read_dispersion_backtest_config, or defaults
};

/// The one-call surface-db dispersion backtest: open the db, build its clock,
/// window it to [date_lo, date_hi], resolve the universe by the route
/// `spec.universe_path` selects, and hand the whole thing to `run_timed` — so the
/// caller gets the engine's untouched `BacktestResult`, its `TearSheet` and the
/// `EngineRunStats` whose `wall_clock_ms` brackets the engine call ALONE.
///
/// EVERY stage failure is returned, none is absorbed. In particular an absent
/// `universe_path` file is an error rather than a silent fall-through to the
/// equal-weight route: falling through would run a book the operator did not
/// author and report it as a success. Each error is re-wrapped with the stage
/// that produced it and this function's name, preserving the underlying
/// `ErrorCode` and message — so "which of five things went wrong" is answerable
/// from the message alone.
///
/// EMPTY WINDOW IS AN ERROR, not an empty run: `Clock::between` rejects a window
/// selecting no partition and names the db's available range, which propagates
/// out of here verbatim. A zero-row tearsheet would otherwise read as "the
/// strategy did nothing" instead of "this db has no such dates".
///
/// NO SHARED SNAPSHOT CACHE IS INSTALLED, and callers should not install one in
/// `spec.config.run.snapshot_cache`. The engine builds a PRIVATE cache exactly
/// when that field is null, and only the private cache may map its archives with
/// `ArchiveBacking::Sealed` (backtest.cpp: a caller-supplied cache can outlive
/// the run and be shared across books, so it must stay Mutable). A SurfaceDb
/// replay is a read-only corpus, which is precisely the case Sealed exists for,
/// so leaving the field null is the perf-correct default here — supplying a cache
/// costs a whole-archive copy per date and gains nothing on a single-pass run.
[[nodiscard]] Result<RunOutcome>
run_surface_db_dispersion_backtest(const SurfaceDbDispersionSpec &spec);

} // namespace atx::vol
