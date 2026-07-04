# p7 Sprint 4 — Turnover & Capacity Realism — Progress Ledger

Base: main @ e0659e4 (docs(p7): Wave-1 close — TRACKER, ROADMAP status + S1-4
correction, relocate sprint ledgers)
Branch: feat/p7-s4
Worktree: C:\atx-wt\p7-s4

## Scope (binding overrides)

- **D3 (binding override):** S4-4 IS permitted to edit
  `atx-engine/include/atx/engine/factory/fitness.hpp` (header only) to add the two
  inline free functions `tradeable_fitness_cfg()` + `turnover_target_from_gate()`
  and the named `inline constexpr` constants, plus the new test file
  `atx-engine/tests/factory/fitness_turnover_test.cpp`. The plan's Owns block lists
  `fitness.hpp` as Must-NOT-touch citing "S1 owns it" — that is a stale Wave-1
  artifact; S1 is already merged. **`src/factory/fitness.cpp` stays UNTOUCHED**
  (helpers are header-only inline; the penalty formula is unchanged).
- Owned (exclusive): `loop/weight_policy.hpp`, `combine/combiner.hpp`,
  `cost/capacity.hpp`, `risk/capacity.hpp`, `factory/fitness.hpp` (header only per
  D3), plus the new test files. Do NOT touch `atx-impl/src/*` (S7/S5),
  `combine/conviction.hpp` / `risk/kelly_sizing.hpp` (S5), `src/factory/fitness.cpp`
  or `factory/gate.hpp` (S1), or any `oracle.hpp` (untouchable).

## Drift notes (test placement — resolved by content per protocol §4)

The plan names test dirs `tests/cost` and `tests/loop`, but the engine test CMake
(`atx-engine/tests/CMakeLists.txt`) registers ONLY these groups:
`alpha risk data factory parallel learn eval library combine fund book core regime store`.
There is no `cost` or `loop` group, and `tests/CMakeLists.txt` is NOT in my Owns
set (editing the `ATX_ALL_TEST_GROUPS` list is forbidden by the protocol). The
EXISTING `cost/capacity.hpp` tests already live in `tests/core/`
(`core/capacity_test.cpp`, `core/cost_integration_test.cpp`); agent.md §7 routes
cross-cutting/misc tests to `core/`. Therefore:

- S4-1 `weight_policy_decay_test.cpp` → `tests/core/`  (builds into atx-engine-core-tests)
- S4-2 `capacity_vector_test.cpp`     → `tests/core/`  (builds into atx-engine-core-tests)
- S4-3 `capacity_scorecard_test.cpp`  → `tests/core/`  (builds into atx-engine-core-tests)
- S4-4 `fitness_turnover_test.cpp`    → `tests/factory/` (real group, as planned)

No CMakeLists edits are required (all four dirs are CONFIGURE_DEPENDS-globbed).

## Unit checklist

- [x] S4-0  marker + ledger (this commit)
- [x] S4-1  EmaDecayPolicy (opt-in stateful EMA-decay WeightPolicy sibling);
            ema_alpha=1.0 inert default. WeightPolicyDecay.* (4) green.
- [x] S4-2  compute_capacity_vector (per-alpha capacity AUM from last-period book).
            CapacityVector.* (3 runtime + reviewer-gate (a)) green; stage_combine.cpp untouched.
- [x] S4-3  CapacityScorecard struct + emit_capacity_scorecard. CapacityScorecard.*
            (3 runtime + reviewer-gate (a)) green; no existing source modified.
- [x] S4-4  tradeable_fitness_cfg() + turnover_target_from_gate() (header-only inline,
            fitness.hpp per D3). FitnessTurnover.* (4) green; fitness.cpp untouched.

## Byte-identity gate (run green before AND after every unit)

`atx-engine-factory-tests --gtest_filter=*Oracle*:*Golden*:*Digest*`
(verified GREEN at base: 18/18 before S4-0.)

## Progress

S4-0: complete (commit 09e372a) — ledger opened; base main @ e0659e4; no source.
S4-1: complete (commit b2a891e, WeightPolicyDecay.* 4/4 green) — EmaDecayPolicy
      opt-in stateful sibling; ema_alpha=1.0 short-circuits to byte-identical
      pass-through. WeightPolicy struct unchanged, kTruncateIters=8 preserved.
      Byte-id gate 18/18; full core suite 303/303 (zero regressions). Test in
      tests/core/ (no `loop` CMake group — drift noted above).
S4-2: complete (commit 4920326, CapacityVector.* 3/3 runtime green) —
      compute_capacity_vector: per-alpha last-period book swept over a 20-pt
      log-spaced AUM grid -> capacity_point. Reuses capacity_for_alpha +
      capacity_point (no second cost model). stage_combine.cpp diff EMPTY
      (class-a reviewer gate); byte-id gate 18/18; full core suite 306/306.
      Class (a) is a reviewer/diff gate not a runtime case (3 runtime b/c/d).
      Test in tests/core/ (no `cost` group — drift noted above).
S4-4: complete (commit 60c0dbf, FitnessTurnover.* 4/4 green) —
      tradeable_fitness_cfg() (slope=2.0, target=0.20) + turnover_target_from_gate
      (L>0->L, else +inf), header-only inline per D3. Named detail constants
      kTradeable{TurnoverSlope,MaxTurnover} + public aliases. FitnessCfg{}
      default unchanged; src/factory/fitness.cpp diff EMPTY (D3 honored). Penalty
      mult verified to 1e-12 at turnover {0.10,0.30,0.60}. Byte-id gate 18/18;
      full factory suite 212/212. (Done before S4-3 per plan commit order;
      independent of S4-3.)
S4-3: complete (commit 8d9f8f1, CapacityScorecard.* 3/3 runtime green) —
      CapacityScorecard{capacity_point_aum, gross_edge_bps, net_edge_at_target,
      curve} + emit_capacity_scorecard. Delegates to capacity_for_book +
      capacity_point + risk::detail::gross_edge_bps (one cost surface). On a
      4-identical-name fixture: monotone curve, gross == analytic, capacity_point
      within 5% of the closed-form crossing, net@target eroded below gross.
      No existing source modified; byte-id gate 18/18; full core suite 309/309.
      Class (a) is the reviewer/diff gate (3 runtime b/c/d). Test in tests/core/.

## Final gate results

- WeightPolicyDecay.* 4/4, CapacityVector.* 3/3, CapacityScorecard.* 3/3
  (all in atx-engine-core-tests), FitnessTurnover.* 4/4 (atx-engine-factory-tests).
  14 new runtime tests; the three "class (a) off-path byte-identity" cases for
  S4-2/S4-3 are reviewer/diff gates (no source call site modified), not runtime
  rows; S4-1/S4-4 class (a) ARE runtime rows.
- Byte-identity gate (atx-engine-factory-tests *Oracle*:*Golden*:*Digest*):
  18/18 green — verified at base AND after every unit (S4-1, S4-2, S4-4, S4-3).
- Full atx-engine-core-tests: 309/309 (303 baseline + 6 new core tests).
- Full atx-engine-factory-tests: 212/212 (208 baseline + 4 new).
- Owned-file diff only: loop/weight_policy.hpp, cost/capacity.hpp,
  factory/fitness.hpp (header per D3), + 4 new test files. NO change to
  src/factory/fitness.cpp (D3), stage_combine.cpp, stage_discover.cpp,
  config.{hpp,cpp}, conviction.hpp, kelly_sizing.hpp, gate.hpp, or any oracle.hpp.
- WeightPolicy struct body unchanged; kTruncateIters=8 preserved.
- No CMakeLists edits; no golden re-baseline; oracle.hpp frozen.

## Post-review fix

Post-review fix (review I-1): guard compute_capacity_vector against non-positive
target_aum. Added `ATX_CHECK(target_aum > 0.0)` at function entry (mirrors the
existing ATX_CHECK discipline in capacity_for_alpha) so the [[nodiscard]] helper
fails closed instead of feeding std::log(<=0) into the log-spaced grid; corrected
the stale header comment (removed the incorrect "degenerate grid / returns grid[0]"
wording, now states target_aum must be strictly positive and the helper checks the
precondition). Precondition-only: byte-identical for all valid (target_aum>0) inputs.
Only cost/capacity.hpp changed. CapacityVector.* 3/3 green; byte-identity slice
(atx-engine-factory-tests *Oracle*:*Golden*:*Digest*) 18/18 green (build unity-OFF).
