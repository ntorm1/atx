# atx-engine — Agent Onboarding

Quant alpha factory + backtesting engine. Consumer of `atx-core` (the C++20 stdlib in the same monorepo) and `atx-tsdb` (mmap segment store). Read this, not the whole tree, to pick up fast.

**Authority:** [`../cpp/agent.md`](../cpp/agent.md) governs all C++ here (safety-critical C++20: no UB, `const`/`constexpr`/`noexcept`/`[[nodiscard]]`, `Result<T>` not exceptions, exhaustive enum switches, functions ≤60 lines, TDD, zero hot-path alloc). This file is engine-specific deltas + how to build without pain. Sprint discipline: [`../../atx-engine/plans/docs/sprint.md`](../../atx-engine/plans/docs/sprint.md); quality bar: [`../../atx-engine/plans/docs/implementation-quality.md`](../../atx-engine/plans/docs/implementation-quality.md).

---

## Build & test (don't reinvent this)

Toolchain: **Ninja + clang-cl + vcpkg**, build dir `build/`, type Debug. Run from a **VS Developer shell** (MSVC env present) with `VCPKG_ROOT` set. A `build/` is usually already configured — reuse it.

```bash
# standard configure (only if build/ is missing or CMakeLists changed):
cmake --preset ninja

# FAST iterative path (sccache compiler cache + shared FetchContent deps) — preferred:
cmake --preset dev                 # cross-worktree object-cache hits; deps cloned once

# build the engine tests / benches:
cmake --build --preset dev --target atx-engine-tests
cmake --build --preset dev --target atx-engine-bench
ctest  --preset dev -R <Suite> --output-on-failure
```

- **Fresh worktree, zero fuss:** `scripts\dev-setup.ps1` once per machine (sets `ATX_DEPS_DIR`, installs sccache), then `scripts\new-worktree.ps1 -Name <x> -Branch feat/<x>` per worktree (adds worktree + `cmake --preset dev`). Details: [`../../scripts/dev-setup.md`](../../scripts/dev-setup.md). The `dev` preset forces embedded debug info (`/Z7`) so debug objects are sccache-cacheable; `sccache --show-stats` to watch the hit rate.
- **clangd works automatically** — committed `.clangd` reads each worktree's own `build/compile_commands.json` (every preset emits it). No symlink, no per-worktree wiring.
- Tests/benches are **auto-globbed** (`tests/*_test.cpp`, `bench/*_bench.cpp`, CONFIGURE_DEPENDS) — drop a file, rebuild, done. Do **not** hand-edit `CMakeLists.txt`.
- The warnings gate `atx_warnings` (`/W4 /permissive- /WX` + strict FP `/fp:precise`) is linked — any warning fails the build.
- **Bash tool CWD can drift** (a stray `cd build` persists). Use absolute paths, `git -C <root>`, or `ctest --preset dev`.

---

## What exists — the as-built layers (compose, don't rebuild)

Header-only under `include/atx/engine/` (namespace `atx::engine` + the nested namespaces below), **except** `atx::core::db::sqlite` which is a compiled TU consumed by `library/`. p0 (Phases 1–4) + p1 (S1–S6) are shipped; S7 is planned (frozen plan, not built). **ROADMAPs are canonical for status** — [`p0/ROADMAP.md`](../../atx-engine/plans/p0/ROADMAP.md), [`p1/ROADMAP.md`](../../atx-engine/plans/p1/ROADMAP.md).

| Layer | Namespace / dir | Role | Origin |
|---|---|---|---|
| Event spine | `event/ bus/ clock/ data/` | `Event` POD, `EventBus` (Disruptor facade), `SimClock` PIT gate, k-way-merge feeds | p0 P1 |
| Backtest loop | `loop/ portfolio/ exec/` | `RollingPanel` (sealed-row PIT), `ISignalSource` seam, `WeightPolicy`, `Portfolio` (Decimal ledger), `ExecutionSimulator` (√-impact temp/perm + slippage + commission) | p0 P2 |
| Alpha DSL + VM | `alpha/` | lexer→parser→typecheck→hash-consed DAG→bytecode→columnar VM, 64+ ops, `compile_batch` CSE, `extract_streams`/`AlphaStreams`, `VmSignalSource`, `unparse` | p0 P3/3b/3c/3d |
| Mega-alpha layer | `combine/` | `AlphaStore`/pool, `AlphaGate` (fitness/corr/turnover), `AlphaCombiner` (equal→rank→IC→LW-shrink→bounded-reg), `CombinedSignalSource`, `pairwise_complete_corr` | p0 P4 |
| Risk + optimizer | `risk/` | `FactorModel` `V=XFXᵀ+D` (factored, Woodbury), `FactorModelBuilder`, `build_exposures`, `PortfolioOptimizer` (turnover-penalized, fixed-iteration deterministic), `capacity_curve` | p0 P4 |
| Eval / validation | `eval/ validation/` | `compute_return_metrics`, `deflated_sharpe`/`probabilistic_sharpe`, `pbo`, purged+embargoed `cpcv`, `stats_ext`, `bias_audit` | p1 S1 |
| Parallel | `parallel/` | `DetPool` (deterministic task pool), `parallel_evaluate`/`parallel_cpcv`, order-fixed merge (digest == single-thread) | p1 S2 |
| Factory | `factory/` | genome/mutation/crossover, `canonical_hash` dedup, sep-CMA-ES `param_search`, pool-aware deflated `fitness`, `search_driver`, `Factory::mine_into`, `ResearchDriver` | p1 S3/S4b |
| Library | `library/` (+ `atx::tsdb`, `core::db::sqlite`) | append-only mmap segment `store`, `dedup_index`, SimHash `corr_index`, PIT `lifecycle` journal, content-addressed `manifest`, `Library` facade | p1 S4 |
| Learned signals | `learn/` | PIT `feature_matrix`, `elastic_net`/`linear_alpha` (walk-forward, CPCV, deflation-gated), `latent`, `LearnedSignalSource`, ML `pipeline` | p1 S5 |
| Cost calibration | `cost/` | IRLS-Huber `calibration`, `temp_perm` split, `capacity` root-find, `cost_aware` knobs (calibrated κ + capacity), `borrow` accrual | p1 S6 |

**Seven carried-forward invariants — tested, never weaken:** determinism (byte-identical digest, incl. parallel merge), no look-ahead (fit/apply firewall + purge/embargo), no survivorship (delisted carried at final value), point-in-time (NaN out-of-universe; lifecycle transitions PIT-only), honest+calibrated cost, no hot-path alloc, differential correctness (every kernel vs an obvious reference). See [`p1/ROADMAP.md`](../../atx-engine/plans/p1/ROADMAP.md) "Carried-forward invariants".

---

## Landmines (the stuff that wastes hours)

- **Shared-branch ref race.** The active line `feat/atx-core-stdlib` is one working tree with multiple agents committing. Commits get **orphaned** when another wins the branch-pointer race. After every commit: `git -C <root> merge-base --is-ancestor <sha> HEAD` — if orphaned, re-attach. Always stage **explicit pathspecs**, never `git add -A`. (The fast-worktree setup above is the way to get true isolation — one branch per worktree.)
- **`cost/` was merged from a separate branch.** S6 lived on `feat/atx-engine-cost` (cut pre-S5) and was merged into the line; it is now present. If a build can't find `cost/*.hpp`, you're on a stale tree — rebase/merge.
- **sqlite is a compiled TU, single-`Database`-per-thread.** `atx::core::db` (`SQLITE_THREADSAFE=2`) is the only non-header-only dependency; `library/` is the first/only consumer. Never hand a `Database`/`Statement` to a parallel worker — the serial admit/flush path owns it. Confirm the test target resolves the sqlite symbols transitively via `atx::core`.
- **clang-tidy is disabled** repo-wide (prohibitively slow on umbrella headers / third-party compile DBs). Do not run it or use it as a gate. The strict `/W4 /permissive- /WX` build + ctest are the gate.
- **Heap-allocate `EventBus<>`.** Default ring ~8 MB; a stack instance overflows the 1 MB Windows stack. `std::make_unique`.
- **Optimizer/solver determinism.** `PortfolioOptimizer` (and any new solver) runs a **fixed iteration count** — no convergence-dependent early exit. NaN α cells get exactly 0 weight and are excluded from reductions (the no-survivorship guard). Don't add an early-exit "optimization".
- **Dangling spans.** `AlphaStore`/`LibraryStore`/`AlphaStreams` read spans alias backing storage and dangle after the next insert/stage/flush — compute and copy out **before** growth. mmap'd `library/` segment spans die with the `Mapping`; hold it alive, `// SAFETY:` every borrow.
- Bench numbers in docs are **Debug** upper bounds — never cite as release figures.

---

## Workflow

- **TDD always** (failing GoogleTest first; cover boundaries + the load-bearing invariant proof; `EXPECT_DEATH` for `ATX_ASSERT` preconditions). A unit is done only when header + tests + **clang-format clean + `/W4 /WX` + `/fp:precise` build + tests green**.
- New sprint? Freeze a `sprint-N-<theme>-implementation-plan.md` first (the frozen *how*), copy the §0-recon / §1-rules / §2-files / §3-gates+handoff / §4-algorithms / §5-per-unit / §6-exit / §7-refs / §8-self-review shape of the S4/S5/S6/S7 plans. Open a `sprint-N-progress.md` ledger as the marker commit.
- Commit per unit `feat(sN-M): …` with the `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` trailer. **Do not push** unless asked.
- Engine adds **no general-purpose primitives** — new numeric/infra needs are `atx-core` requests (Pattern B edges), recorded in the sprint plan + `p1/ROADMAP.md`, shipped engine-local with the lift noted.

---

## Status & what's next

- **p0 (Phases 1–4): ✅ CLOSED** — event spine, backtest loop + portfolio + exec-sim, alpha DSL/VM, mega-alpha + Barra risk + optimizer. ([`p0/ROADMAP.md`](../../atx-engine/plans/p0/ROADMAP.md) canonical.)
- **p1 — the v2 alpha factory:** S1 eval/validation ✅, S2 parallel ✅, S3 factory ✅, S4 library + S4b automated engine ✅, S5 learned signals + ML combiner ✅, S6 cost calibration + capacity ✅ (merged). ([`p1/ROADMAP.md`](../../atx-engine/plans/p1/ROADMAP.md) canonical for SHAs + exact test counts.)
- **S7 — Portfolio Construction & Production Lifecycle: plan frozen, NOT built** — [`p1/sprint-7-portfolio-lifecycle-implementation-plan.md`](../../atx-engine/plans/p1/sprint-7-portfolio-lifecycle-implementation-plan.md). Multi-period optimizer (drives `PortfolioOptimizer` over a schedule), alpha-decay monitor (Page-Hinkley + DSR → `library::mark`), dead-alpha → risk-factor (Kakushadze, plumbs the reserved `FactorModelConfig.n_dead_factors` slot), breadth/Kelly capacity-bounded allocation + book reporting, and the full E2E done-gate. This is the v2 exit. Consumes everything.
- Research grounding: [`../../atx-engine/research/*.md`](../../atx-engine/research/) (WorldQuant + RenTech deep-dives + DSL / backtest-loop dives).
