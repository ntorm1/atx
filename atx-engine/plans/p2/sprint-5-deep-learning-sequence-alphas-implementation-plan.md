# Sprint S5 (p2) — Deep-Learning Sequence Alphas — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax. This is the
> FROZEN *how*; the **what** is the S5 section of [`ROADMAP.md`](ROADMAP.md#s5--deep-learning-sequence-alphas--proposed)
> (this module embeds the S5 spec in the ROADMAP — there is no separate `sprint-5-…​.md` spec file). On conflict,
> **§0 (this plan's as-built amendment) overrides** the ROADMAP sketch.

**Goal:** Add a **richer signal family** — neural *sequence* alphas — to the as-built `p1` S5 learned-signal layer
(`atx::engine::learn`: ridge/elastic-net/GBT/HMM over a PIT feature matrix, deflated-gated). S5 builds a **deterministic,
CPU-only neural substrate** — a minimal pluggable `Module`/`Layer`/`Optimizer`/`Loss` framework (the "abstract framework
to support a wide array of models" the kickoff mandates) — and ships four concrete tiny architectures over it:
**temporal-convolution (TCN)**, **GRU-lite (minimal gated unit)**, **attention-lite (single-head, short-window)**, and an
**autoencoder statistical-factor** alpha (Gu-Kelly-Xiu heritage). Each emits a per-instrument cross-section that **plugs
into the library exactly like a formulaic alpha** and is admitted **only** through `p1` S1's deflated-Sharpe / PBO gate
**and** the `p2` S4 robustness battery. The load-bearing guarantees: **(determinism)** same seed ⇒ byte-identical weights
+ alpha digest — the determinism invariant's hardest stress point; **(no look-ahead)** every sequence window is
trailing-only and every net is trained on a trailing CPCV fold and applied OOS — truncation-invariance is the test; and
**(boundary pin)** a linear single-layer net reduces to the proven `p1` S5 linear alpha, and a linear autoencoder reduces
to atx-core PCA — the regression anchors against the layers S5 generalizes. **THERE IS NO GPU:** every kernel is a
single-threaded order-fixed scalar reduction over `core::linalg`, tiny by construction (16–32 channels, 2–4 layers),
trained for a **fixed epoch budget** (no wall-clock / patience early-exit).

**Architecture:** S5 extends the existing `learn/` layer rather than forking it. A new **NN framework** sub-namespace
`atx::engine::learn::nn` holds the model-agnostic substrate — `Tensor`/`Seq3` (the `(batch×time×feature)` view the layer
absent today, §0.5), a `Module` base with **hand-written forward + backward** (no autodiff library — a small reverse-mode
tape or per-layer adjoints, §0.4), the layer zoo (`Conv1dCausal`/TCN-residual-block, `GruCell`/MGU, `Attention1Head`,
`Linear`, `Dropout`, `LayerNorm`, activations), a fixed-epoch `Optimizer` (SGD-momentum / Adam), `Loss` (MSE/Huber/IC),
and the **single deterministic `Trainer` loop** (seeded init → fixed-order minibatch → forward → loss → backward → step →
deterministic checkpoint-at-best-validation, plus the GKX **seed-ensemble**). The **alpha-facing** types live in
`atx::engine::learn` alongside the existing learners: `sequence_features.{hpp,cpp}` (the PIT temporal-window tensor builder
over the existing `FeatureMatrix`), `tcn_alpha.{hpp,cpp}` (TCN + GRU-lite), `autoencoder_alpha.{hpp,cpp}` (attention-lite +
autoencoder factor), and `nn_source.{hpp,cpp}` (the `LearnedModel`/`ModelKind` extension + the `SeqLearnedSignalSource`
lookback-window live adapter). The meta-book (S2) and combiner consume an NN alpha through the **same** `ISignalSource` and
library-admit seams as any other alpha — S5 adds a signal *family*, not a new pipeline.

**Tech Stack:** C++20; **header (public API + small inline contracts) + `.cpp` source for every non-trivial private
implementation** — matching the **as-built `learn/` layer** (header in `include/atx/engine/learn/`, source in
`src/learn/`, listed in `atx-engine/CMakeLists.txt`) and the kickoff directive ("use cpp source files for private
implementations"). The NN kernels (conv/GRU/attention forward+backward, the trainer, the autoencoder) are heavy private
implementations ⇒ `.cpp`; this also keeps the pinned-FP scalar hot loops in **one TU** (§0.10). Namespaces
`atx::engine::learn` (alpha types) + `atx::engine::learn::nn` (the framework). Reuses
`learn::{FeatureMatrix, FeatureSpec, build_features, LearnedModel, ModelKind, build_augmented_row, predict_blended,
predict_at, oos_deflated_sharpe, oos_ic, LearnedSignalSource, seed_for, TrialCounter, date_label_spans, expand_date_folds,
stack_to_candidate}`, `eval::{deflated_sharpe, pbo, cpcv, compute_return_metrics}`,
`combine::{compute_metrics, CombinedSignalSource}`, `library::{Library, AlphaCandidate, Provenance, AlphaGate}`,
`atx::core::{Xoshiro256pp, linalg (MatX/VecX, ridge, pca, transform, symmetric_eig, solve_spd), hash_bytes, Result,
Status, ErrorCode}`. GoogleTest (`atx-engine/tests/*_test.cpp`, CONFIGURE_DEPENDS — no per-unit CMake edit; the new
`src/learn/**/*.cpp` ARE appended to `atx-engine/CMakeLists.txt`, the one permitted shared-engine edit). clang-cl
`/W4 /permissive- /WX` (`ATX_WERROR=ON`, default). **There is NO `/fp:precise` flag in the tree (§0.10)** — determinism
rests on **single-thread order-fixed scalar reductions**, not a compiler FP mode. Build + ctest are the gates; clang-tidy
disabled (noise).

---

## §0 — As-built reconciliation amendment (the ML-framework code review)

> **Recon target (kickoff):** the merged `p1` S1–S8 engine **+ `p2` S1/S2/S3/S4**. This sprint is cut from `main` after
> S4 merge (S5 consumes S4's robustness battery, the ROADMAP hard dependency). Reconnaissance against the as-built
> `learn/` ML layer + the `eval/` gate + the atx-core L7 numeric substrate surfaces the load-bearing corrections below;
> each changes a unit's scope. **Run the recon as the first act of S5-0** and amend these notes against the *actual*
> merged SHAs (file:line cited below are from the 2026-06-14 review pass) before dispatching S5.1. **This §0 IS the
> requested ML-framework code review** — folded into the plan as the as-built reconciliation, exactly as the S1/S2 plans
> did.

### 0.1 The `p1` S5 ML layer is fully as-built — S5 EXTENDS `atx::engine::learn`, it does not greenfield a new layer
Header + source for every `p1` S5 unit exists under `include/atx/engine/learn/` + `src/learn/`: `feature_matrix.{hpp,cpp}`
(the PIT `(date×instrument)×feature` matrix + multi-horizon forward-return labels), `elastic_net.{hpp,cpp}` (RNG-free
cyclic coordinate descent L1+L2), `gbt.{hpp,cpp}` (deterministic histogram GBT), `hmm.{hpp,cpp}` (log-space Baum-Welch),
`latent.{hpp,cpp}` (PIT PCA latent factors), `linear_alpha.{hpp,cpp}` (walk-forward CPCV trainer + the gate),
`learned_source.{hpp,cpp}` (the `LearnedModel`/`ModelKind` value type + the `ISignalSource` adapter), `ensemble.{hpp,cpp}`
(the deflation-gated stacking combiner + the **library-admit bridge** `stack_to_candidate`), `train.{hpp,cpp}` (the
seed/CPCV scaffold), `pipeline.{hpp,cpp}` (E2E wiring + `full_pipeline_digest`). **Decision:** S5 adds the NN substrate
(`learn/nn/*`) + four NN alpha types + the `ModelKind` extension; **nothing existing is rewritten.** The plan-doc naming
`combine::ISignalSource` is **stale** — the real seam is `atx::engine::ISignalSource` (`loop/signal_source.hpp:103`).

### 0.2 The model abstraction is a **tagged-union value type + free-function dispatch**, NOT an `ILearnedModel` vtable
The deployed model is `struct LearnedModel` (`learned_source.hpp:182`) carrying `ModelKind kind`
(`enum class ModelKind : … { Linear, Gbt }`, `learned_source.hpp:86`), per-horizon params, `blend_w`, standardization
stats, a `LatentAugmentation`, `trial_count`, and the frozen `oos_score_series`. The predict seam is
`predict_blended(const LearnedModel&, std::span<const f64> augmented_row)` (`learned_source.hpp:279`) — an **exhaustive
`switch` on `kind` with NO `default`**, so adding a family is mechanical and the compiler flags every unhandled dispatch.
The fit seam is the free functions `fit_linear` (`linear_alpha.hpp:154`) and `fit_gbt` (`gbt.hpp:277`), both returning
`LearnedModel` and both filling `oos_score_series`/`trial_count` so the **one** gate `oos_deflated_sharpe(model, fm)`
(`linear_alpha.hpp:205`) scores any kind uniformly. **Decision (the pluggability contract, R5):** an NN family adds (a) a
`ModelKind::Tcn`/`Gru`/`Attn`/`Autoencoder` enumerator, (b) an NN parameter payload to `LearnedModel` (parallel to
`coeffs`/`forests` — a serialized `nn::ModuleState`), (c) a `case` to `predict_blended`, (d) a
`fit_nn(fm, seq, aug, cfg) -> LearnedModel` that fills `oos_score_series`/`trial_count`. It then **inherits**
`oos_deflated_sharpe`, `predict_at`, the horizon blend, and `LearnedSignalSource` for free. This is the low-friction
extension the abstraction was designed for ("the Linear arm is the first of several").

### 0.3 The deflation gate, CPCV scaffold, and library-admit bridge are MODEL-KIND-AGNOSTIC — reuse verbatim
`oos_deflated_sharpe` (`linear_alpha.hpp:205`) → `eval::deflated_sharpe(sr, T, skew, exkurt, N=trial_count, var)`
(`eval/deflated_sharpe.hpp:136`); the CPCV firewall is `date_label_spans` + `expand_date_folds` + `TrialCounter`
(`train.hpp:66`/`:79`); the library bridge is `stack_to_candidate` (`src/learn/ensemble.cpp:184`) — it synthesizes a
pnl/position stream via `predict_at`, scores with `combine::compute_metrics`, hashes via `core::hash_bytes`, sets
`Provenance{expr_source="learned:stack:<base>", seed}` (`library/record.hpp:138`), fills `library::AlphaCandidate`
(`library/library.hpp:90`), and admits via `Library::admit(cand, gate)` (`library/library.hpp:141`). **Decision:** S5's NN
alpha reuses **all** of this — the candidate-synthesis path is copied verbatim, changing only `expr_source`
(`"learned:tcn"` / `"learned:gru"` / `"learned:attn"` / `"learned:ae"`). **S5 adds NO new admission machinery** — the
PBO add (S5.4) is `eval::pbo` (already present), not a new gate. The **trial count N must include the architecture × seed
sweep** so the deflation is honest at NN scale (the single biggest snooping risk, §1A B6).

### 0.4 There is NO autodiff and NO tensor type anywhere — both are S5's foundational build items
A tree-wide inventory of atx-core (`linalg/`, `solve/`, `regression/`, `decompose/`) and atx-engine finds **dense 2-D
`MatX` / 1-D `VecX` (column-major f64) only** — no tape, no reverse-mode AD, no batched `(B×T×F)` tensor, no conv/GRU/
attention/optimizer kernel. **Decision:** the largest S5 build item is the **backward pass** — ship either a small
reverse-mode tape **or** hand-coded per-layer adjoints (the recommended path for ≤6 layer types: simpler, allocation-free,
and each adjoint is independently differential-tested, §1A B2). The `(B×T×F)` layout is a thin `nn::Seq3` **view** over
flat `MatX` storage (row-major-by-(batch,time), matching the existing `X[r*n_features+f]` convention in `FeatureMatrix`,
`feature_matrix.hpp:24`) — no owning tensor class is needed. **The dominant correctness risk of S5 is a wrong backward
pass; the finite-difference gradient check (R3) is the load-bearing guard.**

### 0.5 The live `LearnedSignalSource` is RAW-OHLCV-only and reads ONLY the current date — sequence models need a new seam
`LearnedSignalSource::evaluate(PanelView)` (`learned_source.hpp:339`) whitelists open/high/low/close/volume via
`panel_field_of` (`learned_source.hpp:374`) and reads **row 0 (the current date) only**; a spec needing `pool_alphas` is
rejected at construction (the live `PanelView` can't reconstruct stored streams). A sequence model needs a **trailing
window** of features. **Decision:** S5.5 ships a `SeqLearnedSignalSource` (or extends the adapter with `max_lookback > 0`)
that feeds a `lookback`-deep trailing slice of the `PanelView` into the sequence model — a **new, explicitly-gated seam**
that still honors the M7 zero-hot-path-alloc rule (pre-sized window scratch in the ctor, like the existing
`base_`/`augmented_`/`latent_` buffers at `learned_source.hpp:423`). The window is **trailing by construction** (rows
`t-lookback+1 … t`, never `t+1`) — the look-ahead guard is structural (R2).

### 0.6 Determinism is `Xoshiro256pp` + `seed_for` + order-fixed scalar reductions — the NN substrate MUST mirror it exactly
The RNG is `atx::core::Xoshiro256pp` (xoshiro256++, `atx-core/include/atx/core/random.hpp:83`; `normal()` Box-Muller at
`:184`, `jump()` at `:237` for non-overlapping streams); seeds derive via `learn::seed_for(master, tag, a, b)`
(`train.hpp:79`) — a SplitMix mix of `(master_seed, string tag, a, b)`, **never** wall-clock/thread/address/map-order. The
existing learners are order-fixed: elastic-net RNG-free ascending coordinate sweeps; GBT sorted-copy bin edges +
first-max ties + ascending subsample; HMM log-space `logsumexp` with fixed iteration order. **Decision:** the NN trainer
seeds init/dropout/shuffle via `seed_for(master, "nn-init"/"nn-drop"/"nn-shuffle"/"nn-ensemble", layer/step/member, 0)`;
**every** reduction (matmul, conv, softmax, loss, gradient accumulation) is a **scalar ascending fold**, never
`simd::dot` — **`core/simd.hpp:24` documents SIMD reductions agree with a scalar fold only "to within a few ULPs, not
bit-for-bit,"** so any SIMD path that feeds the digest breaks R1. The fast path and the deterministic path diverge: S5
takes the deterministic one. **Box-Muller carries a cached spare across copies** (`random.hpp:176`); weight-init draws in
**one fixed order from one RNG instance** (the HMM `seeded_init` is the precedent), never a copied generator mid-pair.

### 0.7 Fixed-iteration vs early-stopping is the central training tension — resolved as deterministic checkpoint-at-best
The existing learners cap iterations with a deterministic `tol` exit; an NN that "stops when val-loss plateaus" would be
data-dependent in its **stopping step** and could vary build-to-build. **Decision (R1/R2):** the trainer runs a **fixed
epoch budget**, evaluates validation IC/loss at **fixed checkpoints**, and deterministically **selects** the
best-validation checkpoint (`argmin` over the trailing validation fold — a pure function of the data ⇒ byte-reproducible).
This is "restore-best-weights" early stopping made deterministic by fixing the schedule (§1A B6). The validation fold is
**trailing** (inside the training window, never the test block, R2). The epoch budget + checkpoint cadence are frozen
hyperparameters (pre-tuned once via walk-forward, recorded in the config).

### 0.8 The GKX seed-ensemble is a first-class framework dimension, not an afterthought
Small-sample NN prediction variance is the dominant overfitting failure; the single highest-leverage fix is a **seed
ensemble** (Gu-Kelly-Xiu average **5 seeds**, §1A B5). **Decision:** the `Trainer` takes an `ensemble_size` (default 5);
member `m` trains from `seed_for(master, "nn-ensemble", m, 0)`; predictions are the **order-fixed ascending-member mean**
(deterministic). The ensemble is part of the `LearnedModel` NN payload (it stores `ensemble_size` member states). This is
both the variance reducer AND a determinism surface (the mean is a fixed-order reduction).

### 0.9 The autoencoder is a FACTOR extractor (orthogonal to the predictive track), pinned to atx-core PCA
The autoencoder (S5.3) is **not** a direct alpha predictor — it extracts **latent statistical factors** from the returns
panel (Gu-Kelly-Xiu conditional-autoencoder heritage, §1A B4); its output is a *feature*/factor block that augments the
predictive track (or seeds a sleeve's risk model). **Decision:** the autoencoder is trained on a **trailing window**,
frozen, and applied OOS (rolling re-fit, R2). **Boundary pin (R7):** a **linear** autoencoder (linear encoder/decoder, MSE
reconstruction, no nonlinearity) recovers the PCA subspace — differential-tested against atx-core `pca()` (`pca.hpp:51`) +
`transform()` (`:101`) on the explained-variance / subspace match. This pins the AE to the proven PCA the existing
`latent.hpp` already uses.

### 0.10 No `/fp:precise` / `-ffp-contract` flag exists — NN determinism is single-thread order-fixed reductions only
The root `CMakeLists.txt` sets `/W4 /permissive-` (+`/WX` when `ATX_WERROR=ON`) but **no** `/fp:precise` /
`-ffp-contract=off` (tree-wide grep; the same gap S1/S2 recorded). **Consequence:** NN determinism (R1) rests **entirely**
on (a) **single-thread math** (no multithreaded BLAS reduction order — never link a threaded GEMM into a digest path; the
nets are tiny so a scalar/blocked in-house GEMM suffices, §1A B1), (b) **order-fixed ascending scalar folds** in every
reduction (no `simd::dot`, §0.6), and (c) **avoiding FMA contraction / fast-math** in the accumulation (decide the FMA
policy once and keep it consistent across builds — the key is build-to-build reproducibility, §1A B1). Every S5 kernel is
`-Wconversion`-clean (explicit `static_cast` at every `size_t`↔`Eigen::Index` boundary, the linalg convention).

> **Net scope shift vs the ROADMAP sketch:** S5 is **additive and compositional** over the as-built `learn/` layer — it
> extends the `LearnedModel`/`ModelKind` value type (§0.2), reuses the deflation gate + CPCV scaffold + library-admit
> bridge verbatim (§0.3), and pins to the proven linear learner + atx-core PCA (§0.7/§0.9). The genuinely-new build is the
> **deterministic CPU NN substrate** (the `Module`/`Layer`/`Optimizer`/`Loss`/`Trainer` framework + four tiny
> architectures + the backward passes, §0.4) and the **sequence-feature tensor builder** (§0.5) + the **lookback live
> adapter** (§0.5). The dominant correctness risks are a **wrong backward pass** (no autodiff §0.4 — finite-difference
> gradient check R3), a **non-deterministic training run** (multithread/SIMD/Box-Muller-order §0.6/§0.10 — R1), a
> **look-ahead-leaking window or validation fold** (§0.5/§0.7 — R2), and an **un-deflated NN alpha** that snoops the
> architecture/seed sweep (§0.3 — R4). Each is a named gate in §3.

---

## §1 — Research foundation: the deep-learning-alpha design rules (with citations)

Derived from the research north-stars (`renaissance-technologies-systems-deep-dive.md` §3.2/§4.2 statistical-sequence /
noisy-channel heritage, `worldquant-systems-deep-dive.md` §3.2 the millions-of-weak-alphas frontier), the `p1` S5
learned-signal precedent, the `p2` S4 robustness battery, and the carried-forward `p0`/`p1` invariants. **Non-negotiable**;
every S5 unit is checked against them.

| # | Rule | Why / source |
|---|------|--------------|
| **R1** | **Deterministic training — seeded, single-thread, order-fixed, fixed-epoch.** Init/dropout/shuffle/ensemble RNG via `Xoshiro256pp`+`seed_for`; every reduction a single-thread ascending scalar fold (no `simd::dot`, no threaded BLAS); a **fixed epoch budget** with deterministic checkpoint-at-best (no wall-clock/patience exit); same seed ⇒ byte-identical weights + alpha digest. | `p1` invariant #1; §0.6/§0.7/§0.10; the SIMD-not-bit-reproducible warning (`simd.hpp:24`); RepDL fixed-summation determinism; the CPU-multithreading-nondeterminism study (§1A B1). The seed is part of the recorded artifact. |
| **R2** | **No look-ahead — windows + fits are trailing, applied OOS.** Every sequence window ends at `t` (rows `t-L+1…t`, never `t+1`); each net trains on a **trailing CPCV fold** + a **trailing** validation fold and predicts OOS only; the autoencoder fits a trailing window and encodes OOS (rolling re-fit). **Truncation-invariance at every fitted boundary is the test.** | `p1` invariant #2/#4; the fit/apply firewall; the `p1` S5 walk-forward CPCV precedent (`expand_date_folds`); §0.5/§0.7; GKX rolling-window estimation (§1A B4). |
| **R3** | **Differential correctness of every kernel — the gradient check is load-bearing.** Every layer's backward is verified by **central finite-difference** gradient check (relative error ≤ ~1e-5 on f64); every forward (conv/GRU/attention/AE) is checked vs an obviously-correct naive reference; the optimizer step + loss + softmax each ship a reference. **The wrong-backward-pass is S5's #1 risk (no autodiff, §0.4).** | `p1` invariant #7; the `oracle == VM` / GBT / HMM differential-test precedent; standard autodiff verification (finite differences); §0.4. |
| **R4** | **Deflation-gated admission — the architecture × seed sweep is in the trial count.** An NN alpha is admitted **only** through `oos_deflated_sharpe` (reused verbatim) **and** `eval::pbo`; the deflation trial count N includes **every** architecture/hyperparameter/seed trial run (not just the winner) so the multiple-testing correction is honest at NN scale; PBO > ~20–30% ⇒ reject. No NN alpha bypasses the gate. | `p1` invariant (robustness gate); `p1` S1 deflated-Sharpe/PBO; the `p1` anti-roadmap #7 "ML overfits at this N → gate, don't ignore"; Bailey-López de Prado DSR/PBO (§1A B6). |
| **R5** | **One pluggable framework — a new architecture is new Layers, not new training code.** A minimal `Module`/`Layer`/`Optimizer`/`Loss` decomposition with ordered `params()`/`grads()` views; one `Trainer` loop drives every model; a new architecture composes existing `Layer`s (TCN = `Sequential` of `Residual(Conv→Norm→Act→Dropout)²`). The alpha extends `LearnedModel`/`ModelKind` and inherits predict/gate/blend (§0.2). | The kickoff "abstract framework, wide array of models" mandate; the convergent PyTorch `nn.Module` / tiny-dnn / mlpack / MiniDNN decomposition (§1A B2). |
| **R6** | **CPU-only and efficient — tiny nets, single-thread scalar kernels, zero hot-path alloc.** No GPU, no GPU dependency. Architectures are tiny by construction (TCN: 2–4 blocks, k=3, dilations {1,2,4,8}, 16–32 channels; GRU-lite: 1 layer 16–32 hidden; attention: 1 head; AE: K=1–6 factors, 32→16→8). `evaluate()` allocates zero (pre-sized window + activation scratch); training (cold) may allocate. | `p1` invariant #6; the kickoff CPU-only constraint; the short-window-attention-is-already-cheap finding (§1A B3); GKX tiny-AE sizes (§1A B4). |
| **R7** | **Reduce to / pin against the proven layer.** A **linear** single-`Linear`-layer + identity-activation + MSE net, full-batch fixed-iteration GD, reduces to the `p1` S5 ridge/elastic-net linear alpha (to tolerance; bit-for-bit in the closed-form ridge limit); a **linear** autoencoder recovers the atx-core `pca()` subspace. The generalization is pinned to the proven `p1` S5 + PCA layers. | `module.md` carry-forward discipline; the S1→S7 / S2→S1 boundary-pin precedent; §0.7/§0.9. |
| **R8** | **Small-sample NN honesty — heavy regularization + seed ensemble + the S4 battery.** Every NN alpha ships with heavy regularization (dropout, L1/L2/weight-norm, small capacity) and a **seed ensemble** (default 5, §0.8); and must **also pass the `p2` S4 robustness battery** (synthetic-recovery, regime/walk-forward survival, the sealed lockbox) like any other alpha. | `p1` anti-roadmap #7; GKX 5-seed ensemble + L1=1e-4 (§1A B4/B5); the `p2` S4 battery (the ROADMAP S5 hard dependency); seed-ensemble variance reduction (§1A B5). |

**One-sentence thesis:** *`p1` S5 already proves a learned alpha can be PIT-featured, walk-forward-fit, deflation-gated,
and admitted to the library through a model-kind-agnostic value type — so S5 is the layer that (a) builds a **deterministic
CPU-only NN substrate** (a pluggable `Module`/`Layer`/`Optimizer`/`Loss` framework with hand-written backward passes,
R3/R5/R6), (b) ships four tiny **sequence/factor architectures** over it (TCN, GRU-lite, attention-lite, autoencoder),
each (c) trained **trailing-only** and applied OOS (R2), (d) **deflation- + PBO-gated** with the architecture/seed sweep in
the trial count (R4) and (e) **S4-robustness-gated** (R8); pinned bit-for-bit to the proven linear learner + atx-core PCA
on the linear boundary (R7); the only genuinely-new correctness risks are the backward pass's gradient correctness, the
training run's byte-determinism, the window/fold's look-ahead safety, and the gate's honesty at NN trial scale.*

---

## §1A — State-of-the-art research grounding (verified literature)

> Sourced via a web-research pass (CPU-efficient sequence architectures + CPU NN determinism + the pluggable-framework
> decomposition + autoencoder asset pricing + small-sample gating, 2026-06-14) with per-citation VERIFIED/UNVERIFIED
> tagging. Primary sources confirmed against arXiv / DOI / publisher where marked **VERIFIED**; **UNVERIFIED** = a source
> whose existence is confirmed but whose primary PDF did not render as text this pass (claim corroborated from the
> abstract / a readable replication) — recorded so the doc does not propagate it as fact. Full citations in **References**.

### Track A — CPU-efficient sequence architectures (the *what* of the four models)

**A1 — TCN is the recommended default (cheapest + most small-data-robust on CPU).** [grounds R6, §4.3] **[VERIFIED]**
A TCN is dilated **causal** 1-D convolution + residual blocks + weight-norm + dropout. It is **fully parallel** (no
sequential recurrence → no BPTT vanish/explode → cheap, stable CPU training), parameter-efficient, and its reductions are
trivially order-fixable. Bai-Kolter-Koltun show a generic TCN **matches or beats LSTM/GRU across a broad range of sequence
tasks** and recommend it as the *starting point* for sequence modeling. Tiny ship size: 2–4 residual blocks, kernel 3,
dilations {1,2,4,8} (≈16–31-step receptive field), 16–32 channels. *— Bai, Kolter, Koltun (2018). **The §4.3 default; the
causal dilation gives the R2 look-ahead guard structurally (a causal conv cannot read `t+1`).***

**A2 — GRU-lite / Minimal Gated Unit (the path-dependent alternative).** [grounds R6, §4.3] **[VERIFIED]**
The MGU merges the GRU update+reset into a **single** forget gate — comparable accuracy to a GRU with fewer params and
faster training; "slim" variants cut to ~50% of MGU params. Good when state/path-dependence matters more than parallelism;
downside is the sequential recurrence (the slowest CPU pattern). Tiny: 1 layer, 16–32 hidden, single gate. *— Zhou-Wu
(2016); slim-MGU (2017). **The §4.3 second architecture; the backward is BPTT-through-the-window, the hardest gradient
check (R3).***

**A3 — Attention-lite: plain single-head O(L²) is already cheap for short windows; skip linear attention.** [grounds R6, §4.4] **[VERIFIED]**
For short lookback windows (L≈20–60) plain single-head dot-product attention is **already cheap** (60²≈3600 scores — negligible
on CPU); linear/Performer attention only pays off at long L and adds constant overhead that makes it *slower* for short
sequences. *— Shen et al. (2018) efficient attention; short-seq linear-attention overhead analysis. **The §4.4
`Attention1Head` is plain O(L²) single-head over the window; linear attention is explicitly NOT shipped (windows are
short, §4.4).***

**A4 — Massive transformers are the wrong tool for small panels — favor tiny TCN/GRU.** [grounds R6/R8] **[VERIFIED]**
The small-tabular evidence (GBT-beats-DL on small tables; foundation models like TabPFN are pretrained, not
deterministic-from-scratch and not CPU-trivial to retrain) reinforces that large transformers overfit small financial
panels. *— TabPFN (Nature 2024, benchmark only — out of scope as it is not deterministic-train). **The §4.x sizes are tiny
by mandate (R6); TabPFN is recorded as a benchmark, not a dependency.***

### Track B — CPU NN mechanics (the *how* of determinism / framework / autoencoder / gating)

**B1 — Byte-reproducible CPU NN training (the determinism playbook).** [grounds R1, §0.6/§0.10] **[VERIFIED + UNVERIFIED-primary]**
Sources of CPU nondeterminism: (1) **reduction order** (FP add is non-associative; threaded GEMM sums in scheduler order),
(2) **atomic accumulation**, (3) **FMA/fast-math contraction**, (4) **libm differences**, (5) **runtime kernel
autotuning**, (6) **RNG** (init/dropout/shuffle). Fixes: **single-thread the math** (a multithreading study found single-/
fixed-thread runs reproduce exactly, varying thread count breaks it **[UNVERIFIED at primary PDF — corroborated from the
abstract]**); **fix the reduction order** in your own kernels (sequential or pairwise/Kahan summation, structure
independent of scheduling — the RepDL approach); **pin FP semantics** (avoid fast-math; keep FMA policy consistent across
builds); **order-fixed minibatches** from a counter-based seeded RNG; **deterministic RNG** per purpose; **no runtime algo
autotuning**; **verify** by hashing weights across two runs. *— RepDL (2025); Sui et al. ISSRE'21; Intel MKL CNR/CBWR
(KMP_DETERMINISTIC_REDUCTION, MKL_DYNAMIC=FALSE, ~2× cost) if a threaded BLAS is ever linked; PyTorch reproducibility
notes. **The §0.6/§0.10 decisions + the R1 two-builds-equal test; S5 hand-rolls scalar kernels precisely so the reduction
order is owned, not delegated.***

**B2 — The pluggable framework decomposition (Module/Layer/Optimizer/Loss).** [grounds R5, §4.2] **[VERIFIED]**
PyTorch `nn.Module`, tiny-dnn, mlpack ann, and MiniDNN all converge on the same minimal decomposition: a **`Module`** base
(recursive; `forward(x)`, `backward(grad_out)`, ordered `params()`/`grads()`, `state_dict`, `train(bool)`); concrete
**`Layer`s** (`Conv1dCausal`, `GruCell`/MGU, `Attention1Head`, `Linear`, `Dropout`, `LayerNorm`, activations) composed via
`Sequential`/`Residual`; an **`Optimizer`** base (`step(params, grads)` over the ordered views — SGD-momentum, Adam); a
**`Loss`** base (`value`/`grad` — MSE, Huber, IC). One training substrate drives every model; a new architecture = new
Layers, not new training code. *— PyTorch nn.Module C++ docs; mlpack 4; tiny-dnn; MiniDNN (Eigen-only reference).
**The §4.2 `nn::` framework — the "wide array of models" mandate realized as one substrate; backward is hand-written
per-Layer adjoints (no autodiff dependency, §0.4).***

**B3 — Build, don't pull a DL runtime — own the reduction order.** [grounds R1/R6, §0.10] **[VERIFIED]**
Comparison: libtorch-CPU (BSD; ~1GB; determinism only *conditional*, rides MKL/oneDNN reduction order you can't fully
pin), ONNX Runtime CPU (MIT; **inference-only, no training**; EP reproducibility not guaranteed), ggml (MIT; quantized-LLM
inference, wrong workload), tiny-dnn / MiniDNN (header-only references). **Recommendation: hand-roll the layers over
`core::linalg`/Eigen, single-threaded by default** — the nets are tiny so MKL-class GEMM is unnecessary, training is
required (rules out ONNX/ggml), and **owning the reduction order is the only path to true byte-determinism**. Use
tiny-dnn/MiniDNN as design references, not runtime deps. *— libtorch/ONNX/ggml/tiny-dnn/MiniDNN survey. **The §0.10 +
§2.1 decision: engine-local framework now; an atx-core L7 NN-kernel lift recorded.***

**B4 — Autoencoder statistical-factor models (CPU-feasible, look-ahead-safe).** [grounds R7/R8, §4.5] **[VERIFIED + UNVERIFIED-primary]**
The Gu-Kelly-Xiu **conditional autoencoder** is the template: a **beta network** maps firm characteristics → factor
loadings, a **factor network** compresses the return cross-section → K latent factors, loss
`Σ(rᵢ,ₜ − βᵢ,ₜ₋₁ fₜ)² + L1(1e-4)`. Tiny sizes: K=1–6 factors; beta-net depth CA0(linear)/CA1(32)/CA2(32→16)/CA3(32→16→8);
batch-norm (0.99); **5-seed ensemble averaged**. Look-ahead avoidance: **rolling/recursive estimation** (initial train,
rolling validation, rolling 1-yr test, expand + re-fit) — betas use `t−1` characteristics, factors fit in-sample within
the trailing window, nets applied OOS; **never fit the AE on the test block**. *— Gu, Kelly, Xiu (J. Econometrics 2021)
**[VERIFIED venue; exact arch from a readable replication, UNVERIFIED at the primary PDF]**. **The §4.5 autoencoder; the
linear-AE→PCA pin (R7, §0.9) and the rolling-window OOS protocol (R2).***

**B5 — Seed-ensemble variance reduction (the small-sample stabilizer).** [grounds R8, §0.8] **[VERIFIED]**
Different-seed NN ensembles are near-uncorrelated and substantially reduce prediction variance — the single
highest-leverage fix for small-sample NN instability; GKX average 5 seeds. *— seed-ensemble variance-reduction literature;
GKX 5-seed. **The §0.8 first-class `ensemble_size` (default 5); the ascending-member mean is a fixed-order reduction
(R1).***

**B6 — Small-sample NN gating discipline (deflation + PBO + walk-forward, fixed-budget early stop).** [grounds R4/R8] **[VERIFIED]**
Gate sequence: walk-forward / CPCV OOS → **PBO < ~20–30%** → **significant Deflated Sharpe** (deflated for the **trial
count**, sample length, skew/kurtosis) → only then promote; log the trial count so the multiple-testing correction is
honest. Reconcile fixed-iteration determinism with early stopping via **deterministic checkpoint-at-best-validation** (run
a fixed budget, select the best-val checkpoint — `argmin` is a pure function of the data). *— Bailey-López de Prado
(Deflated Sharpe, PBO); CPCV-superiority literature; deterministic restore-best-weights. **The §0.7 + R4 decisions; all
machinery (`eval::deflated_sharpe`/`pbo`/`cpcv`) already in `p1` S1 — reused verbatim (§0.3).***

**B7 — The S5 design synthesis.** S5 = a **hand-rolled deterministic CPU NN framework** (B2/B3, single-thread order-fixed
scalar kernels B1) + four **tiny** architectures (TCN default A1, GRU-lite A2, attention-lite A3 plain-single-head,
autoencoder factor B4) + **trailing-window** training applied OOS (R2, B4 rolling) + a **seed ensemble** (B5) + **deflation
+ PBO + S4-robustness** gating with the architecture/seed sweep in the trial count (B6, R4/R8). Pinned to the `p1` S5
linear learner + atx-core PCA on the linear boundary (R7). The recorded lift: a dedicated `core::linalg` NN-kernel /
autodiff tier (§2.1), with the engine-local framework shippable now.

---

## §2 — File structure

### 2.1 atx-core / Pattern-B requests (decided at kickoff)

> The engine adds no general-purpose primitive (project rule). S5 records the cross-module edge and ships on existing
> primitives + an engine-local framework, exactly as `p1` S1–S8 + `p2` S1–S4 did:
>
> 1. **L7 (or new L-tier) NN primitives → atx-core.** Deterministic autodiff (reverse-mode tape) + fixed-iteration seeded
>    SGD/Adam + conv1d/GRU/attention/layernorm/softmax kernels, single-thread order-fixed. Ship on the **engine-local
>    `learn::nn` framework** (hand-written per-layer adjoints, S5.2/S5.3); the dedicated `core::linalg` NN tier is the
>    recorded lift. Engine fallback is shippable now and bit-deterministic (the nets are tiny ⇒ scalar kernels suffice).
>    *(This is the ROADMAP cross-module table S5.2 row.)*
> 2. **`Xoshiro256pp` / `seed_for` / `pca` / `transform` / `ridge` / `symmetric_eig` / `solve_spd` / `hash_bytes` →
>    already in atx-core (L7) + `learn/`.** Reuse verbatim for init/ensemble seeding, the linear-AE→PCA pin, the linear
>    boundary pin, and the digest — no request.
> 3. **`eval::{deflated_sharpe, pbo, cpcv}`, `combine::compute_metrics`, the CPCV scaffold, `stack_to_candidate` →
>    already in `p1` S1 + `p1` S5.** Reuse verbatim (§0.3) — no request.

### 2.2 Engine files (this sprint builds these — `learn/nn/*` framework + `learn/*` alpha types)

| File | Responsibility | Unit |
|---|---|---|
| `include/atx/engine/learn/nn/tensor.hpp` | `Seq3` — a `(batch×time×feature)` non-owning view over flat `MatX` storage (row-major-by-(batch,time), §0.4); stride helpers; no owning tensor class | S5-1 |
| `include/atx/engine/learn/sequence_features.hpp` + `src/learn/sequence_features.cpp` | `SeqFeatureSpec`, `SequenceTensor`, `build_sequences(FeatureMatrix, spec)` — PIT **trailing** temporal windows (rows `t-L+1…t`, never `t+1`, §0.5/R2) over the existing `FeatureMatrix`; multi-horizon labels carried through | S5-1 |
| `include/atx/engine/learn/nn/module.hpp` | `Module` base (`forward`/`backward`/ordered `params()`/`grads()`/`state_dict`/`train(bool)`), `Sequential`, `Residual` — the pluggable substrate (R5, §4.2); hand-written adjoints (no autodiff, §0.4) | S5-2 |
| `include/atx/engine/learn/nn/layers.hpp` + `src/learn/nn/layers.cpp` | `Conv1dCausal` (dilation), TCN `ResidualBlock`, `GruCell`/`MguCell`, `Attention1Head`, `Linear`, `Dropout` (seeded mask), `LayerNorm`, activations — each forward + backward, order-fixed scalar (R1/R3/R6) | S5-2 / S5-3 |
| `include/atx/engine/learn/nn/optimizer.hpp` + `src/learn/nn/optimizer.cpp` | `Optimizer` base + `Sgd`(momentum) + `Adam` — `step(params, grads)` over the ordered views, fixed-iteration, order-fixed (R1/R5) | S5-2 |
| `include/atx/engine/learn/nn/loss.hpp` | `Loss` base + `MseLoss` / `HuberLoss` / `IcLoss` — `value`/`grad`, deterministic reductions (R1) | S5-2 |
| `include/atx/engine/learn/nn/trainer.hpp` + `src/learn/nn/trainer.cpp` | `TrainConfig` (fixed `epochs`, checkpoint cadence, `ensemble_size=5`), `Trainer` — seeded init → fixed-order minibatch → forward → loss → backward → step → **checkpoint-at-best-val** + **seed-ensemble** (R1/R8, §0.7/§0.8) | S5-2 |
| `include/atx/engine/learn/tcn_alpha.hpp` + `src/learn/tcn_alpha.cpp` | `TcnAlphaCfg`, `GruAlphaCfg`, `fit_tcn` / `fit_gru` — build the TCN / GRU-lite net, train (trailing CPCV), fill a `LearnedModel` NN payload + `oos_score_series`/`trial_count` (§0.2/R2/R7) | S5-2 |
| `include/atx/engine/learn/autoencoder_alpha.hpp` + `src/learn/autoencoder_alpha.cpp` | `AeFactorCfg`, `AttnAlphaCfg`, `fit_autoencoder_factors` (GKX-style, trailing-fit/OOS-encode, linear-AE→PCA pin §0.9/R7) + `fit_attn` (attention-lite alpha, §4.4) | S5-3 |
| `include/atx/engine/learn/nn_source.hpp` + `src/learn/nn_source.cpp` | `ModelKind::{Tcn,Gru,Attn,Autoencoder}` extension + the `predict_blended` cases + the NN payload in `LearnedModel`; `SeqLearnedSignalSource` (the `max_lookback` trailing-window live adapter, §0.5/R6); the `stack_to_candidate`-analog NN candidate bridge (§0.3) | S5-4 / S5-5 |

> **CMakeLists edit (the one permitted shared-engine touch):** append `src/learn/sequence_features.cpp`,
> `src/learn/nn/layers.cpp`, `src/learn/nn/optimizer.cpp`, `src/learn/nn/trainer.cpp`, `src/learn/tcn_alpha.cpp`,
> `src/learn/autoencoder_alpha.cpp`, `src/learn/nn_source.cpp` to the engine source list in
> `atx-engine/CMakeLists.txt` (alongside the existing `src/learn/*.cpp`). No other shared edit.

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`, CONFIGURE_DEPENDS)
`learn_sequence_features_test.cpp` (S5-1 + the trailing-window / truncation-invariance / multi-horizon-label proofs),
`learn_nn_substrate_test.cpp` (S5-2 — **the finite-difference gradient check for every layer**, the optimizer/loss
references, the R1 two-builds-byte-identical proof, the **linear-net→linear-alpha boundary pin R7**),
`learn_tcn_gru_alpha_test.cpp` (S5-2 — TCN + GRU-lite fit/predict, causal-no-look-ahead, seed-ensemble determinism),
`learn_autoencoder_alpha_test.cpp` (S5-3 — **the linear-AE→PCA pin R7**, attention-lite forward/backward, trailing-fit/
OOS-encode), `learn_nn_gate_test.cpp` (S5-4 — deflation + PBO with the architecture/seed sweep in the trial count, the
no-edge-rejected sanity), `learn_nn_source_integration_test.cpp` (S5-5, the determinism + look-ahead + gate + library-admit
+ combiner/sleeve capstone). Bench: `bench/nn_alpha_bench.cpp` (S5-5: alphas/sec for fit + predict across
`(architecture, L lookback, N universe, channels)`; the seed-ensemble cost; train vs predict split).

### 2.4 Ledger
`sprint-5-progress.md` (S5-0), updated per unit (copy the `p2` `sprint-2-progress.md` shape). S5 is **6 units incl.
marker** — within the 4–7 ceiling, **no split expected**; split **S5-a** (S5-0…S5-2: substrate + TCN/GRU) / **S5-b**
(S5-3…S5-5: AE/attention + gate + integration) only if S5.2 (the NN substrate) or S5.3 (the autoencoder) over-run, exactly
as `p1` S4/S7 did.

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

- **TDD:** failing GoogleTest first; `Suite_Condition_ExpectedResult`; cover happy path, **boundaries** (the linear-net →
  `p1` S5 linear-alpha reduction §0.7/R7; the linear-AE → atx-core `pca()` reduction §0.9/R7; a one-step window `L=1`
  reduces to the tabular `FeatureMatrix`; an all-NaN feature column → "no opinion" NaN out, no survivorship; an empty
  schedule / single-date fold; a zero-variance feature; a degenerate `ensemble_size=1`; the `s=0`/insufficient-history
  window fallback), and the **invariant proofs** (R1 two-builds-byte-identical weights+digest, R2 truncation-invariance at
  every fitted boundary, **R3 finite-difference gradient check on every layer's backward**, R4 deflation+PBO with the full
  trial count, R7 the two boundary pins).
- **Determinism (R1):** init/dropout/shuffle/ensemble RNG via `Xoshiro256pp` + `seed_for(master, "nn-…", …)` ONLY (no
  wall-clock/thread/address); **single-thread** math; **every reduction an ascending scalar fold** (NEVER `simd::dot` —
  `simd.hpp:24` is not bit-reproducible); a **fixed epoch budget** with deterministic checkpoint-at-best (NEVER a
  wall-clock/patience exit); Box-Muller init draws in one fixed order from one RNG instance (§0.6). A **two-builds-equal**
  test (same seed → byte-identical weights + alpha digest) is mandatory for S5-2 and S5-5. **Determinism is
  single-thread order-fixed scalar reductions, NOT `/fp:precise` (§0.10).**
- **No look-ahead / PIT (R2):** every sequence window ends at `t` (rows `t-L+1…t`, never `t+1`, structural for a causal
  conv); each net trains on a **trailing** CPCV fold + a **trailing** validation fold and predicts OOS only; the
  autoencoder fits a trailing window and encodes OOS (rolling re-fit). **Truncation-invariance is the test** at every
  fitted boundary. Insufficient-history windows emit NaN ("no opinion"), never a peek.
- **Differential correctness (R3):** every layer's backward passes a **central finite-difference gradient check** (rel.
  error ≤ ~1e-5 on f64); every forward (conv/GRU/attention/AE) is checked vs a naive reference; the optimizer + loss +
  softmax ship references. **The wrong-backward-pass is the #1 risk (no autodiff §0.4).**
- **Deflation-gated admission (R4):** an NN alpha is admitted ONLY through `oos_deflated_sharpe` (reused verbatim) AND
  `eval::pbo`; the trial count N includes EVERY architecture/hyperparameter/seed trial (not just the winner) — the
  honesty condition at NN scale; PBO > ~20–30% ⇒ reject. The NN alpha must ALSO pass the `p2` S4 robustness battery (R8).
- **One framework (R5):** a new architecture composes existing `nn::Layer`s and is driven by the ONE `Trainer` loop; it
  extends `LearnedModel`/`ModelKind` and inherits `predict_blended`/`oos_deflated_sharpe`/the blend/`LearnedSignalSource`
  (§0.2) — do NOT fork the training loop or the gate per architecture.
- **CPU-only + no hot-path alloc (R6 / `p1` #6):** NO GPU, no GPU dependency; tiny nets (R6 sizes); single-thread scalar
  kernels; `evaluate()` allocates zero (pre-sized window + activation scratch in the ctor, the `learned_source.hpp:423`
  precedent); training (cold) may allocate (documented).
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]`; `Result<T>`/`Status` for expected failures (dim
  mismatch, insufficient history, empty fold, singular reconstruction, NaN-only column); weakest sufficient types
  (`std::span`, `const&`, `std::string_view`); functions ≤ ~60 lines; **reuse `learn::{FeatureMatrix, LearnedModel,
  ModelKind, build_augmented_row, predict_blended, oos_deflated_sharpe, seed_for, expand_date_folds, stack_to_candidate}`,
  `eval::{deflated_sharpe, pbo, cpcv}`, `combine::compute_metrics`, atx-core `{Xoshiro256pp, pca, transform, ridge,
  symmetric_eig, solve_spd, hash_bytes}` — do NOT reinvent the feature matrix, the model value type, the gate, the CPCV
  scaffold, the library bridge, the RNG, or PCA**; the only new kernels are the NN framework + the four architectures +
  the sequence builder (the NN-kernel tier recorded as the Pattern-B lift).
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl, `ATX_WERROR=ON`). **No `/fp:precise` flag exists** (§0.10) —
  `-Wconversion`-clean (explicit `static_cast` at every `size_t`↔`Eigen::Index` boundary). clang-tidy disabled — the
  strict build + ctest are the gate.
- **clangd noise:** ignore squiggles; only a real `cmake --build` + ctest are the gates.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx): governed by .agents/cpp/agent.md (safety-critical-grade C++20) and
atx-engine/plans/docs/implementation-quality.md. THIS SPRINT EXTENDS the as-built p1 S5 ML layer (atx::engine::learn) —
do NOT rewrite it. Positive style/API references in-tree (REUSE, do NOT rebuild):
learn/learned_source.hpp (LearnedModel{ModelKind kind, per-horizon params, blend_w, oos_score_series, trial_count} +
predict_blended(model, augmented_row) = an EXHAUSTIVE switch on kind with NO default — ADD a ModelKind::{Tcn,Gru,Attn,
Autoencoder} enumerator + an NN payload + a case; build_augmented_row = the ONE train/eval feature layout); learn/
linear_alpha.hpp (fit_linear -> LearnedModel; oos_deflated_sharpe(model, fm) = the MODEL-KIND-AGNOSTIC gate — your
fit_nn fills oos_score_series/trial_count and inherits it; predict_at; the LINEAR boundary pin: a single-Linear-layer +
identity + MSE net reduces to THIS); learn/feature_matrix.hpp (FeatureMatrix = PIT (date x instrument) x feature +
multi-horizon labels — your sequence builder makes TRAILING windows over it, rows t-L+1..t, NEVER t+1); learn/train.hpp
(seed_for(master, tag, a, b) = the ONLY seed source — Xoshiro256pp, never wall-clock/thread; expand_date_folds +
date_label_spans + TrialCounter = the no-look-ahead CPCV firewall + the deflation N); src/learn/ensemble.cpp
stack_to_candidate (the library-admit bridge: predict_at -> compute_metrics -> hash_bytes -> Provenance{expr_source} ->
AlphaCandidate -> Library::admit — COPY it, change only expr_source to "learned:tcn"/etc); eval/deflated_sharpe.hpp +
eval/pbo (the gate, reused verbatim). atx-core (REUSE): Xoshiro256pp (random.hpp; normal() Box-Muller carries a cached
spare across copies — init draws in ONE fixed order from ONE instance), linalg MatX/VecX + ridge + pca + transform +
symmetric_eig + solve_spd (the linear-AE -> PCA pin), hash_bytes; Result<T>/Status/Ok/Err(ErrorCode, msg) (NO
Infeasible enumerator — use InvalidArgument + message). WARNING: core/simd.hpp dot/sum agree with a scalar fold only to a
few ULPs, NOT bit-for-bit — NEVER use simd::* in any path that feeds the determinism digest; use ascending scalar folds.

THERE IS NO GPU. Tiny nets only (TCN: 2-4 blocks, k=3, dilations {1,2,4,8}, 16-32 channels; GRU-lite: 1 layer 16-32
hidden; attention: 1 single head over a short window; AE: K=1-6 factors, 32->16->8). Hand-roll layers over core::linalg,
SINGLE-THREADED. NO autodiff library — write per-layer backward passes by hand.

THIS SPRINT'S DOMINANT RISKS: a WRONG BACKWARD PASS, a NON-DETERMINISTIC training run, a LOOK-AHEAD-LEAKING window/fold,
and an UN-DEFLATED NN alpha that snoops the architecture/seed sweep. The gates:
  - DETERMINISM (R1): seed init/dropout/shuffle/ensemble via seed_for ONLY; single-thread; EVERY reduction an ascending
    scalar fold (NEVER simd::dot); FIXED epoch budget + deterministic checkpoint-at-best-val (NEVER wall-clock/patience).
    Two builds of SAME seed => BYTE-IDENTICAL weights + alpha digest. Determinism is order-fixed scalar reductions, NOT a
    compiler fp flag.
  - NO LOOK-AHEAD (R2): windows end at t (rows t-L+1..t, never t+1 — structural for a causal conv); train on a TRAILING
    CPCV fold + a TRAILING validation fold, predict OOS only; AE fits a trailing window, encodes OOS. TRUNCATION-INVARIANT
    at every fitted boundary. Insufficient history => NaN, NEVER a peek.
  - GRADIENT CORRECTNESS (R3): EVERY layer backward passes a central finite-difference check (rel err <= ~1e-5, f64);
    every forward checked vs a naive reference. The wrong-backward-pass is the #1 risk (no autodiff).
  - DEFLATION + PBO (R4): admit ONLY through oos_deflated_sharpe AND eval::pbo; trial count N includes EVERY
    architecture/seed trial (not just the winner); PBO > ~20-30% => reject. Must ALSO pass the p2 S4 robustness battery.
  - REDUCE TO THE PROVEN LAYER (R7): a linear single-Linear+identity+MSE net == p1 S5 linear alpha (to tol; bit-for-bit in
    the ridge closed form); a linear autoencoder == atx-core pca() subspace. The load-bearing regressions.

PIT, NaN/delisted names get "no opinion" (NaN) and round-trip verbatim (no survivorship). Reuse ONE gate
(oos_deflated_sharpe + eval::pbo), ONE feature layout (build_augmented_row), ONE RNG (Xoshiro256pp via seed_for), ONE
library bridge (stack_to_candidate pattern). Header public API + .cpp private impl (match src/learn/*.cpp). Functions
<= ~60 lines. Build gate: cmake --build build --preset ninja --target atx-engine-tests (/W4 /permissive- /WX,
ATX_WERROR=ON) + ctest --preset ninja -R <Suite>.

Shared-branch discipline: stage EXPLICIT pathspecs only (git add -- <paths>; git commit -- <paths>); NEVER git add -A;
after committing run `git show HEAD --stat` (only your files); never touch atx-core/* or atx-tsdb/*; the ONLY shared
engine edit permitted is appending source lines to atx-engine/CMakeLists.txt; do not push.
End commit messages with: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## §4 — Architecture & algorithms (data structures + pseudocode)

### 4.1 Sequence-feature tensor builder — PIT trailing windows over `FeatureMatrix` (S5-1)

The bridge from the as-built tabular `FeatureMatrix` to the `(sample × time × feature)` layout a sequence model wants.
Each sample at `(date t, instrument i)` is the **trailing window** of that instrument's feature rows `t-L+1 … t` — never
`t+1` (R2). The multi-horizon forward-return labels carry through from `FeatureMatrix` unchanged.

```cpp
// section: learn/nn/tensor.hpp  (namespace atx::engine::learn::nn)
// A non-owning (batch × time × feature) view over flat row-major-by-(batch,time) storage (§0.4). No owning class.
struct Seq3 {
  std::span<const atx::f64> data;   // length = B*T*F, layout idx(b,t,f) = (b*T + t)*F + f
  atx::usize B, T, F;
  [[nodiscard]] atx::f64 at(atx::usize b, atx::usize t, atx::usize f) const noexcept {
    return data[(b * T + t) * F + f];   // ascending-index addressing (R1)
  }
};
```
```cpp
// section: learn/sequence_features.hpp  (namespace atx::engine::learn)
struct SeqFeatureSpec {
  atx::usize lookback = 32;          // L: the trailing window depth (R6: 20-60 typical)
  bool drop_incomplete = true;       // a window with < L history => emit NO sample (or NaN, "no opinion", R2)
};

struct SequenceTensor {
  std::vector<atx::f64> x;           // flat (n_samples × L × n_features), the Seq3 backing store
  std::vector<atx::f64> y;           // forward-return labels, one per (sample, horizon) — carried from FeatureMatrix
  std::vector<atx::usize> date_of, inst_of;   // sample provenance (for CPCV fold expansion + OOS emit, ascending)
  atx::usize n_samples = 0, lookback = 0, n_features = 0;
};

// Build trailing windows from the PIT FeatureMatrix. Sample (t,i) packs feature rows [t-L+1 .. t] of instrument i.
// TRAILING by construction (R2): the window NEVER reads a row > t. Order-fixed emit in ascending (date, instrument).
[[nodiscard]] atx::core::Result<SequenceTensor>
build_sequences(const FeatureMatrix& fm, const SeqFeatureSpec& spec);
```
**No look-ahead (R2):** the window is `[t-L+1, t]`; the S5-1 test truncates the underlying `FeatureMatrix` after date `t`
and asserts every sample with `date_of ≤ t` is **byte-identical** (truncation-invariance). **Boundary (`L=1`):** the
sequence tensor reduces to the tabular `FeatureMatrix` row-for-row (the degenerate-window pin). **Insufficient history:** a
`(t,i)` with `< L` available rows emits no sample (`drop_incomplete`) or a NaN-filled window ("no opinion"), never a
zero-padded peek into another instrument. **Reuse:** labels + the `(date,inst)` provenance come straight from
`FeatureMatrix` (`feature_matrix.hpp`); the builder adds **only** the windowing.

### 4.2 The NN framework — Module / Layer / Optimizer / Loss / Trainer (S5-2)

The pluggable substrate (R5). One `Module` base, hand-written forward + backward (no autodiff, §0.4), ordered param/grad
views (the determinism + optimizer contract), and one `Trainer` loop that drives every architecture. **The "abstract
framework to support a wide array of models" the kickoff mandates.**

```cpp
// section: learn/nn/module.hpp  (namespace atx::engine::learn::nn)
// The recursive unit. forward/backward are PURE functions of input + params; grads accumulate into a FIXED-ORDER buffer.
class Module {
public:
  virtual ~Module() = default;
  [[nodiscard]] virtual atx::core::linalg::MatX forward(const atx::core::linalg::MatX& x) = 0;  // cache acts for backward
  [[nodiscard]] virtual atx::core::linalg::MatX backward(const atx::core::linalg::MatX& grad_out) = 0;  // dL/dx
  virtual std::span<atx::f64> params() = 0;         // FLAT, ASCENDING order — the determinism + optimizer contract (R1)
  virtual std::span<atx::f64> grads()  = 0;         // parallel to params(), accumulated order-fixed
  virtual void train(bool on) noexcept { training_ = on; }   // toggles dropout/norm behavior
  virtual void state_to(std::vector<atx::f64>& out) const = 0;   // serialize (the LearnedModel NN payload, §0.2)
  virtual void state_from(std::span<const atx::f64> in) = 0;
protected:
  bool training_ = false;
};
// Sequential (ordered submodules) + Residual (y = f(x) + x) wrappers — TCN = Sequential of Residual(Conv->Norm->Act->Drop)^2.
```
```cpp
// section: learn/nn/optimizer.hpp  (namespace atx::engine::learn::nn)
class Optimizer {                       // step over the ORDERED params/grads views — order-fixed (R1)
public: virtual ~Optimizer() = default;
  virtual void step(std::span<atx::f64> params, std::span<const atx::f64> grads) = 0;
};
// Sgd{lr, momentum} and Adam{lr, b1, b2, eps} — fixed update, NO per-step RNG. Iterate params ascending.

// section: learn/nn/loss.hpp  (namespace atx::engine::learn::nn)
class Loss { public: virtual ~Loss() = default;
  [[nodiscard]] virtual atx::f64 value(const atx::core::linalg::MatX& pred, const atx::core::linalg::MatX& target) = 0;
  [[nodiscard]] virtual atx::core::linalg::MatX grad(const atx::core::linalg::MatX& pred,
                                                     const atx::core::linalg::MatX& target) = 0;   // dL/dpred
};   // MseLoss / HuberLoss / IcLoss — reductions are ascending scalar folds (R1)
```
```cpp
// section: learn/nn/trainer.hpp  (namespace atx::engine::learn::nn)
struct TrainConfig {
  atx::usize epochs        = 200;     // FIXED budget — NO wall-clock/patience exit (R1, §0.7)
  atx::usize batch_size    = 256;
  atx::usize ckpt_every    = 10;      // evaluate trailing-val + checkpoint at this cadence (deterministic select, §0.7)
  atx::usize ensemble_size = 5;       // GKX seed ensemble (§0.8/R8) — members averaged ascending
  atx::u64   master_seed   = 0;       // every draw via seed_for(master_seed, "nn-…", …) (R1, §0.6)
};

// Train ONE architecture deterministically. Returns the seed-ensemble of best-val checkpoints (the NN payload, §0.2).
// seeded init -> fixed-order minibatch (shuffle = pure fn of (seed,epoch)) -> forward -> loss -> backward -> step
//   -> at ckpt_every: eval TRAILING val fold, keep argmin-val weights (deterministic restore-best, §0.7).
[[nodiscard]] atx::core::Result<std::vector<std::vector<atx::f64>>>   // [ensemble_member] -> serialized best-val state
train(Module& proto, Optimizer& opt, Loss& loss,
      const Seq3& x_train, const atx::core::linalg::MatX& y_train,
      const Seq3& x_val,   const atx::core::linalg::MatX& y_val, const TrainConfig& cfg);
```
**Determinism (R1):** init draws via `seed_for(cfg.master_seed, "nn-init", layer, 0)` in one fixed order from one RNG
instance (§0.6); the minibatch shuffle is `seed_for(master, "nn-shuffle", epoch, 0)` (a pure function of `(seed, epoch)`);
dropout masks via `seed_for(master, "nn-drop", layer, step)`; the ensemble member `m` via `seed_for(master,
"nn-ensemble", m, 0)`; every matmul/conv/softmax/loss reduction is an **ascending scalar fold** (never `simd::dot`); the
epoch budget is **fixed** with **deterministic checkpoint-at-best** (§0.7). The S5-2 two-builds-equal test asserts
byte-identical serialized state. **Gradient correctness (R3):** the S5-2 test runs a **central finite-difference gradient
check** on every `Layer` (perturb each param ±ε, compare `(L(+ε)-L(-ε))/2ε` to the analytic grad, rel. error ≤ ~1e-5) —
**the load-bearing autodiff-replacement proof.** **Boundary pin (R7):** a `Sequential{Linear}` + identity activation +
`MseLoss`, full-batch `Sgd`, fixed iterations on a tabular (`L=1`) tensor must converge to the `p1` S5 ridge/elastic-net
linear coefficients (to tolerance; bit-for-bit in the closed-form ridge limit) — the S5-2 pin against the proven linear
learner. **No hot-path alloc (R6):** activation + grad scratch is pre-sized per `Module` at construction; training (cold)
may allocate.

### 4.3 TCN + GRU-lite sequence alphas (S5-2)

The two predictive architectures, composed from §4.2 `Layer`s and driven by the one `Trainer`. **TCN is the default**
(A1): dilated **causal** conv (the causal mask is the R2 look-ahead guard, structurally — a causal conv cannot read
`t+1`), residual blocks, weight-norm + dropout. **GRU-lite** (A2) is the path-dependent alternative (a single-gate MGU
cell; its backward is BPTT-through-the-window, the hardest gradient check, R3).

```cpp
// section: learn/tcn_alpha.hpp  (namespace atx::engine::learn)
struct TcnAlphaCfg {
  atx::usize blocks = 3, kernel = 3, channels = 24;   // tiny (R6); dilations = {1,2,4,8,...} per block (A1)
  atx::f64 dropout = 0.1, l2 = 1e-4;                   // heavy reg (R8)
  nn::TrainConfig train{};                             // fixed epochs + ensemble (R1/R8)
};
struct GruAlphaCfg { atx::usize hidden = 24; atx::f64 dropout = 0.1, l2 = 1e-4; nn::TrainConfig train{}; };

// Fit a TCN (resp. GRU-lite) over the TRAILING CPCV folds; fill a LearnedModel NN payload + oos_score_series/trial_count
// (so it inherits oos_deflated_sharpe + predict_blended, §0.2). TRAILING-fit, OOS-predict (R2) via expand_date_folds.
[[nodiscard]] atx::core::Result<LearnedModel> fit_tcn(const SequenceTensor& seq, const TcnAlphaCfg& cfg);
[[nodiscard]] atx::core::Result<LearnedModel> fit_gru(const SequenceTensor& seq, const GruAlphaCfg& cfg);
```
**Causal-conv look-ahead guard (R2):** `Conv1dCausal` left-pads the window so output `t` depends only on inputs `≤ t`; the
S5-2 test asserts predicting at date `t` is invariant to truncating the panel after `t`. **The fit reuses the firewall:**
`expand_date_folds` (`train.hpp`) maps CPCV date-folds → sequence-sample rows so the net trains on trailing folds and
predicts OOS — **no new CPCV machinery** (§0.3). **Deflation N (R4):** `fit_tcn`/`fit_gru` increment `trial_count` for
**every** architecture/seed trial they evaluate (via `TrialCounter`), not just the winner — so `oos_deflated_sharpe`
deflates honestly. **Seed-ensemble determinism (R8/R1):** the prediction is the ascending-member mean of the
`ensemble_size` best-val checkpoints.

### 4.4 Attention-lite alpha (S5-3)

A single architecture point that proves the framework spans attention. For short windows (L≈20–60) **plain single-head
O(L²) dot-product attention is already cheap** (A3) — linear/Performer attention is explicitly **not** shipped (it only
helps at long L and is slower for short windows). One `Attention1Head` block over the window + a `Linear` head.

```cpp
// section: learn/autoencoder_alpha.hpp  (namespace atx::engine::learn)   [attention-lite lives here with the AE, S5-3]
struct AttnAlphaCfg { atx::usize d_model = 24; atx::f64 dropout = 0.1, l2 = 1e-4; nn::TrainConfig train{}; };
[[nodiscard]] atx::core::Result<LearnedModel> fit_attn(const SequenceTensor& seq, const AttnAlphaCfg& cfg);
```
`Attention1Head::forward` computes `softmax(QKᵀ/√d) V` over the window with a **causal mask** (R2) and **ascending scalar
softmax + matmul folds** (R1); `backward` is the hand-written attention adjoint, finite-difference-checked (R3). **Cost
(R6):** `L²·d` per sample — negligible for `L≤60` on CPU.

### 4.5 Autoencoder statistical-factor alpha (S5-3)

The **factor** track (orthogonal to the predictive track, §0.9). A Gu-Kelly-Xiu-style autoencoder (B4) compresses the
return cross-section into K latent factors (tiny: K=1–6, beta-net 32→16→8); trained on a **trailing window**, frozen, and
applied OOS (rolling re-fit). The output is a latent-factor block that augments the predictive track (or seeds a sleeve's
risk model) — emitted as an alpha cross-section through the same `LearnedModel`/library bridge (§0.3).

```cpp
// section: learn/autoencoder_alpha.hpp  (namespace atx::engine::learn)
struct AeFactorCfg {
  atx::usize k_factors = 4;                  // K latent factors (R6: 1-6, sweep)
  std::vector<atx::usize> beta_hidden{32, 16, 8};   // beta-net depth (GKX CA0..CA3, B4); empty => linear (the PCA pin)
  atx::f64 l1 = 1e-4;                         // GKX L1 (R8/B4)
  nn::TrainConfig train{};                    // fixed epochs + 5-seed ensemble (R1/R8)
};

// Fit the autoencoder on a TRAILING window; return the encoder/decoder as a LearnedModel (kind=Autoencoder) that emits
// the K latent factors OOS. Rolling re-fit across folds (R2/B4). A LINEAR beta-net (empty beta_hidden) recovers PCA (R7).
[[nodiscard]] atx::core::Result<LearnedModel> fit_autoencoder_factors(const SequenceTensor& seq, const AeFactorCfg& cfg);
```
**Boundary pin (R7/§0.9):** with `beta_hidden` empty (a **linear** encoder/decoder, no nonlinearity, MSE reconstruction),
the autoencoder recovers the principal subspace — the S5-3 test asserts the latent factors match atx-core `pca()`
(`pca.hpp:51`) + `transform()` (`:101`) on explained-variance / subspace alignment (sign-and-rotation-tolerant). This pins
the AE to the proven PCA the existing `latent.hpp` already uses. **No look-ahead (R2):** the AE fits the trailing window
and encodes OOS only; truncation-invariance is the test. **Tiny + CPU-feasible (R6):** encode/decode of a few-hundred-wide
panel through 32→16→K is sub-millisecond on CPU.

### 4.6 NN-alpha gate + library/combiner/sleeve integration, bench, close (S5-4 / S5-5)

The capstone wiring — **all reused, nothing new** except the `ModelKind` extension + the lookback live adapter.

```cpp
// section: learn/nn_source.hpp  (namespace atx::engine::learn)
// (1) EXTEND the value type (§0.2): ModelKind gains {Tcn, Gru, Attn, Autoencoder}; LearnedModel gains an NN payload
//     (serialized nn::Module states, one per ensemble member); predict_blended gains the four cases (the no-default
//     switch flags any miss). predict_blended for an NN kind runs the ensemble forward + ascending-member mean.

// (2) The deflation + PBO gate (S5-4) — REUSED VERBATIM (§0.3): oos_deflated_sharpe(model, fm) + eval::pbo over the
//     CPCV fold metrics; trial_count carries the architecture/seed sweep (R4). admitted := dsr > 0 && pbo < ~0.2..0.3.

// (3) The library bridge (S5-5) — the stack_to_candidate pattern (src/learn/ensemble.cpp:184), expr_source =
//     "learned:tcn"/"learned:gru"/"learned:attn"/"learned:ae"; predict_at -> compute_metrics -> hash_bytes ->
//     Provenance{expr_source, seed=master_seed} -> AlphaCandidate -> Library::admit (§0.3).

// (4) The lookback live adapter (S5-5, §0.5): a sequence model reads a TRAILING window of the PanelView.
class SeqLearnedSignalSource final : public atx::engine::ISignalSource {   // loop/signal_source.hpp:103
public:
  SeqLearnedSignalSource(LearnedModel model, atx::usize lookback);   // pre-size window + activation scratch (R6)
  [[nodiscard]] std::span<const atx::f64> evaluate(PanelView pv) override;  // feeds rows [t-L+1..t] -> the net; zero alloc
private:
  LearnedModel model_; atx::usize lookback_;
  std::vector<atx::f64> window_, out_;   // pre-sized (R6, the learned_source.hpp:423 precedent)
};
```
**S5-4 (gate):** the deflation + PBO gate is `eval::deflated_sharpe` + `eval::pbo`, reused verbatim; the only S5 addition is
**ensuring the trial count N includes the full architecture × seed sweep** (R4) and that the NN alpha **also passes the
`p2` S4 robustness battery** (synthetic-recovery, regime/walk-forward, lockbox — R8). The S5-4 test asserts a real planted
sequence signal is admitted and a pure-noise sequence panel admits ~0 under the same bar (the non-vacuousness pin).
**S5-5 (integration):**
- **Integration test** (`learn_nn_source_integration_test.cpp`): PIT panel → `FeatureMatrix` → `SequenceTensor` (trailing
  windows) → `fit_tcn`/`fit_gru`/`fit_attn`/`fit_autoencoder_factors` → deflation+PBO gate → `stack_to_candidate` →
  `Library::admit` → combiner / a `fund::Sleeve` (S2), asserting **all gates simultaneously:** R1 (two builds
  byte-identical weights + alpha digest), R2 (truncation-invariance at every fitted boundary), R3 (the gradient checks
  pass), R4 (deflation+PBO with the full trial count), R7 (the linear-net and linear-AE pins), and that the admitted NN
  alpha flows through the **same** library/combiner/sleeve seams as a formulaic alpha.
- **Bench** (`nn_alpha_bench.cpp`): alphas/sec for fit + predict across `(architecture ∈ {tcn,gru,attn,ae}, L ∈ {20,40,60}
  lookback, N ∈ {500,1000,3000} universe, channels ∈ {16,24,32})`; the seed-ensemble cost (×`ensemble_size`); the
  train-vs-predict split (predict must be cheap — R6); the single-thread scalar-kernel throughput (the determinism cost vs
  a SIMD reference, recorded — NOT used in the digest path, §0.6).
- **Close ceremony:** residuals → ROADMAP backlog (the `core::linalg` NN-kernel / autodiff-tape lift §2.1; a learned
  attention/AE feature feeding S3's datafield family; an intraday-window sequence alpha once S6.4 lands; the
  conditional-AE-with-characteristics full GKX variant; a multithreaded-deterministic GEMM via MKL CBWR if scale demands
  it §0.10); ROADMAP status table `⏳ → ✅ <sha>`; `Last reviewed` bump; "What S5 proves" + "Next sprint priorities" baton;
  user reference `sprint5.md`; merge (`--no-ff`, the user's gate).

---

## Exit criteria

- A `SequenceTensor` builds **trailing** `(sample × L × feature)` windows over the as-built `FeatureMatrix` (rows
  `t-L+1…t`, never `t+1`); truncation-invariance holds; `L=1` reduces to the tabular matrix (§4.1/R2).
- The `nn::` framework (`Module`/`Layer`/`Optimizer`/`Loss`/`Trainer`) drives every architecture from one loop; **every
  layer's backward passes a finite-difference gradient check** (R3); the trainer is seeded + single-thread + order-fixed +
  fixed-epoch with deterministic checkpoint-at-best (R1); a `Sequential{Linear}`+identity+MSE net reduces to the `p1` S5
  linear alpha (R7).
- TCN (default), GRU-lite, and attention-lite sequence alphas fit on **trailing** CPCV folds and predict OOS (R2), each a
  tiny CPU-only net (R6) with a seed ensemble (R8), each filling a `LearnedModel` NN payload that inherits
  `oos_deflated_sharpe`/`predict_blended` (§0.2).
- The autoencoder factor extractor fits a trailing window, encodes OOS, and a **linear** AE recovers the atx-core `pca()`
  subspace (R7/§0.9).
- NN alphas are admitted **only** through `oos_deflated_sharpe` + `eval::pbo` with the **architecture × seed sweep in the
  trial count** (R4), and **also** pass the `p2` S4 robustness battery (R8); a real planted sequence signal is admitted, a
  pure-noise panel admits ~0 (the non-vacuousness pin).
- An admitted NN alpha flows through the **same** `stack_to_candidate` → `Library::admit` → combiner / `fund::Sleeve`
  seams as a formulaic alpha; the `SeqLearnedSignalSource` feeds a trailing window zero-alloc per date (§4.6/R6).
- The integration test passes all gates simultaneously; the bench reports fit/predict alphas/sec + the ensemble + the
  single-thread-determinism cost.
- `/W4 /permissive- /WX` (`ATX_WERROR=ON`) clean — **no `/fp:precise` flag** (determinism single-thread order-fixed,
  §0.10); one test file per unit; full engine suite stays green per unit; the only shared edit is appending
  `src/learn/**.cpp` to `atx-engine/CMakeLists.txt`.

## Invariants this sprint must prove

All carried-forward invariants (ROADMAP "Carried-forward invariants"), with five explicit stress points — S5 + S7 are the
two places the old invariants are easiest to break (the ROADMAP's own warning):
1. **Determinism (R1)** — the seeded NN training is the new stress point; it must be single-thread, order-fixed scalar,
   fixed-epoch, and the two-builds digest byte-identical (NOT `/fp:precise`, NOT `simd::dot`). The seed is the artifact.
2. **No look-ahead (R2)** — windows + CPCV folds + the AE fit are all **trailing**; truncation-invariance at every fitted
   boundary is the test. The causal conv makes the window guard structural.
3. **Differential correctness (R3)** — the **finite-difference gradient check** on every backward pass is the
   autodiff-replacement proof; without it the hand-written adjoints (§0.4) are unverified. **The #1 S5 risk.**
4. **Deflation honesty (R4)** — the trial count includes the architecture × seed sweep, or the deflation is a fiction at
   NN scale; PBO + the S4 battery are the additional firewalls.
5. **Reduction to the proven layer (R7)** — the linear-net → linear-alpha and linear-AE → PCA pins anchor the NN
   generalization to the layers it extends; without them S5 is not demonstrably a superset.

## Dependencies

- **Upstream (`p1` + `p2` S1–S4, assumed merged):** `p1` S5 (`learn::{FeatureMatrix, FeatureSpec, build_features,
  LearnedModel, ModelKind, build_augmented_row, predict_blended, predict_at, oos_deflated_sharpe, oos_ic,
  LearnedSignalSource, seed_for, TrialCounter, date_label_spans, expand_date_folds, stack_to_candidate}`), `p1` S1
  (`eval::{deflated_sharpe, pbo, cpcv, compute_return_metrics}`, `combine::compute_metrics`, reused verbatim), `p1` S4
  (`library::{Library, AlphaCandidate, Provenance, AlphaGate}`), `p2` S2 (`fund::Sleeve` — an NN alpha drops in as a new
  sleeve), `p2` S4 (the robustness battery the NN alpha must also pass — the ROADMAP S5 hard dependency).
- **atx-core (already available — reuse, no edge):** `Xoshiro256pp`, `linalg` `MatX`/`VecX` + `ridge` + `pca` +
  `transform` + `symmetric_eig` + `solve_spd` + `cholesky`, `hash_bytes`, `Result`/`Status`/`Err(ErrorCode, msg)`.
  **NOT** `core/simd.hpp` in any digest path (`simd.hpp:24` is not bit-reproducible, §0.6).
- **atx-core (Pattern B — new edge raised by this sprint):**

| S5 unit | Needs from `atx-core` | Engine-side fallback (shippable now) |
|---|---|---|
| S5.2 | **L7 (or new L-tier) NN primitives** — deterministic reverse-mode autodiff + fixed-iteration seeded SGD/Adam + conv1d/GRU/attention/layernorm/softmax kernels (single-thread, order-fixed) | engine-local `learn::nn` framework — hand-written per-layer adjoints + a fixed-epoch trainer over `core::linalg`; tiny nets ⇒ scalar kernels are exact + cheap |

## Explicitly NOT in this sprint

- **No GPU / GPU dependency, no large transformer.** CPU-only, single-thread scalar kernels, tiny nets by mandate (R6);
  large transformers overfit small panels (A4) — TabPFN-style foundation models are a recorded benchmark, not a
  dependency. A multithreaded-deterministic GEMM (MKL CBWR, §0.10) is a recorded scale lift, not shipped.
- **No autodiff library / DL runtime (libtorch / ONNX / ggml).** Hand-rolled framework over `core::linalg`, owning the
  reduction order for byte-determinism (§0.10/B3); the `core::linalg` NN-kernel tier is the recorded Pattern-B lift.
- **No linear/Performer attention.** Plain single-head O(L²) is already cheap for short windows (A3); long-sequence
  efficient attention is unnecessary (and slower) here.
- **No conditional-autoencoder-with-firm-characteristics full variant.** S5 ships the plain/statistical AE factor (the
  PCA-pinned form, §4.5); the full GKX characteristics-conditioned beta-net is a recorded refinement.
- **No new general-purpose primitive in the engine** (anti-roadmap #7) — the NN kernels are a recorded `core::linalg`
  request (Pattern B), engine-side fallback only until the L-tier kernels land.
- **No live / streaming / optimal execution** — an NN alpha emits a cross-section through `ISignalSource`; routing is the
  broker's job. **No alt-data / intraday features** — those are **S6** (an intraday-window sequence alpha is recorded for
  once S6.4 lands).
- **No bypassing the gate** — every NN alpha goes through `oos_deflated_sharpe` + `eval::pbo` + the S4 battery (R4/R8);
  the `p1` anti-roadmap #7 "ML overfits at this N" is honored by gating, not ignored.

## Baton → next

S5 hands the `p2` frontier a **richer signal family** — deterministic CPU-only neural sequence + factor alphas (TCN,
GRU-lite, attention-lite, autoencoder) on a pluggable framework, deflation- + PBO- + robustness-gated, that **plug into the
library and the S2 meta-book exactly like a formulaic alpha**. **S6** (alt-data / multi-asset / intraday) feeds the
sequence builder richer features (and an intraday window unlocks a sub-daily sequence alpha). **S8** (the robust-alpha
capstone) routes the NN alphas — alongside the formulaic (S3/S4) and classical-ML (`p1` S5) families — through the whole
pipeline and the sealed lockbox; the NN family is one more set of robustness-proven signals the terminal evaluation reads.
With S5 closed, the signal-family frontier (formulaic S3/S4 + classical-ML `p1` S5 + deep-learning S5) is complete; the
abstract NN framework is the substrate S6/S8 extend, never rewrite.

---

## References

> All sources tagged during the 2026-06-14 web-research pass (CPU-efficient sequence architectures + CPU NN determinism +
> the pluggable-framework decomposition + autoencoder asset pricing + small-sample gating) with per-citation
> VERIFIED/UNVERIFIED confirmation. **[LB]** = load-bearing (a unit's design rests on it); **[S]** = supporting; **[X]** =
> a circulated claim that **failed/lacked verification** — recorded so the doc does not propagate it as fact.

### Track A — CPU-efficient sequence architectures

1. **[LB]** S. Bai, J. Z. Kolter, V. Koltun. *An Empirical Evaluation of Generic Convolutional and Recurrent Networks for
   Sequence Modeling.* arXiv:1803.01271, 2018. https://arxiv.org/pdf/1803.01271 **[VERIFIED]** *(TCN = dilated causal conv
   + residual + weight-norm + dropout; matches/beats LSTM/GRU broadly; "a natural starting point for sequence modeling" —
   the §4.3 default + the causal R2 guard.)*
2. **[S]** *Temporal Convolutional Networks for Financial Time Series Forecasting: A Survey.* ResearchGate 392102503,
   2024. https://www.researchgate.net/publication/392102503 **[VERIFIED — exists]** *(TCN in finance — supporting §4.3.)*
3. **[LB]** G.-B. Zhou, J. Wu, C.-L. Zhang, Z.-H. Zhou. *Minimal Gated Unit for Recurrent Neural Networks.* Int. J. of
   Automation and Computing **13**(3):226–234, 2016. https://link.springer.com/article/10.1007/s11633-016-1006-2
   **[VERIFIED]** *(single-gate MGU ≈ GRU with fewer params — the §4.3 GRU-lite.)* Slim-MGU: arXiv:1701.03452
   https://arxiv.org/pdf/1701.03452 **[VERIFIED]**.
4. **[S]** Z. Shen, M. Zhang, H. Zhao, S. Yi, H. Li. *Efficient Attention: Attention with Linear Complexities.*
   arXiv:1812.01243, 2018. https://arxiv.org/abs/1812.01243 **[VERIFIED]**; short-seq linear-attention-overhead analysis
   https://haileyschoelkopf.github.io/blog/2024/linear-attn/ **[S]**. *(plain single-head is cheaper for short windows —
   the §4.4 decision NOT to ship linear attention.)*
5. **[S]** N. Hollmann et al. *TabPFN: Accurate predictions on small data with a tabular foundation model.* Nature, 2024.
   https://www.nature.com/articles/s41586-024-08328-6 **[VERIFIED venue]** *(small-tabular DL benchmark — A4; recorded as
   a benchmark, NOT a dependency: not deterministic-train, not CPU-trivial to retrain.)*

### Track B — CPU NN mechanics (determinism / framework / autoencoder / gating)

6. **[LB]** *RepDL: Bit-level Reproducible Deep Learning.* arXiv 2510.09180, 2025. https://arxiv.org/html/2510.09180v1
   **[VERIFIED — exists]** *(correct-rounding + fixed summation order for byte-reproducible training — the §0.6/§0.10
   determinism playbook.)*
7. **[S]** Z. Sui et al. *On the Nondeterministic Impact of CPU Multithreading on Training Deep Learning Systems.*
   ISSRE'21. https://yuleisui.github.io/publications/issre21.pdf **[UNVERIFIED — primary PDF did not render this pass;
   claim (single/fixed-thread reproduces, varying thread count breaks) corroborated from the abstract]** *(the
   single-thread mandate, §0.10.)*
8. **[S]** Intel oneMKL Conditional Numerical Reproducibility (CBWR; `KMP_DETERMINISTIC_REDUCTION`, `MKL_DYNAMIC=FALSE`;
   ~2× cost). https://www.intel.com/content/www/us/en/docs/onemkl/developer-guide-windows/2023-2/get-started-with-conditional-num-reproducibility.html
   **[VERIFIED]** *(the recorded multithreaded-deterministic GEMM lift if scale demands it, §0.10; NOT shipped — S5 is
   single-thread.)*
9. **[S]** PyTorch Reproducibility notes (seeds, `use_deterministic_algorithms`, seeded DataLoader generator).
   https://docs.pytorch.org/docs/2.12/notes/randomness.html **[VERIFIED]** *(the per-purpose-seeded-RNG + no-autotuning
   discipline, §0.6.)*
10. **[LB]** PyTorch C++ `nn::Module` API (recursive submodules, `forward`, ordered parameters).
    https://docs.pytorch.org/cppdocs/api/nn/index.html **[VERIFIED]** *(the Module/Layer decomposition — §4.2/R5.)*
11. **[S]** R. R. Curtin et al. *mlpack 4: a fast, header-only C++ machine learning library.* arXiv:2302.00820, 2023.
    https://arxiv.org/pdf/2302.00820 **[VERIFIED]** *(FFN/CNN/RNN over one layer abstraction + a separate optimizer —
    the §4.2 framework reference.)*
12. **[S]** tiny-dnn (header-only, dependency-free C++) https://github.com/tiny-dnn/tiny-dnn ; MiniDNN (Eigen-only
    reference) https://github.com/yixuan/MiniDNN **[VERIFIED — repos exist]** *(the Module/Layer/Optimizer/Loss reference
    designs — §4.2; design references, NOT runtime deps, B3.)*
13. **[LB]** S. Gu, B. Kelly, D. Xiu. *Autoencoder Asset Pricing Models.* Journal of Econometrics **222**(1B):429–450,
    2021. https://www.aqr.com/Insights/Research/Working-Paper/Autoencoder-Asset-Pricing-Models ; SSRN 3335536.
    **[VERIFIED venue]**; exact arch (CA0–CA3, K=1–6, L1=1e-4, BN 0.99, 5-seed ensemble, rolling OOS) from the readable
    replication https://pmc.ncbi.nlm.nih.gov/articles/PMC10389732/ **[VERIFIED — replication; UNVERIFIED at the primary
    PDF]**. *(the §4.5 autoencoder + the rolling-window OOS protocol R2 + the linear-AE→PCA pin R7.)*
14. **[S]** Reference CA implementation. https://github.com/rongwang0824/Autoencoder-Asset-Pricing-Models
    **[VERIFIED — repo exists]** *(supporting the §4.5 sizes.)*
15. **[LB]** D. H. Bailey, M. López de Prado. *The Deflated Sharpe Ratio: Correcting for Selection Bias, Backtest
    Overfitting and Non-Normality.* Journal of Portfolio Management **40**(5):94–107, 2014. (overview)
    https://en.wikipedia.org/wiki/Deflated_Sharpe_ratio **[VERIFIED — concept; primary venue standard]** *(the R4 gate,
    reused verbatim from `p1` S1; the trial-count-honesty condition.)* PBO: ResearchGate 318600389 **[VERIFIED — exists]**.
16. **[S]** *Combinatorial Purged Cross-Validation* superiority for OOS finance (lower PBO, better DSR).
    https://www.sciencedirect.com/science/article/abs/pii/S0950705124011110 **[VERIFIED — exists]** *(the R2/R4 walk-
    forward/CPCV gating — reused from `p1` S1's `eval::cpcv`.)*
17. **[S]** Seed-ensemble variance reduction (near-uncorrelated different-seed nets). arXiv:2304.01910 / arXiv:2103.04514
    **[VERIFIED — exist]** *(the §0.8 first-class `ensemble_size=5`, R8.)*

> **Verification caveats carried from the research pass:** (a) the **ISSRE'21 multithreading paper (7)** and the **GKX
> primary PDF (13)** did **not** render as text this pass — the single-thread-reproducibility claim and the exact CA
> architecture (CA0–CA3, K=1–6, L1=1e-4, 5-seed ensemble, rolling windows) come from the abstract + the readable PMC
> replication respectively; re-extract the primaries before publishing those exact figures. (b) **pod-shop NN-training
> lore** (specific dropout rates, layer counts) is trade-press — the rigorous anchors are Bai-Kolter-Koltun (1) for TCN
> and GKX (13) for the AE. (c) The **deflated-Sharpe / PBO / CPCV** machinery is **already in `p1` S1** (`eval::*`) — S5
> reuses it verbatim (§0.3); the citations here document the *why*, the code is the proven `p1` layer. (d) The
> **`simd.hpp:24` "few ULPs, not bit-for-bit"** caveat is an in-tree as-built fact (the code review), not a literature
> claim — it is load-bearing for §0.6/R1 and was confirmed by reading the header.
