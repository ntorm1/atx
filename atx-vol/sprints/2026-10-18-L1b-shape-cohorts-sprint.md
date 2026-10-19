# Sprint note — L1b shape-class cohorts (2026-10-18)

Lane `L1b-shape-cohorts`, branch `feat/vol-L1b-shape-cohorts-20261018`, base `7cebbbd3`
(PM prebase = root + knot-ladder + al-speed + deferred-tests), pool-36. Plan
`docs/plans/2026-10-17-atx-vol-sota-fit-dag-plan.md` W1 / L1b. Tool-only (Python): no fit, no C++,
no policy JSON, no new market data. Full tables: `C:\atx-cache\quote-benchmark\shape-classes-20261014\RESULTS.md`.

## What

- `atx-vol/tools/shape_cohort.py build --cohort NAME=DATASET ... [--implied-forward NAME=REPORT] --output DIR`
  classifies prepared cohorts into the nine plan-§2 classes and writes `<class>.csv` (one line per
  member cohort/underlier/expiry), `features.csv` and `manifest.json` (rules, counts, overlaps,
  representatives, carry cross-check). Seven symbol classes (index_etf_dense, mega_cap_single,
  high_vol, sparse, low_price_wide, dividend_payer, carry_abnormal) and two expiry filters
  (short_dated `dte <= 3`, long_dated `dte > 365`) applied to every underlier.
- `quote_benchmark.py score --class-manifest manifest.json` (opt-in) adds `class_strata`: per class
  (plus `no_symbol_class`, `all_rows`) the scorer's own metrics, standard-symbol coverage, and served
  shape violations (monotone / convex / calendar, tol 1e-6, the knot-ladder `check_served.py`
  definitions). The manifest cohort is bound by the dataset's `quotes.parquet` sha256; every class file
  is sha256-verified.
- Tests: `python/tests/test_shape_cohort.py` (6) and 4 new cases in `test_quote_benchmark.py`, wired
  into the existing `atx-vol-quote-benchmark-tools` ctest (one CMake line): 163 passed.

## Why these rules

Label isolation is structural: only 15 fixed `quotes.parquet` columns are read, label columns in the
quote file are refused, `labels.parquet` is never opened (a test moves it away and the class files stay
byte-identical). Each rule is one Python expression that is both the executed code (`eval`, no
builtins) and the manifest text, so the published predicate cannot drift. Thresholds were fixed from
feature distributions before any score was computed. Carry: median implied yield from the dev200
implied-forward report (applied chains, tenor >= 30 d) else the supplied `sdiv`; on dev200 11 of 116
implied-carry underliers would flip class with the supplied carry, so `carry_abnormal` is the least
stable class.

## Numbers (dev200 / dev200b / cohort5: symbols, rows)

index 0 / 0 / 1 (SPY 13,168); mega 4 (ASML, META, TSM, V) / 6 / 1 (AAPL); high-vol 3 / 4 / 2 (MSTR,
CRWV); sparse 78 / 69 / 0; low-price 78 / 73 / 1 (RIVN); payers 99 / 110 / 2 (= composite-20261017's
payer list, 45,754 / 36,914 rows); carry 17 / 15 / 0; short-dated 10,576 / 9,126 / 1,786 rows;
long-dated 9,064 / 7,820 / 4,736 rows. Absent representatives: QQQ, IWM, MSFT, NVDA, AMZN, TSLA,
COIN, SOFI, F, XOM, GME, AMC. CRWV is present but not sparse (20 expiries, 88 rows / expiry); it is
high-vol.

Baselines (existing CSVs by sha256: graded-v1 1348f63d / 4c70d2bc, repair-v3 14d9dc6d / 81cc69e0, C5
146471ea / 095ff45d, ladder point 6ebd63c5 / d53d2a0d, strict 858d6512 / c5ab9121, C6 / C6d from
composite-20261018-c6; cohort5 supplementary). Today's §1.2 misses: coverage in nearly every class for
the slice policies; mega-cap median |IV − srVol| 18-61 bp vs <= 5 bp under every policy; sparse
MAE/spread 0.24-0.31 on dev200b vs <= 0.15; SPY (cohort5) 11-47% coverage, 0.14-0.78 MAE/spread,
68-96% containment. Violations: 0 in every class for graded-v1, repair-v3 and both ladders; the
status-routed composites break calendar across sources in every class (C5 dev200 24 convex + 2,511
calendar, dev200b 36 + 1,917; every C5 calendar break pairs two different serving sources; C6 / C6d
similar), so a composite C7 needs a cross-source calendar certificate to pass the hard gate.

## Hashes

Default scorer output: stdout byte-identical to the base tool and score JSON identical except the
`evaluator_sha256` value (the tool's own hash) on 4 receipts — proof in
`C:\atx-cache\quote-benchmark\L1b-shape-cohorts-20261018-parity\`. Class manifest `ad323ccb…`,
`shape_cohort.py` sha256 `377fc7ab…`, scorer sha256 `fbecf2cb…`; every output file in the folder's
`sha256.txt`.
