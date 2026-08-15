# atx-vol — module facts for agents

Vol surface / vol-derivatives library. Source groups: `core pricing fitting marketdata storage analytics backtest` (+ `simd`, `server`). Targets: `atx-vol` (`atx::vol`), `atx-vol-tests` (one monolithic gtest exe → `build/bin/atx-vol-tests.exe`), `atx-vol-tools`, `atx-vol-research`. Public API: `include/atx/vol/api/**` with umbrella `include/atx/vol/api/vol.hpp` (Tier-A manifest, contract-tested — public-surface changes must keep the umbrella suite green).

## Test lanes

- `ctest -L atx_vol_fast` — the inner loop (~2.9k gtest cases; count is measured, not invariant). Preset shortcut: `ctest --preset ninja-fast`.
- `ctest -L atx_vol_slow` — slow suites split out by `ATX_VOL_SLOW_FILTER` (tests/CMakeLists.txt:345).
- `-L atx_vol` regex-matches BOTH labels plus script/aux lanes (python, reference reports, ForceScalar).
- Python wheel: `python/` is a standalone scikit-build-core + pybind11 project (`atxvol`), 26 pytest files, driven into ctest as `atx-vol-python` (SKIP_RETURN_CODE 77).
- ~63 tests SKIP on a bare host (AVX2/counters/market-data/env classes — enumerated in README `## Build & test`).

## Gates (pre-merge, in order)

1. Full fast suite green: `powershell scripts\atx-build.ps1 -Ctest -L atx_vol_fast` (from repo root).
2. Include hygiene: `cmake --preset hygiene && cmake --build --preset hygiene` (PCH OFF; the default build masks include rot).
3. Module gates: `powershell atx-vol\ci\run_all_gates.ps1` — determinism (1-thread vs N-thread bit-identical NAV), golden replay (82-session SPY corpus, fails closed if licensed corpus absent), lakehouse-off link, pool soak. All on `dev` preset — correctness only.
4. Perf claims ONLY from `rel-avx2` (`build-rel-avx2/`) against `bench/baselines/` pins.

**Trap:** `dev-shared` preset is for iteration relinks ONLY, NEVER the test gate — per-DLL counter globals split and SolveLedger/BacktestExec observer suites fail deterministically.

## Memory protocol

- `docs/LEDGER.md` — append-only fact ledger. Grep it before re-deriving build traps, measured numbers, or past decisions. Append one line on gate-pass, trap discovery, or decision. Never rewrite history lines; corrections get a new line referencing the old.
- Sprint narratives: `sprints/YYYY-MM-DD-<topic>-sprint.md` (38 exist — search before starting overlapping work).
- Deep docs index: `docs/` (surface-db-build, backtest-lakehouse, adjoint_greeks_design, simd_fastpath, api-placement, …). Bench anchors: `bench/ANCHORS.md`.

## Editing rules

- House style first: `../.agents/cpp/agent.md` (mandatory).
- New test → correct suite file under `tests/`; slow suites belong in the `ATX_VOL_SLOW_FILTER` glob, not the fast lane.
- CHANGELOG.md entries follow existing prose-heading structure (`### NEW — …` / `### FIXED — …`); cite code, don't restate it.
