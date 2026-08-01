# SP100 Projection-Strangle YTD Backtest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the projection-path dispersion-strangle strategy (long SP100 40Δ 3-month single-name strangles vs a short SPY strangle, vega-flat at entry, a new clip every trading day, daily delta-hedged, held to expiry) runnable end-to-end from Python against a real full-year SurfaceDb, and produce an HTML report via `atxvol.report`.

**Architecture:** Three code tasks close the three gaps between the existing route-B machinery (`make_dispersion_strangle_spec` → `DeclarativeStrategy` → `run_backtest`) and a real NYSE calendar / real (holey) surface corpus: (1) synthetic expiries snapped onto the backtest session grid so hold-to-expiry settlement finds an exact snapshot; (2) `UnpricedLotPolicy::ExcludeAndReport` extended to the two remaining fail-closed paths (delta-hedge trading, settlement) so one-session provider gaps don't abort a 145-session run; (3) a Python driver script that wires SurfaceDb → Clock → strategy spec → engine → tearsheet → `atxvol.report.dispersion.build_report`. Data operations (tail pull, YTD surface rebuild into a scratch root) are controller-executed runbook steps, not subagent tasks.

**Tech Stack:** C++20 (atx-vol), pybind11 bindings (`atxvol._core`), Python 3.12 (`atxvol.report`, stdlib+numpy HTML/SVG reports), GoogleTest, pytest.

## Global Constraints

- Builds ONLY via `.\scripts\atx-build.ps1 build atx-vol-tests --parallel 8` from the worktree root (never `dev --parallel`, never `-j`, direct PowerShell, no nested shells). ctest via `.\scripts\atx-build.ps1 -Ctest -R "<regex>"`.
- Bit-identical output across thread counts is a standing engine contract. Any new engine control flow must be deterministic and thread-count-invariant.
- Everything under `C:\atx-data` is READ-ONLY. Never write into `C:/atx-data/opra-hive`, `C:/atx-data/surface-db/*`, or any prod/pilot root. Tests build fixtures under `fs::temp_directory_path()` / pytest `tmp_path` only.
- The Databento API key lives in `C:/atx/.env`. Never print it, never echo the environment, never log or persist it.
- Existing behavior under `UnpricedLotPolicy::Error` must be bit-identical after Task 2 — the Error policy remains fully fail-closed.
- Existing behavior with `TenorSpec::snap_to_sessions == false` (the default) must be bit-identical after Task 1.
- `TenorSpec::snap_to_listed == true` keeps failing with NotImplemented — Task 1 adds a *different* snap, do not repurpose that flag.
- Python tests run through the ctest lane (`.\scripts\atx-build.ps1 -Ctest -R "atx-vol-python"`) or the repo's pinned driver — never bare `pytest` (the machine-wide scikit-build editable install hijacks `atxvol` resolution; `python/tests/conftest.py` hard-fails on contamination).
- Commit messages end with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

---

### Task 1: Session-snapped synthetic expiry

**Files:**
- Modify: `atx-vol/include/atx/vol/strategy.hpp` (TenorSpec ~line 45, StrategySpec ~line 159)
- Modify: `atx-vol/src/strategy.cpp` (`canonicalize_tenor` ~line 50 and its call sites in `expand_leg`)
- Modify: `atx-vol/include/atx/vol/dispersion_strangle.hpp` (DispersionStrangleConfig ~line 24)
- Modify: `atx-vol/src/dispersion_strangle.cpp` (spec assembly ~lines 28-160)
- Modify: `atx-vol/python/src/bindings/strategy.cpp` (bind the new fields)
- Test: `atx-vol/tests/strategy_test.cpp`, `atx-vol/tests/spy_dispersion_pnl_test.cpp`, `atx-vol/python/tests/test_strategy_bindings.py` (or the existing strategy-binding test file if named differently — find with `grep -l DispersionStrangleConfig python/tests`)

**Interfaces:**
- Produces: `TenorSpec::snap_to_sessions` (bool, default false); `StrategySpec::session_ts` (`std::vector<std::int64_t>`, sorted ascending, default empty); `DispersionStrangleConfig::snap_expiry_to_sessions` (bool, default false). Python: same three exposed as read/write properties (`session_ts` as a list of ints).
- Consumes: `canonicalize_tenor` (strategy.cpp:50), `kNsPerYear` (portfolio_pricer.hpp).

**Semantics (exact):** When a leg has `tenor.snap_to_sessions == true` and the enclosing spec's `session_ts` is non-empty:
1. Compute `raw_expiry = valuation_ts_ns + round(target_T * kNsPerYear)` exactly as today.
2. If `raw_expiry > session_ts.back()`: leave the expiry UNSNAPPED (`raw_expiry`). Such a cohort out-lives the corpus and never reaches settlement inside the run; it is liquidation-marked at run end. This is intentional.
3. Else: `snapped = *(std::upper_bound(session_ts.begin(), session_ts.end(), raw_expiry) - 1)` — the greatest session ts ≤ raw_expiry. If the iterator would underflow (`upper_bound == begin()`) or `snapped <= valuation_ts_ns`, return `Err(InvalidArgument, "expand_leg: snapped expiry is not after valuation")`.
4. The canonical tenor becomes `{snapped, (snapped - valuation_ts_ns) / kNsPerYear}` — the recomputed T feeds strike resolution and pricing so the contract key and the greeks agree.
- `snap_to_sessions == true` with an EMPTY `session_ts` is `Err(InvalidArgument, "expand_leg: snap_to_sessions requires session_ts")` — never a silent no-op.
- `make_dispersion_strangle_spec` copies `cfg.snap_expiry_to_sessions` onto `tenor.snap_to_sessions` of every leg (names and index). `session_ts` stays empty in the builder — the CALLER fills `spec.session_ts` (from `Clock` refs) after building the spec, because the builder is corpus-agnostic.

- [ ] **Step 1: Write the failing C++ unit tests** in `atx-vol/tests/strategy_test.cpp` (append to the existing suite; follow the file's fixture conventions — read neighboring tests first):

```cpp
TEST(TenorSnap, SnapsToGreatestSessionAtOrBeforeRawExpiry) {
  // sessions: valuation day + 3 sessions with a weekend-shaped gap; raw expiry
  // lands between two sessions -> expect the earlier one, T recomputed from it.
}
TEST(TenorSnap, ExactSessionHitIsUnchanged) {
  // raw expiry == a session ts -> snapped == raw, T identical to unsnapped math.
}
TEST(TenorSnap, BeyondCalendarStaysUnsnapped) {
  // raw expiry > session_ts.back() -> expiry == raw_expiry.
}
TEST(TenorSnap, SnappedAtOrBeforeValuationIsInvalidArgument) {
  // 1-day tenor whose only earlier session is the valuation itself -> Err.
}
TEST(TenorSnap, SnapWithoutSessionsIsInvalidArgument) {
  // snap_to_sessions true, spec.session_ts empty -> Err, message names session_ts.
}
TEST(TenorSnap, DefaultOffIsBitIdentical) {
  // snap_to_sessions false: resolved expiry_ts_ns and T equal the pre-change
  // values for the same leg (compare against a hand-computed raw offset).
}
```
The concrete test bodies exercise whatever the file's existing pattern is for building a snapshot + resolving a spec (see the existing `resolve_spec`/`expand_leg` tests in that file); assert on `ResolvedLeg::expiry_ts_ns` and `ResolvedLeg::T`.

- [ ] **Step 2: Run to verify they fail** — `.\scripts\atx-build.ps1 build atx-vol-tests --parallel 8` fails to compile (new fields don't exist yet). That compile failure IS the red state; proceed.

- [ ] **Step 3: Implement.** `strategy.hpp`: add `bool snap_to_sessions{false};` to TenorSpec (keep the snap_to_listed comment intact, extend it: "snap_to_sessions snaps the synthetic expiry onto StrategySpec::session_ts instead"); add `std::vector<std::int64_t> session_ts;` to StrategySpec (comment: sorted ascending snapshot timestamps of the run's clock; consumed only by legs with snap_to_sessions). `strategy.cpp`: extend `canonicalize_tenor` to take `(valuation_ts_ns, requested_T, bool snap, std::span<const std::int64_t> sessions)` implementing the semantics above; thread the spec's `session_ts` down from the resolution entry points (`resolve_spec_with_policy` / `expand_leg` — pass a span, no copies). `dispersion_strangle.hpp/.cpp`: add `snap_expiry_to_sessions{false}` and copy it onto every leg's tenor in the builder.

- [ ] **Step 4: Build + run the tests** — `.\scripts\atx-build.ps1 build atx-vol-tests --parallel 8` then `.\scripts\atx-build.ps1 -Ctest -R "TenorSnap"`. Expected: all new tests pass.

- [ ] **Step 5: Write the failing e2e ladder test** in `atx-vol/tests/spy_dispersion_pnl_test.cpp`: the existing hold-to-expiry fixture uses a synthetic DAILY clock including weekends with `tenor_days=6` precisely because settlement needs an exact ts (stated at lines 11-13 / 180-182 — read them). Add a test that builds a WEEKDAY-ONLY synthetic clock (5-on/2-off gaps), `tenor_days` chosen so raw expiry lands on a "weekend", `snap_expiry_to_sessions = true`, `spec.session_ts` filled from the clock; run the backtest; assert: run succeeds, every in-window cohort settles (final `n_open_lots` counts only cohorts whose raw expiry exceeds the last session), and the run with 1 thread equals the run with 4 threads bit-identically (follow the file's existing thread-invariance assertion pattern).

- [ ] **Step 6: Verify the new test fails without snapping** (temporarily assert with `snap_expiry_to_sessions=false` that the engine errors with "no exact expiry observation" — keep that as a second assertion in the test: snapping OFF on a gapped calendar errors, snapping ON succeeds).

- [ ] **Step 7: Build + run** — `.\scripts\atx-build.ps1 -Ctest -R "SpyDispersionPnl|TenorSnap"`. Expected: PASS.

- [ ] **Step 8: Bind to Python.** In the pybind strategy bindings (`grep -rn "DispersionStrangleConfig" atx-vol/src/bindings/` to find the file): expose `TenorSpec.snap_to_sessions`, `StrategySpec.session_ts` (list[int] property), `DispersionStrangleConfig.snap_expiry_to_sessions`. Add a pytest in the existing strategy-binding test file asserting round-trip set/get and that `make_dispersion_strangle_spec` propagates the flag onto `spec.legs[i].tenor.snap_to_sessions`.

- [ ] **Step 9: Run the python lane** — `.\scripts\atx-build.ps1 -Ctest -R "atx-vol-python"`. Expected: PASS (pre-existing failures, if any, must be listed in the report as pre-existing with evidence they fail on the base commit too).

- [ ] **Step 10: Full gate + commit.** `.\scripts\atx-build.ps1 -Ctest -R "atx-vol"` (or the project's full-suite regex used on this branch — same one the previous sprint gated with). Commit:

```bash
git add -A
git commit -m "feat(vol): snap synthetic expiries onto the run's session grid"
```

### Task 2: ExcludeAndReport tolerance for hedge trading and settlement

**Files:**
- Modify: `atx-vol/src/backtest.cpp` (hedge fail-closed ~line 2279-2287; settlement ~line 940-951; the Lot struct's home — find with `grep -n "struct Lot" include/atx/vol/backtest.hpp`)
- Modify: `atx-vol/include/atx/vol/backtest.hpp` (UnpricedLotPolicy doc comment ~line 491; Lot if a deferral flag is added)
- Test: `atx-vol/tests/backtest_test.cpp` (or wherever the existing F1(a)/F1(b) unpriced tests live — `grep -rn "no surface for delta hedge" atx-vol/tests`)

**Interfaces:**
- Consumes: `UnpricedLotPolicy` (backtest.hpp:497), the step's unpriced counters (`n_unpriced_lots` accumulation, backtest.cpp:1898).
- Produces: no new public API. Behavior change under `ExcludeAndReport` ONLY:
  1. **Hedge:** a uid the daily delta hedge wants to trade whose surface is absent this step → skip that uid's hedge trade this step, add 1 to this step's `n_unpriced_lots` tally. (Error policy: unchanged hard error at backtest.cpp:2287.)
  2. **Settlement:** a lot at its exact expiry step (`lot.expiry_ts_ns == shifted.ts_ns()`) whose surface is absent → mark the lot deferred (new `bool settlement_deferred{false}` on Lot or a parallel book-side set — implementer's choice, must be deterministic), count 1 in `n_unpriced_lots`, keep the lot open. On each LATER step, a deferred lot settles at intrinsic against THAT step's spot the first time its surface is present again (count that step too); if the surface never returns, the lot stays open to run end (excluded from greeks/PnL as today, counted daily). (Error policy: unchanged hard errors at backtest.cpp:951.)
  3. The exact-ts settlement guard for NON-deferred lots is UNCHANGED under both policies: a lot whose expiry passed between sessions without ever hitting an exact step still errors ("no exact expiry observation") — that is Task 1's domain; do not relax it here, it fails closed against calendar bugs.

- [ ] **Step 1: Write the failing tests** (follow the existing unpriced/F1 test conventions in the file you find them in):

```cpp
TEST(UnpricedTolerance, HedgeSkipsAbsentUidAndCountsUnderExcludeAndReport) {
  // 2-name book, daily DeltaToZero hedge; name B's surface absent on step k.
  // ExcludeAndReport: run succeeds, step k's n_unpriced_lots includes B's skip,
  // B's hedge share count is unchanged across step k, A hedges normally.
}
TEST(UnpricedTolerance, HedgeAbsentUidStillAbortsUnderError) {
  // Same fixture, unpriced=Error: Err(NotFound) mentioning "delta hedge".
}
TEST(UnpricedTolerance, SettlementDefersAcrossAOneSessionGap) {
  // Lot expires on step k (exact ts), surface absent on k, present on k+1.
  // ExcludeAndReport: settles on k+1 at intrinsic vs k+1 spot; counted on k;
  // final NAV includes the settlement; n_open_lots drops at k+1.
}
TEST(UnpricedTolerance, SettlementAbsentStillAbortsUnderError) {
  // Same fixture, unpriced=Error: Err mentions "no surface for settling lot".
}
TEST(UnpricedTolerance, NeverReturningSurfaceStaysOpenAndCounted) {
  // Surface absent from step k to run end: lot never settles, counted every
  // step from k, run completes, lot present in final n_open_lots.
}
TEST(UnpricedTolerance, ErrorPolicyPathIsBitIdenticalOnCleanData) {
  // A gap-free fixture run under Error before/after produces identical results
  // (regression guard: refactor must not perturb the clean path).
}
```

- [ ] **Step 2: Run to verify failure** — build + `-Ctest -R "UnpricedTolerance"`. The hedge/settlement tolerance tests must FAIL (engine currently errors).

- [ ] **Step 3: Implement** per the interface semantics. Keep all new branching outside the threaded pricing loops (hedge and settlement are serial control flow); no `std::unordered_*` iteration order may leak into results — if a set of deferred lots is kept book-side, key it by lot id and iterate in id order.

- [ ] **Step 4: Build + run** — `-Ctest -R "UnpricedTolerance"`. Expected: PASS.

- [ ] **Step 5: Full engine suite** — `-Ctest -R "Backtest|SpyDispersionPnl|SurfaceDbDispersionBacktest"` green (settlement/hedge touch everything; run wide).

- [ ] **Step 6: Update the UnpricedLotPolicy doc comment** (backtest.hpp:491-504) to state the two new ExcludeAndReport behaviors and that Error remains fully fail-closed.

- [ ] **Step 7: Commit.**

```bash
git add -A
git commit -m "feat(vol): extend ExcludeAndReport to hedge trading and deferred settlement"
```

### Task 3: Python driver + HTML report wiring

**Files:**
- Create: `atx-vol/tools/run_sp100_strangle_backtest.py`
- Test: `atx-vol/python/tests/test_run_sp100_strangle_backtest.py`
- Modify (only if a binding gap surfaces): `atx-vol/src/bindings/strategy.cpp`, `atx-vol/src/bindings/backtest.cpp`

**Interfaces:**
- Consumes: `atxvol._core`: `SurfaceDb.open`, `Clock.from_surface_db(db).between(date_lo, date_hi)`, `DispersionStrangleConfig`, `make_dispersion_strangle_spec`, `HedgeSpec`, `MissingNameSpec`/`MissingNamePolicy`, the strategy construction path for a `StrategySpec` (find how the existing python tests turn a spec into a runnable strategy — `grep -rn "DeclarativeStrategy\|make_dispersion_strangle_spec" python/tests python/src`; if NO python-visible constructor exists from `StrategySpec` to a runnable `IStrategy`, add one binding: `DeclarativeStrategy.create(spec)` mirroring the C++ factory), `RunConfig` (`unpriced=UnpricedLotPolicy.EXCLUDE_AND_REPORT`, `record_every_n=1`), `run_backtest`, `tearsheet`, `write_backtest_pnl_tsv`. Report: `atxvol.report.dispersion.build_report(result, sheet, meta, path)` — meta MUST carry a `friction_regime` key whose value is one of `atxvol.report.dispersion.REGIMES` (read that dict first; frictionless run → use the regime key that represents zero frictions).
- Consumes from Tasks 1-2: `DispersionStrangleConfig.snap_expiry_to_sessions`, `StrategySpec.session_ts`, ExcludeAndReport hedge/settlement tolerance.
- Produces (CLI contract):

```
python atx-vol/tools/run_sp100_strangle_backtest.py
    --db <surface-db root> --universe <sp100 csv> --from YYYY-MM-DD --to YYYY-MM-DD
    --out <dir> [--exclude BK[,SYM...]] [--index SPY] [--delta 0.40]
    [--tenor-days 90] [--theta-per-name 10.0] [--hedge-band 0.0] [--label <str>]
```
Writes into `--out`: `track.tsv` (via `write_backtest_pnl_tsv`, meta header carrying friction_regime + every knob + resolved window + universe hash/count), `tearsheet.tsv` (key\tvalue from `TearSheet.to_dict()`), `report.html` (via `build_report`). Prints one summary line per artifact plus headline stats (sessions, names, final NAV, total PnL, max drawdown, mean |net vega| at entry, unpriced counts). Exit 0 on success, 1 with the engine error on failure, 2 on usage error.

**Behavioral requirements:**
- Universe parsing: TAB-delimited `effective_date  symbol  raw_weight  source  as_of` header format of `atx-vol/data/universe/sp100_2026-07.csv`; the `--index` symbol is removed from the names list if present; `--exclude` symbols removed (case-insensitive); duplicates rejected; final names count printed.
- Strategy config: `target_abs_delta=--delta`, `tenor_days=--tenor-days`, `entry_every_n_days=1`, `hold_to_expiry=True`, `snap_expiry_to_sessions=True`, `hedge=HedgeSpec(DELTA_TO_ZERO, DAILY, --hedge-band)`, `theta_per_name_daily=--theta-per-name`, `missing=MissingNameSpec(DROP_RENORMALIZE, <existing default min_names>)`.
- `spec.session_ts` = the clock's snapshot timestamps, ascending, taken from the SAME `Clock.between` window the run uses.
- The script must locate the worktree-built `_core` (the ctest python lane's resolution), not the machine-wide editable install — copy the resolution-pinning preamble pattern from `python/tests/conftest.py` / the repo's pinned driver rather than inventing one. No `os.add_dll_directory` beyond what that pattern uses.

- [ ] **Step 1: Write the failing pytest** `python/tests/test_run_sp100_strangle_backtest.py`. Build the smallest synthetic SurfaceDb the existing python tests already know how to build (`grep -rn "SurfaceDb.create\|write_partition" python/tests` and reuse that fixture pattern; 3 names + SPY, ~8 weekday sessions, `tenor_days=4`, snap on). Invoke the driver via `subprocess` (same interpreter, pinned env) or by importing its `main(argv)`. Assert: exit 0; `track.tsv` exists with `# friction_regime=` in its header; `tearsheet.tsv` has `final_nav`; `report.html` exists, contains the label and is non-trivially sized (> 20 kB); a second identical invocation writes byte-identical `track.tsv` (determinism).

- [ ] **Step 2: Run to verify it fails** — `.\scripts\atx-build.ps1 -Ctest -R "atx-vol-python"` (or the repo's file-scoped python driver if one exists for a single test file). Expected: FAIL (script doesn't exist).

- [ ] **Step 3: Implement the driver** per the CLI contract. argparse with the exact flags above; `main(argv=None) -> int`; all engine work inside `main` so the test can import it.

- [ ] **Step 4: Run the test** — same lane. Expected: PASS.

- [ ] **Step 5: Whole python lane** — `-Ctest -R "atx-vol-python"` green (modulo documented pre-existing failures).

- [ ] **Step 6: Commit.**

```bash
git add -A
git commit -m "feat(vol): add the SP100 projection-strangle backtest driver and report wiring"
```

---

### Task 4 (controller-executed runbook — NOT a subagent task): YTD data build

Outputs live under `C:\atx-scratch\` (create it; NOT under C:\atx-data). Binaries: the worktree's Release build (`build-rel/bin/atx-vol-surface-db-build.exe`, `atx-vol-surface-db.exe`) — build them first if absent.

1. **Tail pull (2026-07-27..31), cost-gated:** mini-hive at `C:\atx-scratch\opra-hive-tail`. Copy `C:/atx-data/opra-hive/_absent/*.json` into it first (so known-absent cells aren't re-billed). `python atx-vol/tools/pull_opra_hive.py --start 2026-07-27 --end 2026-07-31 --universe atx-vol/data/universe/sp100_2026-07.csv --snap-et 15:55 --out C:\atx-scratch\opra-hive-tail --cap 15 --dry-run` → inspect the free cost preflight. If preflight cost ≤ $15, re-run without `--dry-run`. If it blocks or exceeds the cap, SKIP the tail and run YTD through 2026-07-24 (report this to the user).
2. **Build YTD surfaces** into `C:\atx-scratch\surface-db\sp100-2026`: orchestrator run 1 (main window, existing hive read-only): `python atx-vol/tools/run_surface_db_backfill.py --universe atx-vol/data/universe/sp100_2026-07.csv --hive C:/atx-data/opra-hive --db-prefix C:\atx-scratch\surface-db\sp100 --from 2026-01-02 --to 2026-07-24 --phase build --snap-et 15:55 --rates atx-vol/data/rates/us_3m_monthly.csv --build-exe build-rel/bin/atx-vol-surface-db-build.exe --admin-exe build-rel/bin/atx-vol-surface-db.exe --chunk-sessions 4 --fit-workers 0 --log-dir C:\atx-scratch\logs\sp100 --dry-run` first (verify resolved `--db`, `--r 0.0430`, DST-split snapshot suffixes), then real. Add the recovery flags the build CLI supports (`--deep-selection --retry-disabled`) if the orchestrator passes them through; if it does not, rebuild ONLY the failed cells' dates directly with the C++ CLI using those flags afterward. Orchestrator run 2 (tail, if pulled): same `--db-prefix`, `--hive C:\atx-scratch\opra-hive-tail --from 2026-07-27 --to 2026-07-31`.
3. **Verify:** `--phase verify` over each window; then enumerate absences (`atx-vol-surface-db.exe partitions`) — REQUIRED outcome: SPY present in every partition (the 18 sp100-2026 SPY holes are fit failures over hive-present data and must recover; if any SPY date still fails after `--deep-selection`, STOP and report — the run window shrinks to index-complete stretches). Expected residual absences: BK from 2026-05-21 (provider), CMCSA 2026-01-05, FDX 2026-06-01, HON 2026-06-29, SPGI 2026-07-01 (provider, one session each).
4. Record: partition count, absent-cell census, wall time, disk size.

### Task 5 (controller-executed runbook): the real run + report

1. Rebuild the python extension for the branch (`_core` must carry Tasks 1-3 bindings) via the repo's python build lane.
2. `python atx-vol/tools/run_sp100_strangle_backtest.py --db C:\atx-scratch\surface-db\sp100-2026 --universe atx-vol/data/universe/sp100_2026-07.csv --from 2026-01-02 --to <2026-07-31 or -24> --out C:\atx-scratch\runs\sp100-strangle-ytd --exclude BK --label "SP100 40d 3m strangles, daily clips, vega-flat vs SPY, YTD 2026"`.
3. Sanity gates before declaring success: exit 0; `n_unpriced_lots` totals consistent with the known one-session gaps (order 10², not 10⁴); every cohort with raw expiry ≤ last session settled; final NAV finite; report.html renders the regime banner and full track.
4. Deliver: report path + headline stats to the user; note BK exclusion, entry-time (not continuous) vega-flatness, and any window truncation.

---

## Self-review notes

- Spec coverage: strategy expressibility (Tasks 1-2), python wiring + report helpers (Task 3), data pulled + surfaces built (Task 4), run it (Task 5). "Projection method not listed" is inherent: route B prices exclusively through `project_option_contract` — no OPRA contracts anywhere.
- Type consistency: `session_ts` is `std::vector<std::int64_t>` on StrategySpec, `std::span<const std::int64_t>` at the resolution boundary, `list[int]` in Python.
- Deliberate scope cuts (surface in the final report to the user): vega-flatness is per-clip at entry (`CrossLegConstraint::FlatVega`), not continuously maintained book-level — the engine has no vega-hedge overlay and building one is out of scope; BK excluded (provider outage 2026-05-21→end makes its lots unpriceable forever); direction is the route-B native long-names/short-index (the user did not specify a side).
