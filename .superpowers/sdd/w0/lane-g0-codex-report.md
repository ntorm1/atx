# G0 truth-delta report

Outcome: COMPLETE. All frozen-input measurements and artifact checks passed; no alpha promoted.
Branch: `feat/w0-g0-codex-20260925`, pool-3.
Frozen W0 input/source base: `bc5cc646b46f6a7c23a60e87d28dfa9b972671ec`.
Evidence root: `C:/atx-wt/g0-data/bc5cc646_20260925`.

The 14 context axes were checked to contain only sessions before 2020-01-01 and
their payload hashes recorded in `input_contexts.json`. Input artifacts were read
only. The only write beneath C:/atx is the plan-authorized atomic heavy-run lock,
acquired separately for each process and removed only with matching ownership token.
No warehouse was opened and no alpha setting was tuned.

## Measured results

| Measurement | Old | New frozen W0 | Result |
|---|---|---|---|
| 2013 native baseline | Abort, period 6, security 150340 on 2013-04-12 | Frozen W0 same error; corrected replay completes with 32 assumed liquidations | Stress diagnostic; ineligible as alpha evidence |
| L7 PIT risk scorecard | Five configurations, old archived JSON | All headline metrics unchanged; three scalar differences at 1e-9 or 1e-10 | No material delta |
| L10 fundamental zoo | No candidate; 0 positive t>2 | No candidate; 0 positive t>2 | Completed, exit 0 |
| L9 guarded mining | 0 admitted; blend validation net SR -1.218738 | 0 admitted; blend net SR -0.535778 | Completed, exit 0; no alpha admitted |
| cp21 scorecard | R16-8 0/29 candidates; two individual h21 cut CIs above zero | R16-8 0/29; no individual h21 cut CI above zero | Completed; no candidate |

Frozen native baseline receipt: `logs/base2013.receipt.json`, exit 1, 2.031 seconds,
peak working set 0.146 GiB. Exact stderr:

```
replay: missing/nonpositive required close at period=6 instrument=604 session_key_ns=1365724800000000000 security_id=150340
```

The replay and D12 fixes were integrated at
`8ee78be46c0cfc01d0c892e77fd4a2671ce942f7` after the owning compiled gates and an
independent D12 reduced-universe oracle passed. The corrected unpinned binary is
preserved in `bin/corrected` with a source marker and hashes. Native baseline completed
at the original boundaries in 2.266 seconds / 0.146 GiB; explicit Abort control still
fails at exactly the same security and session (1.766 seconds / 0.146 GiB).

**The completed native baseline is ineligible for alpha performance evidence.** Its
nested report correctly records `usable_for_alpha_evidence=false` and
`ineligible-assumed-missing-price-liquidation`. All 32 terminal actions are assumed
missing-price stress liquidations; zero are evidenced delistings. Their aggregate
PnL is -$1,957,929.80, including -$154,981.83 across seven flagged shorts. Gap carries
are zero. Security 150340 and 351548 are known nonterminal holes; this run does not
relabel them as observed delistings or supply lookahead prices.

| Corrected native baseline diagnostic | Value |
|---|---:|
| Initial / final NAV | $100,000,000 / $105,910,188.38 |
| Total return / annualized Sharpe | 0.0591018838 / 1.3333732017 |
| Gross / net PnL | $7,807,188.32 / $5,910,188.38 |
| Trade / borrow costs | $496,629.56 / $1,400,370.38 |
| Max drawdown | 0.0274650390 |
| Intervals / trades | 188 / 38,397 |
| Unfilled targets | 12 |

These numbers describe the stress scenario only. The frozen run has no completed
performance summary, so comparison tables leave its unavailable fields blank. G0
also found that the baseline top-level summary omitted the nested quarantine fields.
The tested presentation repair was cherry-picked alone at
`c32df9512075879827b75f5e465f2640c579d4c8`. Separate archive
`bin/corrected-disclosed` has executable SHA256
`2edbf4f5177ca3f9a8169ff6ee8ba3e4e939ceaf98e9b450a0bad77ee2f0870e`.
Its native rerun completes in 3.782 seconds; explicit Abort control reproduces the
same error in 2.015 seconds (both peak 0.146 GiB). Top-level qualification is now
`failed`, with all ten nested eligibility/count/PnL fields copied exactly. The entire
nested replay summary is byte-identical to the original 8ee output: all 49 leaves
unchanged. Only 12 top-level leaves change. Both versions remain preserved.

Frozen L7 receipt: `logs/l7.receipt.json`, exit 0, 61.625 seconds, peak working set
0.105 GiB. All 151 JSON leaf fields compared in `comparisons/l7_all.csv`; changes:

| Metric | Old | New | Delta |
|---|---:|---:|---:|
| hybrid_baing_ewma min-variance Q | 2.359139480 | 2.359139481 | +1e-9 |
| hybrid_baing_lw2020_spec_ewma min-variance bias | 0.9976938098 | 0.9976938097 | -1e-10 |
| hybrid_baing_lw2020_spec_ewma min-variance Q | 2.459494247 | 2.459494248 | +1e-9 |

The fourth changed field is only the input path's slash convention. These are
shared-host wall measurements; no speedup claim is made. Old native baseline timing
was Debug; old L10 and L7 runs did not record peak RAM.

| Runtime / peak working set | Historical | G0 |
|---|---:|---:|
| Native baseline, frozen Abort | 8.96 s / 165,318,656 B (Debug) | 2.031 s / 0.146 GiB (Release) |
| L7 | 81.5 s / unrecorded | 61.625 s / 0.105 GiB |
| L10 | 338.5 s / unrecorded | 302.079 s / 1.201 GiB |
| L9 | 877.5 s / 0.963 GB reported | 987.109 s / 1.007 GiB |
| cp21 baseline total | 28 s | 33.967 s, 0.111-0.346 GiB per cell |
| cp21 IC serial sum | 729.5 s; historical parallel wall 218 s | 574.439 s serial, 0.261-0.862 GiB per cell |

Historical cp21 cells took 30.6-85.0 seconds each, versus 21.812-68.391 seconds now.
Historical scorecard runtime is unrecorded. Compiler contention, old parallelism,
different correctness semantics and L9's disabled holdout prevent performance claims.

L10 receipt: `logs/l10.receipt.json`, exit 0, 302.079 seconds, peak working set
1.201 GiB; FundamentalZoo.RealDataIcReport passed 1/1 tests. The predicted direction
remains unsupported: none of the 60 expressions has positive non-overlapping t>2
on either cut. Selected horizon-21 changes (all metrics are in the sidecar CSVs):

| Signal / cut | Old rank IC | New rank IC | Old non-overlap t | New t |
|---|---:|---:|---:|---:|
| qual_gpa / t1000 | 0.01652 | 0.0166986 | 1.670 | 1.76392 |
| qual_gpa / t3000 | 0.02324 | 0.0226574 | 1.816 | 1.65685 |
| inv_iss / t1000 | 0.01805 | 0.0183854 | 1.371 | 1.68415 |
| inv_iss / t3000 | 0.01564 | 0.0158505 | 1.176 | 1.29324 |
| pead_sue / t1000 | 0.01294 | 0.0129431 | 0.754 | 0.457558 |
| pead_sue / t3000 | 0.00851 | 0.00846499 | 1.471 | 1.45926 |

L10 comparison tables cover 1,098 pooled metrics (758 changed), 8,052 annual metrics
(7,252 changed), 2,480 split metrics (1,437 changed), and all 253 alignment metadata
leaves (none changed). Primary attribution is E-09's delayed return alignment; E-02
changes interval estimates and A0 changes affected DSL operators. These effects were
not separately isolated; the table does not claim a single-defect attribution.

L9 receipt: `logs/l9.receipt.json`, exit 0, 987.109 seconds, peak working set
1.007 GiB. Seeds, seed, population, generations, costs and gating thresholds are
unchanged. Holdout was off; all train and validation inputs/boundaries were preserved.

| L9 metric | Old guarded run | New frozen W0 |
|---|---:|---:|
| Seeds / invalid | 378 / 0 | 378 / 0 |
| Candidates / scored | 2,243 / 2,065 | 2,504 / 2,306 |
| Decorrelation family | 52 | 56 |
| Alphas admitted | 0 | 0 |
| Historical descriptive N_eff | 5.714 | 6.750301 |
| Actual new DSR cluster count | Not recorded under old rule | 56 |
| Blend validation net SR | -1.218738 | -0.535778 |
| Blend validation gross SR | -0.131066 | 0.156475 |
| Blend validation one-sided p | 0.950334 | 0.770325 |
| Blend mean net bps/day | -0.8531 | -0.723268 |
| Blend turnover | 0.1523 | 0.186678 |
| Family mean validation net SR | -0.479157600 | -0.300800177 |
| Positive family validation SR | 10 / 52 | 20 / 56 |
| Best family validation net SR | 0.584294 | 0.929935 |
| Minimum family raw p / BY p | 0.178811 / 1 | 0.0908452 / 1 |
| Search digest | 729454a1ea25f532 | cdf326b6f5d6e3a8 |

The blend remains rejected with BY and Romano-Wolf p=1. The new registry stores
2,306 in-sample records over a 1,510-session train+validation calendar, with the
1,008-session training window explicit; its chain head is `14faf8293000bf69`.
The new DSR rule is cluster-mc-floor-v2, using 56 clusters. N_eff is retained as a
descriptive comparison and is not the new DSR trial count.

Attribution: D-01 raw-basis dollar-volume fields and A0 DSL semantics change the
search path; E-01/E-16 change trial accounting and DSR. The new digest prevents
candidate-by-candidate causal comparisons. All 372 gate-report leaves (113 changed)
and ten validation aggregates (nine changed) are published in the comparison CSVs.

CP21: all 13 baseline and IC cells completed, using the frozen family pin on corrected
source 8ee. The unchanged Python scorecard completed in 938.703 seconds / 0.303 GiB,
writing 9,080 scorecard rows and 58 capacity rows; all 13 cells have ADV inputs.
R16-8 remains **0/29 candidates**, and passing that diagnostic would still not establish
tradeability. At h21 / IncludeAuditedTerminalV1 / full, the number of individual
pooled cut CIs with positive lower bounds falls from two to zero. The maximum DSR
at fixed cumulative N=570 rises from 0.000559 to 0.002007, still far below acceptance.

| Signal / cut | Old pooled net SR [95% CI] | New pooled net SR [95% CI] |
|---|---|---|
| intraday_mom_252 / t1000 | 0.277603 [-0.406103, 0.945790] | 0.024571 [-0.798367, 0.768488] |
| intraday_mom_252 / t3000 | 0.676475 [0.003924, 1.383343] | 0.492608 [-0.225724, 1.280178] |
| day_minus_night_252 / t1000 | 0.263513 [-0.584436, 1.049396] | 0.307033 [-0.584041, 1.175884] |
| day_minus_night_252 / t3000 | 0.809007 [0.071870, 1.461723] | 0.889331 [-0.001467, 1.654306] |
| blend_equal / t1000 | 0.421997 [-0.192577, 1.086579] | 0.170088 [-0.468324, 0.775243] |
| blend_equal / t3000 | 0.451590 [-0.160824, 1.120576] | 0.424886 [-0.240122, 1.192628] |
| momentum_252 / t1000 | 0.466839 [-0.211558, 1.166128] | 0.210760 [-0.386538, 0.824319] |
| momentum_252 / t3000 | 0.431913 [-0.239128, 1.121693] | 0.438991 [-0.239836, 1.216774] |
| momentum_126 / t1000 | 0.423837 [-0.151676, 1.052742] | 0.194049 [-0.376749, 0.801804] |
| momentum_126 / t3000 | 0.448004 [-0.185227, 1.148206] | 0.368184 [-0.298963, 1.115365] |
| mom252_sector_neutral / t1000 | 0.365796 [-0.308948, 1.042901] | 0.186070 [-0.410584, 0.783669] |
| mom252_sector_neutral / t3000 | 0.443536 [-0.233295, 1.133278] | 0.447026 [-0.228623, 1.223170] |

CP21 comparisons cover 192,360 scorecard metric cells (82,130 changed), 580 capacity
metric cells (471 changed), 1,817,608 IC summary leaves (1,072,570 changed), 351 baseline
summary leaves (85 changed), and 3,369 IC manifest leaves (645 changed), including
all predictions-confirmed counters. The attribution is joint D12 as-of membership,
E09 delayed returns, E18 minimum names and affected DSL/inference corrections. The
fixed Python scoring algorithm isolates those engine-output changes from scorer changes;
it does not separate the causal contribution of each engine fix.

## Reproduction and verification

- `g0_measure.py` records exact argv/env, source and binary hashes, exit, wall time and
  peak working set. Its immutable frozen binaries and DLLs are in `bin/unpinned`,
  with `bin/unpinned-hashes.json`.
- Release configure and explicit target build exited zero. Logs are `logs/configure.log`
  and `logs/build-release.log`; targets were atx-impl, atx-impl-tests, atx-shm-worker,
  atx-engine-bench. Only selected opt-in real-data test/benchmark is run for evidence.
- `g0_compare.py` publishes full metric tables rather than selecting favorable rows.
- Full comparison generation passed: 54 tables, 2,027,028 metric/metadata cells,
  1,165,558 changed. `comparisons/index.json` lists counts; no favorable rows are omitted.
- Final verification checks all 35 measurement receipts, recursive engine manifest
  bindings, frozen scorecard input/output/script/design hashes, all 13 pinned recipes,
  disclosure parity and Abort identity. Passed: 33 recursively discovered manifests
  (including the copied audit), 597 bound evidence files; final
  `g0-artifact-manifest.json` published last. The three expected failed Abort attempts
  preserve their `.pending` markers and failure records; no successful output is pending.

The cp21 archive uses the corrected source above plus the exact diagnostic patch
`bin/cp21-pin.patch`: 26 frozen families, 22 retained families, numeric checkpoint 21.
Production source files were restored immediately after archiving. Every cell's
signal definitions and cost recipe are checked against its old cp21 manifest; each
has 29 streams (three baseline streams plus 26 families) and declared cell N=80.
The frozen scorecard retains the historical overall N=570. No rerun was retuned.

The D12 mask changes 2013 readiness while preserving raw feature warmup:

| Cell / readiness metric | Old | New |
|---|---:|---:|
| 2013 t1000 admitted cells | 144,225 | 141,902 |
| 2013 t1000 eligible cells | 227,303 | 188,298 |
| 2013 t1000 minimum admitted names | 638 | 633 |
| 2013 t3000 admitted cells | 145,552 | 145,497 |
| 2013 t3000 eligible cells | 647,286 | 562,254 |
| 2013 t3000 minimum admitted names | 639 | 639 |

The new mask rejects 39,005 / 85,032 nonmember cells at t1000 / t3000. Observations
remain 189; ready evaluation cells remain 223,577 / 626,607. Baseline/IC summary
sidecars cover every cell, not just these examples.

The combined cp21 corrections materially change sparsity. E-18 raises the minimum
names from 2 to 50; as-of masking and delayed returns also affect available samples.
The runbook's expectation of essentially zero impact is not supported by the actual
manifest counters. `dates_below_min_names` for the named momentum_252 / audited / full
block sums its five horizons, so these are not counts of distinct calendar dates:

| Cell | Old below-minimum count | New |
|---|---:|---:|
| 2013-2016, both available cuts | 0 | 0 |
| 2017 t1000 | 8 | 73 |
| 2018 t1000 | 80 | 303 |
| 2018 t3000 | 67 | 259 |
| 2019 t1000 | 0 | 25 |
| 2019 t3000 | 0 | 25 |

Maximum counts across individual blocks are also preserved in every manifest delta
table (for example 2013: 0 to 187; 2017 t1000: 18 to 249). These simultaneous changes
are not separately isolated; no favorable sample or threshold was substituted.

## Scope and limitations

- D0 construction fixes cannot be measured by rerunning the same prebuilt contexts.
- Frozen L10 does not use the CLI's new baseline/IC membership mask.
- cp21 will retain its 26 historical families and N=570. The existing frozen Python
  scorecard computation is preserved; its old year-union metadata needs an explicit
  erratum because corrected engine eligibility is as-of per feature date.
- The diagnostic pin retains the production `iteration22-cross-section-ic-` trial-ID
  prefix and its trial-count prose. Numeric checkpoint 21, declared N=80, exact
  historical families and fresh dedicated G0 ledger disambiguate this metadata;
  it is not a new cp22 search or an increase to the frozen scorecard N=570.
- L9 holdout is off. cp21 retains its two 2019 cells under owner ruling R1: development
  data. Data at or after 2020 remains sealed.
- The identified replay default Abort pin was measured and corrected. Missing-close
  fallback completion is an adverse stress assumption, not observed terminal evidence.

## Pending integration

Root owns I-24 status errata and ledger updates. The corrected old guarded L9 family
blend validation Sharpe is -1.218738142997244 (p=0.9503335211679915), not +0.6; prior
holdout status was reused with three prior reads. No historical log is silently rewritten.
