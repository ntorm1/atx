# L5u — a LABEL-FREE full-board quote-bench dataset, input census, one 1 % sample run (2026-10-20)

Lane `L5u-universe-dataset`, branch `feat/vol-L5u-universe-20261020` (base `be379ad4`, pool-34, RunId
`vol-20261020r3-L5u`), vol-sota sprint, Fable 5.1. Non-build Python lane. Evidence folder:
`C:\atx-cache\quote-benchmark\universe-20261014\` (`RESULTS.md`, `sha256.txt`); report
`.superpowers/sdd/2026-10-18-vol-sota/lane-L5u-universe-dataset-report.md`.

## What
A whole-board quote dataset in the dev200 layout **without labels** — the robustness input for "a wide array of
underlying assets" — plus the input-quality census the W4 harness needs, the nine L1b class counts on the universe,
a seeded stratified 1 % sample (61 underliers) and ONE sample fit on the W2b binary as the first universe-sample
speed / refusal-taxonomy row.

PM decisions 3-4 (roadmap §4) in force: the quote-slice path is THE universe fitter; a universe dataset necessarily
contains the holdout underliers, so it is allowed ONLY label-free — no `labels.parquet`, no `srPrc` / `srVol`
anywhere, never scored, never an input to tuning. `split` is the constant `universe` on every row so holdout
membership is not marked. SpiderRock marks were never read.

## Why this source (Step 0)
The preferred 2026-08-21 OPRA board is absent from the host: `C:\atx-data` does not exist (the whole L134 / L147 /
L186 corpus is gone; one C: disk). The complete board that IS on disk is the SpiderRock
`OptionIntradayHist_30min_2026-10-14.parquet` (sha256 `a0e0416f…`, 34,011,291 rows, 6,067 underliers, 20 buckets)
— the source every sprint cohort was prepared from. Board = its **10:30 ET bucket** (2,082,476 rows). Only the
adapter's 20 raw quote / contract / environment columns were requested from it. The OPRA pull command is in
RESULTS.md §0 and was not run (no authorisation; $0 flat-rate per L133).

## How (new tool, no existing file edited)
`quote_benchmark.py prepare` cannot be driven label-free (it requires `srPrc` / `srVol` in the source and writes
`labels.parquet` unconditionally), so NEW `atx-vol/tools/build_universe_dataset.py` imports the adapter
(`normalize_quotes`, `QUOTE_SCHEMA`, `INPUT_COLUMNS`) from the sibling file and reads
`ParquetFile.iter_batches(columns=INPUT_COLUMNS)` only; it writes `quotes.parquet`, `quotes.csv`, `manifest.json`
in the shape `verify_dataset` accepts (`quote_benchmark.py run` and `shape_cohort.py build` consume it unchanged).
NEW `atx-vol/python/tests/test_build_universe_dataset.py` (4 passed): the reader never requests a label column
(instrumented), a label-less and a label-poisoned source give sha256-identical outputs, rows equal `prepare`'s in
every column but `split`, filters / tags / refusals. dev200 (72,348 rows) and cohort5 (22,200 rows) are
row-for-row SUBSET-IDENTICAL to the universe on every column but `split` (same adapter, same source row ids).

## Numbers (RESULTS.md §1-5; all label-free, nothing scored)
- Dataset: **2,082,476 rows · 6,067 underliers · 37,086 chains · 1,580,163 two-sided contracts (1,480,862 American)**;
  `input_status` ok 1,926,970 (92.5 %), unsupported_exercise 114,149 (European roots), invalid_option_quote 34,286,
  invalid_time 3,734, invalid_underlier_quote 3,040, invalid_carry 297. `quotes.parquet` sha256 `09d0e800…`,
  `quotes.csv` `2c499e73…` (624 MB).
- Tiers (two-sided per underlier): ≥1000 298 names / 41.3 % of contracts · 500-999 420 / 18.5 % · 200-499 936 /
  17.7 % · 100-199 1,347 / 11.9 % · 50-99 1,704 / 7.9 % · 25-49 891 / 2.2 % · <25 471 / 0.4 %. Cumulative ≥500
  718 / 59.8 %, ≥100 3,001 / 89.5 %, ≥50 4,705 / 97.4 % — the same cliff as the OPRA board (L136 / L191).
- Expiries per underlier: modal 4-5 (4,150 names; p50 4, p90 12, max 55); 70 % of names quote ≤ 5 expiries.
  Strikes per expiry p50 21 / p90 52 / p99 163 / max 700.
- Quote quality: two-sided 75.9 %, one-sided 24.1 % (zero-bid wings 15.2 %, zero-bid core 6.9 %), crossed 0,
  locked 2 rows; core (|ln K/S| ≤ 0.2) two-sided spread/mid p50 0.222 · p90 1.149 · p99 1.930, 22.6 % above the
  bench's default `--max-spread-to-mid 0.6`.
- Dividend payers (`ddiv_pv > 0`): 3,182 / 6,067 underliers (52.5 %); 31.9 % of rows, 33.8 % of eligible rows
  (L228: 42.1 % of tune rows).
- Nine classes on the universe: index_etf_dense 3 · mega_cap_single 71 · high_vol 172 · sparse 1,947 ·
  low_price_wide 2,176 · dividend_payer 3,173 · carry_abnormal 352 (supplied carry) · short_dated 6,017 underliers /
  6,070 expiries · long_dated 1,868 / 4,076; 80 underliers unclassified (no eligible row).
- Sample (seed `L5u-1pct-v1`, largest-remainder per tier: 3 / 4 / 9 / 14 / 17 / 9 / 5): 61 underliers, 18,606 rows,
  371 chains (ok 18,272); roster sha256 `e3a6c408…`; `sample-1pct\quotes.parquet` `76f63a7c…`.

## The one sample run (W2b binary e86e0b98, graded-v1 d7f89d13, funding-pv, --timing-detail)
Load gate: host sampled every 2 min for 15 min, 37-99 %, never below 30 % → the brief's fallback
`ATX_VOL_FIT_WORKERS = 4`. Attempt 1 (PowerShell-wrapped `& python … 2> file`, `$ErrorActionPreference = 'Stop'`)
produced no fit: the binary died with exit 4294967295 (−1, the `TerminateProcess` / `Process.Kill` code) and an
empty stderr — a path the binary's own `main` cannot produce (0, or 1 after a `std::exception` message); the
identical command under the Bash tool is THE run. Timing unqualified (shared host 23-99 % load, one run).

- **32.56 s `fit_and_reprice_seconds` (33.04 s wall) @ 4 workers** for 18,606 rows / 61 underliers / 360 chains
  attempted → **308 fitted (85.6 %)**; graded A 56 · B 218 · C 34; profiles ordinary 218 / sparse 142 / dense 0.
- **Served 15,240 rows = 81.9 % of rows (83.4 % of eligible); 61 / 61 underliers served.** Refusals by `status`:
  calendar_arbitrage 1,806 (9.7 %) · arbitrage_failure 542 · unsupported_deliverable 470 · input_rejected 334 ·
  invalid_volatility 142 · insufficient_quotes 72. `quote_status`: admitted 1,576 (8.5 %) · excluded_side 8,806 ·
  bidless 4,482 · wide_spread_to_mid 1,616 · wide_spread_vol 1,322 · not_attempted 804. Fit failures: 32 calendar
  crossings; 11 "nondividend American: scaled European upper bound violated at strike K" (K 1.50-54.98, low-price
  names); 4 slope / convexity nodes; 6 too-few-channels.
- Per-tier served % of rows: ≥1000 83.2 · 500-999 64.5 · 200-499 90.5 · 100-199 88.0 · 50-99 86.8 · 25-49 75.1 ·
  <25 67.8 — not monotone in density; calendar crossings refuse whole slices.
- **Chain latency p50 144.6 ms · p90 629 ms · p99 1.51 s · max 2.96 s**; Σ chain wall 90.15 s (0.25 s / chain);
  surface wall 32.50 s → 2.77 of 4 workers busy.
- **87.3 % of chain wall is American IV inversion (deam 78.67 s)**; price 7.5 %, certify 4.6 %, calendar 0.6 %,
  solve 0.1 %. **193,275 AL boundary solves (≈ 10.4 per row) @ ≈ 407 µs** of deam wall each; 242,999 IV Newton
  iterations. `deam_inversion_*` = 0 because graded-v1 is keys-OFF (no `graded.fit_space vol` / Premium inversion;
  W2b all-keys SPY: 75,153 solves @ ≈ 149 µs).
- **Whole-board extrapolation (Inferential)**: 4.85 ms CPU per row → 2,082,476 rows ≈ 10,100 s CPU ≈ 2.8 CPU-hours
  (chains: 37,086 × 0.25 s ≈ 9,300 s); ≈ 60 min wall at 4 workers, ≈ 15 min at 16 workers at the observed 69 %
  efficiency — vs the gap-report budget of ≈ 11 s at 16 cores (7.2 µs / contract): **≈ 80× off**. L5m runs the
  board on the owner's binary, not on e86e0b98.
- Step caps (PM directive 2026-10-20): every compute step of this lane ran ≤ 65 s (universe build 64 s, census
  2.5 s, classes 5.9 s, sample build 28 s, run 33 s); the 15-min load gate was idle waiting, not compute.

## Regenerate / consumers
Commands with hashes: RESULTS.md §8. W4 harness: `census\per_underlier.csv` (one row per underlier, every
feature) + `shape-classes\*.csv` memberships. L5m: the W-root binary on this folder's `quotes.csv` with
`ATX_VOL_FIT_WORKERS = N`; §6-7 of RESULTS.md are the baseline. Holdout: contained, unmarked, unlabelled, never
scored.

## Deviations
`-E -P` instead of `-I` (`-I` drops the user site where polars 1.40.1 lives); new tool instead of `prepare` (see
How); test run ad hoc with `--noconftest`, CMake registration left to the integrator (no existing file edited);
`shape-classes\` beside `census\`; timing unqualified (shared host, load sampled before / after).
