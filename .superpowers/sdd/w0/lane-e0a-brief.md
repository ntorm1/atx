# Lane W0-E0A: Inference: HAC, block length, execution delay, caps

Host tag: **RAM:light** · Batch: **W0a** · Pool: **`C:\atx-wt\pool-5`** ·
Branch: **`feat/w0-e0a`** (run id `aes-w0-e0a`) · Base: the W0 base commit (head of
`feat/w0-integration` at lease time; see `progress.md`).

Read first: `.superpowers/sdd/w0/RULES.md` (binding), then this brief, then the plan and findings
docs under `docs/plans/` (all inside your pool).

## Goal (plan §7, verbatim)

#### W0-E0a · Inference: HAC, block length, execution delay, caps
- **Closes:** E-02, E-03, E-08, E-09, E-15.
- **Owns:** new `eval/hac.hpp`, `eval/cross_section_ic.{hpp,cpp}`, `combine/signal_combiner.cpp` (t-stat sites only), `combine/orthogonalize.cpp` (the `marginal_ic` t-stat only), `combine/signal_store.hpp` (the winsor fix only).
- **Build:**
  - Newey-West and Hansen-Hodrick t-statistics, and Politis-White automatic block length.
  - Block length ≥ 2h, or a stationary bootstrap (`BlockLenRule::V1` keeps old streams).
  - `execution_delay` parameter, default 1. Forward returns start at t+delay, and the embargo becomes h+delay.
  - The 4096 caps become runtime sizing with a preflight (supports ≥ 16384), or per-date streaming.
  - HAC t-stats in ICIR-EWMA, `column_tstats` and `marginal_ic`.
  - Winsorize after standardizing. Pearson IC uses winsorized returns.
- **Suites:** `EvalHac_*`, `EvalIcCoverage_*`, `EvalIcDelay_*`, `EvalIcCaps_*`.
- **Accept:**
  - NW t matches a statsmodels fixture to 1e-8.
  - A simulated MA(20) IC series gives 95% CI coverage in [93%, 97%] over 2000 repetitions.
  - A same-day reversal signal's IC collapses at delay 1.
  - A panel of 6,624 ids × 1,750 dates is accepted.
  - Frozen streams reproduce under V1.
- **Deps:** none. **Load:** light.

## Cited findings rows (verbatim; every ID must end CLOSED or explicitly DEFERRED in your report)

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| E-02 | H | `eval/cross_section_ic.hpp:480-483` | Block length max(5, ⌈h/2⌉) on MA(h−1) overlapping returns makes CIs about 35% too narrow at h=21/63. | W0-E0a |
| E-03 | H | `signal_combiner.cpp:223-233,47-63`; orthogonalize `marginal_ic` | IID t-stats on overlapping IC series are inflated by about √h, so the haircut barely bites. | W0-E0a |
| E-08 | H | `cross_section_ic.hpp:95-96` | `kMaxIcDates/Instruments = 4096`, but the 2013–19 t3000 union is 6,624. | W0-E0a |
| E-09 | H | `cross_section_ic.cpp:330-352`; `stage_equity_ic.cpp:112-113` | The forward return starts at the signal close t while books trade at t+1. There is no knob. | W0-E0a |
| E-15 | L | `signal_store.hpp:82-96` | Restandardizing after winsorizing breaks the limit. Pearson IC uses unwinsorized returns. | W0-E0a |

## Scope

- Files in scope (the ONLY files you may modify; new test files per RULES §2 are always allowed):
  - new `atx-engine/include/atx/engine/eval/hac.hpp` (header-only)
  - `atx-engine/include/atx/engine/eval/cross_section_ic.hpp`, `atx-engine/src/eval/cross_section_ic.cpp`
  - `atx-engine/src/combine/signal_combiner.cpp` — t-stat sites only
  - `atx-engine/src/combine/orthogonalize.cpp` — the `marginal_ic` t-stat only
  - `atx-engine/include/atx/engine/combine/signal_store.hpp` — the winsor fix only (E-15)
- Files forbidden: everything else — in particular files owned by other W0 lanes (see
  `progress.md` ownership table). Needs elsewhere → report "Integration notes".

## Gate closure

- Test groups for `build-equity`: `eval;combine` (reconfigure only if the tree differs).
- Owning targets: `atx-engine-eval-tests`, `atx-engine-combine-tests`.
- Suites: `EvalHac_*`, `EvalIcCoverage_*`, `EvalIcDelay_*`, `EvalIcCaps_*` (+ `CombineHacTstat_*` for the combine sites); must stay green: whole eval + combine targets.
- Anchored runs: `-Ctest -Preset equity-dev -R '^<Suite>'` per suite; whole owning executable once
  before review (`--gtest_brief=1`).

## Done criteria

Every plan **Accept** item above MET with a named test and pasted evidence; every cited ID CLOSED
or DEFERRED with reason; owning targets green; tree clean; report committed at
`.superpowers/sdd/w0/lane-e0a-report.md`.

## Lane notes (orchestrator)

- NW t must match a statsmodels fixture to 1e-8: check `python -c "import statsmodels"`; if present, generate the fixture with a committed script under `atx-engine/tests/eval/fixtures/` (no network installs). If absent, say so and use an independent reference implementation of the identical statsmodels formula (`cov_type='HAC'`, Bartlett kernel, `use_correction` as documented) — mark the item PARTIAL and flag it.
- E-09 consumer wiring at `atx-impl/src/stage_equity_ic.cpp:112-113` belongs to I0b (W0b): expose `execution_delay` (default 1) and write the integration note.
- Old streams must reproduce under `BlockLenRule::V1` (prove with a test).

## Out of scope

Anything the plan assigns to W1+ lanes; any real-data run; any CMake/preset edit (except O1);
refactors not required by a cited defect.
