# Sprint S2 (p2) — Multi-Strategy Meta-Book & Risk Budgeting — Implementation Progress

**Status:** ✅ CLOSED — 2026-06-14. All 6 units (S2-0..S2-5) landed; **54/54 Fund tests** green; **full engine suite 1298/1298** green (399 s, the two `*_NOT_BUILT` entries are the unbuilt `atx-core`/`atx-tsdb` test exes — out of this worktree's target, not failures). Subagent-driven development (fresh implementer per unit + two-stage spec/quality review). Awaiting the merge gate (the user's call — `finishing-a-development-branch`).
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
| S2-1  | Sleeve — wrap `MultiHorizonOptimizer` over a library subset (`fund/sleeve.hpp`)         | ✅       | `2d4718e`     | 4     | `SleeveTag`/`SleeveConfig`/`Sleeve`; pure-delegation `run` (R7 structural pin); `DelegationTransparency` byte-identical (digest + per-elem `bit_cast`) to `MultiHorizonOptimizer.run`. Header-only (no `.cpp`, no CMake edit). spec COMPLIANT + quality APPROVED. 3 🟢 minor IWYU/comment nits deferred (test drops unused `combine/store.hpp`; header `library.hpp`→lighter `AlphaId` home; trim `run` comment). |
| S2-2  | Meta-allocator — risk budget + portfolio-Kelly (`fund/meta_allocator.{hpp,cpp}`)        | ✅       | `7c25c49`     | 20    | ERC log-barrier CCD (fixed `solve_iters`, NO early-exit; positive-root) / HRP single-linkage (no Ω⁻¹; iterative seriate+bisection) / inverse-vol; portfolio-Kelly + gross-clipped vol-target (gross-then-box ⇒ `Σ\|c\|≤max_gross` ∧ `c_s≤cap_s`); §0.8 degenerate→inverse-vol fallback; config scalars validated (NaN-reject). First `src/fund/*.cpp` + CMake source-list line. spec COMPLIANT + quality APPROVED (5 review fixes folded: seriate de-recursion, scalar validation, exhaustive-switch, HRP cross-block test, caps≤0 test). |
| S2-3  | Cross-sleeve risk — aggregate exposure + fund risk split + Euler RC (`fund/cross_sleeve_risk.{hpp,cpp}`) | ✅ | `014756f` | 10 | `b_fund=XᵀW`; factor/specific split via `risk(W)−WᵀDW` identity (NO `F` accessor, NO dense M×M — K5 zero engine-touch); `Ω` from `pairwise_complete_corr·σ·σ` (order-fixed, PIT-pure); Euler `RC_s=c_s(Ωc)_s/sqrt(cᵀΩc)` ⇒ `Σ RC_s=sqrt(cᵀΩc)` (R4 proof — the Ω-based sleeve vol, distinct from V-based `sigma_fund`). spec COMPLIANT + quality APPROVED (3 🟢 nits folded: dead `<cstddef>`, `factor_var≥0` FP floor, `fund_risk` NaN-policy doc). |
| S2-4  | Netting — net book + gross/net turnover + priced benefit (`fund/netting.{hpp,cpp}`)     | ✅       | `ca4d181`     | 11    | `W=Σc_sw_s`; `T_gross=Σ_iΣ_s\|c_sΔw\|`, `T_net=Σ_i\|Σ_s c_sΔw\|`, `T_net≤T_gross` (triangle, by construction) + `crossing_benefit_bps≥0` priced via `book::CostInputs.round_trip_cost_bps` (linear; convex §4.4.1 deferred); identical-sleeves⇒crossed=0, opposite-sleeves⇒fund=0/crossed=1; PIT same-timestamp aggregation. spec COMPLIANT + quality APPROVED (NaN-policy doc folded). |
| S2-5  | Meta-book driver — two-pass walk + FundReport + attribution + S1 pin (`fund/meta_book.{hpp,cpp}`) | ✅ | `03c208d` | 9 | Two-pass: PASS 1 independent causal sleeve walks + realized P&L from the **as-built `returns_at(period)` callback** (resolves the §4.5 P&L-source TODO; drives Ω + report, NOT the books ⇒ R7 holds); PASS 2 TRAILING Ω from P&L strictly `p<s` (R2) → allocate → net → fund book. Report: Euler attribution (return/risk/crossing each ΣΣ to fund — R4), one fund Sharpe via `compute_metrics`, Meucci `N_Ent` effective-bets (Eigen `SelfAdjointEigenSolver`, 0·ln0 guarded). **All 5 gates asserted NON-VACUOUSLY** (R1 byte-identical digest; R2 truncation-invariance `{0..4}`→`{0,1,2}` + `capital_varies` guard; R3 net≤gross ∧ benefit≥0 ∧ real crossing; R4 three sums ~1e-9 nonzero; R7 fund_books==sleeve[0].books==standalone MHO via `bit_cast`, `some_nonzero` guard). spec COMPLIANT + quality APPROVED (3 IWYU nits folded into the amend: header `linalg.hpp` unused→moved to `.cpp`; bench `<functional>` dropped). |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `f93fece` | S2-0 | docs(s2-0): open p2 sprint-2 meta-book ledger + kickoff recon (K1–K6) + plan/ROADMAP |
| `2d4718e` | S2-1 | feat(s2-1): sleeve wraps MultiHorizonOptimizer over a library subset (fund/sleeve.hpp) |
| `7c25c49` | S2-2 | feat(s2-2): meta-allocator — ERC/HRP/inverse-vol risk budget + portfolio-Kelly (fund/meta_allocator.{hpp,cpp}) |
| `014756f` | S2-3 | feat(s2-3): cross-sleeve risk — aggregate exposure + fund risk split + Euler component risk (fund/cross_sleeve_risk.{hpp,cpp}) |
| `ca4d181` | S2-4 | feat(s2-4): internal-crossing netting — net book + gross/net turnover + priced benefit (fund/netting.{hpp,cpp}) |
| `03c208d` | S2-5 | feat(s2-5): meta-book driver — two-pass walk + fund report + Euler attribution + S1 boundary pin (fund/meta_book.{hpp,cpp}) |

**Test totals:** S2-1 +4 (FundSleeve) + S2-2 +20 (FundMetaAllocator) + S2-3 +10 (FundCrossSleeveRisk) + S2-4 +11 (FundNetting) + S2-5 +9 (FundMetaBook) — **54 Fund tests, 54/54 green** (4.98 s).

---

## What S2 proves

The `fund/` layer (`atx::engine::fund`) turns N proven single-sleeve S1 optimizers into ONE fund by strict composition — it adds a
trailing risk budget, internal crossing, and Euler attribution on top of the S1 driver and re-implements none of it. The five sprint
gates, each asserted **non-vacuously** in `fund_meta_book_integration_test.cpp` (real ≥2-sleeve, ≥3-period, anti-correlated fixture):

- **R1 — determinism.** Two `run(...)` on identical inputs are byte-identical (whole-fund FNV digest + per-element `bit_cast<u64>` over
  fund books, capital, turnovers). Order-fixed reductions, no RNG / clock / unordered; the Meucci PCA uses Eigen's deterministic
  `SelfAdjointEigenSolver`. (Also pinned per-unit: `FundMetaAllocator.Allocate_IdenticalInputs_ByteIdenticalWeights`,
  `FundNetting.Determinism_IdenticalInputs_ByteIdentical`.)
- **R2 — no look-ahead (the central trap).** Capital `c[s]` is allocated from the TRAILING window of realized sleeve P&L **strictly
  `p < s`** (cpp `trailing_pnl_slice`: `for (p = lo; p < s; ++p)`). Truncating the schedule `{0..4}`→`{0,1,2}` leaves every fund book,
  capital vector and turnover at `p ≤ 2` byte-identical — and a `capital_varies` guard proves the budget genuinely moved (not a trivial
  constant-`c` pass). At `s == 0` the window is empty ⇒ the allocator degenerate fallback fires (§0.8).
- **R3 — netting / honest cost.** Every period `turnover_net ≤ turnover_gross` (triangle, by construction) and
  `crossing_benefit_bps ≥ 0`, priced via `book::CostInputs.round_trip_cost_bps`; the anti-correlated sleeves cross with a strictly
  positive benefit in ≥1 period (non-vacuous), while identical sleeves cross zero.
- **R4 — Euler attribution additivity.** All three by-sleeve vectors sum to the fund total: `Σ return_contrib = R_fund` (linear, exact),
  `Σ risk_contrib = sqrt(cᵀΩc)` (the S2-3 Euler identity over a representative full-sample Ω + final c), `Σ crossing_credit = total
  benefit` (pro-rata by contributed gross volume; equal-split guard at zero volume) — each to ~1e-9 and each non-vacuously nonzero.
- **R7 — S1 boundary pin (load-bearing).** One sleeve + `c = [1.0]` every period + one-sleeve netting ⇒ the fund book schedule is
  **byte-identical** to that sleeve's own `MultiHorizonResult.books` AND to a standalone `MultiHorizonOptimizer::run` over the same
  fixture (`bit_cast<u64>`, with a `some_nonzero` guard so it is not a vacuous all-zero match). The overlay adds **no arithmetic** to the
  realized book in the degenerate case — composition, not reinvention.

**Engine-touch discipline held:** the only shared edit was the four additive `atx-engine/CMakeLists.txt` source-list lines (K2); the
factor/specific split rides the `risk(W) − WᵀDW` identity (§0.4 / K5) with **zero** new `FactorModel` accessor and no dense M×M; no
`atx-core/*` or `atx-tsdb/*` file was touched. Private kernels live in `src/fund/*.cpp` per the user directive; headers carry API + contracts.

## Residuals → p2 / atx-core backlog

- **Joint mega-QP (§0.7).** S2 is net-after-optimize (each sleeve solves independently, the fund nets after). A single joint QP over all
  sleeves' names with cross-sleeve risk in the objective is the richer (and far costlier) alternative — deferred; S2's composition is the
  shippable baseline.
- **Library-driven sleeve membership / clustering (§0.2 → S4/S8).** `SleeveConfig.members` (an `AlphaId` subset) and `SleeveTag`
  (universe/family) are carried but not yet *driven* by `library::Library` lifecycle state or a clustering pass — that wiring lands when the
  genetic-search / library sprints (S4/S8) populate sleeves from discovered alphas.
- **`core::linalg::risk_budget` lift (§2.1).** The ERC log-barrier CCD + HRP single-linkage kernels live in `src/fund/meta_allocator.cpp`.
  If a second consumer appears they should lift into `atx-core` `linalg` as a reusable risk-budget primitive (forbidden to touch core this
  sprint, so deliberately left in `fund/`).
- **Convex-impact crossing variant (§4.4.1).** Netting prices the crossing benefit linearly (`gross − net) · round_trip_cost_bps`). A
  convex / square-root market-impact crossing model is the follow-up; the linear form is the honest floor.
- **`MultiPeriodResult` / `MultiHorizonResult` unification (§0.5).** The two result types still co-exist; a later refactor should unify
  them. Out of S2 scope.
- **`returns_at` realized-P&L source (S2-5 as-built).** The driver takes `returns_at(period) → span<const f64>` to form realized P&L (the
  §4.5 TODO). It feeds Ω + the report only (never the books), so R7 is invariant to it — but a production wiring should source these returns
  from the same PIT feed the sleeves consume, not a separate scripted callback.

## Baton → next

S2 delivers a deterministic, no-look-ahead, attribution-exact multi-strategy meta-book that provably reduces to S1 at the boundary —
the composition substrate the rest of p2 builds on. The immediate consumers: **S3 (Alpha DSL)** and **S4 (genetic search)** produce the
alphas that will *populate* sleeves (closing the §0.2 membership residual), and **S8 (vendor-grade risk)** can supply the cross-sleeve Ω
that `cross_sleeve_risk` currently estimates from realized P&L. The `fund/` API (`Sleeve`, `MetaAllocator`, `cross_sleeve_risk`,
`net_fund_book`, `MetaBook`) is stable and header-documented; the next sprint can treat it as a fixed seam. **Merge gate is the user's
call** — `finishing-a-development-branch` options below; do not auto-merge.

---

## Shared-branch / discipline
Dedicated worktree `feat/p2-s2-meta-book` (true isolation). Explicit-pathspec commits only (`git add -- <paths>`; never `git add -A`);
`git show HEAD --stat` after each commit (only this sprint's files); NEVER touch `atx-core/*` or `atx-tsdb/*`; engine-source touch limited to
new `fund/` files + the additive `atx-engine/CMakeLists.txt` source-list lines (K2/K5); do not push. Commit trailer:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
