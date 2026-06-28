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

- [ ] S4-0  marker + ledger (this commit)
- [ ] S4-1  EmaDecayPolicy (opt-in stateful EMA-decay WeightPolicy sibling);
            ema_alpha=1.0 inert default. WeightPolicyDecay.* (4) green.
- [ ] S4-2  compute_capacity_vector (per-alpha capacity AUM from last-period book).
            CapacityVector.* (4) green; stage_combine.cpp untouched.
- [ ] S4-3  CapacityScorecard struct + emit_capacity_scorecard. CapacityScorecard.*
            (4) green; no existing source modified.
- [ ] S4-4  tradeable_fitness_cfg() + turnover_target_from_gate() (header-only inline,
            fitness.hpp per D3). FitnessTurnover.* (4) green; fitness.cpp untouched.

## Byte-identity gate (run green before AND after every unit)

`atx-engine-factory-tests --gtest_filter=*Oracle*:*Golden*:*Digest*`
(verified GREEN at base: 18/18 before S4-0.)

## Progress

S4-0: complete (commit <pending>) — ledger opened; base main @ e0659e4; no source.
