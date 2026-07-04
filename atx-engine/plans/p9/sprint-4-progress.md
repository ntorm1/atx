# Sprint 4 progress ledger — Capacity + Turnover as First-Class NSGA Objectives

**Goal:** two OPT-IN NSGA-II objective columns — `kObjCapacity` (a √-law-impact-derived
per-alpha capacity score, reusing `cost::round_trip_cost_bps` + `cost::capacity_point`) and
`kObjTurnover` (a signal AR(1) autocorrelation / alpha-decay-persistence score, reusing
`alpha::detail::ou_ar1_fit`) — so the GA optimizes FOR high-capacity, low-turnover alphas, not
just high-Sharpe ones. ZERO new estimator math; both reuse frozen fitters. The load-bearing
determinism invariant: with both flags off (the default) the objective-vector width used for
domination/selection stays exactly what it was pre-S4 (NSGA-II sizes off `n_objectives`, never
`kMaxObjectives`), so `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` stays byte-identical.

- **Worktree:** `C:\atx-wt\p9`
- **Branch:** `feat/p9`
- **Build gate:** `powershell -File <scratch>\p9-build.ps1 -Target atx-engine-factory-tests`
  (and `atx-impl-tests`) then `p9-ctest.ps1 -R <Suite>` (self-contained MSVC-env wrappers).

One line per clean review (ROADMAP §141). Newest last.

| Unit | Commit  | Deliverable                                                                                        | Review |
|------|---------|----------------------------------------------------------------------------------------------------|--------|
| S4-0 | e024ebf | ledger opened; `kMaxObjectives` 7->9, `kObjCapacity=7`/`kObjTurnover=8` APPEND-ONLY (static_assert frozen-prefix); inert-default `capacity_objective`/`turnover_objective` on SearchConfig+FitnessCfg; report/core carriers | SHIP |
| S4-1 | bbdee31 | `capacity_sqrt_law_score` — bounded [0,1) √-law capacity headroom via `book_cost_bps` + `cost::capacity_point`, saturating (+inf→1.0) transform (unwired) + 4 unit tests | SHIP |
| S4-2 | 5806a67 | `turnover_autocorr_score` — \|w\|-weighted mean AR(1) coeff via frozen `ou_ar1_fit`, degenerate-fit SKIP (not zero-in) + dead-name skip (unwired) + 4 unit tests | SHIP |
| S4-3 | bdd705a | gate both computes in `fitness_core`; `finish_report` writes `objectives[7]/[8]` + `std::max` width bump; `gen_fit` SearchConfig→FitnessCfg wire; `--capacity-objective`/`--turnover-objective` CLI + manifest KV + fail-loud `--target-aum` guard | SHIP |
| S4-4 | f81aeb3 | four determinism classes: real-`factory::dominates` front-membership flips (width 8/9), end-to-end objective gap, twice-run bit-identity, seq==parallel digest-invariance | SHIP |
| S4-R | (this commit) | review closeout: realistic-(default-)slippage capacity-discrimination E2E test (`EndToEndCapacityDiscriminatesUnderRealisticSlippage`) + ledger SHIP | SHIP |

**Review (clean, adversarial):** SHIP — no Critical/Important findings. The reviewer verified:

- **Golden digest byte-identical.** `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`
  (`kGoldenDigest = 0xff95ac12512e0e91`) proven unmoved after S4-0 (enum grew 7->9) AND after S4-3
  (the gated wire landed). The mechanism holds: NSGA-II builds its `ObjMatrix` over
  `k = max(n_objectives)`, NEVER `kMaxObjectives`; slots 7/8 stay at their uniform zero default
  (inert in pure-max dominance) and `n_objectives` is bumped via `std::max` ONLY when a gate flag is
  set. `search_state.hpp`/`search_progress.hpp` size off `kMaxObjectives` but are on no golden path,
  and `pareto.hpp` is generic over `n_objectives` — none moved the digest. Also pinned by the new
  `CapacityTurnoverObjectives_DefaultOff_ByteIdentical`.
- **Both domination flips are genuine, through the REAL `factory::dominates`** (no hand-rolled mock),
  at the exact widths `finish_report` produces. Capacity: `high=[0.5,0.8,1.0,0,0,0,0,0.9,0]` vs
  `low=[…,0.1,0]` — at width 7 (capacity slot excluded) neither dominates; at width 8 (slot included)
  `high` STRICTLY dominates `low`. Turnover: `slow=[…,0(cap),0.9]` vs `churny=[…,0(cap),-1.0]` — width
  7 neither, width 9 `slow` STRICTLY dominates `churny`. The end-to-end path (`pool_aware_fitness`)
  independently populates `objectives[kObjCapacity]` with a genuine ADV-driven gap (deep-ADV 0.794 vs
  thin-ADV 0.274 under isolated impact), and now ALSO under realistic slippage (see below), so the flip
  holds with real evaluator output, not only hand-built vectors.
- **The capacity-fixture change was ruled an HONEST FIX, not a softened assertion.** The plan's
  original S4-1 fixture (volumes 1e7 vs 1e2, a 10 bps gross edge, DEFAULT `SlippageCfg{}`) sat exactly
  on the fixed-cost floor: `SlippageCfg{}.bps == 5.0` charges a `2×`-per-round-trip ~10 bps that is
  ADV-INDEPENDENT and exactly cancels the 10 bps gross edge, collapsing the capacity score to its
  grid-floor constant (0.0909) for EVERY ADV level (measured: cost asymptotes to ~10.0 bps as ADV→∞).
  The score itself was independently verified monotone in ADV and correctly oriented (deeper ADV ⇒
  higher capacity); only the fixture needed to sample the discriminating region. The fix isolates the
  ADV-dependent √-impact term (`impact_only_cost` zeroes `slippage.bps`) and picks interior volumes —
  the frozen cost model is untouched.
- **Documented limitation (real, recorded).** The capacity objective is INERT (a uniform grid-floor
  score ≈ 0.0909 for every candidate) for any alpha whose realized gross edge ≤ the fixed round-trip
  slippage cost. In that regime `--capacity-objective` adds a constant column and therefore exerts NO
  selection pressure on sub-floor / low-edge universes. This is arguably correct semantics (no net edge
  ⇒ no capacity), but it means the objective's discriminating power is CONDITIONAL on gross edge
  exceeding fixed costs — worth knowing before relying on it to steer the GA on thin-edge universes.
- **S4-R closes the reviewer's one coverage Minor.** `EndToEndCapacityDiscriminatesUnderRealisticSlippage`
  now proves the discrimination survives the PRODUCTION default cost (`SlippageCfg{}`, real ~10 bps
  round-trip slippage — NOT `impact_only_cost`): fixed slippage is an additive, ADV/AUM-independent
  downshift, so once the gross edge clears the floor the zero-crossing still moves with ADV. Measured,
  same close, only volume differs: **deep-ADV (vol 1e4) `objectives[kObjCapacity]` ≈ 0.6439 >
  thin-ADV (vol 1e3) ≈ 0.1515**, both strictly interior (0,1), `n_objectives == 8`.
- **Frozen-file discipline held.** `git diff --stat` across S4-0..S4-4 shows ONLY the owned files
  (`factory/fitness.{hpp,cpp}`, `factory/search_driver.{hpp,cpp}` — the SearchConfig fields + the
  `gen_fit` copy only, no other mechanics —, `atx-impl/src/config.{hpp,cpp}`, `stage_discover.cpp`)
  + this ledger + new/extended tests. `pareto.hpp`, `search_state.hpp`, `search_progress.hpp`,
  `cost_aware.hpp` (`round_trip_cost_bps`), `risk/capacity.hpp`, `alpha/ts_ops.hpp` (`ou_ar1_fit`),
  and `alpha/oracle.hpp` are all UNTOUCHED (called, never re-derived).
- **Full regression:** `atx-engine-factory-tests` **246 passed / 0 failed** (245 + the S4-R test);
  `atx-impl-tests` **296 passed / 0 failed / 4 pre-existing conditional skips** (ORATS-fixture +
  gated long-run capacity sweeps, none S4-related).

## Determinism contract — all four classes satisfied

- **(a) off-path byte-identity:** `ScalarRaw_ReproducesGoldenDigest` +
  `CapacityTurnoverObjectives_DefaultOff_ByteIdentical` (both flags inert-default false) reproduce
  `0xff95ac12512e0e91` byte-for-byte; `DefaultsAndEnumPin` pins `kMaxObjectives==9`,
  `kObjCapacity==7`, `kObjTurnover==8`, and both defaults false.
- **(b) on-path RED→GREEN:** `CapacityObjective_FlipsFrontMembership` /
  `TurnoverObjective_FlipsFrontMembership` (real `dominates`, width 7↔8 / 7↔9) +
  `EndToEndObjectivesReflectCapacityGap` (isolated impact) +
  `EndToEndCapacityDiscriminatesUnderRealisticSlippage` (production default cost).
- **(c) twice-run bit-identity:** `TwiceRun_ObjectivesBitIdentical` (`memcmp` of the full 9-slot
  vector, both objectives active) + `CapacityObjective.PureFunction_TwiceRunByteIdentical` +
  `TurnoverObjective.PureFunction_TwiceRunByteIdentical`.
- **(d) seq==parallel:** `CapacityTurnoverObjectives_DigestInvariantAcrossWorkers` — identical
  `SearchResult::digest` across `n_workers ∈ {1,2,4}` with BOTH objectives on.

## Notes

- Growing `kMaxObjectives` 7->9 widens the on-disk `--resume` checkpoint record
  (`search_progress.hpp` sizes off `kMaxObjectives`) — a pre-S4 checkpoint is not expected to resume
  byte-compatibly across this change, the same accepted, precedented consequence as the novelty
  (3->4) and dsr (6->7) width bumps. Out of S4's scope to solve.
- The plan text says "S4-2's 5 tests" but its own code blocks list only 4 turnover tests; the 4 given
  were implemented (a miscount in the plan, not a missing test).
