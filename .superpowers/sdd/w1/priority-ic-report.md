# Priority multi-horizon IC screen: implementation and bounded qualification

2026-09-26. This is the owner-requested subset of W2-A4 pulled forward while the
long W0 benchmark is deferred. It is not completion of W2-A4 or a tradeable-alpha
claim. No actual market data was loaded by these checks.

## Implemented behavior

Immutable, byte-budgeted forward labels and ranks at 5/21/63/126 sessions feed
real xsimd cross-sectional correlation reductions and exact tied signal ranks.
The VM signal is reused before full fitness, transaction costs and CPCV. With
multi-fidelity enabled, an IC-only VM pass precedes the first backtest rung;
survivors can evaluate the VM again without retaining population-sized panels.
Decision membership, return guards, delay and exclusive label maturity are
explicit. Missing/sparse/short/uncertain evidence passes through.

Rejected canonical identities persist through search checkpoints and all five
Factory admission paths. Mine registers metadata-only observations in explicit
TrialRegistry V3, never fake P&L. Raw trial N includes rejections; partial-P&L
correlation accounting is unavailable. DSR uses total raw N with a benchmark
equivalent to max(observed full-P&L variance, candidate marginal variance).
Legacy V1/V2 registry byte paths and explicit disabled screening remain available.
Configuration, reports, counters and resume identities bind the active recipe.
Injected-cache resume is explicitly refused until support identity is persisted.

Source imports: kernel45682ec9/0c9820d7/ee24fa6d/d7729a35; searcha9606e6d/
8ad38bef/9f25cd8f/d65eec3b; registry561ed7bf/10dfdc80; mined8f0968e/
4924341a/e504a206/7702ca1c; discoverd060cd81/f9a6be88/71ee76ea/fbeb7064.
Final focused source is abe8fec6, following test correction1840317b.

## Completed bounded checks

- Engine focused target: 59/59 passed in11.994s. Covers kernel, search/fidelity,
  checkpoint compatibility, Factory rejection propagation and V1/V2/V3 registry.
- Impl focused target: 42/42 passed in37.707s. Covers config, mine core/pipeline/
  CLI and discovery persistence. Includes zero-P&L screen accounting, cutoff
  perturbations, inherited search rejects, cache/guard fail-open and DSR floor.
- Additional wider cohort: 1/1 passed in21.148s at abe8fec6.
- After refreshing source provenance, changed discovery consumer: 8/8 passed
  in7.060s. Earlier unaffected passing suites were reused.

These are102 distinct focused checks, zero failures or skips. They are not full
owning subsystem suites. Direct gtest targets preserve normal CTest registration.
Receipts/XML/logs are under pool-2/build-equity/ic-screen-*.

The first build compiled all changed production code and three focused engine
test TUs, then found a missing argument in a new registry test (402.686s).
After fixing that call, the incremental completion took78.394s. A later cohort
addition and source-provenance refresh took43.617s including warm configure;
only the cohort test TU and stage_discover provenance TU recompiled. Jobs1,
existing PCH/compiler cache, no dependency/worker rebuild. These observations
are not a controlled compiler or screen throughput benchmark.

## Important unresolved screen qualification

At the provisional practical IC floor0.02 and confidence multiplier3.5:

| Synthetic diagnostic | Nulls rejected | Planted signals retained |
|---|---:|---:|
| 512 dates ×96 names, distinct horizons | 0/6 | 18/18 |
| 512 dates ×512 names | 0/12 | 12/12 |

The first cohort includes weak0.002/0.005, inverse, regime-specific and stronger
controls, with rank monotonicity and short-window fail-open checks. The second
adds wider noisy null/weak/inverse cases. The seeds were fixed before execution;
counts were reported without enforcing a desired quota. Cases share synthetic
prices, and some share signal seeds: these are not independent population recall
estimates. Exact constructed nulls are rejected and bypass backtests, but the
noisy cohorts establish no practical rejection benefit. A statistical review
of the per-horizon one-SE retention guard is open. Do not call the screen's
rejection efficiency, general retention or production speed qualified yet.

No long benchmark, full mining rerun, large RSS experiment, warehouse change,
actual 2020+ data read, main fast-forward or push occurred.
