# Sprint S5 (p2) — Deep-Learning Sequence Alphas — Implementation Progress

**Status:** ✅ CODE-COMPLETE — 2026-06-14 (all 6 implementation units S5-0…S5-5b shipped + 2-stage spec/quality reviewed; pending the full-suite close gate + user merge). Built the deterministic CPU-only NN substrate + four tiny sequence/factor architectures (TCN, GRU-lite, attention-lite, autoencoder) over the as-built `p1` S5 `atx::engine::learn` layer. Subagent-driven development (fresh implementer per unit + two-stage spec/quality review).
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
| S5-2a | NN framework substrate — `learn/nn/{module.hpp,layers,optimizer,loss,trainer}`                   | ✅     | `a01e53c`  | 25/25 | `Module`/`Sequential`/`Residual` + Linear/act/Dropout/LayerNorm + Sgd/Adam + Mse/Huber/Ic + Trainer (fixed-epoch, ckpt-at-best-val, seed-ensemble). **Finite-diff grad check per layer (R3)** + two-builds byte-identical (R1) + linear-net→atx-core `ridge()` pin (R7). Spec+CQ review passed. |
| S5-2b-i | Sequence layers — `learn/nn/seq_layers.{hpp,cpp}` (Conv1dCausal/TCN-block/Gru-Mgu cell + temporal pool) | ✅     | `4d438cc`  | 18/18 | dilated causal conv + TCN residual block + GRU-lite/MGU cell (BPTT) + last-step pool; finite-diff grad check each incl. BPTT (R3) + causal-mask bit-identical structural test (R2). Spec+CQ review passed. |
| S5-2b-ii | TCN + GRU-lite alphas — `learn/tcn_alpha.{hpp,cpp}` + `LearnedModel` NN payload + `predict_nn`     | ✅     | `d28691d`  | 12/12 | `ModelKind::{Tcn,Gru}` + `NnPayload` + `predict_nn` (window→ascending-member-mean); shared `fit_seq_alpha` core mirrors `fit_linear` CPCV-OOF (trial_count incl. sweep, `oos_score_series`); single deployed ensemble on blend-weighted target; **also wired decoupled L2 weight-decay into `nn::Trainer` (TrainConfig.l2)**. R1 two-builds byte-identical + R2 truncation-invariant predict + seed-ensemble mean + planted-signal non-vacuous. Spec+CQ review passed (fixed: 0·NaN blended-target poison, payload-arch OOB guard). |
| S5-3a | Attention-lite alpha — `Attention1Head` (seq_layers) + `fit_attn` (tcn_alpha) + `ModelKind::Attn`   | ✅     | `417a1f8`  | 12/12 | single-head causal dot-product attention (causal mask = R2 guard) + softmax adjoint (R3, incl. dbk≡0 floor verified) + `fit_attn` reusing `fit_seq_alpha`; R1/R2/ensemble/dispatch. Spec+CQ review passed (2 accepted cold-path/cosmetic minors). |
| S5-3b | Autoencoder factor extractor — `learn/autoencoder_alpha.{hpp,cpp}` + `ModelKind::Autoencoder`        | ✅     | `10724f5`  | 11/11 | `fit_autoencoder_factors` (GKX-style, centered last-step design, trailing-fit/OOS-encode); **linear-AE→atx-core `pca()` subspace pin (R7)** (recon-MSE + projector ‖P_ae−P_pca‖<1e-2) + `predict_ae` leading-factor; **Trainer L1 wiring**. Spec+CQ review passed. |
| S5-4  | NN deflation + PBO gate — `learn/nn_gate.hpp` (`gate_nn_sweep`)                                  | ✅     | `747da91`  | 10/10 | `gate_nn_sweep` reuses `eval::deflated_sharpe` + `eval::pbo_cscv` verbatim; **deflation N = Σ sweep trial_count (R4 honesty)** + winner DSR + CSCV PBO; planted-admit / pure-noise-reject (PBO channel) + trial-count-honesty pins; 1 real-fit wiring test + 9 hand-built. Spec+CQ review passed. |
| S5-5a | `nn_source` integration — `learn/nn_source.{hpp,cpp}` (SeqLearnedSignalSource + NN library bridge)  | ✅     | `a193552`  | 8/8   | `SeqLearnedSignalSource` (real `Result<SignalView>`, K2; trailing-window reversal `(L-1)-l`) + `nn_to_candidate`→`Library::admit`; all-gates capstone: live↔offline bit-identical pin, R2 truncation-invariance, R1 two-builds, R4 gate flow, expr_source. Spec+CQ review passed (R6 zero-alloc claim qualified honestly). |
| S5-5b | Bench — `bench/nn_alpha_bench.cpp`                                                                 | ✅     | `93a9c41`  | 7 cases | fit (tcn/gru/attn/ae) + predict (predict_nn + SeqLearnedSignalSource) throughput; seed-ensemble ×cost (1 vs 5); bounded down-scaled diagonal subset; /WX-clean, runs exit 0 (predict ~4ms vs fit ~1.6s, ensemble ×5≈4.5×). |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `d309a9b` | S5-0 | docs(s5-0): open p2 sprint-5 deep-learning-alphas ledger + kickoff recon (K1–K7) |
| `fe978a6` | S5-1 | feat(s5-1): PIT trailing sequence-window tensor builder over FeatureMatrix (learn/sequence_features + nn/tensor.hpp) |
| `a01e53c` | S5-2a | feat(s5-2a): deterministic CPU NN substrate — Module/Layer/Optimizer/Loss/Trainer + finite-diff grad checks + ridge pin (learn/nn) |
| `4d438cc` | S5-2b-i | feat(s5-2b-i): causal-conv / TCN-block / GRU-lite-MGU sequence layers + BPTT grad checks (learn/nn/seq_layers) |
| `d28691d` | S5-2b-ii | feat(s5-2b-ii): TCN + GRU-lite sequence alphas — LearnedModel NN payload + predict_nn + CPCV-OOF fit + Trainer L2 (learn/tcn_alpha) |
| `417a1f8` | S5-3a | feat(s5-3a): single-head causal attention layer + attention-lite sequence alpha (learn/nn/seq_layers + tcn_alpha) |
| `10724f5` | S5-3b | feat(s5-3b): autoencoder statistical-factor alpha — linear-AE→PCA pin + GKX seed-ensemble + Trainer L1 (learn/autoencoder_alpha) |
| `747da91` | S5-4 | feat(s5-4): NN-alpha deflation + PBO gate — sweep trial-count aggregation + planted-admit/noise-reject (learn/nn_gate) |
| `a193552` | S5-5a | feat(s5-5a): SeqLearnedSignalSource lookback adapter + NN library bridge + all-gates integration (learn/nn_source) |
| `93a9c41` | S5-5b | bench(s5-5b): NN-alpha fit/predict micro-benchmarks — TCN/GRU/Attn/AE across (L,N,channels) + seed-ensemble cost (bench/nn_alpha_bench) |

---

## Shared-branch / discipline
Dedicated worktree `feat/p2-s5-dl-alphas` (true isolation). Explicit-pathspec commits only (`git add -- <paths>`; never `git add -A`); `git show HEAD --stat` after each commit (only this sprint's files); NEVER touch `atx-core/*` or `atx-tsdb/*` (READ-only reuse); engine-source touch limited to new `learn/` + `learn/nn/` files + the additive `atx-engine/CMakeLists.txt` source-list lines (K3); do not push. Commit trailer:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

---

## What S5 proves (close baton)

S5 adds a **deep-learning signal family** to the as-built `p1` S5 learned layer, **additively and compositionally** — nothing existing was rewritten:

- A **deterministic CPU-only NN substrate** (`learn/nn/`): `Module`/`Sequential`/`Residual` + generic layers (Linear, activations, Dropout, LayerNorm) + sequence layers (dilated **causal** Conv1d, TCN residual block, GRU-lite/MGU cell with BPTT, single-head causal attention) + `Sgd`/`Adam` + `Mse`/`Huber`/`Ic` loss + a fixed-epoch, checkpoint-at-best-val, seed-ensemble `Trainer` with decoupled L1/L2. **Every layer's backward is finite-difference gradient-checked (R3 — the #1 risk, the autodiff replacement).**
- **Four tiny architectures** over it: **TCN** (default), **GRU-lite**, **attention-lite** (the predictive track), and an **autoencoder statistical-factor** alpha (the GKX factor track) — each filling a `LearnedModel` NN payload that **inherits `oos_deflated_sharpe`/`predict_blended`/the blend** (§0.2), admitted through the **same** `Library`/combiner/sleeve seams as a formulaic alpha.
- The **load-bearing invariants are pinned**: **R1** two-builds byte-identical weights + alpha digest (no `simd::dot`, single-thread, order-fixed); **R2** trailing windows + trailing-CPCV folds + causal conv, with the live `SeqLearnedSignalSource` ↔ offline `predict_nn` **bit-identical** window-reversal pin; **R4** deflation `N = the full architecture×seed sweep` + CSCV PBO (planted-admit / pure-noise-reject); **R7** the linear-net→atx-core `ridge()` and linear-AE→atx-core `pca()` boundary reductions.

**Baton → S6 / S8.** S6 (alt-data / multi-asset / intraday) feeds the sequence builder richer features (and an intraday window unlocks a sub-daily sequence alpha). S8 (robust-alpha capstone) routes the NN family — alongside formulaic (S3/S4) and classical-ML (`p1` S5) — through the pipeline + sealed lockbox. The abstract `nn::` framework is the substrate S6/S8 **extend, never rewrite**.

## Residuals → ROADMAP backlog (recorded, not shipped)

1. **Allocation-free live NN forward** (R6 honesty): `SeqLearnedSignalSource::evaluate` pre-sizes its own `window_`/`out_` scratch, but `predict_nn` rebuilds the ensemble + allocates `Module::forward`'s `MatX` per call. A fully alloc-free live path (pre-built ensemble cached in the ctor + pre-sized activation scratch) needs a substrate-level allocation-free `forward` — recorded, not shipped.
2. **`core::linalg` NN-kernel / autodiff tier** (the §2.1 Pattern-B lift): the engine-local `learn::nn` framework is the shippable fallback; a dedicated atx-core deterministic autodiff + conv1d/GRU/attention/layernorm/softmax kernels is the recorded request.
3. **AE library-emission test**: `nn_to_candidate`'s `Autoencoder` arm (`predict_ae` over the F-dim last-step) is correct-by-construction + wired but not exercised end-to-end in the capstone (covered at unit level by `LearnAutoencoderAlpha`). A light AE-candidate admit test is a follow-up.
4. **Full GKX conditional-autoencoder** (characteristics-conditioned beta-net), a learned attention/AE feature feeding S3's datafield family, an intraday-window sequence alpha once S6.4 lands, and a multithreaded-deterministic GEMM (MKL CBWR) if scale demands it (§0.10) — all recorded refinements.

## Close gate + merge (user's gate)

Full **un-excluded** `ctest --preset dev` (incl. the slow MHO suites excluded per-unit during dev) run once at close as the regression gate: **1444/1444 engine tests PASS (414 s, MHO suites RiskMultiHorizon/FundMetaBook/FundSleeve included).** (The only ctest non-passes are the `atx-core-tests_NOT_BUILT` / `atx-tsdb-tests_NOT_BUILT` placeholder targets — not built in this engine worktree; atx-core/atx-tsdb are read-only and untouched.) Merge to `main` (`--no-ff`) is the **user's** decision — this branch is NOT auto-merged or pushed.
