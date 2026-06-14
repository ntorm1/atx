# Sprint S5 (p2) — Deep-Learning Sequence Alphas — Implementation Progress

**Status:** 🟡 IN PROGRESS — 2026-06-14. Building the deterministic CPU-only NN substrate + four tiny sequence/factor architectures over the as-built `p1` S5 `atx::engine::learn` layer. Subagent-driven development (fresh implementer per unit + two-stage spec/quality review).
**Worktree:** `C:/Users/natha/atx-wt/p2-s5` (dedicated, isolated)
**Branch:** `feat/p2-s5-dl-alphas`
**Base:** `main` @ `f7f4e01` (the merged `p1` S1–S8 engine **+ `p2` S1/S2/S3/S4**; S5 consumes S4's robustness battery)
**Started:** 2026-06-14
**Source plan:** [`sprint-5-deep-learning-sequence-alphas-implementation-plan.md`](sprint-5-deep-learning-sequence-alphas-implementation-plan.md)
**Build gate:** VS Dev env (`Enter-VsDevShell`, MSVC x64) + `cmake --preset dev` (this env HAS `sccache` @ `C:/atx-cache/bin` + `ATX_DEPS_DIR=C:/atx-cache/deps`, so the `dev` preset's cross-worktree object cache + shared FetchContent IS used) →
`cmake --build --preset dev --target atx-engine-tests` (`/W4 /permissive- /WX`, `ATX_WERROR=ON`; **no `/fp:precise` flag exists** — determinism order-fixed scalar reductions, §0.10) → `ctest --preset dev -R <Suite>`.

> **User directive (this sprint):** private/non-trivial implementations live in **`.cpp` source files** (`src/learn/*.cpp`, `src/learn/nn/*.cpp`), not header-only inline as the plan's §4 pseudocode sketches. Headers (`include/atx/engine/learn/*.hpp`, `include/atx/engine/learn/nn/*.hpp`) carry the public API + contracts; the NN kernels (conv/GRU/attention forward+backward, trainer, autoencoder) go in the source files (`.agents/cpp/agent.md` §6; keeps the pinned-FP scalar hot loops in one TU, §0.10). Header-only is reserved for the `Seq3` view (`tensor.hpp`) and trivially-inlineable contracts.
> **User directive (dev speed):** the slow `MultiHorizonOptimizer::run`-driven suites are EXCLUDED from the per-unit regression gate via `ctest -E "RiskMultiHorizon|FundMetaBook|FundSleeve"` (these are S1/S2 MPC-optimizer tests, untouched by S5; full un-excluded suite is run once at close). S5's own `learn_*` suites never touch MHO.

---

## §0 — Kickoff recon amendment (vs `main` @ `f7f4e01`)

The plan's §0.1–§0.10 ARE the as-built ML-framework code review (authored last session). The controller's kickoff re-recon against the **actual** branched tree confirms every cited seam still resolves and adds the build-system + signature facts below; each is briefed into the affected unit.

- **K1 — All `p1` S5 + gate seams CONFIRMED present at (or near) the §0-cited lines.** `learn/learned_source.hpp`: `enum class ModelKind : atx::u8 { Linear, Gbt }` (`:86`), `struct LearnedModel` (`:182`), `build_augmented_row` (inline, `:221`), `predict_blended` (inline EXHAUSTIVE switch, NO default, `:279`); `learn/linear_alpha.hpp`: `fit_linear` (`:154`), `oos_deflated_sharpe(model, fm)` (`:205`); `learn/train.hpp`: `TrialCounter` (`:66`), `seed_for` (inline, `:79`), `date_label_spans` (`:109`), `expand_date_folds` (`:137`); `src/learn/ensemble.cpp`: `stack_to_candidate` (`:184`); `atx-core/random.hpp`: `class Xoshiro256pp` (`:83`), `normal()` (cached-spare note, `:184`). **The §0.2 pluggability contract holds: add a `ModelKind` enumerator + NN payload + `predict_blended` case + `fit_nn`, inherit the gate.**

- **K2 — `ISignalSource::evaluate` returns `Result<SignalView>`, NOT `std::span<const f64>`.** [`loop/signal_source.hpp:87/103`] The real seam is `[[nodiscard]] virtual atx::core::Result<SignalView> evaluate(PanelView panel) = 0;`. The plan §4.6 `SeqLearnedSignalSource` pseudocode wrote `std::span<const atx::f64> evaluate(PanelView)` — a sketch simplification. **S5-5 MUST override the real signature** (`Result<SignalView>`), mirroring the as-built `LearnedSignalSource` (`learned_source.hpp:339`). The interface already documents a `max_lookback` concept (`:131`) — the lookback adapter aligns with that.

- **K3 — The engine static lib uses EXPLICIT source registration, NOT a glob.** [`atx-engine/CMakeLists.txt`] Every `src/<mod>/*.cpp` is listed by hand in `add_library(atx-engine STATIC ...)`. **Consequence:** every new `src/learn/*.cpp` AND `src/learn/nn/*.cpp` MUST be appended to that list (the ONE permitted shared-engine edit; units run sequentially ⇒ no conflict). Headers under `include/atx/engine/learn/` + `learn/nn/` need no CMake edit. (`learn/nn/` is a new include subdir — no CMake impact, header-only there is the `Seq3` view.)

- **K4 — Test sources ARE auto-globbed** (`tests/CMakeLists.txt`: `file(GLOB ... CONFIGURE_DEPENDS "*_test.cpp")`). Dropping a new `tests/learn_*_test.cpp` re-globs on the next configure — no test-CMake edit. (`bench/*_bench.cpp` likewise.)

- **K5 — Error vocabulary `atx::core::ErrorCode` has NO `Infeasible`/`DimensionMismatch`/`Empty`.** A dim mismatch / insufficient history / empty fold / singular reconstruction / NaN-only column ⇒ `Err(ErrorCode::InvalidArgument, "<msg>")`; bad index ⇒ `OutOfRange`. Do NOT add an enumerator to `atx-core` (forbidden touch).

- **K6 — `core/simd.hpp:24` is NOT bit-reproducible** ("agree to within a few ULPs, not bit-for-bit"). LOAD-BEARING for R1: NEVER route `simd::dot`/`simd::sum` through any reduction that feeds the determinism digest. Every NN reduction (matmul, conv, softmax, loss, grad accumulation, ensemble mean) is an **ascending scalar fold**. (The fast SIMD path may be benchmarked off the digest path only.)

- **K7 — Build needs the VS Developer environment.** This shell is NOT a dev shell by default (`ninja` off PATH, `cl`/`INCLUDE`/`LIB` empty). Canonical invocation (used by the controller + every coding sub-agent):
  ```powershell
  Import-Module "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
  Enter-VsDevShell -VsInstallPath "C:\Program Files\Microsoft Visual Studio\2022\Community" -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
  Set-Location "C:\Users\natha\atx-wt\p2-s5"
  cmake --build --preset dev --target atx-engine-tests
  ctest --preset dev -R <Suite> -E "RiskMultiHorizon|FundMetaBook|FundSleeve"
  ```
  (Submodule `atx-core/third-party/databento-cpp` was `git submodule update --init`'d in this worktree — a build prereq, not an atx-core source edit.)

---

## Per-unit status

| Unit  | Title                                                                                          | Status | Commit SHA | Tests | Notes |
|-------|------------------------------------------------------------------------------------------------|--------|------------|-------|-------|
| S5-0  | Marker + ledger + kickoff recon (K1–K7) + baseline build green                                  | ✅     | `d309a9b`  | green | ledger + recon; baseline `atx-engine-tests` green (44s, dev preset PCH) before S5.1. |
| S5-1  | Sequence-feature tensor builder — `learn/nn/tensor.hpp` (Seq3) + `learn/sequence_features.{hpp,cpp}` | ✅ | `fe978a6`  | 10/10 | PIT trailing windows `[t-L+1..t]` over `FeatureMatrix`; truncation-invariance + `L=1`→tabular pin + insufficient-history NaN. Spec+CQ review passed. |
| S5-2a | NN framework substrate — `learn/nn/{module.hpp,layers,optimizer,loss,trainer}`                   | ⏳     | —          | —     | `Module`/`Layer`/`Optimizer`/`Loss`/`Trainer`; **finite-diff gradient check per layer (R3)** + two-builds determinism (R1) + linear-net→`p1` S5 linear-alpha pin (R7). |
| S5-2b | TCN + GRU-lite alphas — `learn/tcn_alpha.{hpp,cpp}`                                              | ⏳     | —          | —     | `fit_tcn`/`fit_gru` over trailing CPCV folds; causal-no-look-ahead (R2) + seed-ensemble determinism (R8). |
| S5-3  | Autoencoder + attention-lite — `learn/autoencoder_alpha.{hpp,cpp}`                               | ⏳     | —          | —     | `fit_autoencoder_factors` (GKX-style) + `fit_attn`; **linear-AE→atx-core `pca()` pin (R7)** + attention backward (R3). |
| S5-4  | NN deflation + PBO gate                                                                          | ⏳     | —          | —     | `oos_deflated_sharpe` + `eval::pbo` reused verbatim; trial count incl architecture×seed sweep (R4); planted-signal admit / noise reject pin. |
| S5-5  | `nn_source` integration + bench + close — `learn/nn_source.{hpp,cpp}`                            | ⏳     | —          | —     | `ModelKind::{Tcn,Gru,Attn,Autoencoder}` + `predict_blended` cases + `SeqLearnedSignalSource` (real `Result<SignalView>` sig, K2) + library bridge + all-gates capstone + `nn_alpha_bench.cpp`. |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `d309a9b` | S5-0 | docs(s5-0): open p2 sprint-5 deep-learning-alphas ledger + kickoff recon (K1–K7) |
| `fe978a6` | S5-1 | feat(s5-1): PIT trailing sequence-window tensor builder over FeatureMatrix (learn/sequence_features + nn/tensor.hpp) |

---

## Shared-branch / discipline
Dedicated worktree `feat/p2-s5-dl-alphas` (true isolation). Explicit-pathspec commits only (`git add -- <paths>`; never `git add -A`); `git show HEAD --stat` after each commit (only this sprint's files); NEVER touch `atx-core/*` or `atx-tsdb/*` (READ-only reuse); engine-source touch limited to new `learn/` + `learn/nn/` files + the additive `atx-engine/CMakeLists.txt` source-list lines (K3); do not push. Commit trailer:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
