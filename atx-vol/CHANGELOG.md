# atx-vol — changelog

Breaking behavioural changes are recorded here with their migration. Anything
that silently changes a NUMBER a caller already depends on belongs in this file.

## Unreleased

### BREAKING — oracle/DAG harness now fails closed with run-owned leases and ordered bootstrap states

The old advisory worktree marker and permissive sprint integration behavior are
removed. `scripts/lease-worktree.ps1` now publishes an atomic v3 record and requires
`-RunId` plus an explicit durable owner: caller PID/process-start identity or a
run-unique heartbeat with an independently running continuous keeper. Foreground
commands and before/after pulses do not own liveness. The short-lived lease command
cannot silently own a production lease; the old `-Pulse` command is removed.
Acquisition waits for and records an
authenticated keeper-ready pulse. Existing branches must still point at the
frozen base SHA; corrupt records fail closed and require explicit guarded recovery.

`vol-sprint` no longer excludes failed lanes and integrates the rest. Every planned
lane is mandatory, every Fix gets a fresh exact-SHA review, and any incomplete or
non-APPROVE lane aborts before integration. Approved lane leases are released before
the verifier acquires a new isolated integration lease; main checkout integration
is forbidden. Only exit-code-zero commands may support success; failed attempts are
diagnostics. The integration result identifies the frozen base, run lease, exact
reviewed lane SHA list, and newly leased integration branch/SHA.

**BREAKING:** full regression/release suites and full-repository hygiene are removed
from the oracle loop entirely. Every lane now supplies a closed mapping from scoped
files to affected unit targets, anchored unit regexes, hypothesis-specific
OracleBench tests, and owning PCH-off targets. Integration mechanically derives the
exact changed-file gate set, adds Mode A/B aggregate smoke+tune scorecards and the
quiet pinned-speed microbenchmark, and requires one exact-SHA receipt per command
without omission or extras. Labels, bare/unanchored ctest, broad builds/runners,
and full-repo hygiene fail the oracle contract even when reported as diagnostics.
Full regression and release qualification remain separate, outside-loop work.

`vol-oracle-iter` no longer maps all missing tooling into a vol-sprint bootstrap.
It hard-selects one state in order: `missing_data`, `missing_mode_a`,
`missing_conventions`, `missing_mode_b`, `ready`. A bootstrap invocation dispatches
exactly one fixed implementation lane through Build, exact-SHA Review, optional Fix,
fresh Review, scoped verification, and atomic landing on `refs/heads/oracle/canonical`.
Its capability agent has no general file tools and can run only the fixed
aggregate-only probe. The probe internally parses committed cohort manifests only
to recompute canonical sorted holdout-membership SHA-256 and disjointness;
membership never reaches its agent or report, and it never opens licensed rows or
benchmarks holdout. Only ready state runs the RSI loop; a failed sprint
returns FAILED before holdout and does not count as REJECT. Attribute is tool-less
and receives a validated aggregate smoke/tune payload. Only Ratchet opens holdout,
recomputes its membership digest, tests the exact reviewed integration SHA in a new
lease, and atomically lands ACCEPT; REJECT leaves canonical unchanged. Successful
results return typed aggregate metric deltas and their evidence receipts. Ratchet's
agent no longer decides ACCEPT/REJECT: workflow code verifies delta arithmetic,
target improvement, the 2% aggregate bound, pinned speed, applicable modes, digest,
scoped gates, and market evidence. Agents prepare candidate commits; a minimal
finalizer performs the exact canonical compare-and-swap only after validation, and
an independent audit reports the actual ref even when the finalizer report is lost.

Capability completion is no longer inferred from filenames. Each bootstrap stage
must publish its exact versioned aggregate receipt; the probe validates closed
keys/enums/target sets, artifact digests, count invariants, SHA ancestry, and
prior-receipt provenance. Legacy or malformed receipts fail closed. Attribute
schema v2 likewise removes free-form source strings. Ratchet metric IDs,
classification, direction, target limit, and exact commands are frozen by the
workflow. Baseline and speed-pin numerics are derived only from exact typed Measure
command output; Measure cannot self-report or override them. Ratchet candidates are
likewise derived from exact typed gate numeric results. Bootstrap stages 2-4 use the
fixed `oracle-targeted-gate.ps1` adapter to turn ordinary targeted tool output into
closed semantic PASS receipts. Exit zero with no tests, zero processed rows, or an
incomplete metric set is failure; targeted ctest uses `--no-tests=error`. Per-file
gate mappings now impose mandatory commands, so planner additions are additive and
cannot substitute another allowed suite or hypothesis. Exact gate commands return typed results, and
typed NBBO means/distances are recomputed by workflow code before suspect exclusion.

## 1.2.0

The public-surface api-restructure. Every public header moved from the flat
`include/atx/vol/*.hpp` layout to an 8-module tree,
`include/atx/vol/api/<module>/*.hpp` — `analytics`, `backtest`, `core`,
`fitting`, `marketdata`, `pricing`, `simd`, `storage` — with the umbrella at
`include/atx/vol/api/vol.hpp`. This is a path rename plus a re-derived
public/private split, not a Tier-A/Tier-B promotion or demotion: the SET of
frozen Tier-A headers is unchanged (see README.md's "API stability policy"
table), and every header that stays public keeps its existing tier. Every
consumer's `#include` line changes regardless.

### BREAKING — public header include paths moved under `atx/vol/api/<module>/`; non-public headers are no longer shipped at all

**Spelling rule.** `atx/vol/X.hpp` → `atx/vol/api/<module>/X.hpp` for the 75
headers that stayed public. Examples:

```
atx/vol/vol.hpp        -> atx/vol/api/vol.hpp
atx/vol/american.hpp   -> atx/vol/api/pricing/american.hpp
atx/vol/session.hpp    -> atx/vol/api/fitting/session.hpp
atx/vol/data.hpp       -> atx/vol/api/marketdata/data.hpp
atx/vol/backtest.hpp   -> atx/vol/api/backtest/backtest.hpp
atx/vol/surface_db.hpp -> atx/vol/api/storage/surface_db.hpp
```

The full 75-entry old-path -> new-path mapping is
`atx-vol/scripts/api_restructure_measure.py`'s generated
`tmp/api-restructure/rewrite_map.csv` (regenerate with that script; see
`atx-vol/docs/api-placement.md`).

**The external-only public-surface rule.** A header is public iff it is
transitively `#include`d, directly or indirectly, from a translation unit
under one of the three EXTERNAL-ONLY roots — `atx-options-engine`,
`atx-vol/python`, `atx-vol/test-package`. Everything else — including the
per-family calibrators (`svi_calib.hpp`, `essvi_calib.hpp`, `cstar.hpp`,
`cstar_calib.hpp`, `c8_calib.hpp`; the shared vocabulary `c8.hpp`/`calib.hpp`
stays public), the whole former `detail/` tree bar one generated header, and
every fixture-only header (`spy_fixture.hpp`) — moved to `src/<module>/` and
is **private**: no `include/` path, not installed, not includable by an
external consumer at all. A caller that included one of these directly gets a
missing-header compile error, not a moved-header one; there is no forwarding
shim.

**Install change.** `cmake/atx-vol-install.cmake` narrowed its header install
from the old blanket `install(DIRECTORY .../include/atx/vol/ ...)` (which
shipped whatever sat under `include/atx/vol/`, `api/` subtree or not) to
exactly `install(DIRECTORY .../include/atx/vol/api/ DESTINATION
.../atx/vol/api)`. The one non-`api/` header that still ships is the
generated `atx/vol/detail/version_generated.hpp` (configure_file'd from
`project(VERSION)`, included by `version.hpp`); nothing else under the old
`detail/` path, and nothing under `src/`, is reachable from an installed
prefix.

**Migration.** Update every `#include "atx/vol/...hpp"` line to its
`atx/vol/api/<module>/...hpp` spelling per the mapping above. There is no
compatibility flag: the old paths do not exist in the source tree, the
install tree, or a forwarding header.

## 1.1.0

The backtest-production-lakehouse sprint. Adds a content-addressed track
lakehouse (`TrackKey` / `TrackStore` / `Catalog` / cache-first sweep driver /
GC — see [docs/backtest-lakehouse.md](docs/backtest-lakehouse.md)), a
quote-side fill model, a Reg-T + scenario-grid margin engine, and a rigor
tearsheet (PSR/DSR/MinTRL). Tier-A stays frozen throughout — every new type
this sprint introduced enters at Tier-B or `research/`, per the sprint's own
global constraint — but the backtest engine's `RunConfig` picks up three
behavioural default flips inside that constraint, all with migration lines
below.

### BREAKING — swap fixings on a clock coarser than the daily schedule now fail closed by default

`RunConfig::swap_fixing_cadence` (new enum `SwapFixingCadence`, `backtest.hpp`)
is appended as `RunConfig`'s 18th field (arity pin `17` → `18`). The
realized-variance leg used to book exactly one fixing per **clock step**
regardless of how many NYSE sessions actually elapsed between steps, so a
clock coarser than a swap lot's implicitly-daily fixing schedule silently
overstated `annualization * Σr² / n_done` by roughly the gap factor (a
weekly clock ~5x), with no error and no count.

**New default, `RequireEverySession`.** A step whose elapsed weekday-session
count (`weekday_sessions_between` — Mon–Fri, holiday-blind, no new calendar
dependency) is not exactly 1 now returns
`Err(ErrorCode::SwapFixingScheduleViolation)`, naming the offending step and
the expected-vs-seen session count, instead of completing with the
overstated variance.

**Migration.** A caller whose corpus is intentionally coarser than daily
(and was silently accepting the overstated variance before) sets

```cpp
cfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;
```

which books the gap's one observed return while scaling `n_obs_done` by the
elapsed-session count (instead of always advancing it by one), so the
daily-strike convention is not overstated by the gap.

`ErrorCode::SwapFixingScheduleViolation` is appended to `atx-core::ErrorCode`
(existing values unchanged).

### BREAKING — `RollAtHorizon` now closes the aged cohort even when re-entry fails soft (no opt-out)

`DeclarativeStrategy::on_step`'s `RollAtHorizon` lifecycle used to close the
live cohort only when a fresh entry cohort resolved on the *same* step. On a
step where the live cohort was **at** its roll horizon but re-entry failed
soft (e.g. `DropRenormalize` finding too few surviving names), the close was
silently dropped in the no-trade branch — the aged cohort then rode past its
own roll horizon, potentially all the way to expiry, on any corpus where
re-entry stayed unbuildable. The horizon close is now unconditional there,
exactly as it already was for `CloseAtHorizon`; only re-entry itself stays
conditional on `prepare_cohort` succeeding (`CloseAtHorizon` is untouched —
`lifecycle_decide` never sets a clear in that mode).

**Effect.** Any `RollAtHorizon` corpus that ever hit a no-trade horizon step
now closes that cohort's position (and books its NAV/cash effect) at the
horizon instead of silently continuing to carry it — a real NAV-moving fix,
not a diagnostic. **No opt-out**: the prior behavior was a lifecycle defect,
not a documented configuration choice.

**New diagnostic.** `BacktestResult::n_steps_entry_skipped` (run-total
scalar, non-wire — absent from `kBacktestSeriesColumns`/RunArchive, so
`ra_schema_hash()` is untouched) counts steps whose entry resolution reached
the soft no-trade path, in every holding mode; `IStrategy` gains the
matching `n_steps_entry_skipped()` accessor (default `0`), the same
additive-virtual pattern as `hedge_spec()`/`referenced_uids()`.

### BREAKING — deferred expiry settlements now settle at the frozen expiry-date spot, and stale substitutions are counted (no opt-out)

`DeferredSettlementBook` (`backtest.cpp`, reachable only under
`UnpricedLotPolicy::ExcludeAndReport` — the one policy that defers a lot
instead of failing the run closed) used to settle a deferred lot's intrinsic
value against whatever *later* step's board happened to reappear first,
which could have drifted arbitrarily far from the lot's true expiry-date
value. It now records the underlying spot at the moment the board first goes
missing and settles against **that** recorded spot; only when no spot was
ever recorded (the base board was *also* absent at the deferral step) does
resolution fall back to the reappearing step's own spot — and that fallback
is now counted via `BacktestResult::n_settlements_at_stale_spot` (run-total
scalar, non-wire; always `0` under `UnpricedLotPolicy::Error`, which never
defers).

**Also fixed: a permanent NAV-vs-liquidation gap.** A deferred lot leaves
`book.lots`/`book_totals.pv` the instant it defers, but NAV's explain lane
previously booked nothing for that drop on the deferral row, and — when the
base mark was unknown — nothing at resolution either, even though cash
always received the full intrinsic. The explain lane now sheds the lot's
carried mark on the deferral row and recognizes the full intrinsic
unconditionally at resolution, closing the gap in both directions.

**Effect.** Moves NAV for any run with deferred-settlement lots — both from
the corrected settlement spot and from the NAV-vs-liquidation gap closing.
**No opt-out**: the prior numbers were a genuine accounting error, not a
documented configuration. The 82-session golden SPY dispersion pin is
unaffected (verified bit-for-bit, `24740.624124996561`) — that corpus has
zero deferred-settlement lots in its window.

### BREAKING — NAV reconciliation and entry-fill slippage are on by default

`RunConfig::reconcile_nav` and `RunConfig::book_entry_fill_slippage`
(`backtest.hpp`) both flip `false` → `true`. Pure default-value changes — no
field added, reordered, or removed; `aggregate_arity_is_v<RunConfig, 22>` is
unaffected by this pair.

**Effect.** An entry fill away from the model mark now hits `cost` (and
therefore `nav`/`cash`) by default instead of being silently absorbed at the
model mark. Every completed run's NAV is now audited by default against an
independently recomputed liquidation value; a run whose drift exceeds
`reconcile_nav_tol` now returns `Err` (no `BacktestResult` at all) instead of
completing with an undetected accounting gap.

**Migration.** A caller that needs the pre-1.1.0 shape — no fill-slippage
booking, no reconciliation abort, e.g. for a bit-exact replay suite pinned
against the old numbers — sets both fields back explicitly:

```cpp
cfg.reconcile_nav = false;
cfg.book_entry_fill_slippage = false;
```

Verified bit-for-bit unchanged on the real 82-session SPY corpus with both
flags set this way (frictionless regime, `cost == 0` before and after).

### BREAKING — multi-name financing rate resolution fails closed instead of silently using archive order

`FinancingConfig` (`backtest.hpp`) gains two appended fields:
`std::uint32_t reference_uid{0}` and `std::optional<double> flat_r{}`.

**Old behaviour.** `finance_premium`'s cash-carry rate always came from
`base->surface_at(0)` — the first surface in *archive order*, an arbitrary
constituent on any multi-name corpus.

**New behaviour.** `flat_r`, if set, is used directly. Otherwise
`reference_uid`, if non-zero, names the uid to read `r` from. Otherwise
(`reference_uid == 0`, the default): on a base snapshot with at most one
surface, this reproduces the old `surface_at(0)` pick bit-for-bit — there is
only one surface to choose. **On a genuinely multi-name corpus, the run now
fails closed with `ErrorCode::InvalidArgument` naming the surface count and
date, instead of silently keying off archive order.**

**This is a compatibility break, not a bug fix — call it out explicitly
before upgrading.** A `finance_premium = true` run over a multi-name corpus
that used to complete (picking whichever surface happened to sort first in
the archive) now aborts unless it names a rate source. The ambiguous
reference is treated as a configuration mistake, not a data gap, so the
check fires unconditionally rather than only when the day's cash balance
happens to be nonzero.

**Migration.** Set `FinancingConfig::reference_uid` to the uid whose rate
should fund `finance_premium` (set it to the archive-index-0 uid to
reproduce the exact prior numeric behaviour), or set `flat_r` directly to an
explicit rate that bypasses any board lookup. Single-name callers are
unaffected — the default keeps working exactly as before.

### NEW — quote-side fill model

`FrictionModel::SpreadKind::QuoteSide` (value `3`, appended) fills at
`mid ± f(leg_count)·half_spread` off a recorded two-sided quote
(`FrictionModel::RawQuote` / `QuoteLookup` / `quote_lookup`), ORATS
convention, instead of charging a synthetic spread cost on top of the model
mark the way `PriceBps`/`VolTicks` do. `crossing_fraction_single{0.75}` /
`crossing_fraction_complex{0.53}` select by how many lots the entry/close
cohort contributes (one leg vs. more).

**Semantics note, since this is easy to get backwards against the existing
lanes:** `QuoteSide` does not stack with a separate spread charge —
`half_spread()` returns `0.0` under `QuoteSide` specifically because the
crossing distance chosen for the fill price *is* the spread cost; charging
both would pay the spread twice. `impact_fraction` still applies additively
on top, same as every other `spread_kind`. `spread_kind == QuoteSide` now
*requires* `book_entry_fill_slippage` (`validate_run_config` enforces this) —
otherwise the fill-vs-mark gap this lane produces never reaches NAV. A new
`FrictionRegime` enum (`Frictionless` / `Modeled` / `QuoteSide`) and
`BacktestResult::friction_regime` classify which regime a run actually took,
stamped once per run from `FrictionModel` alone.

**No caller-visible flip.** `spread_kind` still defaults to `None`; a run
that never opts into `QuoteSide` is bit-identical, including with a live
`quote_lookup` and non-default crossing fractions wired but unused (the new
fields are structurally inert unless `QuoteSide` is selected — a dedicated
invariant test proves it). No migration action needed.

### NOTE — scenario-grid margin's vol-shock magnitude is a documented default, not a spec value

`MarginScenarioSpec::vol_shock` (`margin.hpp`, the Reg-T + scenario-grid
margin engine's `scenario_margin`) defaults to `±0.10` absolute vol points.
The originating brief text for this constant was truncated before a number;
`±0.10` is a conservative, explicitly documented choice, overridable per call
— not a value re-derived from any external spec. Flagging here so a caller
who assumed a different convention notices before relying on the default.

### NOTE — `psr`/`dsr`/`min_trl` take RAW kurtosis, opposite of the sibling atx-engine convention

`atx::vol::psr` / `dsr` / `min_trl` (`tools/tearsheet.hpp`, the PSR/DSR/MinTRL
rigor tearsheet) take `kurt` as Bailey & Lopez de Prado's `γ4` — **raw
(Pearson) kurtosis**, where a Gaussian is `3`. The sibling
`atx-engine/eval/deflated_sharpe.hpp` implements the same formula but takes
**excess kurtosis**, where a Gaussian is `0`. Both are individually correct
against their own documented formula; a future integration point that
computes moments once and feeds both **must convert explicitly** between the
two conventions. No code in either module does this conversion for you.

### NEW — backtest CLI realism knobs, and economics stamps on every emitted artifact

`mag7_dispersion_backtest` and `spy_dispersion_pnl` (`examples/`) gain a
shared flag surface (`examples/dispersion_realism_flags.hpp`) exposing
knobs that previously silently took `RunConfig{}`'s defaults no matter what
an operator asked for: `--unpriced`, `--exercise-policy`, `--margin-breach`,
`--friction-spread-kind` (`none|price_bps|vol_ticks|quote_side`,
`--half-spread-bps`, `--vol-tick`, `--impact-fraction`,
`--per-contract-cost`, `--hedge-slippage-bps`,
`--crossing-fraction-{single,complex}`), and `--borrow-rate`,
`--finance-premium`, `--shares-carry`, `--initial-cash`,
`--financing-reference-uid`, `--financing-flat-r`.

**Every artifact both CLIs emit now prints `friction_regime` and
`economics_rev`** — `series.csv` / `strategy_metrics.csv` /
`engine_metrics.csv` / `db_stats.csv` for `mag7_dispersion_backtest`;
`pnl_track.tsv` for `spy_dispersion_pnl`; and, on the
`atxvol_spy_dispersion_backtest` tool route, `run_config.tsv`,
`surface_tearsheet.tsv` / `surface_pnl_track.tsv`, `run.atxrun`'s `meta`
section (`run-backtest` / `run-projected-backtest`), and every affected
console summary line. `encode_meta_section`'s `extra` parameter appends
**rows**, not columns, so this is additive metadata: no `RunArchive` section,
column layout, or schema hash changed on any artifact.

### BREAKING — `mag7_dispersion_backtest`'s unpriced-lot default flips from lenient to fail-closed

`mag7_dispersion_backtest`'s default `UnpricedLotPolicy` flips from the
CLI's own `ExcludeAndReport` override to `RunConfig{}`'s production default,
`Error`. The CLI used to force `rc.unpriced =
UnpricedLotPolicy::ExcludeAndReport;` unconditionally, silently overriding
the engine's own already-fail-closed default; that override is deleted, not
replaced with a different one.

**Migration.** Pass `--unpriced exclude` to restore the prior lenient
behaviour. `spy_dispersion_pnl`'s own default (`ExcludeAndReport`, required
for its held-to-expiry corpus's expected calendar-edge gaps) is unchanged,
and is now overridable via the same flag on that CLI too.

### FIXED — dispersion's `apply_to_financing` rate now reaches the field the engine actually reads

`dispersion_engine_run_config_from`'s `apply_to_financing` translation wrote
the flat discount rate into `FinancingConfig::borrow_rate` — the wrong
field. `borrow_rate` prices the short-shares borrow proxy, an unrelated cost
lane, and this silently clobbered whatever `financing_borrow_rate` spec key
a caller had separately set; meanwhile `finance_premium`'s own rate source
(`flat_r` / `reference_uid`, the fields `backtest.cpp`'s accrual block
actually reads) was never set at all — so the first multi-name dispersion
run to flip `rate_applies_to_financing` on would have hit the fail-closed
multi-name check above with no valid `reference_uid`. Fixed: the rate is now
written into `FinancingConfig::flat_r`, which takes unconditional priority
over the `reference_uid` lookup, so an `apply_to_financing = true` run can no
longer reach that fail-closed branch on any name count. No `run_spec.tsv` in
the repository sets `rate_applies_to_financing`, so the golden 82-session
pin is unaffected.

**No `kBacktestEconomicsRev` bump was needed for this fix**, and the
reasoning is structural, not usage-odds: `canonical_config_bytes` folds
`flat_r` by presence-then-value, and this fix always flips that presence bit
on the `apply_to_financing = true` path (pre-fix: `flat_r` never set,
presence `0`; post-fix: always set, presence `1`) — independent of
`kBacktestEconomicsRev`, independent of what `borrow_rate` used to hold. A
pre-fix track's `TrackKey` can therefore never collide with a post-fix run
of "the same" config, because the config bytes themselves already diverge
structurally.

### FIXED — the 82-session golden NAV pin corrected to the already-current value (no economics change)

`research/include/atx/vol/research/golden_pin.hpp`'s `kGolden82SessionFinalNav`
(introduced this sprint, Task D1) shipped as `247.4065016443293` — copied from
stale sprint/CHANGELOG prose without reproducing it against the corpus. That
literal was already TWO generations out of date before D1 ever typed it in:
commit `2de65c7` (2026-07-25, pre-dates this sprint) had already superseded it
once to `247.40624124981315` ("already stale by one generation" per that
commit's own message) and, in the same commit, applied the E1 sizing
migration's exact ×100 rescale to `24740.624124981368`. Task E3 re-measured
directly against the real 82-session SPY corpus at this sprint's tip and got
`24740.624124996561` — bit-for-bit identical to Task A3's independent
measurement earlier in this same sprint, and within 6.1e-13 relative of
2de65c7's post-E1 figure (ordinary pricing-path drift, seven orders of
magnitude below a cent, the same class 2de65c7 itself already catalogued as
non-economic). `kGolden82SessionFinalNav` is corrected to `24740.624124996561`.

**No `kBacktestEconomicsRev` bump.** Nothing landed in this sprint moved this
corpus's NAV — Task A3 proved its own default-flip bit-identical before/after
on this exact corpus, and the one other candidate (Task E1's
`apply_to_financing`→`flat_r` routing fix, commit `5292cae`) is inert here
because `bt-sota-baseline/run_spec.tsv` never sets
`rate_applies_to_financing`. This is a correction of a mistyped literal, not a
re-pin driven by a real NAV movement — bumping the rev would be wrong twice
over: it would misrepresent what happened, and (folded into `make_engine_id()`
→ `TrackKey`) it would invalidate every cached BacktestDb/TrackStore entry in
the lakehouse for no economics reason at all. See `golden_pin.hpp`'s own
comment for the full chain of evidence (git commits, control experiment,
exact numbers).

### Tier promotion — five research-tier headers become Tier-B public surface

`sweep_driver.hpp`, `track_key.hpp`, `track_store.hpp`, `catalog.hpp` and
`snapshot_pool.hpp` move (`git mv`) from
`research/include/atx/vol/research/` to `include/atx/vol/` — Tier-B
32 → **37**, `research/` 17 → **12** (`VolUmbrella.TierCountsMatchTheReadmeTable`
now asserts 58 / 37 / 30; see the README's *API stability policy (1.x)*
section for the full count history). Tier-A is untouched: none of the five
is reachable from `vol.hpp`, and `backtest.hpp`'s forward-declaration of
`SnapshotPool` stays a
forward declaration specifically so Tier-A's closed-under-inclusion rule does
not pull it in.

This is a promotion, not a rewrite: all five were already documented
Arrow/SQLite-free at the header level (only their `.cpp`s need
`ATX_VOL_LAKEHOUSE`, unchanged by the move), and the namespace was already
plain `atx::vol` with no `research::` sub-namespace. It reflects that these
five are the lakehouse's stable identity/storage/orchestration vocabulary —
`TrackKey`, the Parquet track store, the SQLite catalog, the cache-first
sweep driver, the sealed snapshot pool — rather than one-off run-orchestration
drivers, and now carries the same "public but not frozen for 1.x" promise as
any other Tier-B header.

**Migration.** Update `#include "atx/vol/research/{sweep_driver,track_key,
track_store,catalog,snapshot_pool}.hpp"` to `#include "atx/vol/{name}.hpp"`.
No namespace change. See
[docs/backtest-lakehouse.md](docs/backtest-lakehouse.md) for the full lake
layout, the `TrackKey` recipe, the economics-rev invalidation story (and its
compile-time-enforced golden-NAV tripwire pairing), GC/compaction
operations, and the DuckDB/Python (`atxpy.tracks`) query cookbook.

## 1.0.0

The first release with a stability promise. Everything below happened during the
production-v1 release sprint, on the way from an internal library to one that
installs into a prefix and can be depended on.

**What 1.0.0 actually promises** is a *tier*, not the tree: the 57 headers
`atx/vol/vol.hpp` includes are frozen for 1.x, and the manifest that says which
those are is machine-checked (`kTierA` in `atx-vol/tests/vol_umbrella_test.cpp`).
(The set said 56 until the release audit re-derived it. Since the release gate's
pre-flight, the *count* is machine-checked too —
`VolUmbrella.TierCountsMatchTheReadmeTable` asserts 57 against the live manifest,
alongside Tier-B 31 and `detail/` 28 — so this digit can no longer rot silently
the way it did.)
Everything else — Tier-B, `detail/`, `tools/`, `research/` — is public-but-
unfrozen or internal. The full policy, with the counts and the tests that
enforce it, is the *API stability policy* section of `README.md`. Read it before
depending on a header: this release moved a lot of them, deliberately, precisely
so the frozen set could be small and honest.

Because that reshaping is the release, **this section is mostly breaking
changes**. They are grouped by what a caller has to do about them.

### BREAKING — the public surface was tiered, and headers moved

Nothing was deleted in the tiering itself; every relocation is a `git mv`.

* **12 headers demoted to `detail/`** (`#include "atx/vol/X.hpp"` →
  `"atx/vol/detail/X.hpp"`): `parallel_for`, `pricing_executor`, `counters`,
  `phase_profile`, `prepared_fitting`, `prepared_policy`, `prepared_portfolio`,
  `strip_grid`, `run_archive_schema`, `backtest_series_columns`,
  `risk_surface_validation`. `listed_quote_key` was demoted and then **returned
  to Tier-B**, because `listed_opra.hpp` names `ListedQuoteKey` in a public
  signature — a caller could not use that parameter without naming a type the
  tier says carries no promise.
* **6 headers → `atx-vol-tools`** (`"atx/vol/X.hpp"` → `"atx/vol/tools/X.hpp"`):
  `run_report`, `surface_db_admin`, `surface_db_build`, `surface_db_exit_codes`,
  `surface_db_populate`, `tearsheet`.
* **9 headers → `atx-vol-research`** (`"atx/vol/X.hpp"` →
  `"atx/vol/research/X.hpp"`): `backtest_driver`, `dispersion_backtest`,
  `dispersion_run`, `dispersion_workflow`, `listed_definitions_cache`,
  `listed_dispersion_pipeline`, `listed_dispersion_reconciliation`,
  `run_archive`, `run_diagnostics`. The split line is **driver vs vocabulary**:
  headers that *compose* a research run moved; the dispersion domain vocabulary
  they are written in stayed public.
* **`atx/vol/curve.hpp` → `atx/vol/rates_curve.hpp`** (Tier-A). No symbol
  renamed; the rates vocabulary just collided visually with the vol-smile family
  (`vol_curve.hpp` / `spline_curve.hpp`).
* **`spy_fixture.hpp`** moved the other way — out of `tests/support/` and onto
  the public surface as Tier-B `atx/vol/spy_fixture.hpp` — because the shipped
  Python module and a bench reached into `tests/` for it.
* **The umbrella is now exactly Tier-A.** 7 headers joined it (`adjusted_greeks`,
  `corpus`, `priced_surface_view`, `query_pricing`, `spline_curve`, `surface_db`,
  `surface_policy`) and 14 left it for Tier-B (`batch`, `c8_calib`, `cstar`,
  `cstar_calib`, `essvi_calib`, `svi_calib`, `historical_projection`,
  `listed_dispersion`, `listed_dispersion_schedule`,
  `listed_dispersion_strategy`, `listed_opra`, `occ_ess`, `panel`, `s3`).
  Reaching those 14 now needs a direct include. `curve_selector` and
  `dense_slice` are deliberately NOT in the joined set — both were in the
  umbrella throughout 0.1.0 and describe no change for an upgrading caller — and
  `portfolio` / `portfolio_risk` are not in the Tier-B set, because they were
  **removed outright rather than demoted** (see REMOVED below).
* **`Surface<Slice>`, `SviSurface`, `EssviSurface`, `C8Surface`, `CStarSurface`**
  moved to `detail/legacy_surface.hpp`, `detail/legacy_c8_surface.hpp`,
  `detail/legacy_cstar_surface.hpp`. The **namespace did not change** — they are
  still `atx::vol::` — so the migration is one added include. The canonical
  pipeline is `CurveSurface` (fit) → `PricedSurface`/`PricedSurfaceView` (serve)
  → `SurfaceSet` (portfolio), and public headers may no longer name the demoted
  containers even in prose.
* **The templated `derivatives.hpp` entries are now instantiated for
  `VolSurface`.** `var_swap_fair_strike`, `vol_swap_fair_strike`, `deriv_price`
  and `deriv_greeks` are templates on the surface type whose bodies live in
  `derivatives.cpp`; every instantiation used to be on a demoted container, so
  these Tier-A declarations could only be linked against by including a
  `detail/` header. `VolSurface` — which answers `iv(k_log, T)`, the template's
  whole requirement — joins the instantiation set, and the demoted pair stays
  for source compatibility. Purely additive: no existing call changes.

### BREAKING — error model: batch entries report how many lanes they wrote

Ten `Status`-returning batch entries now return `Result<std::size_t>`, carrying
the number of lanes written and defined only on success. `Result<T>` itself is
unchanged (`tl::expected<T, atx::core::Error>`); error codes and messages are
byte-identical.

`black76_price_batch`, `black76_price_from_lnfk_batch`,
`black76_value_and_vega_batch`, `implied_vol_batch`, `black76_greeks_batch`,
`essvi_w_batch` (`batch.hpp`); `american_price_batch`,
`american_price_batch_resolved`, `american_greeks_batch`
(`american_batch.hpp`); `american_implied_vol_batch` (`american_iv.hpp`,
Tier-A).

*Migration.* Inline uses (`if (f(...))`, `ASSERT_TRUE(f(...))`) bind unchanged —
both types are `expected`. Only declaration-form sites move:
`const Status st = f(...)` → `const Result<std::size_t> st = f(...)`, and
forwarding sites change `return st;` → `return Err(st.error());`. Output spans
and the per-lane `std::span<Status>` channel are untouched.

Two smaller shape changes: `configure_pricing_executor` returns
`[[nodiscard]] Status` instead of a discardable `bool`, and
`ticker_seed_profile` returns `std::optional<ProfileKind>` instead of taking an
out-param (`if (ticker_seed_profile(t, kind))` → `if (const auto k =
ticker_seed_profile(t); k.has_value())`).

### BREAKING — positional aggregate initialisation is no longer supported

`AlOpts`, `RunConfig`, `SessionInputs` and `SurfaceParityReport` were reordered
and pinned with a field-count `static_assert`. **Use designated initializers.**
`AlOpts{3, 3, 1, 1.0e-1}` is now wrong — `n_quad_price` moved from last to
third. Each of the four headers states that this is the last layout change
allowed; post-1.0 knobs append at the end with no positional promise. Python is
unaffected: keyword names, arity and signature are unchanged.

`RunConfig`'s pin moved **15 → 16** later in the same sprint when `cancel` was
added (see *Embedding* below). It was INSERTED beside `step_observer`, its
semantic group — not appended — which is precisely the freedom the new convention
buys and the old one forbade. Named initialisation is unaffected by construction;
a positional one would have rebound, which is why none is allowed to exist.

It then moved **16 → 17** when the release branch merged `main` (2026-08-02),
which brought the backtest-replay work's `RunConfig::prefetch_depth`
(`std::size_t`, default `2`). This one is **appended at the end**, the form the
convention prescribes for a new knob. Two notes a caller may care about:

* **It changes no output at any value.** `prefetch_depth` is purely an I/O
  schedule — how many future snapshots may be in flight — never which bytes are
  deserialised nor the order the economics consume them.
  `Backtest.PrefetchDepthIsBitIdenticalToSingleStepLookAhead` pins that
  bit-identity, and the SPY-dispersion NAV determinism legs reproduce their
  anchors bit-exactly across the merge that introduced it.
* **The default is `2`, not the historical single-step `1`.** It arrived from
  `main` at `1` and was moved to `2` in this release (v1 closeout sprint Task
  4.8, plan item 6.7) on a paired measurement: one binary with the depth
  alternated inside a single session, 12 interleaved rounds on the 135-session
  SPY-dispersion replay, medians and win-counts only — `1 → 2` **+15.2 % (11/12
  rounds)**, then `1 → 4` +19.8 % (10/12) and `1 → 8` +19.6 % (10/12), while `2 → 4` (+1.9 %,
  7/12) and `4 → 8` (+1.6 %, 7/12) are washes. The curve is a step, not a ramp:
  overlapping the first load is the whole win, so `2` is the cheapest default
  that takes it. **A run that wants the old shape sets `1` explicitly and gets
  it bit-for-bit** — by the note above, no value of this field moves a number.
  The cost of the new default is one extra in-flight snapshot and a private
  cache of `4` slots instead of `3`.
  `0` is still normalised to `1` — "no look-ahead" is expressed by
  `prefetch_snapshots = false`, not by a zero depth. A caller-supplied
  `snapshot_cache` must retain at least `depth + 2` entries or the LRU drops a
  completed prefetch before its step reaches it (costing throughput, never
  correctness); `run_backtest`'s private cache is sized from the field
  automatically.

Python's ARITY and keyword names are unaffected by both moves — the binding is a
hand-kept `def_readwrite` list, and `prefetch_depth` is exposed through it
(`python/src/bindings/backtest.cpp`) as an attribute, not as a constructor
keyword. A Python caller who never touches the attribute therefore picks up the
new default exactly as a C++ caller does;
`python/tests/test_backtest.py::test_run_config_prefetch_depth_round_trips`
asserts it.

### REMOVED

* **The deprecated `VolSurface`-bound portfolio engine**: `portfolio.hpp`,
  `portfolio_risk.hpp` and 34 symbols (`PortfolioLeg`, `LegKind`, `AggMode`,
  `bulk_price`, `scenario_pnl`, `project_compare`, …). Replacements, honestly:
  multi-shock scenarios → `scenario_grid.hpp`; theoretical legs →
  `contract_projection.hpp`; factor attribution → `pnl_attribution.hpp`; `ByUid`
  aggregation → `reduce_risk_buckets`. **Stock/cash legs, bulk selection, and
  the ByUidExpiry / ByGroupId aggregation views have no canonical counterpart.**
  Deleting the header also resolved a latent ODR conflict: `atx::vol::LaneStatus`
  had two different definitions, and the `american_batch.hpp` one survives.
  `scenario_grid.hpp` and `pnl_attribution.hpp` were promoted to Tier-A.
* **The `SurfaceArchive` v1 writer/reader** (`write_surface_archive[_file]`,
  `class SurfaceArchive`, `SurfaceArchiveWriteOpts`,
  `archive_identity_from_header`) and the `atx-vol-archive-v1` library.
  ATXVSA2 is the only shipped surface-archive format. The retired format's
  on-disk record declarations are kept as reference — `RunDir::run_identity_hash`
  still recognises such a file by its magic. Note this header used to declare
  symbols a plain `atx::vol` link could not resolve.
* `calib_pool.hpp` (`calibrate_pool`, `CadenceQueue`); `vola_parity.hpp`;
  `arb_project_calendar_essvi_total`; the four `derivatives.hpp` unit
  constexprs (`var_dec_to_points`, `var_points_to_dec`, `vol_dec_to_points`,
  `vol_points_to_dec`); `dispersion_build_schedule`, `dispersion_run_backtest`,
  `dispersion_verify`, `DispersionVerifyReport`.

### Numbers that moved

These change results without changing a signature — the category this file
exists for.

* **Deep-OTM put premia.** Black-76 puts are priced from `Φ(-d)` instead of
  `1 - Φ(d)` in `black76_aux`, `black76_value_and_vega`, `black76_greeks`, the
  implied-vol Halley loop and the AVX2 kernels. Far-wing values move; they were
  catastrophically cancelling to zero or negative.
* **AVX2 P&L.** The vector kernel adopts the scalar association tree, so
  `total == sum(terms)` holds and a position's P&L no longer depends on its
  batch index — a contract `simd/pnl_batch.hpp` already claimed.
* **Archive bytes and content identity are now reproducible.** Slice-params
  padding is no longer memcpy'd into archive records, so the same fitted slice
  produces the same `payload_crc32c` every time. Archives written by earlier
  builds are not byte-reproducible by this one.
* **`VolSurface::iv` returns NaN** for non-finite or non-positive `T` (was
  `+inf`).
* **A moved-from `PricedSurfaceView` is structurally empty** and answers no
  queries; it previously answered and could index out of bounds.
* **Vol-time is fail-closed.** `trading_hours_between`, `vol_time_years`,
  `time_to_expiry_years` and `tenor_years` return `Result<double>` instead of
  `double`, and `VolTimeCalendar` requires an explicit coverage window
  (`us_default()` covers 2024-01-01..2028-12-31). Out-of-window queries are
  `ErrorCode::OutOfRange` rather than a silently credited 7.5h session.
* **`all_symbols` / `universe_at`** lost their `index_symbol = "SPY"` default;
  the argument is required.
* **Corrupt archives that used to be accepted now fail with `ParseError`** —
  the LinearVariance node axis is validated and slice payload extents must be
  monotone and disjoint. Backtests fail closed on backwards snapshot timestamps.
* **`PortfolioPricer`'s returning `price()` / `pnl_explain()` are genuinely
  concurrent-const-safe** (per-call workspace), at the cost of the cross-call
  cached workspace.
* **`BacktestResult::validate()`** is new and enforced at `run_backtest` and the
  three TSV/CSV writers: every column must be empty or exactly `size()` long. A
  producer handed a skewed result now returns `InvalidArgument`.
* **Loose dispersion result TSVs are off by default**, behind
  `DispersionRunConfig::emit_tsv_diagnostics` (spec key of the same name,
  default `false`). Retained-input and evidence TSVs are unaffected.
* **`surface_insert_vol_slice(..., with_no_arb_check = true)` now actually
  checks.** The parameter used to be accepted and discarded, leaving
  `InsertedSliceHandle::no_arb_status == 0` unconditionally. It now runs a dense
  butterfly/calendar sweep over the resolved slice and reports through
  `no_arb_status` (`kNoArbStatusButterfly` / `kNoArbStatusCalendar` /
  `kNoArbStatusNotEvaluated`) plus the `kFlagNoArbWarning` provenance bit. It is
  a report, never a rejection — the handle is still returned, with the same
  numeric contents. The default (`false`) path is unchanged and still costs
  nothing, so no shipped caller's numbers move.

### NEW — every public batch kernel is now reachable from Python

Three `batch.hpp` entries were bound, which completes the set — verified by
enumerating `batch.hpp`'s six entries against the module rather than by
inspection, because an earlier draft of this section claimed completeness while
`black76_price_from_lnfk_batch` was still unbound:

* `black76_greeks_batch(F, K, T, sigma, r, df, side)` — a dict of SoA numpy
  columns (`delta`/`gamma`/`vega`/`theta`/`rho`/`vanna`/`volga`/`charm`/`price`).
* `black76_value_and_vega_batch(F, K, T, sigma, df, side, sqrt_t=-1.0)` —
  `(value, vega)` for one expiry slice (`T` and `sqrt_t` shared, as in the C++
  signature).
* `black76_price_from_lnfk_batch(F, K, T, sqrt_t, sigma, df, ln_fk, side)` — the
  bind-step shortcut for a caller that already holds `ln(F/K)` and `sqrt(T)`.
  `sqrt_t` is **required and has no sentinel** here: the scalar kernel consumes
  it verbatim, so there is no negative value meaning "recompute". Its scalar
  companion `black76_price_from_lnfk` is bound alongside it, so the batch's
  bit-exactness is checkable from the same interpreter.

Binding-only: no kernel changed, and all three go through the validated
`batch.hpp` entry points rather than the raw `simd::` kernels.

Three notes for callers:

* **None returns a per-lane `status` column, and that is the NaN + per-lane
  convention rather than a departure from it.** A parallel status exists where
  the kernel HAS a per-lane failure channel a binding must not erase
  (`implied_vol_batch`'s `span<Status>`; the American batch's `LaneStatus`).
  These three have none — `black76_greeks`, `black76_value_and_vega` and
  `black76_price_from_lnfk` are `noexcept` total functions whose degenerate lanes
  collapse to the documented intrinsic result — so an all-`STATUS_OK` column
  would advertise a diagnostic carrying no information. Only a malformed *call*
  raises.
* **Greeks and value+vega agree with their scalars to the SIMD gate, not
  bit-for-bit; from-lnFK is bit-identical.** The first two dispatch to AVX2 at
  `n >= 4`; from-lnFK has no vector kernel. The gate is per output column —
  ~1e-6 absolute + 1e-7 relative on prices and Greeks, ~1e-5 absolute on the
  fused batch's `vega`.
* **`side` now also accepts a single `Side`, broadcast across the batch**, on
  every binding that takes a `side` column (the American batches included). Pure
  widening: a per-lane integer column behaves exactly as before, and a float
  column is still refused with `ErrorCode::InvalidArgument` rather than
  truncated onto `Side::Call`.

### NEW — embedding: a diagnostic sink and cooperative cancellation

The two things a library has to offer before a host can embed it: give up the
process's streams, and be stoppable.

* **`atx/vol/log.hpp` (Tier-B) — diagnostic sink.** `install_log_sink(sink, user)`
  routes every diagnostic atx-vol emits to a callback carrying a `LogLevel`, a
  `LogStream` and one newline-free line. **All 13 library stream writes across 5
  source files now go through it**; no `fprintf`/`printf`/`cerr`/`cout` to a
  process stream remains in library code.
  **With no sink installed, output is byte-identical to 1.0.0-pre**: the same
  text on the same stream, so this is not a behavioural change for any existing
  consumer. The stream is carried on the record rather than derived from the
  level, precisely so the two Info-level sites that historically wrote to
  *different* streams both stay unchanged.
  The callback must not throw (the emit path is `noexcept`), must not re-enter
  atx-vol, and **must tolerate concurrent invocation** — pricing-pool workers
  emit, so records arrive on threads the host never created, and record order
  across threads is not defined. Install once, before the first emitting call.
* **`ErrorCode::Cancelled` (atx-core)** — appended last, so no existing
  enumerator's `u16` value moved.
* **Cooperative cancellation on the four long-running entries.** A `CancelToken`
  (`atx/vol/types.hpp`) is a non-owning view of a caller-owned
  `std::atomic<bool>`; a default-constructed one never cancels and costs one
  branch per poll. Plumbed as `RunConfig::cancel` (**this is the 15 → 16 field
  above**), `CorpusConfig::cancel`, `SurfaceDbPopulateConfig::cancel`, and — for
  the run-dir-only entry that has no caller-supplied config — a defaulted
  trailing parameter on `dispersion_run_projected_var`.
  Cancellation is a **clean early return with `ErrorCode::Cancelled`, never a
  partial write**. Each entry polls at the top of a loop iteration, before that
  iteration writes anything: `run_backtest` returns no result at all (and writes
  no files in any case); `build_corpus` leaves no manifest, so the corpus never
  claims to be complete; `populate_surface_db` leaves a **valid database holding
  a prefix of the dates**, because each date is committed atomically with a
  generation-bumped manifest — stop a long backfill and re-run to resume;
  `dispersion_run_projected_var` writes its artifacts only after the work it
  cancels, so the run dir is untouched. The two fan-out entries (`build_corpus`,
  `populate_surface_db`) additionally poll at the **top of each fit task**, so a
  stop drains the queued fits instead of running them to completion — the cancel
  shortens the run rather than only declining to publish its index. A fit already
  **in flight** is never abandoned: the call returns once the boards already
  running finish.

### Packaging, versioning and ABI

* **`find_package(atx-vol)` works from an install prefix.**
  `cmake --install <build> --prefix P` ships headers, static archives and
  `atx-volConfig.cmake`; the exported targets are `atx::vol`, `atx::core`,
  `atx::tsdb`, `atx::vol-tools`, `atx::vol-research`, with `atx::vol::tools` /
  `atx::vol::research` recreated so in-tree source compiles against the install
  unchanged. `Result<T>` is still `tl::expected<T, atx::core::Error>` and
  `tl-expected` installs into the same prefix.
* **The version is single-sourced** from `project(atx VERSION ...)` through a
  generated `atx/vol/detail/version_generated.hpp`. `atx::vol::version()` no
  longer carries its own literal. New: `ATX_VOL_VERSION_{MAJOR,MINOR,PATCH}`,
  `ATX_VOL_VERSION_STRING`, `ATX_VOL_VERSION_NUM(a,b,c)` and `ATX_VOL_VERSION`
  for preprocessor feature-gating, plus `atx::vol::kVersionString`.
* **Package compatibility is now `SameMajorVersion`** (was `SameMinorVersion`,
  correct only while the version was 0.y.z). A `find_package(atx-vol 1.0)`
  consumer accepts any 1.z.
* **atx-vol 1.x is distributed static-only**, with no `ATX_VOL_API` export
  macro — see the *Linkage and distribution policy* section of `README.md` for
  why (header-inline instrumentation globals get one instance per image on
  Windows). `BUILD_SHARED_LIBS` now fails configure with the reason instead of
  being silently ignored, and `cmake --install` refuses a shared build.
* **The `ATX_VOL_COUNTERS` / `ATX_VOL_PROFILE` ODR trap is closed.** Those
  options change the definition of inline entities in
  `atx/vol/detail/counters.hpp` and `atx/vol/detail/phase_profile.hpp`; a
  consumer that disagreed with the library used to silently read a plane nobody
  wrote. The configuration is now part of an inline namespace name, so a
  mismatch fails to **link**, naming both sides. No struct layout changed and no
  computed value moves. `ATX_VOL_PROFILE_CONCAT[_INNER]` are renamed
  `ATX_VOL_PROFILE_DETAIL_CONCAT[_INNER]` and defined in both configurations.
* **Archive format naming is unified on the on-disk magic**: the live format is
  **ATXVSA2** (magic `ATXVSA20`) and the retired one is **ATXVSA03** (magic
  `ATXVSA03`). The old "v1" / "v3" ordinals are gone from the headers — they
  named the same format both ways. Comment-only; no identifier changed.

### REMOVED / NEW — the bespoke strangle-vs-varswap strategy is now a declarative spec (swap-lane DSL sprint)

**Removed.** `StrangleVsVarswapStrategy` + `StrangleVarswapConfig`
(`strangle_varswap.hpp/.cpp`), the 600-line
`atx-vol-strangle-varswap-driver`, and their test suite. The class was a
one-analysis special: its cycle lifecycle, swap sizing and signal mirror were
generic machinery trapped in bespoke code.

**New, in its place — the grammar now expresses the whole analysis:**

* `LifecycleSpec::Holding::FixedExpiryRestrike` — a cycle fixes ONE expiry
  (ceil-snapped onto `StrategySpec::session_ts`; the legs' shared
  `tenor.target_T` is the cycle tenor) and every entry tick restrikes the
  option legs at it; keep-strikes on soft resolution failures with the
  `skipped_restrikes` / `unopened_entry_steps` counters; `Entry` is the
  restrike cadence.
* `StrategySpec::swap_legs` (`SwapLegSpec` + `SwapSizeSpec`:
  `FixedQty` / `TargetVega` / `MatchGroupVega`) — one fair-struck swap leg per
  cycle, opened on the cycle-open step only, every refusal counted in
  `skipped_swap_cycles`. Empty `swap_legs` is bit-identical to the old
  grammar (the additive-lane rule).
* `swap_leg.hpp` — the reusable toolkit: `swap_contract_for_lot` (the engine's
  `SwapLot`→`DerivContract` transcription), `solve_cycle_swap` (fixing-count +
  bridge-priced fair strike + vega-targeted qty, fail-soft by contract), and
  `SwapSignalProbe` (the engine-accrual mirror behind the
  `swap_delta/gamma/vega/theta/rho` signal columns, NaN discipline included).
* `DeclarativeStrategy::signals` — emitted only when the spec carries swap
  legs: the probe's five columns + `options_vega` (the old `strangle_vega`,
  renamed lane-agnostic; `tools/render_strangle_vs_varswap.py` reads either
  spelling) + cumulative `skipped_restrikes` / `skipped_swaps`.

**Migration.** `examples/varswap_compare_example.cpp` is the old driver's
replacement: the full comparison as a ~20-line spec. Old config fields map
1:1 (`target_abs_delta` → the strangle selectors, `tenor_years` →
`tenor.target_T`, `contracts` → `FixedContracts`, `enable_swap_leg` → the
`swap_legs` vector).

**Why the numbers are trustworthy.** The deletion sat behind a track-parity
gate: old vs new through the same engine on the full XOM 2026 fixed-db window
(137 sessions, both legs, delta-hedged) — per-row `nav`/`pnl_total` within
7e-9 dollars, `swap_pv`/`swap_pnl` bit-identical, identical lot-id watermarks
and skip counters; plus synthetic-corpus parity including a dark-session
keep-strikes run. The example reproduces the reference track to the cent
(combined −7499.69 / swap −6065.38 / strangle −1434.31).

### FIXED — calendar level repairs no longer fabricate slice levels from extrapolated-wing crossings (fit fidelity budget + tradeable-overlap band)

**The defect.** Every parametric calendar repair — `arb_project_calendar_svi_pair`
/ `_essvi_pair` / `_c8_pair` at the `fit_slice_curve` serving seam, and the
surface-level `arb_project_calendar_svi` / `_essvi` in `run_surface_parity`'s
`CalendarRepair::Project` branch — closed w(k)-crossings by shifting or scaling
the longer slice's LEVEL (`a` / `theta`) by the WORST-CASE deficit over a fixed
k-band (±0.6 at the pair seam, **±3.0** in the Project branch). For slices
quoted to |k| ≲ 0.1–0.3 (every weekly and most mid-tenor chains), nearly that
whole band is extrapolation on BOTH slices: the closed forms' wings there are
unidentified by any quote, so the "crossing" was fiction — and the repair
converted it into a real ATM error, slice after slice, since each repaired slice
becomes the next pair's floor. The result passed every downstream gate
(shape-preserving shifts keep butterflies clean; calendar is clean by
construction; risk admission checks geometry, not fidelity to quotes) and was
stored. On the sp100-2026 database this served XOM 2026-02-18 mid-tenors at up
to **+25 vol pts over their own quotes** (43d ATM: quotes 28.4, served 53.4; the
SVI `a` shifted 0.0091 → 0.0331 by a k=±0.6 wing deficit), and — because the
fitted wing shapes flip day to day — produced the ±8–11 pt one-day ATM
spike-reverts on XOM/CVX that surfaced as artificial P&L spikes in the
strangle-vs-varswap backtest.

**The fix, structural.**
1. **Pair projections act only on the tradeable overlap** of the two slices'
   data-supported k-ranges (∩ the risk band) — the exact rule
   `SplineVolCurve::project_calendar` already applied; the parametric branches
   now follow it. `fit_curve_surface` tracks each committed slice's observation
   k-range and hands it to the next pair (`prev_data_k_range`, previously
   populated only for SplineVol). A crossing with no traded witness no longer
   moves any level.
2. **Every level repair carries a fidelity budget**
   (`kCalendarRepairMaxAtmShiftFrac = 0.10` of the slice's pre-repair ATM total
   variance, floor `1e-6`): a repair that would move the ATM further returns
   `Unavailable` and leaves the slice/surface untouched (all five projectors are
   now transactional). Failure flows into the existing honest paths: the slice
   is dropped (soft) or the risk candidate walks the family ladder.
3. **`CalendarRepair::Project` repairs over the certified band** (±0.5,
   `static_assert`-tied to both `RiskSurfaceValidationConfig` and
   `strip::kCertifiedWingHalfBand`), not the ±3.0 diagnostic grid — repair now
   covers exactly what admission checks. The ±3.0 grid remains as the
   `calendar_arb_free` DIAGNOSTIC and may now honestly report unrepaired wing
   crossings.
4. **The SVI fallback ladder reaches the dense rung** (`kFromSvi` gains
   `LinearVariance`, which the risk path substitutes with `ConvexDense`) — the
   one family list that did not descend to the direct-variance curve its own
   contract promises. Invisible while the fabricated repair force-admitted
   every SVI candidate; load-bearing now that rejection is honest.

**Effect on existing callers.** Numbers move wherever a calendar repair used to
fire on out-of-overlap or beyond-budget crossings — deliberately: those numbers
were fabrications. Boards whose parametric candidates genuinely cannot
reconcile quotes with in-band calendar monotonicity now serve the dense model
via the ladder (XOM 2026-02-18 does exactly this) or drop the offending slice.
Re-fit of the poisoned XOM cell: every tenor within ~1 vol pt of its own
quotes (worst tenor was +25.0). Gates: `ArbProjectCalendarPair.*Refuses*`,
`ArbProjectCalendarSvi/Essvi.Refuses*`,
`VolCurve.SviPairProjectionActsOnlyOnTheTradeableOverlap` /
`SviPairProjectionStillRepairsInsideTheOverlap`.

**Databases built before this fix carry the fabricated levels** — the carry-over
fingerprint does not cover the fitter, so a resume will NOT repair them (see
"Re-running at the corrected --r does NOT repair the database" in
docs/surface-db-build.md for the identical mechanism). Rebuild affected
symbols' partitions from the hive, or rebuild into a fresh root.

**Validated on a full XOM+CVX 2026 rebuild** (release binary, fresh root
`scratch-fitfix-2026`, 140 sessions): CVX 140/140, XOM 137/140 with three
honestly-rejected boards (2026-02-13 / 02-18 / 06-11: butterfly- or
calendar-inadmissible on every ladder rung; the backtest engine's documented
dark-day drop handles them). Daily 3M ATM fit noise: XOM 3.28 → **0.93**
vol pts/day, CVX 5.28 → **0.92** — both now indistinguishable from AAPL/MSFT —
with the spike-revert autocorrelation gone (lag-1 −0.50 → −0.17) and the
largest one-day ATM move 11.6 → 2.6 pts. The strangle-vs-varswap reference
run's legs land on the same scale (strangle −$1,434, swap −$6,065 over the
window). An AAPL one-cell control rebuild serves IVs identical to ~1e-10:
surfaces whose repairs never fired are numerically untouched.

### CHANGED — the variance strip now reads flat-vol tails beyond the certified wing band (wing clamp)

**What changed.** `var_swap_fair_strike` (and everything that prices through it:
`deriv_price` on every swap kind, `deriv_greeks`, the backtest swap lane's daily
marks) now clamps its SURFACE READS to a wing trust band. Strip nodes beyond the
band keep their true strikes but price under the BAND-EDGE vol — flat-vol tails
over the wings, never a truncated span, so the log-contract replication stays
complete. The band is `DerivConfig::wing_clamp_k`: `0` (the default) selects the
certified validation band `strip::kCertifiedWingHalfBand = 0.5` — the |k| ≤ 0.5
band `RiskSurfaceValidationConfig` actually certifies, `static_assert`-tied so
the two cannot drift — `> 0` is an explicit half-band, `< 0` restores the old
unclamped reads, `NaN` is `InvalidArgument`. A new provenance flag
`DerivFlags::WingClamped` (structural: "tails were in effect", set even when the
smile is flat and the clamp moves nothing) records it on every quote. The strip
span, node count, adaptive widening and truncation flags are untouched: this
clamps *reads*, not the grid.

**Why.** A parametric eSSVI/SVI slice serves an *unbounded linear-in-|k|*
extrapolation at any k, no quote ever disciplines it beyond roughly the ATM
region, and the fit pipeline certifies nothing outside |k| ≤ 0.5 — yet the
Standard strip integrated it to ±1.5 with 1/K weighting. On the sp100-2026 XOM
corpus that fiction read 82 vol at k = −1.0 (swinging ±22 vol pts/day), inflated
the 3M fair strike to ~38 vol against a ~30 ATM and ~29.6 realized, and put
~98% of the daily mark variance beyond |k| = 0.25 — the mechanism behind the
XOM 2026 reference run's swap leg bleeding −$42k with ±$90k single-day marks
and sign-flipping FD gamma. With the default clamp the same corpus prices
~33.9 with ~3.5 vol pts/day of mark noise, in line with the ATM's own 3.3.

**Effect on existing callers.** This moves K_var on ANY surface with wing
structure beyond ±0.5 — deliberately. A flat or near-flat surface is unchanged
(band-edge vol == wing vol), so the analytic `K_var == σ²` contracts all hold;
a caller whose wings ARE quote-disciplined and who wants them integrated raw
sets `wing_clamp_k = -1.0` (or any negative) and gets the pre-clamp number
bit-for-bit. Gate: `WingClamp` (tests/derivatives_test.cpp), including a
node-for-node flat-tail oracle at 1e-12.

**The XOM 2026 reference run, re-taken under the clamp** (same corpus, gen 225,
same window/config; artifacts in `xom-strangle-varswap-2026-wingclamp/`):
strangle **+$1,501.64 — bit-identical**, the option lane never touches
`DerivConfig`; variance swap **−$10,436.00** (was −$42,468.19); per-cycle swap
P&L +$32,009 / −$35,471 / −$6,974 against strangle +$80,743 / −$50,173 /
−$29,068 — the two legs finally live on the same scale. Daily swap-mark noise
$18.0k (was $24.5k); what remains is the in-band sp100-2026 fit noise (ATM
itself moves 3.3 vol pts/day on this corpus), which is the refit's job, not the
pricer's. `swap_gamma`'s sign still flips day to day — that is a CONVENTION
fact, not a defect: marks read the surface at fixed log-forward-moneyness, so
the smile floats with a spot bump and a var swap's FD gamma is numerical noise
around zero.

### NEW — strangle-vs-varswap comparison backtest + the XOM 2026 reference run (strangle-vs-varswap sprint, Tasks 1-5)

**What shipped.** `strangle_varswap.hpp`/`.cpp` add
`StrangleVsVarswapStrategy`, an `IStrategy` that runs one fixed-expiry,
daily-restriked 40Δ strangle against one uncapped variance swap struck fair and
sized to that strangle's **entry** vega, both on a single clock and
delta-hedged daily. Equal vega is enforced in DOLLAR terms (per-share vega ×
contracts × multiplier) at each cycle open, and the swap's strike comes from
the `deriv_price` bridge's `fair_strike_dec` — not the `PricedSurface`
`var_swap_fair_strike` — so strike and engine mark share one carry and the swap
opens at a bitwise `+0.0` PV. Per-step `swap_delta/gamma/vega/theta/rho`,
`strangle_vega` and cumulative `skipped_restrikes`/`skipped_swaps` counters ride
out as strategy signals. `examples/strangle_varswap_driver.cpp` (target
`atx-vol-strangle-varswap-driver`, `ATX_BUILD_EXAMPLES`) drives it over a
`SurfaceDb` and writes `track.tsv`; `tools/render_strangle_vs_varswap.py`
renders the five-panel comparison report.

**Effect on existing callers.** Additive only — a new header, a new example
target, a new Python tool, and one new test module. No library type, engine
path or existing test changed; `write_backtest_pnl_tsv`'s frozen column set is
deliberately *not* widened (its `{name, order}` is `static_assert`-pinned to the
RunArchive registry that feeds `ra_schema_hash()`), so `swap_pv`/`swap_pnl`
reach the TSV as signal columns instead and the schema hash is untouched.

**Operational contracts worth knowing.** A live variance swap makes the engine
fail the whole run closed on any missing board — `unpriced = ExcludeAndReport`
governs option lots only and is no escape for the swap lane. The driver
therefore probes every partition in the window and builds its clock from the
sessions where the symbol's surface exists; a partition the manifest lists whose
*archive will not open* is reported as an inconsistent database (exit 1), not as
a dark session, so a broken db can never silently shorten a measured window. The
final cycle of a corpus whose calendar the tenor outruns may legitimately run
one-legged (`skipped_swaps ≥ 1`, no swap lot). `swap_theta` is legitimately NaN
within one bump width of expiry while the swap is live, so liveness is read off
`swap_vega`.

**The reference run** (`XOM`, `sp100-2026` gen 225, 2026-01-02 → 2026-07-24,
139 sessions, 1 dark, three 91d cycles) is recorded for provenance:
strangle **+$1,501.64**, variance swap **−$42,468.19**, combined
**−$40,966.55**. The equal-vega identity held to 0 ULP at all three cycle opens,
`reconcile_nav` held on every row, and the leg split closes back to NAV to
1.5e-11.

**Read that run's economics with the corpus in mind.** The two legs' daily P&Ls
are essentially *uncorrelated* on this data (ρ = +0.038), and the swap's path is
the *noisier* of the two (daily σ $24,478 vs $21,739). That is a property of the
corpus, not of the strategy: each leg is independently reproducible from the raw
surface — the strangle from the 40Δ/ATM implied-vol move (R² = 0.69) and the
swap from a 1/K²-weighted replication of the variance strip (R² = 0.81) — but in
this corpus those two regions of the surface are themselves only ρ = +0.24
related, because the deep put wing carries ~27 vol points/day of fit noise
(vs 3.3 at ATM) that is *anti*-correlated with the ATM move (ρ = −0.39), and
every strike's daily IV change has lag-1 autocorrelation ≈ −0.5 — the signature
of independent per-day fit noise rather than a coherent vol path. A variance
swap integrates exactly that region. Treat the sp100-2026 wings as unfit for
strip-sensitive P&L attribution until they are refit.

### NEW — vol-derivatives production surface: capped/mid-life swaps, greeks, dated fixings, DerivBook (derivatives-production sprint, Tasks 1-10)

**What shipped.** `derivatives.hpp` gains two capped product kinds
(`DerivKind::CappedVarSwap`, `CappedVolSwap`) plus a mid-life dispatch for
`VolSwap` contracts with `0 < n_done < n_total` (previously priced only at the
two exact-aged endpoints, inception and full accrual). All three price their
model leg against a lognormal distribution for the future realized-variance
leg (Gauss-Hermite / split-domain Gauss-Legendre quadrature,
`detail/rv_lognormal.hpp`): closed-form for the capped variance swap, kinked
split-domain quadrature for the capped vol swap, plain Gauss-Hermite for the
smooth mid-life sqrt payoff. A new `DerivConfig::vol_of_vol` knob (0 =
auto-calibrate so the lognormal's `E[sqrt(W)]` reproduces the surface's own
Carr-Lee `K_vol`) drives all three. `deriv_greeks` differentiates every
product kind via sticky-strike finite differences, with the center's resolved
strip grid and any auto-calibrated vol-of-vol pinned into every bump so a
bumped evaluation cannot land on a different quadrature scheme than the
center. `RealizedTracker::observe_dated` adds a strictly-ascending-timestamp
fixing entry point for daily-fixing drivers. New `deriv_book.hpp` prices a
book of swap positions against a `SurfaceSet` (additive companion to
`portfolio_pricer.hpp`'s option book, combined via `combine_totals`), and
`backtest.hpp`'s strategy-aware engine gains an additive variance/vol-swap
lane (`PortfolioState::swap_lots`, held to expiry, no early close in v1).

**Effect on existing callers.** Additive only. Every new type/field defaults
to the prior behavior: `DerivKind::VarSwap`/`VolSwap` dispatch is unchanged,
`DerivConfig::vol_of_vol = 0.0` auto-calibrates but that resolver is reachable
only from the new capped/mid-life dispatch paths, and
`PortfolioState::swap_lots` defaults empty (an empty-swap-lots book prices,
accrues and settles exactly as it did before this lane existed). One field's
OBSERVABLE SENTINEL does change: `DerivQuote::integration_error_est` was
unconditionally `NaN` (documented as "this port does not yet run the
Richardson half-step refinement"); `var_swap_fair_strike` now populates it
with a real Richardson half-grid estimate (`|I_h - I_2h|/15`) whenever the
resolved strip node count is `4m+1` — every quality-tier default and the E2
adaptive-wing rescale land there. A caller gating on `(x == x)` to mean "not
estimated" now sees a real number on those grids; a caller-pinned
`strip_nodes` that isn't `4m+1` still leaves it `NaN`, unchanged.

**Not shipped.** The RV distribution-affine / Monte-Carlo QE pricing engines
and the discrete-monitoring full-Monte-Carlo correction remain reserved and
actively return `NotImplemented`. CBOE variance-future marking
(`DerivMarkingConvention::CboeVarianceFuture`) is declared but unenforced — no
pricing path reads `DerivContract::marking` yet. `BacktestDb` refuses (rather
than silently drops) a run, checkpoint, or append that actually carries swap
data — its checkpoint and series schema predate the swap lane; schema support
is a deferred follow-on. Swap-lot entry is frictionless (zero cost, no
spread/impact) in v1, and `DerivBook` prices its positions single-threaded.

### BREAKING — `DispersionConfig::target_vega` is now dollars per VOL POINT (E1 / AN-P1-1)

**What changed.** `build_dispersion_book` (the projected / surface dispersion
route) sized the index leg as

```
straddle_qty = target_vega / (straddle_vega * multiplier)
```

where `DispersionLeg::straddle_vega` is a per-share `dP/dsigma`, i.e. vega per
**unit** vol. The listed route (`build_listed_dispersion_roll`) has always sized
off `vega_per_contract_per_vol_point = vega_per_unit_vol * multiplier * 0.01`,
i.e. vega per **vol point**. The same conceptual knob therefore carried two
conventions 100x apart: `target_vega = 10000` built a projected-route book
carrying \$10,000 of vega per 1.00 of sigma (\$100 per vol point) while the
listed schedule built one carrying \$10,000 per vol point.

Cross-route PnL and parity comparisons were meaningless unless the caller knew
to rescale by hand.

**The canonical unit is now dollars of gross index vega per ONE VOL POINT** — a
0.01 move in sigma. This is the industry convention and the unit the listed
route already used. `build_dispersion_book` divides by
`straddle_vega * multiplier * 0.01`, textually matching the listed route.

**Effect.** For an unchanged `target_vega`, projected-route books GROW BY EXACTLY
100x — contract counts, gross notional, premium, PnL and NAV all scale by 100.
`K`, `T`, `sigma`, `straddle_vega`, `call_mark` and `put_mark` are unchanged
bit-for-bit; only `straddle_qty` (and everything downstream of it) moves.

Note: the sprint plan's parenthetical said projected books would "shrink 100x".
That is the wrong direction — dividing by an extra factor of 0.01 makes the
quantity larger. The plan's normative formula (divide by
`straddle_vega * multiplier * 0.01`) and its cross-route test gate are both what
is implemented here.

**Migration.** Any caller tuned against the old projected-route behaviour must
DIVIDE its `target_vega` / `gross_index_vega` by 100 to keep the same book size.
This includes `DispersionBacktestConfig::gross_index_vega` and the
`gross_index_vega` key in `run_spec.tsv` for surface-route backtest runs.
Callers that were already feeding the listed route's number now get a book that
matches it, which is the point.

**Migration also applies to RISK LIMITS, and silently if you skip it.**
`DispersionRiskLimits::max_gross_vega` / `max_gross_notional` and
`DispersionRunConfig::capital` (`dispersion_run.cpp`, `strategy.hpp`) are
compared against a book that is now 100x larger. Following the migration above
(divide `gross_index_vega` by 100) DOES fix them, because
`measure_book`/`binding_limit` scale with the book — but a spec that sets a
limit and is NOT migrated starts CLAMPING or HALTING with no error: the same
limit value now binds at 1/100th of the intended book size. The failure mode is
a book that quietly stops trading, not a diagnostic. Migrate the limits with the
target, or set them to 0 (unlimited) while you do.

**Affected surfaces.** `DispersionConfig::target_vega`,
`DispersionBacktestConfig::gross_index_vega`, the `gross_index_vega` run-spec
key, `dispersion_run_surface_backtest`, `dispersion_run_projected_var`, and
every artifact they emit. The listed schedule route
(`gross_index_vega_target_per_vol_point`) is UNCHANGED — it was already correct.

**Golden replay pins.** The 82-session and 135-session `surface_backtest.tsv`
reproducibility pins move as a direct consequence (position scale -> NAV scale).
Re-pinning is a single coordinated event owned by the sprint controller; see the
disp-hotpath STATUS doc.

**Test gate.** `ListedDispersionSchedule.ProjectedAndListedRoutesAgreeOnVegaUnit`
builds both routes over the SAME three `PricedSurface`s at the same tenor,
multiplier and target, and asserts the two index-leg dollar-vega-per-vol-point
figures agree to 5%. Before this change it failed by exactly 100x (projected
100 vs listed 10000).

### FOLLOW-UP — the X3 gross-vega limit now honours the multiplier (C-2)

E1 above migrated the SIZING to dollars per vol point but left the X3 risk probe
(`measure_book`, `dispersion_strategy.cpp`) summing a bare
`|straddle_vega * straddle_qty|`. That expression discards the `multiplier` the
function is handed AND the per-vol-point scale, so
`DispersionRiskLimits::max_gross_vega` was compared in the advertised unit only
at the historical `multiplier == 100`; elsewhere the measured exposure was off by
exactly `100 / multiplier` (10x under-reported at 1,000, 10x over-clamped at 10).
`multiplier` is a real typed run-spec key on this branch, so non-100 books are
reachable from production.

**Effect.** `max_gross_vega`, and the `risk_clamp_scale` / `risk_breach_reason`
telemetry it drives, are UNCHANGED at `multiplier == 100` (the default, and the
82-session golden's value — which also configures no limits at all, so the golden
path never measures). At any other multiplier the measured gross vega changes by
`multiplier / 100`; a spec that pins both a non-100 multiplier and a
`max_gross_vega` must restate the cap in dollars per vol point.

The conversion now lives once, in `contract_vega_per_vol_point` (dispersion.hpp).
Projected sizing, the listed schedule's `vega_per_contract_per_vol_point` column
and its round-trip validator all adopt it with the same operand association, so
those three are bit-identical. Guarded by
`Strategy.DispersionGrossVegaLimitIsDollarsPerVolPointAtNonHistoricalMultiplier`,
whose oracle (`2 * target_vega`, for any multiplier) is hand-derived from the
sizing contract rather than re-evaluated from the code.

### NEW — theo-vol overlay engine, breakeven-vol label pipeline, and an ML seam (theo-module sprint, Tasks 1-10)

**What shipped.** Three new Tier-B headers (bumping the Tier-B count 31 → 35
across this sprint, including a pre-sprint `var.hpp` drift sync folded in on
the way): `realized_vol.hpp` (five OHLC realized-vol estimators —
close-to-close, Parkinson, Garman-Klass, Rogers-Satchell, Yang-Zhang — plus a
trailing-window `RvPanel`); `breakeven.hpp` (a four-layer label pipeline:
`bev_replay_pnl` fixed-sigma delta-hedged single-option replay,
`solve_breakeven_vol` bisection root-find, `solve_breakeven_batch`
deterministic parallel fan-out, `load_bev_path` real-surface-corpus loader);
`theo.hpp` (`TheoEngine`, a theo-vol overlay measure composed beside a
`PricedSurface`'s served mark — `theo_vol`/`theo_price` are bit-identical to
the surface's own `iv()`/`fair_value()` with zero overlays engaged, by
construction, not by tolerance). Two shipped overlays (`make_rv_blend_overlay`,
`make_event_var_overlay`) and an `IFairVolModel` ML seam
(`load_linear_fair_vol_model` + `make_fair_vol_model_overlay`, a fixed
8-feature versioned schema) compose additional vol-space adjustments onto the
engine. `compute_theo_sheet` is a batch convenience over the engine's
allocation-free `value_into` hot path. A new example driver,
`examples/bev_label_factory.cpp`, walks a delta-lattice strike ladder across
entry dates and emits a byte-deterministic TSV of breakeven-vol labels —
target (`log_ratio`) plus join keys and solve diagnostics only, NOT the
`kFairVolFeatureSchemaV1` feature block `IFairVolModel` implementations
consume (final-review I2 correction: that feature block is assembled offline
by the trainer, from the surface corpus plus a separately-computed RV
history — see `theo.hpp`'s ML-seam banner). `TheoEdgeSignalStrategy`
(`examples/spy_leaps_strangle_backtest.cpp`) is a read-only `IStrategy`
decorator that records `theo_edge_atm`/`theo_band_atm` per step without
altering any order, hedge, or NAV path — verified byte-identical to the
undecorated run on real SPY 2019 data (899/899 shared `track.tsv` cells) in
a one-off manual A/B whose artifact was not preserved (requires the local
`spy-2019` corpus to reproduce) and whose arms also differed in snapshot
surface backing; the decorator has no automated test coverage (see the
sprint summary's Validation state).

**Engine extension, not a parallel engine.** `bev_replay_pnl`'s
implementation is appended to the end of `src/backtest.cpp` specifically to
reuse that file's local `HedgeLedger` share ledger (`add`/`get`) and mirror
the existing engine's rebalance/settlement/financing conventions (the
single-instrument rebalance ordering is re-derived inline, not delegated);
`git diff` on that file is exactly one inserted `#include` line and a pure
append after the existing close. `run_backtest`'s step loop, `RunConfig`, and `HedgeLedger`'s
class definition are byte-untouched.

**Effect on existing callers.** None. All three new headers are Tier-B,
reachable only by explicit include, outside the `vol.hpp` umbrella; no
existing signature, default, or served number changes. The one library file
touched outside the new headers (`src/backtest.cpp`) gains only an
`#include` and an append — no existing symbol in that file is modified.

**Not shipped (see the sprint summary's residual-work register for the
full list).** Quantile heads on `IFairVolModel` (`OverlayAdjust::band` from
the fair-vol overlay is a `|dvol| * 0.5` placeholder); `TheoFlagBits::
Extrapolated` is declared but never set (no surface extrapolation predicate
exposed yet); label storage is TSV-only (no Parquet — house rule, revisit
with the lakehouse); dividend/borrow inputs for single names and purged-CV/
embargo tooling stay Python-side; the theo signal probe assumes a SPY corpus.

### NEW — BEV label factory emits the full fair-vol feature schema; corpus batch runner and QA report (feature-factory sprint)

**What shipped.** `bev_label_factory` (`examples/bev_label_factory.cpp`)
closes roadmap gap G1 (`docs/research/2026-08-09-theo-ml-alpha-iteration-
plan.md` §1), with the close-to-close and day-resolution-events caveats
below: an optional `--events <tsv>` calendar input (`iso_date_to_ns`,
midnight-UTC epoch ns per announcement date; `load_events_tsv`; an empty
or omitted path means no calendar loaded, `n_events_to_expiry` NaN for
every row — never conflated with a loaded-and-empty calendar's `0`); a
one-time spot-history pre-pass (`load_spot_history`) feeding a
per-entry-date trailing realized-vol panel (`RvEstimator::CloseToClose`,
252.0 annualization, up to `kRvHistoryBars = 253` spot-mirror closes,
tallied by a new `n_entry_dates_rv_short` counter when the trailing window
is too short); and the full `kFairVolFeatureSchemaV1` eight-column feature
block (`theo.hpp`) appended to every label row, assembled by the driver
itself rather than left to an offline trainer join (`theo.hpp`'s ML-seam
banner is updated to match). The label TSV grows from 14 to 22
tab-separated columns: `entry_ts_ns, uid, strike, expiry_ns, side,
sigma_be, sigma_entry_iv, log_ratio, premium, vega, n_days, iters, flag,
snapped, log_moneyness, tenor_years, market_vol, rv_21d, rv_63d,
iv_minus_rv, n_events_to_expiry, delta_abs`; the header gains `#
feature_schema=1` and an `# events=<path>` meta echo. A non-finite/
non-positive `surf.forward_at(T)` now skips a whole entry date's candidate
lattice up front (new `n_entry_dates_forward_invalid` counter), since
`log_moneyness` is undefined for every candidate that date could produce.

Two new Python scripts under `atx-vol/scripts/` (stdlib-only, no CMake/C++
build relationship — see that directory's README): `bev_corpus_run.py`
fans one driver invocation out per (run x tenor) pair in a JSON manifest,
sequentially, capturing per-invocation stdout/stderr plus the parsed `#
key=value` meta header into a byte-stable `manifest_out.json`;
`bev_label_qa.py` reads one or more label TSVs and writes a single
markdown QA report over their union — row accounting by flag/snapped,
`log_ratio` distribution overall and by tenor x delta bucket,
per-feature-column NaN coverage, a cross-file duplicate-key check
(nonzero exit on a hit), and a report-only Pearson leakage tripwire that
never affects the exit code.

**Two data-quality caveats, both by design, both documented in the
driver's own banner and in `theo.hpp`'s ML-seam banner.** `rv_21d`/
`rv_63d` are close-to-close over spot-mirror bars (`O=H=L=C=spot`), not
real OHLC — Parkinson/Garman-Klass/Rogers-Satchell/Yang-Zhang stay dormant
until real bars land in the corpus (roadmap §5). `n_events_to_expiry` is
day-resolution (midnight UTC per announcement date, from a hand-supplied
TSV), not an intraday-timestamped, point-in-time vendor calendar.

**Effect on existing callers.** None. `bev_label_factory` is an
`ATX_BUILD_EXAMPLES`-gated example driver, off by default, with no served
library caller; its label-TSV schema changing (14 → 22 columns) affects
only files it itself writes. The one library header touched (`theo.hpp`)
gains a comment-only update to its ML-seam banner — no signature,
default, or served number changes.

**Not shipped (see the sprint summary's residual-work register for the
full list).** Real OHLC bars and a point-in-time earnings-calendar
vendor feed (both data-acquisition items, roadmap §5 — the `--events` TSV
format and the RV panel exist, but nothing produces either input from a
real source yet); the S3 trainer and purged-CV/embargo validation harness
(Python-side, unbuilt).
