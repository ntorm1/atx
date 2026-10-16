# Frozen common-model resolvent development experiment

Prepared before new synthetic or development scoring. Lane
`feat/vol-resolvent-dev-plan-20261016`, frozen base
`b2909ede55300a6a47751292a0d09daec3806b38`. This lane changes only this note and
experiment JSON. It does not change pricing, fitting, policy defaults, caps or
the benchmark tools. The parent owns the reviewed native/audit integration.

## Fixed hypothesis and policies

The positive variance resolvent removes explicit CFL subdivision while keeping
the same deterministic calendar carry, discount, cash convention and exercise
layers. Its two sweeps cost twice the logical state visits per transition.
The question is whether this numerical tradeoff changes development coverage,
quote fit and paired vendor-reference errors under unchanged resource caps.
It is a first-order approximation with material coarse-step bias; positivity
does not certify continuous-American accuracy.

The following files are frozen in
`bench/experiments/2026-10-16-common-resolvent/`:

| Trial | Method | Both maximum clocks | Policy SHA256 |
| --- | --- | ---: | --- |
| E0 | Explicit Euler | 1/52 | `3d42a3bb5d661fa3d262aedf09474aebbfc6b8a7a1c4b5b4310d6d996c7a5217` |
| R104 | Variance resolvent | 1/104 | `dc5e0d56170ff78e66e5c46b009a8f3c2e0b247c710a8956bff8cd54712a6d2e` |
| R416 | Variance resolvent | 1/416 | `f3cc39c5dc7b333263dadd88574e470d0e1d27db998eb205a83f6d2d3e16f41f` |

Files are `e0-policy.json`, `r104-policy.json` and `r416-policy.json`.
Each explicitly freezes the whole expanded profile, market, classifier and IV
configuration rather than relying on future defaults. The only differences
between files are diffusion method and the two maximum clocks.

Keep the sparse 65-state / concentration .20 and ordinary/dense 129-state /
concentration .10 grids, upper domain 4, profile assignments, shared/per-interval
time shapes, sigma bounds, priors, all optimizer/resource limits and
`midpoint_weight=0`. The new transition-work budget is 8,000,000 in every arm;
the row budget remains 1,000,000. Ordinary/sparse fit work remains 50,000,000;
dense remains 100,000,000. Quote seeds are disabled: initial sigma is exactly
.25, initial/prior vectors are empty, and native execution must not call the
quote-seed estimator. Empty priors retain the fitter's fixed initial-vector
prior semantics. There is no seed key in the closed policy schema at this base.

All trials use the same funding-PV continuous-yield approximation and flat
equivalent-IV reporting convention. No dividend ex-dates are inferred. Local
sigma is never substituted for an unidentifiable equivalent IV.

R104/R416 provide a predeclared fourfold maximum-step contrast. Individual
intervals use ceilings and original expiry endpoints, so realized step counts
need not differ fourfold. E0 retains CFL subdivision and its original 1/52
clock limits: E0 versus a candidate is a method-plus-clock comparison, not a
pure operator comparison at identical time layers. Do not retune these clocks
from development labels or promote a default from this panel.

## Cohort and byte-parity gate

Use the immutable development-only dataset
`C:/atx-cache/quote-benchmark/20261014-dev200`: 72,348 selected/output rows and
63,942 eligible rows. No validation data or validation labels may be opened.
Native fitting receives only its closed raw quote CSV. `srPrc` and `srVol`
remain separate evaluation labels read after predictions are published.

The fixed baseline is `20261016-common-storage-parity-v2.csv.run.json`, SHA256
`df85b627e2b4cf991249698840a8f90a0ccdd23183971b8a9310560cfe5a1a77`.
Its prediction CSV and the original common baseline share SHA256
`3f6965872f72e137956f8a8534adbd0e2755f6804023cec333860e7faf835cd3`.
It serves 25,014 prices and 18,978 identifiable IVs. The full failed/rejected
rows stay in the output and in all coverage denominators.

After independent exact-SHA review of the parent seam, archive the executable
and adjacent runtime dependencies with its source/build hashes. First run E0
standalone, with a fresh immutable start/run receipt and native progress journal.
Require exact prediction-byte parity against the fixed baseline before launching
R104/R416. New method/work provenance fields may differ from the old receipt;
compare the old numerical/configuration fields separately. A parity failure
blocks the candidate launch and is investigated rather than accepted as noise.

The campaign harness has no byte-parity gate. After E0 passes, launch a fixed
two-trial R104/R416 campaign against the original fixed baseline, using copies
of these already frozen policy files. Freeze its plan, policies, executable,
runner/campaign source, raw inputs, manifest and baseline before any candidate
scoring. Use new output directories, durable journals and exclusive outcomes;
never overwrite or silently resume an interrupted run. The eventual run receipt
must record the exact reviewed source SHA and executable hash; neither is
invented in this pre-execution plan.

## Raw-only work feasibility

`raw-feasibility.json` records the calculations and source provenance, SHA256
`7f01b2e9b15e92424e127ab79a4cc56abf204e053885669b4e670e5a471f92ee`.
The source is the existing native storage-parity report, SHA256
`1575729e86f5c9dc2a04c6e19673f01471511d518328628f30791970c1963524`.
Only its raw-market cumulative clocks, per-expiry quote counts, resolved grid,
profile and unchanged limits enter the feasibility calculation. No fitted
sigma, model price, reference price or reference volatility enters it.

For interval j and denominator D, use represented binary64 arithmetic:

```
n_j = max(1, ceil(delta_calendar_j / (1/D)),
             ceil(delta_variance_j / (1/D)))
model_rows = N * sum(n_j)
model_transition_work = 2 * model_rows
fit_work = 2 * N * sum_j(quote_count_j * sum_{k<=j}(n_k))
```

This funding-PV route has no explicit cash-grid transitions; state count N is
constant within each snapshot. Quote counts were reconciled exactly to all
184 snapshots that reached fitting, totalling 45,070 rows. The other snapshots
remain refused before fitting under the unchanged policy/market rules.

| Trial | Cap-feasible snapshots | Row upper bound | Eligible fraction upper bound | Largest feasible fit work |
| --- | ---: | ---: | ---: | ---: |
| R104 | 184 | 45,070 | 70.4858% | 27,417,144 |
| R416 | 176 | 34,528 | 53.9989% | 49,314,636 |

These are cap-feasible ceilings, not predictions of numerical admission or fit
success. Across all considered snapshots, R104 uses at most 247 transitions /
31,863 logical rows; R416 uses at most 951 transitions / 122,679 rows. Thus the
identified R416 refusals come from quoted-prefix fitting work, not model rows.

R416 necessarily exceeds the unchanged 50,000,000 ordinary fit-work cap for
ADP, CVX, EXPE, HUT, IEF, MOD, SCHW and UPRO: eight snapshots and 10,542 rows.
All eight already failed that cap in E0. The evidence JSON preserves exact
counts, work and refusal reasons. This list is evidence about this cohort;
neither policies nor reusable library code contain symbol-specific treatment.

## Synthetic checks before candidate execution

Run both fixed native spatial profiles, 65/.20/upper4 and 129/.10/upper4, at
maximum clocks 1/104, 1/416 and diagnostic 1/1664. This is six predetermined
resolvent probe configurations. The standalone storage probe needs an explicit
concentration-scale override: its historical fixture widths differ, so changing
`--nodes` alone is insufficient. Preserve both requested/resolved scale in the
receipt. The parent owns that small probe extension.

Retain the probe's complete seven-fixture panel: martingale/no cash, short low
volatility with unequal clocks, high volatility/tail exposure, dated cash,
local volatility, paused variance and deterministic cash collapse. Report
compile refusals explicitly. Native candidate execution also requires the
owning dense inverse/transpose/martingale tests, independent active-exercise
dense Snell checks and default parity gates to pass. No resource cap is raised
to force a synthetic model to compile.

For numerical diagnostics, compare exact unchanged contract signatures against
saved independent references, plus exact European/deterministic controls where
applicable. Preserve unsupported and unresolved references. The original-state
moment/mass checks diagnose finite-model consistency; temporal, spatial and tail
error remain separate. Do not require monotonically decreasing American error:
the prior fixed-grid experiment demonstrated error cancellation. The diagnostic
1/1664 probe does not add a fourth raw-data trial or authorize clock retuning.

## Required reporting

Report own-population coverage and errors separately from exact paired results.
Create E0/R104, E0/R416 and R104/R416 comparisons, plus fixed three-way price
and IV intersections. Preserve each arm's unmatched successes, failed fits,
model/resource refusals, input exclusions and unidentifiable IVs. Changing
coverage must not make a selective aggregate look like an accuracy improvement.

Include price/IV MAE, RMSE, signed bias, median, p95/p99 absolute errors and
improved/worse/tied counts on the same contracts. Retain raw profile, DTE, side,
moneyness and bidless strata. Report strict raw bid/ask containment and interval
error magnitudes on both own and shared prediction populations, independently
of reference-label availability. Keep optimizer termination/evaluations,
failed-evaluation counts, realized steps, logical rows, 2N work and stage timing.
Timing remains unqualified unless independently measured on a quiet pinned host.

Report terminal upper and zero boundary mass per fitted snapshot, clearly
identified as unconditional mass at its largest expiry. It is neither a
per-contract boundary probability nor a pricing-error bound. Provide unweighted
and row-weighted summaries, plus predeclared exceedance counts at 1e-6, 1e-3
and 1e-2. Never filter scoring rows using boundary mass.

E0 has 162 fitted snapshots. Upper-mass exceedances at those thresholds are
56/36/17 snapshots, associated with 10,826/7,430/3,436 rows; maximum mass is
.15304467644489098. Sparse/ordinary fixed upper domains can therefore dominate
some outcomes even after time refinement. No score or synthetic result in this
experiment automatically promotes a method, grid, prior or production default.

## Preparation validation

After writing the files, strict JSON parsing checked duplicate keys and
nonfinite constants. Policy hashes match the evidence receipt. Removing only
the newly explicit method/work fields and profile base selectors reproduces
the baseline expanded configuration exactly; normalizing method/clocks makes
R104 and R416 exactly equal to E0. Fixed starts, empty seed/prior vectors,
zero midpoint weight and unchanged caps were checked separately. Feasible plus
refused row counts reconcile to 45,070 in both candidate arms. Git whitespace
checks pass. No new C++ or Python source was written, so no build/test suite was
needed for these documentation/configuration changes. Synthetic controls, E0
execution and the candidate campaign remain pending parent seam review.
