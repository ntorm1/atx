# Common-model policy experiments and status-routed hybrids (dev200)

Development-cohort only (`20261014-dev200`, 63,942 eligible rows). Labels were used only to score
published predictions. Nothing here promotes a default or touches holdout data. Full tables,
receipts and SHA256s are in `C:/atx-cache/quote-benchmark/policy-experiments-20261016/RESULTS.md`
and `artifact-sha256.json` there.

## Setup

- Base: frozen `bench/experiments/2026-10-16-common-resolvent/r104-policy.json` (variance
  resolvent, clocks 1/104). The coordinator switched the base before any launch, so the E0-based
  policy set was generated but never run. Each experiment changes only the named keys.
- Binary: `atx-vol-quote-bench` from `5e978fed` (vol-rel-avx2), archived in
  `common-seed-20261016-bin/`, SHA256 `2f3767f9…de92`. The control X0 (unchanged R104 policy)
  reproduces the reference R104 predictions byte for byte (`6b129f64…`), so every difference below
  is attributable to the policy alone.

## Results (exact intersection with R104 unless noted)

| Run | Change | Price coverage | MAE vs R104 | Note |
|---|---|---|---|---|
| X1 | quote-seed init + sparse prior 0.001 | 70.49% (=) | 0.3519→0.3428 (44,468 rows) | class A 0.733→0.688, core vol 1,136→1,053 bp |
| X2 | ordinary 5 knots ±0.3/±0.6 | 70.49% (=) | 0.3519→0.3559 | 84/91 ordinary fits hit the 256-evaluation limit |
| X3 | ordinary per-variance-interval | 43.14% (cap 32 refuses 16 more snapshots) | 0.3899→0.3894 (27,162) | ≤7d vol worse; every ordinary fit evaluation-limited |
| X5 | X1+X2+X3 | 37.32% | 0.4055→0.3951 (23,461) | same gain as X1; class B worse |
| X4 | market quotes 8192, dense params 96 | 86.79% (+ASML/TSM/V) | identical on R104 rows | new dense fits unconverged; ASML MAE 3.77 vs mid 0.80; META still refused |
| Hybrid B | quote-slice → X1, by status | 75.70% | 0.3519→0.2983 | own MAE 0.2919 on 47,769 rows |
| Hybrid A | X1 → quote-slice, by status | 75.70% | identical to X1 | forfeits the quote-slice on 7,358 shared rows |

Verdicts:

- H2 (sparse prior pull) is partially supported. The seed applied to only 8 of the 39 class-A
  snapshots; the other 31 stay near 0.25 because the hinge loss is flat inside wide quotes.
- H5 (5 knots) and H4 (per-interval time shape) are not supported under the unchanged evaluation
  and parameter budgets. Both runs were evaluation-limited, so they are not capacity tests at
  convergence.
- H3 (caps) is supported for coverage but not for accuracy.
- H6 (quote-slice-first status routing) is supported.

## Tool

`atx-vol/tools/compose_predictions_by_status.py` merges an ordered list of prediction CSVs that
share the dataset row order. Each row comes from the first input whose `status` is `ok`;
otherwise the first input's refusal row is kept. It reads only the status column, never labels or
errors. It preserves the union of columns, adds `source_model`/`source_index`, and writes an
exclusive `<output>.run.json` with input SHA256s and per-source counts. Given `--receipts`, it
verifies the inputs' prediction hashes and copies the quote digests, so
`quote_benchmark.py score/compare` accept the composed receipt. Mixed-model rows are not jointly
arbitrage-checked.

## Open follow-ups (each needs its own predeclared trial)

Possible next trials:

- Scale `max_evaluations` with parameter count for 5-knot or per-interval shapes.
- Make the seed sparse-aware, so wide and bidless quotes contribute.
- Split X1 into seed-only and prior-only runs.
- Add `midpoint_weight=0.1` to the R104 base. On cm-fitted well-levelled snapshots cm has
  0.268 vs R104 0.333.
- Raise dense parameter capacity for META.
- Route quote-slice-first in the runner.
