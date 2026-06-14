# Sprint S2 (p2) — Multi-Strategy Meta-Book & Risk Budgeting — Implementation Progress

**Status:** 🟡 OPEN — opened 2026-06-13. Subagent-driven development (fresh implementer per unit + two-stage spec/quality review).
**Worktree:** `C:/Users/natha/atx-wt/p2-s2` (dedicated, isolated)
**Branch:** `feat/p2-s2-meta-book`
**Base:** `main` @ `13dbacf` (the merged `p1` S1–S8 engine **+ `p2` S1** multi-horizon optimizer)
**Started:** 2026-06-13
**Source plan:** [`sprint-2-multi-strategy-meta-book-implementation-plan.md`](sprint-2-multi-strategy-meta-book-implementation-plan.md)
**Build gate:** VS Dev env (vcvars64) + `cmake --preset dev` (this env HAS `sccache` @ `C:/atx-cache/bin` + `ATX_DEPS_DIR=C:/atx-cache/deps`, so the `dev` preset's cross-worktree object cache + shared FetchContent IS used, unlike S1) →
`cmake --build build --preset dev --target atx-engine-tests` (`/W4 /permissive- /WX`, `ATX_WERROR=ON`; **no `/fp:precise` flag exists** — determinism order-fixed, §0.10) → `ctest --preset dev -R <Suite>`.

> **User directive (this sprint):** private/non-trivial implementations live in **`.cpp` source files** (`src/fund/*.cpp`), not header-only inline as the plan's §4 pseudocode sketches. Headers (`include/atx/engine/fund/*.hpp`) carry the public API + contracts; the numeric kernels (ERC solve, HRP, netting, Euler attribution, two-pass driver) go in the source files. Matches `.agents/cpp/agent.md` §6 ("put implementation in `.cpp` files whenever possible"). The R7 boundary-pin delegations that must stay trivially-inlineable (`Sleeve::run`) may remain inline.

---

## §0 — Kickoff recon amendment (vs `main` @ `13dbacf`)

The plan's §0.1–§0.10 ARE the as-built reconciliation (authored from a full recon pass last session). The controller's kickoff
re-recon against the **actual** merged tree confirms them and adds the build-system facts below; each is briefed into the affected unit.

- **K1 — All upstream deps MERGED in `main` @ `13dbacf`. The §0 recon target ("merged `p1` S1–S8 + `p2` S1") is SATISFIED.**
  Confirmed present: `risk/multi_horizon.hpp` (`MultiHorizonOptimizer`/`MultiHorizonConfig`/`MultiHorizonResult`/`HorizonSources`/
  `SignalHorizon`), `risk/factor_model.hpp` (`FactorModel`: `risk(w)`, `apply`, `apply_inverse`, `exposures()`=X, `specific_var()`=D,
  `n_instruments()`, `n_factors()`), `book/allocation.hpp` (`size_book`, `effective_breadth`, `AllocationConfig`), `book/multi_period.hpp`
  (`book::CostInputs{kappa, round_trip_cost_bps, capacity_gross}`), `combine/metrics.hpp` (`compute_metrics`), `combine/correlation.hpp`
  (`pairwise_complete_corr`; **NO** `pairwise_complete_cov`), `library/library.hpp` (`Library`, `AlphaId`, `LifecycleState`, `state_as_of`).
  No `Sleeve`/`Strategy`/`MetaBook`/`RiskBudget` type anywhere (§0.1) — `fund/` is greenfield.

- **K2 — The engine static lib uses EXPLICIT source registration, NOT a glob.** [`atx-engine/CMakeLists.txt`](../../CMakeLists.txt) lists each
  `src/<mod>/*.cpp` by hand in `add_library(atx-engine STATIC ...)`. **Consequence:** every new `src/fund/*.cpp` MUST be added to that list (the
  one shared guard-file edit per unit; units run sequentially so no conflict). Headers under `include/atx/engine/fund/` need no CMake edit.

- **K3 — Test sources ARE auto-globbed** (`tests/CMakeLists.txt`: `file(GLOB ... CONFIGURE_DEPENDS "*_test.cpp")`). Dropping a new
  `tests/fund_*_test.cpp` re-globs on the next configure — no test-CMake edit. (`bench/*_bench.cpp` likewise in `bench/`.)

- **K4 — Error vocabulary `atx::core::ErrorCode` (enum class) has NO `Infeasible`/`Unbounded`/`DimensionMismatch`/`Empty`.** Full set:
  `Unknown, InvalidArgument, OutOfRange, NotFound, AlreadyExists, PermissionDenied, Unavailable, Internal, NotImplemented, IoError, ParseError`.
  A dim mismatch / empty sleeves / singular `Ω` / infeasible risk budget ⇒ `Err(ErrorCode::InvalidArgument, "<msg>")`; bad index ⇒ `OutOfRange`.
  Do NOT add an enumerator to `atx-core` (forbidden touch).

- **K5 — ZERO engine-source touches outside new `fund/` files (cleaner than S1).** The factor/specific split is the `risk(W) − WᵀDW` identity
  (§0.4) — no new `FactorModel` accessor. The only shared edit is `atx-engine/CMakeLists.txt` (K2, additive source-list lines).

- **K6 — Build needs the VS Developer environment.** This shell is NOT a dev shell by default (ninja off PATH, `VCINSTALLDIR` empty). Canonical
  invocation wraps `vcvars64.bat`:
  `cmd /c "\"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat\" && cd /d C:\Users\natha\atx-wt\p2-s2 && cmake --build build --preset dev --target atx-engine-tests"`.

---

## Per-unit status

| Unit  | Title                                                                                  | Status   | Commit SHA(s) | Tests | Notes |
|-------|----------------------------------------------------------------------------------------|----------|---------------|-------|-------|
| S2-0  | Marker + ledger + kickoff recon (K1–K6) + plan/ROADMAP carried onto branch              | 🟡       | _(this)_      | —     | this ledger; sprint-2 plan + S2 ROADMAP wiring committed onto the branch. |
| S2-1  | Sleeve — wrap `MultiHorizonOptimizer` over a library subset (`fund/sleeve.hpp`)         | ⏳       | —             | —     | `SleeveTag`/`SleeveConfig`/`Sleeve`; pure-delegation `run`; R7 boundary pin (§4.1). |
| S2-2  | Meta-allocator — risk budget + portfolio-Kelly (`fund/meta_allocator.{hpp,cpp}`)        | ⏳       | —             | —     | ERC log-barrier (CCD/Newton) / HRP / inverse-vol; fixed-iter; capacity box; CCD-vs-Newton differential (§4.2). |
| S2-3  | Cross-sleeve risk — aggregate exposure + fund risk split + Euler RC (`fund/cross_sleeve_risk.{hpp,cpp}`) | ⏳ | — | — | `b_fund=Σc_sXᵀw_s`, `risk(W)−WᵀDW` split, `Ω` from `pairwise_complete_corr`, Euler-sum proof (§4.3). |
| S2-4  | Netting — net book + gross/net turnover + priced benefit (`fund/netting.{hpp,cpp}`)     | ⏳       | —             | —     | `W=Σc_sw_s`; `T_net≤T_gross`; `crossing_benefit≥0` via `book::CostInputs`; opposite-books-net-zero (§4.4). |
| S2-5  | Meta-book driver — two-pass walk + FundReport + attribution + S1 pin (`fund/meta_book.{hpp,cpp}`) | ⏳ | — | — | trailing allocate (R2) → net → fund first move; Euler attribution; **one-sleeve byte-identical to S1** (§4.5). |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| _(this)_ | S2-0 | docs(s2-0): open p2 sprint-2 meta-book ledger + kickoff recon (K1–K6) + plan/ROADMAP |

**Test totals:** _(accumulated per unit)_

---

## What S2 proves
_(filled at close — R1 determinism, R2 trailing no-look-ahead, R3 netting/honest-cost, R4 Euler attribution additivity, R7 S1 boundary pin)._

## Residuals → p2 / atx-core backlog
_(filled at close — joint mega-QP §0.7; library-driven sleeve membership/clustering §0.2 → S4/S8; `core::linalg::risk_budget` lift §2.1; convex-impact crossing variant §4.4.1; `MultiPeriodResult`/`MultiHorizonResult` unification §0.5)._

## Baton → next
_(filled at close)._

---

## Shared-branch / discipline
Dedicated worktree `feat/p2-s2-meta-book` (true isolation). Explicit-pathspec commits only (`git add -- <paths>`; never `git add -A`);
`git show HEAD --stat` after each commit (only this sprint's files); NEVER touch `atx-core/*` or `atx-tsdb/*`; engine-source touch limited to
new `fund/` files + the additive `atx-engine/CMakeLists.txt` source-list lines (K2/K5); do not push. Commit trailer:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
