# SPY Dispersion Two-Route Parity — Implementation Plan

Goal: verify that a listed-options dispersion backtest matches a backtest that projects
the same option definitions onto historical fitted surfaces and reprices the theoretical
portfolio daily via surface interpolation. Fix the defects that currently prevent the
comparison, extend the window from 3 sessions to the full OPRA range, and ship a
self-contained comparative HTML report showing the P&L tracks match.

## Background (evidence: scratchpad/deepdive/*.md)

Both routes already share the identical P&L fold and attribution engine
(portfolio_pricer.cpp:1446-1468), delta-hedge daily (DeltaToZero), build vega-neutral
books (short index straddle, long name straddles, ShortIndexLongNames), and mark from the
fitted PricedSurface. Verified divergences:

- **D1 (bug):** one spec key `gross_index_vega` wired as per-VOL-POINT into the listed
  schedule build (spy_dispersion_backtest.cpp:415) but as per-UNIT-VOL into the synthetic
  surface route (:536, :611) → 100x book-size mismatch.
- **D2 (modeling):** synthetic route projects its own contracts (ATM-forward strike,
  calendar +30d expiry) while the listed schedule holds real contracts (e.g. Feb-20
  monthly, 49d — nearest expiry where index AND ≥10 names all form valid straddles).
  Gamma/theta cumulative ratio 1.57 ≈ 49/30 confirms tenor mismatch dominates.
- **D3 (blocker):** ListedDispersionStrategy::on_step enforces bit-exact
  `seed.greeks().price == leg.model_mark` (listed_dispersion_strategy.cpp:116) and pins
  `lot.entry_price = leg.model_mark` (:62). Blocks replaying the schedule under
  `QueryExecution::Configured` (interpolated) marks.
- **D6 (data):** definitions.tsv + OCC ESS cover only 2026-01-02/05/06; OPRA parquet has
  135 sessions; surfaces (bt-sota corpora, V2/ATXVSA20) cover 84-137. DATABENTO_API_KEY
  exists in C:\atx\.env (the C++ puller reads getenv only). All logged Databento pulls
  cost $0.00; OCC ESS pull (tools script download_occ_ess.py) is keyless/free.
- **D7 (cosmetic):** `gross_delta/gamma/vega/theta` TSV columns are signed NET (name is a
  misnomer). Do NOT rename (schema stability); note in report.

## Global constraints

- Branch: main, in-place (session precedent). Implementers commit ONLY their own files —
  the tree carries unrelated uncommitted work. `git add <explicit paths>` only; never
  `git add -A`/`-u`.
- Builds: release preset only, one build at a time (shared C:\atx-cache\deps — never run
  Debug and Release concurrently). Build via
  `C:\Users\natha\AppData\Local\Temp\claude\c--atx\b8ae4870-03de-493c-ad84-2006e8f7409e\scratchpad\build_example.bat`
  (vcvars64 + `cmake --preset rel -DATX_BUILD_EXAMPLES=ON` + explicit ninja path), target
  `atxvol_spy_dispersion_backtest`. C++ tests via the same preset's ctest.
- ninja hazard: after `git checkout -- <file>` restores, touch the file and confirm a
  `Building CXX object` line, else a stale object may link.
- TSV schemas are stable interfaces: extend by ADDING columns/files, never rename/reorder
  existing ones.
- No behavior change to existing ColdReference replay: byte-identical backtest.tsv on the
  existing paired fixture is a hard acceptance gate for T1/T2.
- Existing goldens: scratchpad/paired/run/ holds the 3-session fixture (backtest.tsv,
  surface_backtest.tsv, trade_schedule.tsv) reproduced from current HEAD.

## Task 1 — Library: mark policy for schedule replay (route-P enabler)

Files: atx-vol/include/atx/vol/listed_dispersion_strategy.hpp,
atx-vol/src/listed_dispersion_strategy.cpp, atx-vol/tests/ (existing listed dispersion
strategy test file; extend it).

1. Add `enum class ScheduleMarkPolicy { ExactArchive, Record };` (header, doc comment:
   ExactArchive = current behavior, bit-exact gate against `leg.model_mark`, replay of the
   frozen archive; Record = accept the live seed mark, for repricing the same definitions
   through a different query route, recording divergence instead of failing).
2. `ListedDispersionStrategy::create(schedule, delta_band, policy = ExactArchive)` —
   default preserves every existing caller.
3. In `on_step`: under ExactArchive keep the exact-equality gate unchanged. Under Record:
   no gate; collect per-leg `MarkDivergence { uid, strike, expiry_ts_ns, side,
   schedule_mark, live_mark }` into a member accessible via
   `const std::vector<MarkDivergence>& last_mark_divergences()` (cleared per step).
4. Entry price: under ExactArchive keep `lot.entry_price = leg.model_mark` (bit-identical
   guarantee). Under Record set `lot.entry_price` to the live seed price
   (`seed.greeks().price`) so the replay is self-consistent under its own marks.
   `materialize_listed_dispersion_roll` gains an optional entry-price override span/vector
   (or the strategy patches lots after materialize — pick the cleaner, keep the free
   function's existing signature working).
5. Tests (TDD, red first): (a) ExactArchive still errors "archive mark differs from
   schedule" on a perturbed mark; (b) Record accepts a perturbed mark, records exactly the
   perturbed legs with correct schedule/live values, and sets entry_price to the live
   mark; (c) default-argument call sites compile unchanged (existing tests still pass).

Acceptance: focused ctest green; full atx-vol test suite no new failures; existing
3-session `run-backtest` output byte-identical (T2 verifies end-to-end).

## Task 2 — Example: projected-replay subcommand + D1 units fix

File: atx-vol/examples/spy_dispersion_backtest.cpp (+ its README/help text).

1. New subcommand `run-projected-backtest`: identical wiring to `run-backtest` EXCEPT
   (a) `ListedDispersionStrategy::create(..., ScheduleMarkPolicy::Record)`;
   (b) `QueryExecution::Configured` end-to-end (PriceOptions + run config);
   (c) surfaces get the prepared fast query tier attached (`with_query_pricing`, the
   fast/cached-surrogate tier used elsewhere in atx-vol for Configured queries — pick the
   tier the corpus supports; if attachment fails for a surface, propagate the error, do
   not silently fall back to cold) so interpolation is genuine;
   (d) writes `projected_backtest.tsv` (same 27-column schema) and
   `mark_divergence.tsv` (columns: date, symbol, raw_symbol, strike, expiry_ts_ns, side,
   schedule_mark, live_mark, diff, abs_diff_bps_of_mark) from `last_mark_divergences()`
   after each roll step.
2. D1 fix: in the synthetic surface route wiring, convert units at the boundary:
   `config.gross_index_vega = spec.gross_index_vega * 100.0` (:536 area) and
   `dispersion.target_vega = spec.gross_index_vega * 100.0` (:611 area), with a comment
   stating spec.gross_index_vega is dollars vega per VOL POINT per side and the library
   configs take dollars vega per UNIT vol. No library change.
3. Acceptance: (a) rebuild example; (b) rerun `run-backtest` on
   scratchpad/paired — backtest.tsv byte-identical to the existing golden; (c)
   `run-projected-backtest` on the same inputs produces projected_backtest.tsv with 3
   rows, n_unpriced_lots = n_unpriced_greeks = 0, and nav within a few percent of
   backtest.tsv nav (record actual numbers in report file); (d) rerun
   `run-surface-backtest` — turnover_vega now 2,000,000 (was 20,000).

## Task 3 — Data: extend window to full OPRA range (controller-driven ops, not a subagent)

1. Export DATABENTO_API_KEY from C:\atx\.env into the process env; run the definitions
   puller (tool built from databento_spy_dispersion_definitions.cpp) for all OPRA sessions
   (2026-01-02 .. last session). Watch for 504s; retry per-date on failure.
2. Run tools/download_occ_ess.py (keyless) for the same dates.
3. `build-schedule` over the full window against the bt-sota-full V2 corpus (137 dates);
   then `run-backtest` and `run-projected-backtest`.
4. Fallback if the pull fails: ship the 3-session comparison and say so in the report.

## Task 4 — Python: comparative report builder

Files: atx-vol/python/src/atxvol/report/ (new builder module), atx-vol/python/tests/.

Consumes backtest.tsv + projected_backtest.tsv + mark_divergence.tsv + trade_schedule.tsv
paths; emits one self-contained HTML (base64/inline, no external requests) via the
existing atxvol.report component library (charts.py: line_chart, scatter_chart,
paired_bar_chart, small_multiple; follow the dataviz skill: shared-scale y=x agreement
scatter, color follows route not rank, validated palette, hover layer, dark mode).
Content: (1) hero: tracking stats (corr, tracking error, max |nav gap|); (2) overlaid nav
tracks + residual subchart; (3) daily pnl_total agreement scatter with y=x; (4) cumulative
attribution paired bars (gamma/vega/theta/unexplained) listed vs projected; (5) mark
divergence distribution + worst offenders table; (6) methodology (routes, policies,
units note incl. D7 misnomer, window/coverage). Tests: builder produces valid HTML from
the 3-session fixture TSVs; stats computed correctly on a toy frame.

## Task 6 — Projected-definition schedule (ATM-forward strikes, cold marks): route P canonical

File: atx-vol/examples/spy_dispersion_backtest.cpp only (library needs no change: Record
policy + ColdReference execution is already a supported combination after Tasks 1-2).

Rationale: the user's claim is "listed options backtest ≈ backtest with interpolated
strikes/expiries projected onto historical surfaces". The projected portfolio must
therefore differ from the listed one ONLY by contract idealization (exact interpolated
ATM-forward strike instead of the nearest listed strike), not by tenor (same expiry as the
listed roll) nor by solver tier (cold certified economics both sides). The fast-tier
(Configured) replay from Task 2 remains as a diagnostic mode; its accuracy gap is under
separate investigation.

1. New subcommand `project-schedule --run DIR`: read the frozen listed schedule
   (trade_schedule.tsv) + corpus archives. For each roll, emit a projected roll with the
   SAME roll_date/valuation_ts_ns/cohort/expiry_ts_ns/n_names/weights/side structure, but
   per member (index + each name): strike = the member surface's ATM forward at the roll's
   residual T (same forward accessor the synthetic dispersion route uses — see
   run_dispersion_backtest's strike selection); legs = call+put straddle at that strike;
   `vega_per_contract_per_vol_point` etc. from COLD greeks at the projected strike;
   quantity from the SAME sizing rule as the listed build (target straddle vega per vol
   point x weight / straddle vega per contract per vol point, index negative, names
   positive); model_mark = cold fair_value. Write `projected_schedule.tsv` in the exact
   ATX_LISTED_DISPERSION_SCHEDULE format (so every existing reader/validator works),
   including passing `validate_listed_dispersion_schedule` (net vega ~ 0 within its
   tolerance, gross = 2x target).
2. `run-projected-backtest` gains flags: `--schedule <file>` (default trade_schedule.tsv),
   `--execution cold|configured` (default configured = Task 2 behavior; cold skips fast-
   tier attachment and runs QueryExecution::ColdReference with ScheduleMarkPolicy::Record),
   `--out <file>` (default projected_backtest.tsv). Flag combination for route P canonical:
   `--schedule projected_schedule.tsv --execution cold --out projected_cold_backtest.tsv`.
3. Acceptance on the 3-session fixture copy: (a) project-schedule validates and strikes
   land near-but-off the listed grid (record listed vs projected strikes per member);
   (b) route P canonical run: 3 rows, n_unpriced 0, nav close to listed backtest.tsv nav —
   record both navs and per-day |pnl_total| gaps; (c) default-flag run reproduces Task 2's
   projected_backtest.tsv byte-identically; (d) run-backtest byte-identity gate re-proven
   after rebuild.

## Task 7 — Unblock listed route at HEAD: skip same-session (0DTE) contracts in the listed OPRA join

Files: atx-vol/src/listed_opra.cpp (+ its header if a signature/doc needs it),
atx-vol/tests/listed_opra_test.cpp.

Background (evidence: scratchpad/deepdive/join-regression-rootcause.md): the pg-sota merge
(3f7ba3f) made the OPRA panel keep same-session 0DTE contracts (expiry instant now 16:00 ET
= 21:00Z > the 19:55Z snapshot; pre-merge midnight-UTC semantics dropped them). 0DTE
contracts structurally cannot join the definitions authority: frozen tables predate them
and Databento stamps `expiration` at midnight-UTC of expiry date, which trips the
look-ahead/expiry guard (listed_opra.cpp:296-297) even when a row exists. The listed
dispersion workflow only ever selects 21-60 DTE (ListedDispersionSelectionConfig), so
same-session contracts are noise for this consumer.

1. In `listed_quotes_from_opra` (the loop that errors at listed_opra.cpp:293), skip
   same-session contracts — OSI expiry date == the panel's trade date — with a `continue`,
   exactly parallel to the existing numeric-root skip. Do NOT change opra_panel.cpp (other
   consumers keep pg-sota's 0DTE-inclusive panel semantics). Count skips; if the function
   has an existing stats/diagnostic channel, report the count there; otherwise a doc
   comment naming the invariant is enough.
2. Tests (TDD, red first) in listed_opra_test.cpp, following its existing fixture
   patterns: (a) a panel containing a standard-root same-session contract with NO
   definitions row builds successfully, that contract absent from the result (this is the
   post-merge regression repro — must FAIL before the fix with "contract definition
   missing"); (b) existing behavior preserved: non-0DTE contract missing a definition
   still errors; numeric-root skip still works.
3. Acceptance: (a) module tests green (listed_opra + listed_dispersion suites);
   (b) post-merge `build-schedule` on a fresh fixture copy exits 0 and selects the SAME
   contracts as the pre-merge schedule (same raw_symbols/expiries/strikes — quantities and
   marks may differ numerically because pg-sota changed pricing; record the schedule diff);
   (c) `run-backtest` on that freshly built schedule passes the ExactArchive gate
   (self-consistency) and produces 3 rows.

## Task 5 — Final whole-branch review

requesting-code-review template, most capable model, over all commits from base.

## Acceptance for the whole effort

- run-backtest byte-identical on existing fixture (no regression).
- Both routes over the longest achievable window; report renders; tracks visibly
  coincide; tracking stats quantified; every remaining residual explained in the report.
- All new/changed code covered by tests run and green (atx-vol ctest + python pytest).
