# W1 modeled costs, universe, date CPCV and sparse risk qualification

Date: 2026-09-26. Root pool-2, feat/aes-codex-integration-20260925.

## Result and source

262 distinct C++ checks passed, with zero unresolved failures or skips. D5's
synthetic Python QA checks separately passed 7/7 in 0.268s. This is bounded
implementation qualification, not completion of W1 or its scale/data gates.

Clean production and compiled provenance identity:
`6debcc1086c045ede42a1cdf1d7bc4444a3e2501`. Subsequent commits `c42eb524` and
`402af294` only repair fixtures; they do not alter production algorithms. The
original DAG indexes all B1, D5, R1 and E6 source/import/review SHAs.

| Target / artifact prefix | Result | Native runtime seconds |
|---|---:|---:|
| Cost / w1-models-cost-qualified | 46/46 | 0.846 |
| Data / w1-models-data-qualified | 60/60 | 0.083 |
| CPCV / w1-models-cpcv-qualified | 13/13 | 0.041 |
| Risk / w1-models-risk-qualified | 74/74 | 14.802 |
| Application contracts / w1-models-impl-qualified | initially 68/69 | 9.264 |
| Repaired cutoff fixture / w1-models-cutoff-qualified | 1/1 | 0.001 |

All artifacts are under `C:/atx-wt/pool-2/build-equity/`. Each prefix has log,
XML and receipt JSON with native exit, source, exact filter, executable SHA256
and wall time. The CPCV filter is `EvalCpcv*:EvalCpcvDate.*`; the other first
five runs use their entire focused target. Replacing the one repaired result
in the classname/name union yields 262 passes. This is not a whole-engine or
whole-application suite claim. Prior 283-check evidence at `16c96293` remains
separate; these counts must not be added as though all cases were new.

Python evidence: `w1-d5-integrated-qa-tests.log`, native exit 0. All generated
inputs are synthetic, including invalid future-date refusal cases. No actual
market payload, mixed-era dataset, warehouse, long benchmark or mining rerun
was accessed or launched.

## Repairs and compilation

Six small production objects compiled at `b7a90377` in 9.969s using one worker
before the owner's explicit multiworker instruction. Every subsequent build
used `-Jobs 2` through the owned wrapper, with existing dependencies/PCH.

| Batch prefix | Source | Exit | Wall seconds | Outcome |
|---|---|---:|---:|---|
| w1-models-build | f2f85ac7 | 1 | 35.123 | Production JSON/string comparison error |
| w1-models-resume-build | 6debcc10 | 1 | 209.852 | Production finished; two fixture assertion errors |
| w1-models-fixture-build | c42eb524 | 0 | 48.009 | Five focused executables linked |
| w1-models-cutoff-build | 402af294 | 0 | 17.575 | One fixture object and link |

`6debcc10` explicitly decodes the validated JSON policy string before comparing
it. `c42eb524` separates two GTest fatal assertions that shared a source-line
label and explicitly decodes another JSON string in an assertion. Completed
objects were retained after both stops. No PCH, dependency or worker rebuild
was introduced. The longest invocation was about 3.5 minutes. These are
shared-host timings, not controlled speedup measurements.

The application cutoff check initially omitted the newly required V2
instrument-type path, so validation correctly stopped at the earlier missing
input contract. `402af294` supplies a nonexistent dummy path, reaching the
intended date guard before any path opens. The corrected check passed. The
other 68 application cases retain their initial evidence; production is
unchanged. The initial application executable was replaced by the corrected
one, so its historical hash is receipt evidence, not a currently rehashable file.

Memory was sampled before launches and during the two-worker build. The first
launch had 1344 MiB available and 2.201 GiB commit headroom; the main resumed
launch had 1920 MiB and 2.483 GiB. An in-flight sample observed two clang workers
at about 484/532 MiB, 1483 MiB available and 1.552 GiB commit headroom. These are
samples, not continuous peaks. No other user's process was stopped. Root owns
compilation; agents continue source work without launching competing builds.

## Behavior qualified and limits

- B1: CS/AR/EDGE modeled spreads and complete-window missing states, explicit
  spread scale, differential FIM size/idio adjustments, borrow tier prior with
  strict clocks, annual-fraction/bps conversion and model identity. The cost
  adapters and calibration owning cases also pass. The absolute 13.7/32 bp FIM
  example remains unqualified; no unsupported intercept was inferred. Actual
  consumer migration and empirical calibration are later work.
- D5: QA-v2 provenance/count consistency, strict dated common-stock types,
  inclusive price/liquidity floors, unknown-clock behavior and versioned
  membership/exclusion artifacts. Actual type acquisition, the 19-session
  repair, historical bytes, coverage/churn and 2018 rebuild remain open.
- E6: date-unit embargo, equal-date grouping, merged-window purge, finite
  budgets, explicit combinatorial paths and legacy fold parity. The new
  factory/learn/discovery caller migration is outside these binaries and
  remains in source review.
- R1: CSR/implicit box ordering and dense parity, hard liquidity geometry,
  relative economic feasibility, malformed metadata/budget refusals,
  discretization, actual book rule/config/recipe and allocation checks. Dense
  factor-space work remains explicitly bounded. Actual ADV snapshot wiring,
  M=5000 solve and materialization performance/RSS gates remain open.

No alpha was promoted. The IC screen's prior correctness evidence remains;
practical noisy-null pruning, broad recall and measured search throughput are
still unqualified. The owner-deferred W0 81-case comparison remains deferred,
not passed. Next source batch: D6 panel store, E6 CPCV callers and R2 estimators.

## Artifact bindings

| Artifact | Executable SHA256 | XML SHA256 |
|---|---|---|
| cost | 4eb3a6fbf5aac0bade731d5a353eeccf4a7ea7b9cae242741c2310cd7c29a92d | c187edda42b58853926b6e3c6b75514af59b66b3df41c3a18a22500f90af0410 |
| data | c5290b2cef4d11dd498539c3b0cbe174f2849e8fc482309cea322777cf15d7ef | 9f068a6f92fbae9bfcd35812ca5262e01b86b479bfc9db0a94fafe54fa5da980 |
| CPCV | f8ea40cc3a6ed1824e61b39fdd847afb4b8e3fd60b0196be5b1cb53c0f23eba5 | e86e4a48583fd8e76a84771bc927fc726ab037be2480f01e1d7336451965b472 |
| risk | 877bda9479b0ef6cfea5a882889fdd516c9356104da9573b11a0b3c554724462 | 7d4dd107a647c4cf58b22db3f0299a32f09e478c5c260d6a04853a9ae9a7a85a |
| application initial | c63021c92772f1f6be1d7a9251f32d6f807c244b91842d4e70d74053de08ad15 | 9e56699238fec0631c37aae942eaf15e4b1568af3ed88adb4d0c541320067fa6 |
| application repaired | 134e3e28e254d75c57e8a57cede61108394f5bf78d18a6a6a5e4cccae9519636 | d428af5a15831cbd102496426e22cdafaa9b04a93c8ff67c616b90cd0a3083aa |
