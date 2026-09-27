# Lane W0-R0: Risk estimator blockers

Host tag: **RAM:light** · Batch: **W0b** · Pool: **`C:\atx-wt\pool-8`** ·
Branch: **`feat/w0-r0`** (run id `aes-w0-r0`) · Base: the W0 base commit (head of
`feat/w0-integration` at lease time; see `progress.md`).

Read first: `.superpowers/sdd/w0/RULES.md` (binding), then this brief, then the plan and findings
docs under `docs/plans/` (all inside your pool).

## Goal (plan §7, verbatim)

#### W0-R0 · Risk estimator blockers
- **Closes:** R-03, R-04, R-05, R-06.
- **Owns:** `risk/factor_model.{hpp,cpp}`, `risk/exposures.hpp`.
- **Build:**
  - Lag exposures: X at s−1 explains r_s. Vol, beta and resid-vol windows end at s−1.
  - Sector columns are mapped by group id per date.
  - Floor D with a structural fallback, never below 0.1·median(D), for names with fewer than min_obs observations.
  - Cap-weighted mean and ±3 winsorizing in the z-scores.
  - PIT cap and group per date.
- **Suites:** `RiskFactorModelPit_*`, `RiskSectorColumnsById_*`, `RiskThinNameFloor_*`.
- **Accept:**
  - Future-perturbation invariance of the model built at t.
  - A group missing at s > 0 builds with no out-of-bounds access (run the test under the UBSan/ASan config).
  - No D < 0.1·median.
- **Deps:** none. **Load:** light.

## Cited findings rows (verbatim; every ID must end CLOSED or explicitly DEFERRED in your report)

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| R-03 | H | `factor_model.cpp:579-580,612-613`; `exposures.hpp:281-297,336-365` | Contemporaneous exposures: r_s is regressed on X built at s, whose windows include r_s. | W0-R0 |
| R-04 | H | `factor_model.cpp:627-630` | Sector columns misalign across dates, including an out-of-bounds read of `fit->beta[c]` (UB). Latent today. | W0-R0 |
| R-05 | H | `factor_model.cpp:263-266,688-695,94` | Names with fewer than 2 residual observations get D=1e-12, so the optimizer piles into them. | W0-R0 |
| R-06 | H | `exposures.hpp:477-506`; `factor_model.cpp:579` | Equal-weight z-scores with no winsorizing. The current cap/group is applied to every historical date. | W0-R0 |

## Scope

- Files in scope (the ONLY files you may modify; new test files per RULES §2 are always allowed):
  - `atx-engine/include/atx/engine/risk/factor_model.hpp`, `atx-engine/src/risk/factor_model.cpp`
  - `atx-engine/include/atx/engine/risk/exposures.hpp`
- Files forbidden: everything else — in particular files owned by other W0 lanes (see
  `progress.md` ownership table). Needs elsewhere → report "Integration notes".

## Gate closure

- Test groups for `build-equity`: `risk` (reconfigure only if the tree differs).
- Owning targets: `atx-engine-risk-tests`.
- Suites: `RiskFactorModelPit_*`, `RiskSectorColumnsById_*`, `RiskThinNameFloor_*`; must stay green: whole risk target (Nightly skipped).
- Anchored runs: `-Ctest -Preset equity-dev -R '^<Suite>'` per suite; whole owning executable once
  before review (`--gtest_brief=1`).

## Done criteria

Every plan **Accept** item above MET with a named test and pasted evidence; every cited ID CLOSED
or DEFERRED with reason; owning targets green; tree clean; report committed at
`.superpowers/sdd/w0/lane-r0-report.md`.

## Lane notes (orchestrator)

- Start by merging `feat/w0-integration` (it contains O1/lane 6).
- No sanitizer build exists in this repo (`.agents/cpp/agent.md` §8), so 'run under UBSan/ASan' cannot be done literally. Prove no OOB with the Debug build's checked STL (`_ITERATOR_DEBUG_LEVEL=2`, `/RTC1`) plus explicit index assertions in the test, and mark the item 'MET-substitute (no sanitizer preset)' for the owner to accept or waive.
- Existing risk tests that encode contemporaneous exposures (R-03) may change expectation — list each with its defect ID.

## Out of scope

Anything the plan assigns to W1+ lanes; any real-data run; any CMake/preset edit (except O1);
refactors not required by a cited defect.
