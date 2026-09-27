# Mega-alpha parent handoff 2 — stopped at the owner's request

Prepared 2026-09-26 (evening, America/New_York) by the resumed parent. This supersedes
`2026-09-26-mega-alpha-parent-handoff.md` as the current checkpoint. That file is still the reference
for older history, pins and the v1-v4 runs. Rolling ledger:
`.superpowers/sdd/mega-alpha-20260926/progress.md` (top section = this session). Task briefs, reports,
reviews and studies are in the same directory.

## 1. Stop state

The owner wrote: "stop here and write a detailed handoff markdown file for the next parent agent + a
goal prompt". All child agents were stopped with TaskStop:

- T2 fix round
- T5 fix round
- T7
- T9
- the T1 review

No build or numerical process owned by this session is running. Every worktree listed below is
tracked-clean. Do not resume from this document alone; the owner's next prompt authorizes resuming.

## 2. Objective and standing rules (unchanged)

Build a combined portfolio of many atx-engine alpha-DSL subalphas. Targets:

- annualized NET Sharpe >= 1 after costs (252 sessions/yr);
- one-way turnover <= 30%/calendar month;
- thousands of stocks (~3000/day liquidity cohort);
- $1bn NAV;
- 2020+ data.

These are targets to measure honestly. **None is met yet.**

Priorities: core runtime, alpha generation, composition, working portfolio construction. Exhaustive
corporate-action registration and detailed realism stay deferred.

Rules:

- Subagent-driven development on Opus 5.5 child agents; **not TDD** (write postimplementation fixtures).
- Root alone builds and runs real data.
- Every real run <= 180 s under `scripts/run_bounded_research.py`.
- No pushes, warehouse writes or broker actions. Never mutate `C:/atx` (read-only inspection is okay).
- Keep 2025+ reserved.
- Select signs, weights and policies on TRAIN (2020-2022) only. Validation (2023-2024) is used once,
  per frozen configuration.
- Owner mid-session instruction: **do not let RAM limits slow progress; if blocked, improve
  efficiency / make progress incremental** instead of waiting.

## 3. Worktrees and Git state at stop

Every worktree listed here is tracked-clean. Root builds only in pool-2.

| Worktree | Branch @ HEAD | Content / status |
| --- | --- | --- |
| pool-2 (root, lease `aes-codex-integ-20260925`) | `feat/aes-codex-integration-20260925` @ `c615ce36` (+ this handoff commit) | Integration. All imports below. |
| pool-3 | `feat/mega-alpha-weights-20260926` @ `aad9779a` | T9 was stopped with NO commits. Redo from brief. (Its lease file still names the old run; T3 was done here earlier.) |
| pool-4 | `feat/mega-alpha-runner-fields-20260926` @ `6d85ac2a` | T7 was stopped with NO commits. Redo from brief. |
| pool-5 | `feat/mega-alpha-nav-20260926` @ `8e25992d` | T2 source, imported. The fix round was stopped with no commit. |
| pool-7 (leased `mega-alpha-lib-20260926`) | `feat/mega-alpha-library-v2-20260926` @ `f2d5fb97` | **T5 fix round 1 committed, NOT imported, NOT reviewed.** |
| pool-8 (leased `mega-alpha-fields-20260926`) | `feat/mega-alpha-fields-20260926` @ `cf36d83c` | T6 imported. |
| pool-6 | baseline | leave alone |

Old branches (`feat/w0-*`) in pools 3/4/5 are intact. The new task branches were cut from root HEADs.
Pools 7 and 8 have a failed default `dev` configure: the atx-vol install target is missing. This does
not matter; they are source-only lanes.

## 4. Completed this session (source -> root import, evidence)

| Element | SHAs | Evidence |
| --- | --- | --- |
| VM arena release + fixture | `8527a839`->`050c0efc`, `23f1541b`->`bfb6b859` | Build `recent-strategy-targets-v1` 36.62 s Jobs2 (6 TUs/5 links). IC 35/35 in 5.141 s; target replay 7/7 in 0.198 s. |
| Pool-3 v3/v4 archive | `58c21bb0`->`470eb1b6` | 46/46 files verified. |
| TRAIN saved-blend export v6 | exe `03607890...` @ `bfb6b859` | v5 stopped on host RAM (preserved). v6 completed in 94.70 s at 899 MB. Orientations (canonical `4a3e8004`), planned targets and daily IC byte-identical to v2. Blend manifest `51740eff...`. |
| Fixed policy comparison + frozen validation | report `.superpowers/sdd/strategy/2026-09-26-saved-blend-policy-qualification.md`, archive `saved-blend-policy-20260926/`, commit `5c9cbaed` | Audit `build-equity/audit-target-replay.py` (SHA `d8e5fb27`) PASS: 2,268 exact baseline f64 values. Budget-v2 mean monthly target change: TRAIN 35.32%->30.66%; frozen VAL 34.44%->30.24% (forced-exit breaches disclosed). Gross 0.90->0.85. |
| T3 price-risk exposures + neutralize | `e4a869b7`->`b9cf4023`, CMake `429cbe43` | Build 21.9 s; target tests 15/15. Review APPROVE (0C/0I/8 minor, `task-T3-review.md`). **Task complete.** |
| T1 candidate cache + pinned weights | `09a18ec3`/`a7fd1c02` -> `af8c38ee`/`445e828d` | Build 32.3 s; IC tests 42/42 in 9.68 s. The real run `mega-v1-train-cache-b` completed in 71.16 s: 37 cache hits + 11 cold, outputs byte-identical to v6. **Task review was started and then stopped: redo it.** |
| SHA-256 Debug optimization | root `6d85ac2a` (`atx-core/CMakeLists.txt`, scoped `/O2` on `sha256.cpp`) | Cache write 2.5 s -> 0.24 s per 52 MB; role load 9.4 s -> 1.4 s; save 4.35 s -> 0.33 s. |
| T5 library v2 (original) | `2c92d658`->`c47ffdaa` | 96 candidates, SHA `0c7f3059...`; `--plan-only` OK (8 slots, lookback 314). **Review found 4 Important issues. Do NOT measure `c47ffdaa`'s library.** |
| T2 NAV replay ($1bn, S1/S2/S3, stale-carry K=5) | `8e25992d`->`c4ba9c80`, CMake `97e6b392` | Build 31.6 s; target tests 24/25. **One fixture fails:** `StrategyNavReplay.ConstantPricesNoCostReproducesTargetReplayPlanned` at test :309, expects `forced_turnover > 0` and gets 0. The fix round was not done. |
| T6 PIT field producer | `cf36d83c`->`c615ce36` (`atx-engine/tools/prepare_research_fields.py`, SHA `892bd33f...`) | Synthetic 9/9. Real TRAIN run `build-equity/recent-fast-train-2020-2022-v1-fields-v1/` completed in 22.03 s, peak 604 MB, manifest SHA `519fc9b2064bb74aa8ca885c09a87f428393708e1e9ac71bb9ea67bfcbb6a875`. **The validation-role fields run has not been done.** |

TRAIN field member coverage:

| Field | Coverage |
| --- | --- |
| `si_shares`, `si_dtc` | 99.93% |
| `iv_atm_21d`, `iv_atm_63d`, `iv_atm_126d` | 92.9% |
| `earn_recent` | 99.97% |
| `shares_out` | 99.67% |
| `mktcap_lagged`, `size_grp` | 75.1% |
| `is_common` | 100% |
| `mkt_ret` | 99.88% |

Caveats:

- `is_common` is not PIT (2026 snapshot); use it only as a coarse filter.
- The spine has mild survival leakage.
- IV vendor vintage is unproven.
- FINRA data before 2021-06 is a later republication.
- `hv_*` was dropped: the vendor HV duplicates IV.

Ledger/doc commits: `ff1b499e`, `bb5bc25b`, `7871c3cc`, `e44a44d5`, `b23d463e`, `aad9779a`, plus this
handoff.

## 5. Key measured findings (the honest state)

All-days observed-component rough return of the fixed 48-alpha blend:

- gross annualized ratio about 0.36 on TRAIN (in-sample signs) and 0.34-0.38 on VAL;
- about 1.9%/yr at about 5% volatility on VAL.

Only a third of days are return-complete in the target proxy (up to 232 missing names/day), so that
proxy cannot give Sharpe. **This is why the NAV replay exists.**

The blend has negative market beta: -0.12 TRAIN, -0.20 VAL.

**TRAIN-only studies** (scripts in `.superpowers/sdd/mega-alpha-20260926/studies/`; diagnostic Python,
not the evaluator):

- Neutralizing against beta252/vol63/logADV63 raises gross from 0.36 to 0.55 at cadence 5 / fraction 0.25.
- Per-candidate neutralized daily factor returns, with signs and weights fit on 2020-21 and a 2022
  holdout, give these holdout gross Sharpe values:

  | Composition | Holdout gross SR |
  | --- | --- |
  | Equal | 0.72 |
  | Inverse-vol | 0.84 |
  | MV-shrink 0.9 | 1.23 |
  | MV-shrink 0.9, nonneg-clipped | 1.28 |

  Mean pairwise factor correlation is 0.18. Seven composition methods were compared on the 2022
  TRAIN holdout; count those as composition trials.
- Construction on the MV blend (2022 holdout, 6 bps + 300 bps):

  | Construction | Gross / net SR | Turnover/month |
  | --- | --- | --- |
  | Daily full | 1.19 / 0.56 | 169% |
  | Cadence 5, fraction .25 | 0.60 / 0.13 | 56% |
  | Cadence 5, fraction .25, no-trade band 1/N | 0.69 / 0.31 | 28% |

- **Conclusions:**
  - The alpha decays fast relative to the 30% monthly budget.
  - A no-trade band beats partial adjustment.
  - Flat 300 bps borrow costs about 0.3 Sharpe.
  - The binding constraints are **alpha quality/slowness plus construction**.
- Preregistered choices (ledger rulings):
  - composition `mv-shrink-0.9-nonneg-v1`, fit on full TRAIN;
  - construction: neutralize + no-trade band;
  - add an SI-tiered borrow scenario (engine `borrow_tiers`: GC 27.5 bps / warm 300 / special) next to
    the flat 300 bps scenario.

## 6. Pending tasks (briefs in `.superpowers/sdd/mega-alpha-20260926/`)

1. **T2 fix round** (pool-5): fix the single failing fixture described above. Decide whether it is a
   fixture problem (no forced exit in the synthetic data) or a code problem. Rebuild with
   `atx-equity-strategy-targets,atx-impl-strategy-target-tests`, then dispatch a T2 task review.
   Design: `nav-backtest-design.md`. Accepted design deviations are in `task-T2-report.md`:
   - the daily CSV has 756 rows with a `return_observation` flag;
   - participation p95 is a histogram upper bound;
   - ex-deployment turnover flags are added.
2. **First NAV runs**, after T2 passes: run `atx-equity-strategy-targets nav --combined ...
   --role ...` on the TRAIN v6 blend (`51740eff`) with baseline-v1 and monthly-budget-v2. Then run
   validation (v3 blend `7407d7e7`) with the frozen policy. Report S2 (primary) net Sharpe, HAC t,
   drawdown, calendar-year returns, execution-month turnover, write-off/stale histogram and capacity
   diagnostics. This gives the first honest NAV numbers.
3. **T4** (brief `task-T4-brief.md`, NOT dispatched): neutralize (`price-risk-v1`, compute exposures
   once per decision, skip rebalancing if amplification > 5 or excluded share > 0.5) plus
   `--band-multiple`, in the target and NAV replay. Intended for the T2 agent after its fix round.
4. **T5 fix round 1 review**: pool-7 `f2d5fb97` fixes I1-I4 (split-adjusted volume, `mkt_ret` field
   instead of `vec_avg`, residual-Sharpe momentum, non-duplicative illiquidity/ivol/MAX) and the
   minors. Unreviewed. The report's "Fix round 1" section may be incomplete (the agent was stopped
   while appending). Review it, then import. Its library needs T7 because it references `mkt_ret`.
5. **T6 validation fields**: run the producer for the validation role, with the same command shape as
   TRAIN (see `task-T6-report.md`). Then review T6.
6. **T7** (brief `task-T7-brief.md`; restart on pool-4 at `6d85ac2a` or the new root HEAD): runner
   `--train-fields/--validation-fields`; load only referenced fields; the cache key covers the fields
   manifest for field-referencing candidates.
7. **T9** (brief `task-T9-brief.md`; restart on pool-3): `atx-impl/tools/fit_composition_weights.py`
   implementing `mv-shrink-0.9-nonneg-v1` on full TRAIN from cached signals, producing pinned weights
   JSON.
8. **T1 task review**: redo it; the package is `review-T1.diff`.
9. **Library v3**: short-interest families (SI ratio, days-to-cover, delta SI), implied-vol families
   (level, term slope, IV change, IV vs realized from close), a size tilt, plus the fixed v2. Families
   are chosen from priors, frozen before measurement, with TRAIN signs and weights.
10. Then:
    - TRAIN run with the cache (the runner already chunks: a guard stop leaves cached signals, so rerun);
    - fit weights (T9);
    - saved blend with pinned weights;
    - NAV with neutralize + band on TRAIN, choosing among a preregistered handful of settings;
    - freeze;
    - one validation run.
    Fundamentals and industry via CIK (~65% coverage, 2020 export seal) are the next tier.

## 7. Tooling notes

- **Build:** `powershell -File C:/atx-wt/pool-2/build-equity/mega-build.ps1 -Tag <new> -Targets "a,b"`.
  The helper is ignored; it sets cwd to pool-2 (the wrapper's wrong-tree guard), admits by RAM, picks
  Jobs 2/3/4 and writes `build-equity/mega-<tag>-receipt.json`. Receipt tags already used:
  - `t1-a` (failed argument split)
  - `t1-b`
  - `t3-a`
  - `sha-o2`
  - `t2-a`

  Configured provenance updates only on reconfigure, so record the source SHA separately.
- **Guard Python:** `C:/Program Files/Python312/python.exe`. The bash `python` is the atx-db venv
  without psutil.
- **DLL PATH** for native runs: `C:/atx-cache/vcpkg_installed/x64-windows/debug/bin;.../bin`.
- **Ruling:** the real-run guard uses `--min-free-mib 512` (RSS 1536 MiB, 180 s unchanged), per the
  owner's RAM instruction. The earlier 768 MiB floor stopped v5 on host memory owned by other processes.
- **Candidate cache:** `build-equity/mega-candidate-cache/<role-sha>/` holds all 48 v1 TRAIN signals
  (~2.5 GB). Library v2/v3 reruns reuse the v1 entries through identical `dsl_sha256`.
- **IC run budget:** a warm 48-candidate TRAIN run takes about 71 s, of which IC is about 29 s and
  composition about 14 s. A 96-candidate run takes about 125 s warm. If composition or IC become the
  bottleneck, optimize those stages; do not wait on RAM.
- **Target replay audit:** `build-equity/audit-target-replay.py` (reviewed, SHA `d8e5fb27`).

## 8. Rulings made this session (each: decision — cost if wrong)

- Guard free floor 768 -> 512 MiB — possible host paging; no correctness impact.
- New task branches were cut from root HEADs in pools 3/4/5, keeping the old branches — none.
- NAV design accepted: stale-carry K=5, S2 $1bn sqrt-impact primary, flat 300 bps registered borrow,
  no cost grid — some cost realism is deferred.
- T2 deviations accepted (756 daily rows with flag, p95 upper bound, ex-deployment flags) — cosmetic.
- Role extra-fields path (Python producer + runner load) instead of a new price projection, and a
  `mkt_ret` field instead of changing the VM `vec_avg` opcode — v2/v3 depend on T7.
- T5 review findings fixed before any measurement — none; no data spent.
- Composition `mv-shrink-0.9-nonneg-v1` and construction neutralize + band chosen on a TRAIN-internal
  2022 holdout (7 composition methods compared) — mild TRAIN-holdout selection; validation untouched.
- An SI-tiered borrow scenario is to be added next to flat 300 bps — must be declared before viewing
  results.
- Scoped `/O2` on `atx-core/src/sha256.cpp` — one TU; Debug CRT/asserts unchanged.

## 9. Interpretation limits

- There is no NAV Sharpe yet: T2 is built but one fixture fails, and it has not run on real data.
- Target-proxy returns cover a non-random third of days.
- Python studies are diagnostics that approximate the C++ path.
- Liquidity cohort, not certified common stock; vendor vintage unverified; no $1bn capacity claim.
- The objective is NOT met.
