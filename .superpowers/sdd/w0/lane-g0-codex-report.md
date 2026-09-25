# G0 truth-delta report (in progress)

Outcome: IN PROGRESS; no G0 completion claim yet.
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
| 2013 native baseline | Abort, period 6, security 150340 on 2013-04-12 | Same exact error, exit 1 | B-04 engine change was not wired into identified report; corrected replay follow-up required |
| L7 PIT risk scorecard | Five configurations, old archived JSON | All headline metrics unchanged; three scalar differences at 1e-9 or 1e-10 | No material delta |
| L10 fundamental zoo | Archived L10 v2 | Running | Pending |
| L9 guarded mining | Archived guarded run | Pending | Pending |
| cp21 scorecard | 13 cells, 26 families | Awaiting reviewed DSL-internal D12 repair before pin/rebuild | Pending |

Frozen native baseline receipt: `logs/base2013.receipt.json`, exit 1, 2.031 seconds,
peak working set 0.146 GiB. Exact stderr:

```
replay: missing/nonpositive required close at period=6 instrument=604 session_key_ns=1365724800000000000 security_id=150340
```

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

## Reproduction and verification

- `g0_measure.py` records exact argv/env, source and binary hashes, exit, wall time and
  peak working set. Its immutable frozen binaries and DLLs are in `bin/unpinned`,
  with `bin/unpinned-hashes.json`.
- Release configure and explicit target build exited zero. Logs are `logs/configure.log`
  and `logs/build-release.log`; targets were atx-impl, atx-impl-tests, atx-shm-worker,
  atx-engine-bench. Only selected opt-in real-data test/benchmark is run for evidence.
- `g0_compare.py` publishes full metric tables rather than selecting favorable rows.
- Final manifest validation/publish remains pending until all artifacts exist.

## Scope and limitations

- D0 construction fixes cannot be measured by rerunning the same prebuilt contexts.
- Frozen L10 does not use the CLI's new baseline/IC membership mask.
- cp21 will retain its 26 historical families and N=570. The existing frozen Python
  scorecard computation is preserved; its old year-union metadata needs an explicit
  erratum because corrected engine eligibility is as-of per feature date.
- L9 holdout is off. cp21 retains its two 2019 cells under owner ruling R1: development
  data. Data at or after 2020 remains sealed.
- The identified replay default Abort pin, discovered before running, is measured as
  a real integration gap. The separate replay lane is responsible for its correction.

## Pending integration

Root owns I-24 status errata and ledger updates. The corrected old guarded L9 family
blend validation Sharpe is -1.218738142997244 (p=0.9503335211679915), not +0.6; prior
holdout status was reused with three prior reads. No historical log is silently rewritten.
