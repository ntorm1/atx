# Sprint I — Integration Status & Handoff (2026-07-16, evening)

State snapshot written at user request mid-Sprint-I. Read together with
`sprints/2026-07-16-atx-vol-sota-parallel-subsprints.md` (the plan; PLAN below),
`sprints/2026-07-14-atx-vol-fit-price-backtest-hotpath-sota-sprint.md` (SPRINT),
`docs/reviews/2026-07-16-hotpath-sprint-midpoint-code-review.md` (REVIEW).

## Where we are

### Sprint P (parallel sub-sprints) — COMPLETE

Three Opus subagents, three worktrees, all exit gates met, nothing pushed:

| Branch | Worktree | Commits | Outcome |
|---|---|---|---|
| `feat/sota-k-inversion` | `C:\atx-wt\wt-k-inversion` | 7 | K1 tol fix (47× round-trip err), K2 Cody-erfc Φ (~1e6× batch accuracy), K3 shelved-with-evidence, K4 R-22 fixed / R-23 half-blocked / R-24 routed scalar (AVX2 <1.2×), K5 shootout (~330 ns/op vs Jäckel 180, tighter accuracy), K6 shelved → Sprint X (Schadner arXiv:2604.24480) |
| `feat/sota-a-american` | `C:\atx-wt\wt-a-american` | 5+1 | A1 seed cheapened bit-identical but `kShipAvx2Boundary=false` (gate 1.46–2.21× under load), A2 batch entry + ⌈N/4⌉ proof (wiring deferred here — `slice_sigma_impl` lives in `boundary_interp.cpp`), A3 shootout (fast ~37 µs/op), A4 R-30 bind-key assert, A5 Healy domain map: current bails all correct, no double-boundary needed |
| `feat/sota-s-surface` | `C:\atx-wt\wt-s-surface` | 7 | S1 CStar arb-projection correctness (reversed c2 bisection fixed, Err propagation, analytic w″, zero false flags), S2 analytic Jacobian ~1.8× (below 2–4× target, honest), S3 projection 14.8×, S4 parallel+projected OPRA ingest (frame-identical), S5 synthetic panel → KEEP R&D (vol-RMSE 5.6× better on modal smiles, ~60–90× cost), S6 universe-cycle harness |

Ledgers appended to each worktree's PLAN copy (§4/§5/§6).

### Sprint I phase 1 — COMPLETE

- **Integration branch `feat/sota-integration`** in `C:\atx-wt\wt-i-integration`, based on `main@51df565`
  (includes Sprint R through the Illinois-step commit). Merge order R → K → A → S honored:
  `afb4277` (K), `cc80c97` (A), `c3a8ebe` (S). Conflicts only in `bench/CMakeLists.txt` /
  `tests/CMakeLists.txt`, resolved keep-all-targets.
- **Debug gate on the merge:** built `atx-vol-tests` (291 targets), ran the `atx_vol` suite:
  **1695/1702 pass; the 7 failures are exactly the 6 known pre-existing + 1 bench-exe-missing CTest.
  Zero new failures from the merges.**
- **Known pre-existing Debug failure baseline** (all on Sprint-R/v2 production TUs, all fail at base
  `main@7fca341` and at `51df565`): `SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`,
  `PricerFitterTest.LocalRiskRefitPublishesCopyOnWriteGeneration`,
  `PreparedPortfolio.GroupedPriceEqualsIndependentOracleAndPinnedFingerprint`,
  `SurfaceV2Qualification.RiskBuildRunsTheModeCarryAndInversionBudgets/{Latency,Balanced}`,
  `OpraBreadthCorpus.UnifiedPolicyFitsEveryAvailableBoard`. Plus environmental:
  `atx-vol-e2e-benchmark-name-coverage` / `atx-vol-american-shootout-name-coverage` fail unless their
  bench exes are built in the config under test.

### Sprint I step 6 (triage) — COMPLETE

`docs/reviews/2026-07-16-debug-failure-triage.md` (committed on the integration branch).
Headline: both triaged failures are direct-routed boards; **R-02's selector-gated fix repairs
neither**. Failure 1 = stale adversarial fixture (suspect `b118439`) or a real far-wing calendar
blind spot — one instrumentation run disambiguates. Failure 2 = vxx-close legitimately sparse
(9 clean strikes, 9/9 in-band) vs the generic `n_clean > 10` floor; a production fix would be an
all-routes served-breadth floor = superset of R-02 (coordinate with Sprint R).

### Sprint I phase 2 — IN FLIGHT (three background agents running right now)

| Agent | Worktree / branch | Mission (PLAN §7 seams) |
|---|---|---|
| I-A (boundary) | `C:\atx-wt\wt-i-boundary` → `feat/sota-i-boundary` | Step 2: wire AVX2 boundary batch into `SigmaBoundaryInterp::build` 9-node solve; route slice-σ cold fallback onto `american_price_batch_resolved`; vectorize BAW Newton seed (tolerance-parity, documented); re-run 2.0× ship gate, flip `kShipAvx2Boundary` only on decisive clear; SPY e2e before/after if flipped |
| I-K (kernels) | `C:\atx-wt\wt-i-kernels` → `feat/sota-i-kernels` | Step 3: R-23 identity aliasing (batch.cpp + the 5 batch_test.cpp assertions); wing-patch removal post-erfc (keep R-22 NaN escape) + tolerance-based `PatchedLanesAreBitExact`; R-24 re-measure → flip both IV entries to AVX2 iff ≥1.2× decisive; Chebyshev-Φ consumer note (no deletion) |
| I-S (panel) | `C:\atx-wt\wt-i-panel` → `feat/sota-i-panel` | Step 5: real-OPRA CStar vs eSSVI panel on SPY + 25-name cohort; per-expiry extraction seam built in owned TUs; final include/keep/kill recommendation appended to the evidence doc; still no `curve_selector.cpp` wiring |

If these need to be stopped early, the dispatching session can kill the tasks; otherwise their
reports arrive on completion and their branches carry the work.

## What remains (in order)

1. **Collect phase-2 agents** (reports + branch verification, same as Sprint P).
2. **Merge phase-2 branches into `feat/sota-integration`** (expected conflicts: CMakeLists trivial;
   `american_boundary_avx2.cpp` note — I-K must not have touched it; verify).
3. **Release gate + bench exes:** configure `rel-avx2` in `wt-i-integration`
   (`FETCHCONTENT_BASE_DIR` per-worktree override mandatory), build tests + ALL bench targets
   (also fixes the two name-coverage CTests), run the suite in Release.
4. **Step 4 — Φ-swap validation on the full accuracy panel + quiet-host benches** (everything else
   idle → this is the quiet window): W0.3 accuracy panel aggregates vs prior baseline (in-band ≥
   prior, χ² ≤ prior, vol-RMSE ≤ prior); `PreparedPortfolio` pinned-fingerprint golden needs a
   documented update if the only delta is the K2 Φ change (bit-identity is a telltale, not a gate —
   PLAN §8.5). Re-measure: boundary ship gate, R-24 routing, K5/A3 shootouts, S3 projection,
   `fit/e2e/spy_real` (Sprint I gate: ≤ 200 ms from 492 ms), 100-name row if time (≥ 4× fit-CPU
   waypoint).
5. **Update the PLAN ledger** with Sprint I results; decide CStar ladder question from the I-S
   real-OPRA evidence (decision belongs to the user; evidence doc will carry the recommendation).
6. **Merge to main — AFTER Sprint R lands.** `feat/sota-integration` is deliberately not merged:
   the user has uncommitted Sprint R work in `C:\atx` (`calib.cpp`, `boundary_interp.cpp`,
   `calib.hpp`, tests). Order stays R → integration. Expected conflict surface at that merge:
   `calib.cpp` / `boundary_interp.cpp` between Sprint R's uncommitted work and I-A's seam — resolve
   with the user present.
7. **Deferred/backlog:** W4.2 sibling fit pool + W4.5 H² guard + small-book cutoff at
   n={1,2,3,4,6,8,12,16} (needs Sprint R's R-14/R-15 executor state — still open); the two
   pre-existing v2 failures (owner: user/v2 — triage doc has concrete fixes); Chebyshev-Φ retirement
   once `american_boundary_avx2.cpp` migrates; Sprint G (DoD closure, universe-cycle baseline,
   published shootouts); Sprint X (Schadner 60 ns IV, SplineVol, AVX-512).

## Incidents & gotchas log

- **calib.cpp sweep/heal on main:** a stale staged calib.cpp rode into docs commit `b16ec45`,
  reverting Task 1 (R-01); restored byte-for-byte from `4f1872b` in `7fca341`. If Sprint R had
  un-staged post-Task-1 calib.cpp edits at that moment, re-verify them.
- **PowerShell invocation traps** (cost two failed gate runs): nested `powershell scripts\atx-build.ps1`
  re-tokenizes args at drive colons (`-DFOO=C:\x` splits); unquoted `-E`/`-L` bind as PowerShell
  params in-session. Working pattern: in-session `& scripts\atx-build.ps1 --preset dev
  '-DFETCHCONTENT_BASE_DIR=C:/…' '-DATX_BUILD_BENCH=ON'` and `-Ctest '-L' 'atx_vol' '-E' '<regex>'`.
- **Worktree deps isolation:** every configure in every worktree passes a per-worktree, per-preset
  `FETCHCONTENT_BASE_DIR` (shared `C:\atx-cache\deps` has the Debug/Release `_ITERATOR_DEBUG_LEVEL`
  mixing race). Never Debug and Release builds concurrently in one worktree.
- **Bench numbers to date are provisional (concurrent host);** the phase-3 quiet-window re-measure
  (item 4) produces the citable numbers.
- One phase-2 agent (I-A) crashed at spawn (returned boilerplate, zero tool calls) and was
  relaunched successfully — watch for that failure mode when dispatching.
