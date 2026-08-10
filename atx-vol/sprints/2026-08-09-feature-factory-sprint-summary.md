# Feature Factory Sprint — Summary

**Dates:** 2026-08-09 – 2026-08-10 (two-day span across Tasks 1–6)
**Branch:** `feat/vol-feature-factory` (worktree `C:\atx-wt\pool-12`), base `079798e` on `main`
**Plan:** `docs/superpowers/plans/2026-08-09-atx-vol-feature-factory.md` · **Ledger:** `.superpowers/sdd/2026-08-09-atx-vol-feature-factory/progress.md`
**Method:** subagent-driven development — task brief + fresh implementer per task, spec/quality review per task, minors tracked (deferred or fixed) in the ledger; this closeout task runs hygiene + the targeted gate only, per the standing user directive to never run full test lanes.

## Goal

Make the BEV label factory (`bev_label_factory`) emit the full
`kFairVolFeatureSchemaV1` feature block beside its target, and ship the
corpus-scale batch/QA tooling around it — producing the first trainable
dataset shape for the `IFairVolModel` seam. Closes roadmap gap G1 (no
feature producer) and the tooling half of G6 (corpus wiring), per
`docs/research/2026-08-09-theo-ml-alpha-iteration-plan.md` §1/§4 S1–S2.
Training itself (S3) was explicitly out of scope.

## Shipped surface

### `atx-vol/examples/bev_label_factory.cpp` + `atx-vol/tests/bev_label_factory_gate_test.cpp` (Tasks 1–3)

- **Events-calendar input (Task 1, `9bbf0c8`).** Optional `--events <tsv>`:
  one ISO (`YYYY-MM-DD`) announcement date per line, `#`/blank lines
  skipped, CR tolerated. `iso_date_to_ns` parses via a Howard-Hinnant
  civil-days round trip to midnight-UTC epoch ns (rejects calendar-invalid
  dates, e.g. `2026-02-30`). `load_events_tsv`: empty/omitted path →
  `Ok(nullopt)` (no calendar); a present-but-all-comment file is a valid,
  empty calendar. Each entry date's scheduled-event count in `(entry,
  entry+T]` (`event_vol.hpp`'s `count_events_at`) is computed once per date
  and shared across that date's whole candidate lattice.
- **Spot-history pre-pass + per-entry-date RV panel (Task 2, `24a1e3e`).**
  `load_spot_history` walks the corpus once, ahead of the entry loop, over
  `[first_entry_idx − kRvHistoryBars, last_entry_idx]` of the full clock,
  emitting one spot-mirror `OhlcBar` (`open=high=low=close=S`) per session.
  For each entry date, a trailing window ending at and including that
  date's own close feeds `realized_vol_panel` (`RvEstimator::CloseToClose`,
  `252.0` annualization; `kRvHistoryBars = 253`, i.e. up to 252 trailing
  log-returns) to produce `rv_21d`/`rv_63d`; a too-short trailing window is
  NaN, tallied by the new `n_entry_dates_rv_short` counter. A pre-pass load
  failure is a hard error (`ATX_TRY`, not a per-date skip) — a corpus that
  serves surfaces but not spots is treated as broken.
- **Fair-vol feature schema v1 emission (Task 3, `a06e7fb`).** The label
  TSV grows from 14 to 22 tab-separated columns — `entry_ts_ns, uid,
  strike, expiry_ns, side, sigma_be, sigma_entry_iv, log_ratio, premium,
  vega, n_days, iters, flag, snapped, log_moneyness, tenor_years,
  market_vol, rv_21d, rv_63d, iv_minus_rv, n_events_to_expiry, delta_abs`
  — the last eight in `kFairVolFeatureSchemaV1` (`theo.hpp`) order, guarded
  by a `static_assert(kFairVolFeatureCount == 8, …)` drift tripwire.
  `log_moneyness = ln(K / surf.forward_at(T))`, ONE `forward_at(T)` call
  per entry date (the same accessor `theo.cpp`'s serving-side
  `build_features` calls) shared across the whole candidate lattice; a
  non-finite/non-positive forward skips the whole entry date up front
  (new `n_entry_dates_forward_invalid` counter) rather than failing
  per-candidate. `market_vol` duplicates `sigma_entry_iv` by construction
  (same `surf.iv(K,T)` read, bit-equal) so the eight columns form a
  self-contained slice; `iv_minus_rv = market_vol − rv_21d`,
  NaN-propagating. Meta header gains `# feature_schema=1` and an `#
  events=<path>` echo. `theo.hpp`'s ML-seam banner (comment-only) is
  updated: it now documents that the label factory assembles this schema
  in-process, not an offline trainer join.
- Gate test file `bev_label_factory_gate_test.cpp`: **9 tests**, suite
  `BevLabelFactoryGate` — byte-determinism (two identical-arg runs
  `memcmp`-identical), `parse_args` field/rejection coverage,
  `load_dividends_tsv` coverage, `EventsTsvParsesAndCounts` /
  `EventsTsvRejectsMalformedDate` / `EventsPathEmptyMeansNoCalendar`,
  `SpotHistoryMirrorsSessionSpots`, `FeatureBlockHeaderAndValues`.

### `atx-vol/scripts/` (Tasks 4–5) — new Python-utility tier

Stdlib-only (`argparse`, `json`, `csv`, `math`, `subprocess`, `pathlib`),
not part of the CMake build, no C++ compile-time relationship. Run via
`python -m pytest atx-vol/scripts/ -q` from the repo root.

- **`bev_corpus_run.py` (Task 4, `e4b1b71`).** Manifest-driven fan-out of
  `bev_label_factory` across a (run x tenor) grid, sequential (the driver
  is already internally threaded via `--threads`). CLI: `--manifest
  run.json --exe <path> --out-dir <dir> [--dry-run]`. Writes
  `<uid>_<entry_start>_<tenor>d.tsv` per invocation plus a sibling `.log`,
  and a byte-stable (`sort_keys=True`) `manifest_out.json` summarizing
  every invocation's argv, exit code, and parsed `# key=value` meta. Exit
  code is nonzero if any invocation failed; the rest still run.
- **`bev_label_qa.py` (Task 5, `444c899`).** Reads one or more 22-column
  label TSVs and writes one markdown QA report over the union of rows: (1)
  row accounting by file/flag/snapped, (2) `log_ratio` mean/stddev/P5/P50/
  P95 overall and by a fixed tenor x delta bucket grid (exact two-pass
  population moments; linear-rank-interpolated percentiles), (3)
  per-feature-column NaN coverage for the 8 schema columns, (4) cross-file
  duplicate `(entry_ts_ns, uid, expiry_ns, strike, side)` key detection
  (exit 1 on a hit — a manifest double-covered range), (5) a report-only
  Pearson leakage tripwire (pairwise NaN-excluding; never affects exit
  code — the trainer's own leakage audit owns that judgment, roadmap §4
  S3). CLI: `bev_label_qa.py <labels.tsv>... --out-md report.md`. Exit
  codes: 0 clean, 1 duplicate keys (report still written), 2 bad
  args/malformed input.
- `atx-vol/scripts/README.md`: tier note (Task 4) + a Scripts-list entry
  for each script (`bev_corpus_run.py`'s entry shipped in Task 4;
  `bev_label_qa.py`'s entry was deferred from Task 5's review and is added
  in this closeout task).

## Measured driver runtime

Run against the real `C:/atx-data/surface-db-r2/spy-2019` corpus (247
session partitions), `bev_label_factory.exe` built under the `dev` preset
(`build/`, **Debug**, `ATX_USE_PCH=ON`) — an operational sense of scale,
**not** a citable perf baseline (no `rel`/quiet-host measurement was taken
this task; see the repo's own noisy-host/build-config disclosure
convention). `--uid SPY --tenor-days 30 --delta-lo 0.05 --delta-hi 0.95
--dividends <empty-tsv> --threads 0`, no `--events`:

| Entry-date range | entry_dates | candidates | prebuild_skipped | solved_ok | rows | wall time |
|---|---:|---:|---:|---:|---:|---:|
| 2019-01-02 .. 2019-01-31 (fixture-scale) | 20 | 760 | 53 | 707 | 707 | 6.89 s |
| 2019-01-02 .. 2019-12-31 (full real corpus) | 247 | 9,386 | 723 | 8,663 | 8,663 | 109.13 s |

Both runs: `rv_short=1`, `forward_invalid=0`, exit 0. The full-year run is
12.35x the entry dates and 12.35x the candidates of the fixture-scale run,
but ~15.8x the wall time — each a single untrended sample on a shared,
non-quiet host (Debug build), so this is reported as observed rather than
attributed to a specific cause; it is not a citable perf regression
finding, just the scale operators should expect from a real corpus-run
invocation.

A `bev_label_qa.py` run against the full-2019 output (illustrative, not
committed): 8,663 rows, flag breakdown `Ok=5794 NoBracket=2692
ExercisedEarly=177`, all rows `snapped=1`; `rv_21d/rv_63d/iv_minus_rv` NaN
for 35/8663 rows (early-year trailing-history shortfall, expected);
`n_events_to_expiry` NaN for all 8,663 rows (no `--events` supplied, as
expected — NaN, not `0`, confirming the "no calendar" contract); zero
duplicate keys; leakage tripwire `corr(log_ratio, iv_minus_rv)=0.054`,
`corr(log_ratio, market_vol)=0.053` (both far from ±1, no leakage red
flag on this single-tenor slice).

## Data caveats (both by design, both documented in-code)

1. **Close-to-close-only realized vol.** `rv_21d`/`rv_63d` come from
   spot-mirror bars (`O=H=L=C=spot`), so only `RvEstimator::CloseToClose`
   is meaningful — Parkinson/Garman-Klass/Rogers-Satchell/Yang-Zhang stay
   dormant until real OHLC bars land in the corpus (roadmap §5, still
   pending).
2. **Day-resolution, hand-supplied events.** `--events` timestamps are
   midnight UTC of the announcement date (a deliberate day-resolution
   approximation, not an intraday-timestamped feed), and the TSV itself is
   hand-supplied — there is still no point-in-time vendor earnings
   calendar wired to the corpus (roadmap §5, also still pending).

## Residual-work register

House convention: roadmap/product residuals, not code-review minors
(those are tracked per-task in the ledger, `.superpowers/sdd/2026-08-09-
atx-vol-feature-factory/progress.md`, and were resolved or explicitly
deferred at each task's own review).

**Named in the brief, still open:**

1. **Real OHLC bars.** Acquisition item (roadmap §5) — already adjacent
   (atx-db price metrics pipeline) or the archive spot series gives
   CtC-only in the meantime. Unblocks the YZ/GK/RS estimators.
2. **Point-in-time earnings-calendar sourcing.** Acquisition item (roadmap
   §5) — vendor calendar, or SEC 8-K timestamps as an as-filed fallback.
   The `--events` TSV format and `count_events_at` plumbing exist; nothing
   produces a real one yet.
3. **S3 trainer + validation harness, next.** Baselines (HAR-RV,
   linear-on-schema) first, then GBT with quantile heads; purged K-fold +
   embargo, CPCV, walk-forward — Python-side, out of this sprint's C++
   scope (roadmap §4 S3).

**Sprint-accumulated (ledger-tracked, deferred as coverage-breadth minors,
not correctness bugs — none blocked any task's review):**

4. `iso_date_to_ns`'s calendar-invalid round-trip branch (e.g.
   `2026-02-30`) has no dedicated test — only the `month>12`-style guard is
   exercised.
5. `n_entry_dates_forward_invalid`'s per-date-skip path has no dedicated
   counter-level test (breadth, not correctness — the guard itself is
   exercised via `FeatureBlockHeaderAndValues`).
6. `bev_corpus_run.py`: malformed-JSON/missing-`--manifest` surfaces a raw
   traceback instead of a clean error message; a nonexistent `--exe` path
   raises an uncaught `FileNotFoundError` that kills the whole batch
   (narrow-impact, global-misconfig scenarios).
7. `bev_label_qa.py`'s `parse_tsv_file` silently drops keys on a
   short/malformed row (`dict(zip(...)`) rather than a clean `ValueError`
   exit-2; the report-only leakage property has unit-level, not
   end-to-end-through-`main()`, coverage.

## Validation state

- **Hygiene (`atx-build.ps1 check`, `build/`, `dev` preset —
  `ATX_USE_PCH=ON`).** The task brief describes this as a "PCH-off"
  check; per `CMakePresets.json`, `ATX_USE_PCH` is only `OFF` under the
  separate `hygiene` preset (`build-hygiene/`), which is **not configured
  in this worktree**. The `check` verb (`scripts/atx-build.ps1`) hardcodes
  `build/` regardless of `-Preset`, so it always runs against `dev`
  (PCH ON) here — the same command and the same PCH-ON behavior Tasks 1
  and 3 already used and reported in this same sprint. Both touched TUs
  compile clean after a forced fresh recompile (mtime-touched, not relying
  on ninja's cached "up to date"):
  `atx-vol/examples/bev_label_factory.cpp` and
  `atx-vol/tests/bev_label_factory_gate_test.cpp`.
- **Targeted `ctest -R` gate** —
  `'BevLabelFactoryGate|Breakeven|BevPathLoader|RealizedVol|TheoEngineTest'`,
  **91/91 passed, 0 failed**: `RealizedVol` (7), `Breakeven` (19),
  `BevLabelFactoryGate` (9), `TheoEngineTest` (51), `BevPathLoader` (5).
- **`python -m pytest atx-vol/scripts/ -q`** — **32/32 passed**
  (`bev_corpus_run_test.py` + `bev_label_qa_test.py` combined).
- Full command transcripts and tails are in this task's own report,
  `.superpowers/sdd/2026-08-09-atx-vol-feature-factory/task-6-report.md`.

## Commit map

`079798e` (base, prior sprint's docs) → `9bbf0c8` events-calendar input
(Task 1) → `24a1e3e` spot-history pre-pass + per-entry-date RV panel
(Task 2) → `a06e7fb` fair-vol feature schema v1 emission (Task 3) →
`e4b1b71` corpus batch runner (Task 4) → `444c899` label-corpus QA report
(Task 5) → this commit, sprint closeout (Task 6: changelog, this summary,
roadmap §1 update, README Scripts-list entry).
