# Sprint S5 — Learned Signals & ML Combiner — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax. This is the FROZEN *how*; the sprint spec [`sprint-5-learned-signals-ml-combiner.md`](sprint-5-learned-signals-ml-combiner.md) is the *what*. On conflict, **§0 (this plan's as-built amendment) overrides** the spec.

**Goal:** Make the alpha pool's **constituents and combiner *learned*, not just formulaic** — a point-in-time feature tensor (raw fields + stored library/pool alpha streams + multi-horizon forward-return labels), **latent/hidden-feature extraction** (PIT PCA factors + interaction terms), **learned signal models** (cross-sectional ridge/elastic-net and a deterministic gradient-boosted tree) that emit a cross-section and plug into the pool through the existing `ISignalSource` → `library::Library::admit` seam exactly like a mined alpha, an **HMM regime detector** (log-space Baum-Welch) for regime-conditional weighting, and a **nonlinear stacking mega-combiner** (alphas-as-meta-features → a learned meta-alpha) admitted **only if it beats the P4 linear combiner out-of-sample after deflation** — the whole thing trained inside S1's purged CPCV, deflated-Sharpe gated, and **bit-for-bit deterministic**.

**Architecture:** A new header-only engine layer `atx::engine::learn::` (`include/atx/engine/learn/`) that is **purely additive** — it edits no `combine/`, `eval/`, `library/`, `factory/`, or `alpha/` source. It consumes atx-core **L7** (`regression::ridge`/`pca`/`linalg`/`solve`), **L6** (`cross_section`, `rolling`), **L1** (`Xoshiro256pp`) directly, and the engine's own seams (`alpha::Panel`/`Engine`/`AlphaStreams`, `combine::{AlphaStore,AlphaGate,AlphaMetrics,AlphaCombiner,ISignalSource}`, `eval::{cpcv_folds,deflated_sharpe,stats_ext}`, `library::Library`). The three numeric kernels atx-core lacks — **elastic-net (L1+L2 coordinate descent), deterministic histogram GBT, log-space HMM forward-backward** — ship engine-local-then-lifted (Pattern B), each behind an obviously-correct reference + a bounded differential test. Determinism is **by construction**: every fit is seeded from a fixed `(master_seed, …)` derivation, every reduction is order-fixed, every fitted object obeys the trailing-fit/apply-forward firewall, so *same data + same seed → byte-identical params **and** byte-identical emitted signal* — the make-or-break invariant for ML in a deterministic engine.

**Tech Stack:** C++20, header-only inline (`#pragma once`), namespace `atx::engine::learn`; reuses atx-core `core::{regression::{ridge,ols,wls,OlsResult},pca::{pca,transform,PcaResult},linalg::{as_matrix,as_vector,MatX,VecX},decompose::symmetric_eig,solve::{solve_spd,inverse},random::Xoshiro256pp,cross_section::{rank,zscore,demean,winsorize},simd::{dot,axpy},hash::hash_bytes}` and engine `combine::{ISignalSource,SignalView,PanelView,AlphaStore,AlphaId,AlphaGate,GateConfig,GateVerdict,AlphaMetrics,compute_metrics,AlphaCombiner,CombinerConfig,CombineMethod,Combination,pairwise_complete_corr}`, `eval::{cpcv_folds,CpcvConfig,CpcvFold,LabelSpan,deflated_sharpe,DsrResult,norm_cdf,skewness,excess_kurtosis,mean_std_pop}`, `alpha::{Panel,Engine,SignalSet,AlphaStreams,Program}`, `library::{Library,AlphaCandidate,Provenance,AdmitKind}`. GoogleTest (`atx-engine/tests/*_test.cpp`, CONFIGURE_DEPENDS — no per-unit CMake edit). clang-cl `/W4 /permissive- /WX` **+ strict FP** (`/fp:precise`, `-ffp-contract=off`). Build + ctest are the gates; clang-tidy disabled (noise).

---

## §0 — As-built reconciliation amendment

The spec was frozen before S4/S4b were verified as-built. Eight corrections; each names the consequence. **§0 overrides the spec on conflict.**

### 0.1 Feature source is **stored streams + raw fields**, not DSL re-eval
The spec says learned models consume "DSL signal outputs (from the S4 library / S3 mining) plus raw panel fields." As-built, the formula re-evaluation path the spec implies — re-parse `Provenance.expr_source` → compile → eval over a fresh `alpha::Panel` — **does not exist**: S4b (which would add `unparse(Ast)→string` and the re-eval adapter) is **spec-only, zero code** (HEAD `518af61` is its brainstorm-spec commit; no `PoolView`/`unparse`/`ResearchDriver`/`mine_into` is implemented). S4b explicitly defers the "Research-panel `ISignalSource` re-eval adapter" to S5/S7.

**Resolution.** The S5-1 feature builder sources features from **(a)** raw `alpha::Panel` fields via `Panel::field_cross_section(field, date)` and **(b)** *already-computed* alpha signal/PnL streams that the library/pool already hold — `combine::AlphaStore::pnl(id)`/`positions(id,t)` and `alpha::AlphaStreams::pnl(a)`/`positions(a,t)` (both exist, both PIT-aligned to the panel date axis). **No re-parse/re-compile is required for training**, because the stored streams ARE the alpha outputs aligned date-by-date. The live re-eval adapter (re-parse `expr_source` → compile → eval) is built **once, optionally, in S5-6** only for deploying a *learned* model as a standalone `ISignalSource` over a brand-new panel; it is NOT on the training path. **Consequence:** S5 has no dependency on the unbuilt S4b seams; it reads stored data through APIs that exist today.

### 0.2 CPCV folds run on the **date axis**; feature rows are (date × instrument)
The feature matrix is `(date, instrument) × feature`. But purge/embargo are *temporal* — you cannot purge an instrument. As-built, `eval::cpcv_folds(std::span<const LabelSpan>, CpcvConfig)` returns `CpcvFold{train_idx, test_idx}` over a **flat observation axis** with per-observation `LabelSpan{t0,t1}`.

**Resolution.** S5 feeds `cpcv_folds` **one `LabelSpan` per DATE** (observation = a trading date; `LabelSpan{date, date + horizon}` is the forward-return label window, which is exactly what drives purge+embargo). The fold's `train_idx`/`test_idx` are **date indices**; the trainer expands each date-fold to the matching feature rows (all instruments at those dates). **Consequence:** `eval::cpcv` is consumed unchanged; the purge correctly removes train dates whose label window overlaps any test date, the embargo correctly drops train dates within `ceil(embargo·N)` after a test date — the no-look-ahead firewall is inherited, not re-implemented.

### 0.3 Deflation N for ML = the **ML trial count**
S1's `deflated_sharpe(sr, T, skew, exkurt, N, var)` takes `N` = number of trials (the selection count) and S3 set `N` = the running canonical-hash trial count. A learned model has no "population"; the analog of a trial is **a distinct fit** — one `(model-class, hyperparameter-setting, horizon)` combination scored in the run.

**Resolution.** The S5 trainer maintains a monotone `trial_count` incremented once per distinct fit scored (every CV configuration, every horizon, every ensemble candidate) and passes it as `N` to `deflated_sharpe`. **Consequence:** the more model configurations you try, the higher the deflation bar — a no-edge model that "wins" only by search is correctly killed (the non-vacuous anti-snooping proof that justifies lifting the p0 ML ban). `N=1` (a single pre-registered fit) reduces to plain PSR, matching S1 semantics.

### 0.4 The mega-combiner is a new `learn::StackingCombiner`, **not** a `CombineMethod` enum value
`combine::CombineMethod` is `{EqualWeight,RankAverage,IcWeighted,ShrinkageMv,BoundedRegression}` — all **linear** rungs fit by `combine::AlphaCombiner::fit(pool, fit_begin, fit_end) → Combination{weights, fit_begin, fit_end}` and applied by `combine::CombinedSignalSource`. The spec's "nonlinear/ensemble mega-combiner" cannot be a sixth enum value without editing `combine/combiner.hpp` (forbidden — additive only) and would break the "weights are a linear blend" contract.

**Resolution.** S5 adds a separate `learn::StackingCombiner` that learns a **nonlinear** blend (a meta-model whose inputs are the pool alphas' cross-sections) and is benchmarked **against `combine::AlphaCombiner{CombinerConfig{method=ShrinkageMv}}`** (the P4 linear combiner) via OOS-after-deflation. **Consequence:** `combine/` is untouched; the linear combiner is reused verbatim as the benchmark the nonlinear one must beat.

### 0.5 Latent/PCA features are fit **on a trailing window**, applied forward (PIT)
The user asks for "higher-order / hidden features." atx-core ships `core::pca::pca(MatX, k) → PcaResult{mean, components, explained_variance, explained_ratio}` and `transform(model, MatX)` (L7). PCA on the *whole* feature tensor would leak the future into the past.

**Resolution.** S5-2 fits the PCA basis on feature rows with `date ≤ t − embargo` only (a trailing window), then **projects forward**: latent factor `f_k(date,inst) = transform(model, X[date,inst])`. The basis is a fitted object under the M2 firewall, re-fit on each walk-forward step. **Consequence:** latent features are truncation-invariant (a factor at date `t` is identical with or without `> t` data), so "hidden features" are discovered without look-ahead.

### 0.6 Multi-horizon is **first-class** in the label/predict path
The spec is implicitly single-horizon. The user explicitly wants "predictions over various time horizons."

**Resolution.** The S5-1 label builder constructs forward-return labels `y_h(date,inst)` for a configured horizon set `H = {h₁,…}` (default `{1,5,21}` trading days; `{1}` recovers single-horizon). Each learned model (S5-3 linear, S5-4 GBT) trains **one fit per horizon**; a **horizon blend** combines the per-horizon predictions with weights = each horizon's OOS information coefficient (fit-on-trailing, M2). **Consequence:** the engine optimizes predictions across horizons, and the blend is itself a deflated, gated object — no horizon is trusted that does not earn it OOS.

### 0.7 Determinism is seeded from a fixed derivation; **no** wall-clock / thread / map-order entropy
ML is where determinism dies (RNG init, parallel reductions, hash-map iteration, floating-point non-associativity). atx-core's `Xoshiro256pp(seed)` is the only sanctioned RNG.

**Resolution.** Every RNG draw derives its seed from `(master_seed, unit_tag, fold_idx, horizon_idx)` via a fixed SplitMix-style mix (mirroring S3's `seed_for`); GBT uses a **deterministic histogram split finder** with first-max tie-break (no time-dependent tie-breaking); coordinate-descent elastic-net is **RNG-free** (cyclic coordinate sweep); HMM init is seeded; all cross-row reductions iterate in fixed instrument/date order. **Every RNG-bearing unit ships a "two builds equal" + "same seed replays byte-identical params AND signal" test.** **Consequence:** the determinism invariant (carried-forward #1) survives the ML layer — the hardest place to keep it.

### 0.8 atx-core L7 is **header-only Eigen-backed**; confirm the link at S5-0
`core::regression`/`pca`/`linalg`/`decompose`/`solve` are header-only but instantiate **Eigen** (`MatX = Eigen::MatrixXd`, column-major). P4's `ShrinkageMv`/`BoundedRegression` combiner already performs eigendecomposition + ridge, so the engine test target very likely already resolves Eigen transitively — but this is a **build risk to retire first**, not an assumption.

**Resolution.** S5-0 adds a one-line link smoke-test (`core::regression::ridge` on a 2×2 system inside a trivial `TEST`) and confirms `cmake --build … --target atx-engine-tests` resolves it before any model unit starts. **Consequence:** if Eigen is *not* transitively linked, it surfaces at S5-0 (cheap) rather than mid-S5-3.

> **Net scope shift vs spec:** the spec's six units (S5.0–S5.5) become **eight** (S5-0…S5-7): the feature builder gains explicit multi-horizon labels (0.6); a new **latent/hidden-feature unit S5-2** (PIT PCA + interaction terms) serves the user's "higher-order features" ask (0.5); linear (S5-3) and GBT (S5-4) become per-horizon; the HMM (S5-5) and the **stacking mega-combiner** (S5-6, with the alpha-of-alphas framing + the optional live re-eval adapter) stay; integration + bench + close split into S5-7. Everything is **purely additive** — no edit to `combine/`, `eval/`, `library/`, `factory/`, `alpha/`. Suggested split: **S5-a** = S5-0…S5-4 (features + latent + linear + GBT), **S5-b** = S5-5…S5-7 (HMM + ensemble + integration/close).

---

## §1 — Research foundation: the learned-signal design rules

Derived from the research north-stars ([`renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md) §3.2/§4.2 HMM·Baum-Welch, §4.3 noisy-channel, §4.4 many-weak-signals, §9.3 combination, §9.4 OOS discipline; [`worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) fitness/correlation gating) and the carried-forward p1 invariants. **Non-negotiable**; every S5 unit is checked against them.

| # | Rule | Why / source |
|---|---|---|
| **M1** | **Determinism under training.** Same data + same seed ⇒ **byte-identical fitted params AND byte-identical emitted signal**. All RNG = `Xoshiro256pp` seeded from `(master_seed, unit, fold, horizon)`; GBT = deterministic histogram + first-max tie-break; elastic-net = RNG-free cyclic CD; HMM init seeded; order-fixed reductions. | Carried-forward invariant #1; ML is the easiest place to break it. The "two-builds-equal" test is mandatory per RNG-bearing unit. |
| **M2** | **Fit/apply firewall (no look-ahead).** Every fitted object — feature standardization stats, **PCA latent basis**, linear coeffs, GBT splits, HMM `(A,B,π)` + regime posterior, horizon-blend weights — is estimated on a **trailing ≤ t−embargo window** and applied forward only. Truncation-invariance is the test. | P4 firewall + S1 purge/embargo; RenTech §9.4 ("walk-forward as a harness primitive, not a convention"). |
| **M3** | **No snooping.** Every model and the ensemble trains **inside S1 CPCV** and reports a **deflated** Sharpe with `N` = the ML trial count (§0.3). A no-edge model is **rejected** by the DSR gate (non-vacuous). The nonlinear combiner is admitted **only if** it beats the P4 linear combiner **OOS-after-deflation**; on a fixture where it doesn't, it is rejected. | RenTech §9.4 ("deflate Sharpe for multiple testing"); the explicit lift-condition for the p0 ML ban. |
| **M4** | **Differential correctness.** Each new kernel ships an obviously-correct reference + a bounded differential test: **elastic-net** vs atx-core `ridge` at `α=0` (ULP-class) and vs the soft-threshold closed form on orthonormal `X`; **GBT** depth-1 stump vs an exact brute-force threshold search; **HMM** forward-backward vs a reference log-space implementation and the forward-likelihood-equals-backward-likelihood identity. | Carried-forward invariant #7; "a test that only checks it returned something is vacuous." |
| **M5** | **Pattern B — no general-purpose primitive in the engine.** Elastic-net, GBT, HMM forward-backward are **atx-core L7 requests**, shipped engine-local-then-lifted, exactly as S1 (`stats_ext`→L6), S2 (`DetPool`→L4), S3 (sep-CMA-ES→L7), S4 (SimHash/SRP→atx-core) did. Ridge, PCA, linalg, RNG, cross_section are **consumed from atx-core**, never re-implemented. | Module rule; the precedent chain. |
| **M6** | **A learned model is just an alpha.** It emits a cross-section per date via the **same `combine::ISignalSource`** seam, is scored by `compute_metrics`, and is admitted through the **same `library::Library::admit(AlphaCandidate, AlphaGate)`** path (with `Provenance{expr_source="learned:<spec>", …}`) that S3/S4/S4b use. It does **not** fork the corr-to-pool math. | RenTech §9.3 ("a learned model plugs into the pool exactly like a formulaic alpha"); collision-avoidance with S4b. |
| **M7** | **No hot-path allocation; cold-fit may allocate.** Training, feature-build, PCA-fit, HMM-fit, snapshot (cold) may allocate. The steady-state `evaluate()` of a **fitted** `LearnedSignalSource` reuses pre-sized scratch (zero alloc per date). | Carried-forward invariant #6. |
| **M8** | **No survivorship · PIT · NaN-verbatim.** Feature build carries delisted instruments with their final bar; out-of-universe / not-yet-knowable cells read **NaN**; NaN is handled **explicitly** (row-dropped from a fit or cross-sectionally imputed — documented per call site), **never silently coerced to zero**. | Carried-forward invariants #3/#4; Straus data-correctness (RenTech §9.1). |

**One-sentence thesis:** *a learned model is a deterministic, deflation-gated, firewall-fit alpha that plugs into the existing pool through the existing seams — the engine learns higher-order structure (latent factors, tree interactions, regimes, alpha-of-alphas) without ever learning to look ahead or to fool its own scorer.*

---

## §2 — File structure

### 2.1 atx-core Pattern-B requests (decided at kickoff; engine-local fallback ships S5)

> The engine adds no general-purpose primitive (project rule). S5 records **three** cross-module edges and ships on existing primitives + engine-local kernels, exactly as S1 (`stats_ext`→atx-core L6), S2 (`DetPool`→L4), S3 (sep-CMA-ES→L7), S4 (SimHash/SRP→atx-core) did:
>
> 1. **Elastic-net (L1+L2 penalized) coordinate-descent solver → atx-core L7.** atx-core ships `regression::{ols,ridge,wls}` (`ridge(X,y,λ)` is L2-only) but **no L1/elastic-net** (verified absent). Ship engine-local in `learn/elastic_net.hpp` (cyclic coordinate descent with soft-thresholding, RNG-free, §4.3); differential-tested vs atx-core `ridge` at `α=0` and the soft-threshold closed form (§0.4, M4). The CD solver is the Pattern-B L7 lift.
> 2. **Deterministic histogram gradient-boosted-tree primitive → atx-core L7 (or new).** No tree/boosting/split-finder exists in atx-core (verified absent). Ship engine-local in `learn/gbt.hpp` (fixed-bin histogram split finder, seeded row/feature subsampling, first-max tie-break, §4.4); differential-tested vs an exact brute-force threshold search on a stump (M4). The histogram split finder is the Pattern-B L7 lift.
> 3. **HMM / Baum-Welch forward-backward (log-space) → atx-core L7.** No HMM/EM/forward-backward exists in atx-core (verified absent). Ship engine-local in `learn/hmm.hpp` (log-space forward-backward + EM re-estimation, seeded init, §4.5); differential-tested vs a reference implementation and the fwd==bwd log-likelihood identity (M4). The forward-backward kernel is the Pattern-B L7 lift.
>
> **Consumed directly from atx-core (no request, no re-implementation):** `regression::ridge` (linear L2 baseline), `pca::{pca,transform}` (latent features, §0.5), `linalg::{as_matrix,as_vector,MatX,VecX}` + `decompose::symmetric_eig` + `solve::{solve_spd,inverse}` (design-matrix math), `random::Xoshiro256pp` (seeded training), `cross_section::{rank,zscore,demean,winsorize}` + `rolling::{RollingMean,RollingStd}` (feature standardization), `simd::{dot,axpy}` (inner loops). **Reused from the engine's own S1 layer (no re-request):** `eval::stats_ext::{norm_cdf,skewness,excess_kurtosis,mean_std_pop,median}` (atx-core lacks these; S1 already shipped them engine-local).

### 2.2 Engine `learn/` layer (this sprint builds these)

| File | Responsibility | Unit |
|---|---|---|
| `include/atx/engine/learn/fwd.hpp` | forward decls + the layer doc block (the M1–M8 rules, the seam map, the determinism contract) | S5-0 |
| `include/atx/engine/learn/feature_matrix.hpp` | PIT `(date×instrument)×feature` tensor builder + multi-horizon forward-return label builder (§4.1; raw `Panel` fields + stored alpha streams; NaN-verbatim; deterministic order-fixed) | S5-1 |
| `include/atx/engine/learn/latent.hpp` | PIT latent/hidden-feature extraction (trailing-fit PCA factors via `core::pca` + bounded pairwise interaction terms; §4.2/§0.5) | S5-2 |
| `include/atx/engine/learn/elastic_net.hpp` | engine-local cyclic-coordinate-descent L1+L2 solver (Pattern-B edge 1; §4.3) | S5-3 |
| `include/atx/engine/learn/learned_source.hpp` | `LearnedSignalSource` — adapts any fitted per-horizon model to `combine::ISignalSource` (§4.6); the cross-section emit path + the optional live re-eval adapter | S5-3 |
| `include/atx/engine/learn/linear_alpha.hpp` | cross-sectional linear learned alpha (ridge + elastic-net), walk-forward CPCV training, per-horizon, horizon blend (§4.3/§0.6) | S5-3 |
| `include/atx/engine/learn/gbt.hpp` | deterministic histogram gradient-boosted-tree model + the GBT learned alpha wrapper (Pattern-B edge 2; §4.4) | S5-4 |
| `include/atx/engine/learn/hmm.hpp` | log-space Baum-Welch HMM + PIT regime posterior (Pattern-B edge 3; §4.5) | S5-5 |
| `include/atx/engine/learn/ensemble.hpp` | `StackingCombiner` — nonlinear meta-model over pool alphas (alpha-of-alphas), regime-conditional + horizon-aware blend, deflation-gated vs the P4 linear combiner (§4.7/§0.4) | S5-6 |
| `include/atx/engine/learn/train.hpp` | shared training scaffold: CPCV-date-fold expansion (§0.2), deflation `N` trial counter (§0.3), seeded RNG derivation (§0.7), the firewall-checked fit/apply driver | S5-1 (grows through S5-6) |

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`, CONFIGURE_DEPENDS)

`feature_matrix_test.cpp` (S5-1), `latent_test.cpp` (S5-2), `elastic_net_test.cpp` + `linear_alpha_test.cpp` (S5-3), `gbt_test.cpp` (S5-4), `hmm_test.cpp` (S5-5), `ensemble_test.cpp` (S5-6), `learn_integration_test.cpp` (S5-7, the determinism + anti-snooping + nonlinear-beats-linear + regime-improves + multi-horizon proofs). Bench: `bench/learn_bench.cpp` (S5-7).

### 2.4 Ledger

`atx-engine/plans/p1/sprint-5-progress.md`, opened at S5-0; per-unit rows, commits table, residuals → ROADMAP backlog, baton → S7. Format = copy `sprint-4-progress.md`.

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

Every unit, before its commit, must clear:

- **Build green:** `cmake --build build --config Debug --target atx-engine-tests` under `/W4 /permissive- /WX` + `/fp:precise` `-ffp-contract=off` — zero warnings.
- **Its own suite + full suite green:** `ctest --test-dir build -R <Suite> --output-on-failure`, then the whole engine suite (no regression).
- **M1 determinism:** any unit that fits a model with RNG ships a `*_Deterministic_*` test asserting same-seed-replays-byte-identical (params hashed via `core::hash::hash_bytes` over the fitted coefficient/threshold bytes, AND the emitted signal cross-section bit-equal).
- **M2 firewall:** any unit that fits ships a `*_TruncationInvariant_*` test (fit on `[0,t)` vs `[0,t+k)` → identical output at dates `< t`).
- **M4 differential:** any new kernel ships its bounded differential test vs the reference (not a "returned something" test).
- **M8 NaN/PIT:** any unit touching the panel ships a NaN-handling test (out-of-universe cell → NaN propagates per the documented policy, never silent zero).
- **No source edits outside `learn/`:** `git diff --stat` shows only `learn/*.hpp`, `tests/*_test.cpp`, `bench/*_bench.cpp`, and the ledger. (S5 is additive — M5/§0.)
- **Explicit-pathspec commit** + ledger row + `git show HEAD --stat`.

```text
HANDOFF — read before implementing any S5 unit
Implementation quality standard (atx): governed by .agents/cpp/agent.md + .agents/atx-engine/agent.md +
  atx-engine/plans/docs/implementation-quality.md. Positive style/API references in-tree:
  combine/combiner.hpp (fit/apply firewall + Combination), combine/gate.hpp (admit verdict order),
  combine/store.hpp (AlphaStore layout), eval/cpcv.hpp (fold API), eval/deflated_sharpe.hpp (DSR),
  eval/stats_ext.hpp (engine-local stats), library/library.hpp (admit/AlphaCandidate/Provenance),
  alpha/panel.hpp (PIT field_cross_section), alpha/streams.hpp (stored pnl/positions),
  factory/search_driver.hpp (seed_for derivation precedent).
THIS SPRINT'S DOMINANT RISK IS NON-DETERMINISM AND LOOK-AHEAD IN A FITTED MODEL — a model that trains
  on its own test window, or whose params depend on hash-map order / thread count / wall clock, is a
  silent correctness failure that the suite must catch by construction (M1, M2).
The gates: same data+seed => byte-identical params AND signal (M1); fit-on-trailing/apply-forward,
  truncation-invariant (M2); trained inside S1 CPCV, deflated, no-edge => rejected (M3); each kernel
  bounded-differential vs a reference (M4); engine builds no general primitive — elastic-net/GBT/HMM are
  engine-local-then-lifted Pattern-B edges (M5); a learned model admits through library::admit like any
  alpha (M6); zero hot-path alloc in evaluate() (M7); NaN verbatim, no survivorship (M8).
No UB, no hidden look-ahead, no second Sharpe/metric definition (delegate to combine/eval). Use ATX_CHECK
  (not ATX_ASSERT) wherever a deref/write/OOB sits OUTSIDE the condition — it must hold under NDEBUG
  (the S4-5 21d7ae1 lesson). Header-only inline (#pragma once), namespace atx::engine::learn. Functions
  <= ~60 lines, one purpose. enum class + strong-id structs + atx vocabulary types {u8,u32,u64,usize,f64}.
  // SAFETY: on every span aliasing / reinterpret. Result<T>/Status for expected failure (.has_value(),
  NOT .ok()). Exhaustive switch on enum class (no default).
Build gate: cmake --build build --config Debug --target atx-engine-tests (/W4 /permissive- /WX, /fp:precise,
  -ffp-contract=off) + ctest --test-dir build -R <Suite> --output-on-failure. Tests/benches auto-globbed —
  do NOT hand-edit CMakeLists.txt. Bench needs -DATX_BUILD_BENCH=ON.
Shared-branch discipline: branch feat/atx-engine-learn off feat/atx-core-stdlib (or in-place on
  feat/atx-core-stdlib per the run's choice); stage EXPLICIT pathspecs only, never git add -A/-u; no push.
End commit messages with: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## §4 — Architecture & algorithms

Pseudocode (informal; `:=` assign, `->` member, `// §` plan refs). Compilable C++ lives only in §5 `TEST(...)` blocks. All matrices are **column-major** (atx-core/Eigen convention); the feature design matrix `X` is `n_rows × n_features` with `n_rows = n_used_dates × n_instruments`.

### 4.1 Feature matrix + multi-horizon labels (`feature_matrix.hpp`, S5-1)

```cpp
struct FeatureSpec {                       // what columns the tensor holds, in fixed order
  std::vector<std::string> raw_fields;     // Panel field names, e.g. {"close","volume","ret1"}
  std::vector<combine::AlphaId> pool_alphas; // stored library/pool alphas used as features (their cross-sections)
  std::vector<u16> horizons;               // H, forward-return label horizons (days); default {1,5,21}
  u16 max_lookback;                        // largest history any feature reads (for PIT bound)
};

struct FeatureMatrix {                     // immutable, deterministic, PIT
  usize n_dates, n_instruments, n_features;
  std::vector<usize> row_date;             // row -> date index (n_rows entries)
  std::vector<usize> row_inst;             // row -> instrument index
  std::vector<f64> X;                      // [n_rows * n_features], row-major-by-row, col = feature
  std::vector<std::vector<f64>> Y;         // Y[h] = [n_rows] forward-return label at horizon H[h]; NaN where unknowable
  std::vector<u8>  row_valid;              // 1 iff all features finite at this row (NaN policy, M8)
};

// PIT build: feature at (date d, inst i) reads ONLY rows with date <= d. Raw fields straight from the
// panel cross-section; pool-alpha features from the already-aligned stored stream. Labels read FORWARD
// (that is the prediction target, never a feature) and are NaN for d+h >= n_dates (M8, not zero).
FeatureMatrix build_features(const alpha::Panel& panel,
                             const combine::AlphaStore& pool,   // or library streams (§0.1)
                             const FeatureSpec& spec):
  for d in 0..n_dates, for i in 0..n_instruments:
    if not panel.in_universe(d, i): continue            // PIT universe (M8)
    row := emit_row(d, i)
    for f in spec.raw_fields:  X[row, f] := panel.field_cross_section(field_id(f), d)[i]   // <= d only
    for a in spec.pool_alphas: X[row, a] := pool.positions(a, d)[i]  // stored, PIT-aligned
    row_valid[row] := all_finite(X[row, :])             // M8: NaN -> row excluded from fits, not zeroed
    for h in spec.horizons:
      Y[h][row] := (d + H[h] < n_dates) ? fwd_return(panel, i, d, H[h]) : NaN   // forward, never a feature
  // fwd_return = close[d+h]/close[d] - 1, read from the panel's close field (a label, computed once)
```

**Determinism (M1):** rows emitted in `(date, instrument)` order; no map iteration. **Firewall (M2):** features read `≤ d`, labels read `> d` and are never fed back as features. **NaN (M8):** `row_valid=0` rows are dropped from any fit, not zero-filled.

### 4.2 Latent / hidden features (`latent.hpp`, S5-2)

Two higher-order feature generators, both PIT (§0.5):

```cpp
// (a) PCA latent factors — "hidden features" via trailing-window factor extraction.
struct LatentBasis { core::pca::PcaResult model; usize fit_upto_date; u32 k; };
// the bundle the linear model consumes: latent factors (optional) + selected interaction pairs.
struct LatentAugmentation { std::optional<LatentBasis> pca; std::vector<std::pair<u32,u32>> interactions; };

LatentBasis fit_latent(const FeatureMatrix& fm, usize t, u16 embargo, u32 k):
  rows := { r : fm.row_date[r] <= t - embargo and fm.row_valid[r] }   // trailing only (M2)
  Xtr  := gather(fm.X, rows)                                          // n_tr x n_features, col-major MatX
  model := core::pca::pca(as_matrix(Xtr, n_tr, n_features), k)        // atx-core L7 (§2.1)
  return { model, t, k }

// apply forward: latent factor scores for ALL rows at dates in (t-embargo, ...], same basis
Matrix apply_latent(const LatentBasis& b, const FeatureMatrix& fm, rows):
  return core::pca::transform(b.model, as_matrix(gather(fm.X, rows), |rows|, n_features))  // n x k scores

// (b) Bounded pairwise interaction terms — explicit higher-order features for the LINEAR model
//     (GBT finds interactions natively; the linear model needs them handed to it).
//     Only the top-m features by trailing |IC| are crossed, capping the blow-up at C(m,2).
std::vector<std::pair<u32,u32>> select_interactions(const FeatureMatrix& fm, usize t, u16 embargo, u32 m):
  ic[f] := |spearman(fm.X[:,f], fm.Y[0])| over trailing valid rows   // fit-on-trailing (M2)
  top   := argsort_desc(ic).take(m)                                  // atx-core algo::argsort_desc
  return { (a,b) : a<b in top }                                      // deterministic, order-fixed
  // interaction feature value = standardize(X[:,a]) * standardize(X[:,b])
```

`k` (PCA factors kept) and `m` (interaction base) are config knobs; `k=0`/`m=0` disables each. The augmented matrix `X' = [X | latent | interactions]` is what the linear model consumes.

### 4.3 Linear learned alpha + horizon blend (`elastic_net.hpp` + `linear_alpha.hpp`, S5-3)

**Elastic-net (Pattern-B edge 1)** — cyclic coordinate descent, RNG-free, on standardized columns:

```cpp
struct ElasticNetCfg { f64 lambda; f64 alpha; usize max_iter=1000; f64 tol=1e-8; };
// objective: (1/2n)||y - Xb||^2 + lambda*( alpha*||b||_1 + (1-alpha)/2*||b||_2^2 )
VecX elastic_net(const MatX& Xs, const VecX& y, const ElasticNetCfg& c):   // Xs column-standardized
  b := 0; precompute col_norm2[j] := sum_i Xs[i,j]^2
  repeat until max_iter or max|Δb| < tol:                       // deterministic cyclic sweep (M1)
    for j in 0..p (fixed order):
      r_j := Xs[:,j] . (y - Xs*b + Xs[:,j]*b[j])                // partial residual (simd::dot)
      b[j] := soft_threshold(r_j / n, lambda*alpha) /
              (col_norm2[j]/n + lambda*(1-alpha))               // closed-form coordinate update
  return b
// soft_threshold(z, g) := sign(z)*max(|z|-g, 0)
// alpha=0  => pure ridge  (M4: must match core::regression::ridge to ULP-class on the same standardization)
// alpha=1  => pure lasso
```

**Cross-sectional linear alpha**, walk-forward inside CPCV, **one fit per horizon**, then blended:

```cpp
struct LinearAlphaCfg { ElasticNetCfg en; bool use_ridge_baseline; CpcvConfig cpcv; u64 master_seed; };

LearnedModel fit_linear(const FeatureMatrix& fm, const LatentAugmentation& aug, const LinearAlphaCfg& cfg):
  folds := expand_date_folds(cpcv_folds(date_label_spans(fm), cfg.cpcv), fm)   // §0.2
  for h in horizons:
    for fold in folds:                                                          // S1 CPCV (M3)
      Xtr,ytr := rows(fm, aug, fold.train_idx, h, valid_only)                   // M2: train dates only
      b_h_fold := cfg.use_ridge_baseline ? core::regression::ridge(Xtr,ytr,cfg.en.lambda).beta
                                          : elastic_net(standardize(Xtr), ytr, cfg.en)
      oos_pred[h][fold] := apply(b_h_fold, rows(fm, aug, fold.test_idx, h))     // apply forward only
      trial_count += 1                                                          // §0.3 deflation N
    b_h := refit_on_full_trailing(fm, aug, h)             // the deployed coeff (fit-on-trailing)
    ic_h := oos_information_coefficient(oos_pred[h], fm.Y[h])
  blend_w := normalize(max(ic_h, 0) over h)               // §0.6 horizon blend, fit-on-trailing (M2)
  return LearnedModel{ kind=Linear, coeffs=b_h per horizon, blend_w, trial_count, aug }

// emitted cross-section at date d: pred(d,i) = Σ_h blend_w[h] * (X'[row(d,i)] . b_h)
```

### 4.4 Gradient-boosted-tree alpha (`gbt.hpp`, S5-4)

Deterministic histogram GBT (Pattern-B edge 2). Captures feature **interactions** (the user's "higher-order / hidden features" via trees, complementary to PCA):

```cpp
struct GbtCfg { u32 n_trees; u32 max_depth; u32 n_bins=64; f64 learning_rate=0.05;
                f64 row_subsample=0.7; f64 feature_subsample=0.7; f64 min_child=1e-3;
                f64 l2=1.0; u64 master_seed; CpcvConfig cpcv; };

// histogram split: pre-bin each feature into n_bins fixed quantile edges (computed on TRAIN only, M2).
SplitFinder build_histograms(Xtr, bin_edges):  // per node: accumulate (Σg, Σh) per (feature, bin)
best_split(node):                              // gain = 0.5*( GL^2/(HL+l2) + GR^2/(HR+l2) - G^2/(H+l2) )
  for f in subsampled_features (seeded order): // deterministic: seed = mix(master_seed, tree, depth)
    scan bins left->right; track best gain; FIRST-MAX tie-break (M1, no time-dependence)
  return argmax_gain

LearnedModel fit_gbt(const FeatureMatrix& fm, const GbtCfg& cfg):
  rng := Xoshiro256pp(seed_for(cfg.master_seed, "gbt", 0, 0))
  for h in horizons:
    for fold in expand_date_folds(...):        // CPCV, deflated (M3); trial_count += per fit (§0.3)
      F := 0 (init prediction)
      for t in 0..n_trees:
        g,hh := grad_hess(squared_error, ytr, F)            // gradients
        rows := seeded_subsample(rng, cfg.row_subsample)     // M1: seeded, reproducible
        tree := grow(rows, build_histograms, best_split, cfg)
        F += cfg.learning_rate * tree.predict(Xtr)
      score OOS on fold.test_idx
    deployed[h] := refit_on_full_trailing(...)               // fit-on-trailing (M2)
  blend_w := horizon_blend(...)                              // §0.6
  return LearnedModel{ kind=Gbt, trees per horizon, blend_w, trial_count }
```

**Determinism (M1):** bin edges from train quantiles (deterministic), feature/row subsampling seeded, first-max tie-break, fixed tree-traversal order — a GBT fit is byte-identical across builds/threads. **Anti-overfit (M3):** GBT overfits hardest, so it carries the strictest deflation bar and the smallest default `n_trees`/`max_depth`.

### 4.5 HMM regime detector (`hmm.hpp`, S5-5)

Log-space Baum-Welch (Pattern-B edge 3) over a low-dim market-state observable (e.g. cross-sectional dispersion + trailing market vol), RenTech's noisy-channel heritage:

```cpp
struct HmmCfg { u32 n_states=3; u32 max_iter=100; f64 tol=1e-6; u64 master_seed; };
struct Hmm { MatX logA; /*NxN*/ std::vector<Gaussian> emit; VecX logpi; };   // diagonal-Gaussian emissions

Hmm baum_welch(const MatX& obs /*T x d*/, const HmmCfg& cfg):
  hmm := seeded_init(Xoshiro256pp(seed_for(cfg.master_seed,"hmm",0,0)), obs)  // M1 seeded init
  repeat until converged or max_iter:
    (logalpha, ll_fwd) := forward_log(hmm, obs)     // log-space, logsumexp (no underflow)
    logbeta            := backward_log(hmm, obs)
    gamma, xi          := posteriors(logalpha, logbeta)   // E-step, normalized in log-space
    hmm                := m_step(gamma, xi, obs)           // re-estimate A, emissions, pi
    assert ll_fwd == backward_loglik (within tol)         // M4 identity check
  return hmm

// PIT regime posterior (M2): fit on obs[0 .. t-embargo], then filter FORWARD (no future) for d > t.
VecX regime_posterior_at(const Hmm& fitted, const MatX& obs, usize d):   // returns P(state | obs[0..d])
  return forward_filter(fitted, obs[0..d]).last_normalized()             // forward-only, causal
```

The fitted HMM is **applied forward only** — at date `d` the regime posterior uses `obs[0..d]`, never beyond. The posterior feeds the regime-conditional ensemble (§4.7).

### 4.6 LearnedSignalSource — the ISignalSource adapter (`learned_source.hpp`, S5-3)

```cpp
class LearnedSignalSource final : public combine::ISignalSource {
  LearnedModel model_;            // fitted coeffs/trees/blend (immutable after fit)
  FeatureSpec  spec_;             // how to rebuild the feature row PIT at eval time
  LatentBasis  latent_;           // the fitted PIT basis (or empty)
  mutable std::vector<f64> scratch_;   // pre-sized; zero alloc per date (M7)
public:
  Result<combine::SignalView> evaluate(combine::PanelView panel) override {  // emit cross-section at the panel date
    rebuild_feature_row(panel, spec_, latent_, scratch_);   // PIT: reads <= current date (M2)
    for i in instruments: out_[i] := Σ_h model_.blend_w[h] * predict_h(model_, h, scratch_, i);
    return SignalView{ out_ };                               // a cross-section, just like any alpha
  }
  usize max_lookback() const noexcept override { return spec_.max_lookback; }
};

// OPTIONAL live re-eval adapter (§0.1) — only for deploying a model whose features include FORMULAIC
// alphas, over a brand-new panel: re-parse Provenance.expr_source -> compile -> alpha::Engine::evaluate.
// Built in S5-6 behind a feature flag; the training path never needs it.
```

### 4.7 Stacking mega-combiner — alphas interact to produce a new alpha (`ensemble.hpp`, S5-6)

The crown jewel and the user's "how alphas interact to produce new alphas." The pool's alphas (formulaic **and** learned) become the **meta-features**; a learned meta-model produces a new cross-section — an alpha *of* alphas — that is admitted only if it beats the linear combiner OOS-after-deflation:

```cpp
enum class AdmitKind : u8 { Accept, RejectFitness };   // learn-domain stacking verdict (distinct from library::AdmitKind)
struct StackingCfg { enum Base { ElasticNet, Gbt } base; HmmCfg regime_cfg; CpcvConfig cpcv;
                     CombinerConfig linear_benchmark{ .method=ShrinkageMv }; u64 master_seed; };

struct StackingVerdict { bool admitted; f64 oos_dsr_nonlinear; f64 oos_dsr_linear; learn::AdmitKind reason; };

StackingVerdict fit_stack(const combine::AlphaStore& pool, const learn::Hmm* regime /*nullable*/,
                          const StackingCfg& cfg):
  // meta-feature matrix: column per pool alpha = its cross-section; rows over (date,inst), PIT.
  M := meta_features_from_pool(pool)                          // alpha-of-alphas inputs
  for fold in cpcv_folds(...):                                // S1 CPCV (M3)
    if regime: per-regime fit (regime-conditional weights, §4.5 posterior gates the train rows)
    meta_nonlinear := fit_base(cfg.base, M[train], y[train]); trial_count += 1
    meta_linear    := combine::AlphaCombiner{cfg.linear_benchmark}.fit(pool, fb, fe)   // P4 benchmark
    score OOS Sharpe of each on fold.test_idx
  (sr_nl, sr_lin) := mean OOS Sharpe nonlinear / linear
  dsr_nl  := eval::deflated_sharpe(sr_nl,  T, skew, exkurt, trial_count, var).dsr      // (M3, §0.3)
  dsr_lin := eval::deflated_sharpe(sr_lin, T, skew, exkurt, /*N=*/1, var).dsr          // benchmark, pre-registered
  admitted := (dsr_nl > 0) and (sr_nl > sr_lin)              // nonlinear must BEAT linear OOS (M3)
  return { admitted, dsr_nl, dsr_lin, admitted?Accept:RejectFitness }
  // on admit: wrap as LearnedSignalSource, library.admit(AlphaCandidate{ prov.expr_source="learned:stack", ...})
```

**Regime-conditional (§4.5):** when `regime` is supplied, the meta-model is fit per inferred regime and the deployed blend selects weights by the **PIT regime posterior** at each date — different combination in quiet/trending/stressed states (RenTech §4.2). **The gate is non-vacuous:** on a fixture where the nonlinear model has no real edge over the linear one, `admitted=false`.

### 4.8 Training scaffold (`train.hpp`, S5-1 → grows)

```cpp
// §0.2 — one LabelSpan per DATE, then expand a date-fold to feature rows.
std::vector<LabelSpan> date_label_spans(const FeatureMatrix& fm, u16 horizon):
  return { LabelSpan{ d, min(d + horizon, n_dates) } : d in used_dates }

Folds expand_date_folds(std::vector<CpcvFold> date_folds, const FeatureMatrix& fm):
  for each date_fold: train_rows := { r : fm.row_date[r] in date_fold.train_idx and fm.row_valid[r] }
                      test_rows  := { r : fm.row_date[r] in date_fold.test_idx  and fm.row_valid[r] }

// §0.7 — fixed seed derivation (mirrors factory seed_for): SplitMix mix of (master_seed, tag_hash, a, b).
u64 seed_for(u64 master, std::string_view tag, usize a, usize b)

// §0.3 — the deflation N is the running count of distinct fits scored; passed to deflated_sharpe as N.
struct TrialCounter { usize n=0; usize next(){ return ++n; } };
```

---

## §5 — Per-unit plan

Sequential dispatch (each unit consumes the prior). Fresh implementer → spec-compliance review → code-quality review → fix loop → ledger SHA, per `superpowers:subagent-driven-development`. **Branch `feat/atx-engine-learn` off `feat/atx-core-stdlib` → explicit-pathspec commits** (handoff block). Suggested split: **S5-a** = S5-0…S5-4, **S5-b** = S5-5…S5-7.

### Task S5-0: Marker + ledger + scaffold + atx-core L7 link smoke-test
**Files:** Create `learn/fwd.hpp`; Create `atx-engine/plans/p1/sprint-5-progress.md`; Test `tests/learn_scaffold_test.cpp`.
**Scope:** §2.4 + §0.8. Open the ledger, scaffold the layer doc block (M1–M8 + the seam map), and **retire the Eigen-link risk** before any model unit. **First verify** that `core::regression::ridge`, `core::pca::pca`, and `core::random::Xoshiro256pp` are includable from the engine test target (the real header paths under `atx/core/`).

- [ ] **Step 1 (scaffold tests):** suite `LearnScaffold` —
```cpp
#include <gtest/gtest.h>
#include "atx/core/regression.hpp"   // First verify the real path; adjust include to as-built
#include "atx/core/pca.hpp"
#include "atx/core/random.hpp"
#include "atx/engine/learn/fwd.hpp"

namespace {
using atx::f64;

TEST(LearnScaffold, AtxCoreRidge_TwoByTwo_Solves) {            // §0.8 link smoke-test
  // y = 2*x1 + 0*x2 ; ridge with tiny lambda recovers ~2,0
  const std::vector<f64> xdata{1, 0, 2, 0, 3, 0};             // 3x2 col-major: col0={1,2,3}, col1={0,0,0}
  const std::vector<f64> ydata{2, 4, 6};
  auto X = atx::core::linalg::as_matrix(xdata, 3, 2);
  auto y = atx::core::linalg::as_vector(ydata);
  auto r = atx::core::regression::ridge(X, y, 1e-6);
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->beta[0], 2.0, 1e-3);
}

TEST(LearnScaffold, AtxCorePca_Includable_FitsRankOne) {      // §0.8 pca link
  const std::vector<f64> xdata{1, 2, 3, 2, 4, 6};             // 3x2, col1 = 2*col0  => rank-1
  auto X = atx::core::linalg::as_matrix(xdata, 3, 2);
  auto p = atx::core::pca::pca(X, 1);
  ASSERT_TRUE(p.has_value());
  EXPECT_GT(p->explained_ratio[0], 0.99);                     // one factor explains ~all variance
}

TEST(LearnScaffold, Xoshiro_SameSeed_SameSequence) {          // M1 RNG determinism precondition
  atx::core::random::Xoshiro256pp a{42}, b{42};
  EXPECT_EQ(a.next_u64(), b.next_u64());
}
} // namespace
```
- [ ] **Step 2:** Build → FAIL (`learn/fwd.hpp` missing; confirm the atx-core include paths resolve — if a path is wrong, fix the include, NOT the test intent).
- [ ] **Step 3:** Create `learn/fwd.hpp`: `#pragma once`, `namespace atx::engine::learn {}` with the doc block (the M1–M8 rules verbatim as a comment, the seam map, the determinism contract, the three Pattern-B edges). Forward-declare `struct FeatureMatrix; struct FeatureSpec; class LearnedSignalSource; struct LearnedModel;`.
- [ ] **Step 4:** `ctest --test-dir build -R LearnScaffold` → 3/3 pass. **If `AtxCoreRidge_*` fails to LINK**, the Eigen transitive link is broken — surface it now (§0.8) and stop for guidance before S5-1.
- [ ] **Step 5:** Open `sprint-5-progress.md` (copy `sprint-4-progress.md` structure): header (Sprint, Branch `feat/atx-engine-learn`, Base `feat/atx-core-stdlib @ <HEAD-sha>`, Status `🚧 OPEN`), the §0 amendment summary (1–8), kickoff risks `(a)` Pattern-B atx-core edges, `(b)` Eigen-link, `(c)` shared-branch discipline, `(d)` **dominant risk: non-determinism/look-ahead in a fitted model**.
- [ ] **Step 6:** Commit: `git add -- atx-engine/plans/p1/sprint-5-progress.md atx-engine/include/atx/engine/learn/fwd.hpp atx-engine/tests/learn_scaffold_test.cpp; git commit -- <them> -m "docs(s5-0): open sprint-5 learned-signals ledger + scaffold + atx-core L7 link smoke-test" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"; git show HEAD --stat`.

### Task S5-1: PIT feature matrix + multi-horizon labels
**Files:** Create `learn/feature_matrix.hpp`, `learn/train.hpp`; Test `tests/feature_matrix_test.cpp`.
**Scope:** §4.1/§4.8 + §0.1/§0.2/§0.6 + M2/M8. The deterministic, PIT `(date×instrument)×feature` tensor + forward-return labels at `H`. **First verify** `alpha::Panel::field_cross_section(FieldId,DateIdx)`, `Panel::field_id(string_view)`, `Panel::in_universe(DateIdx,usize)`, `combine::AlphaStore::positions(AlphaId,usize)` exact signatures + the `eval::cpcv_folds`/`LabelSpan`/`CpcvFold` shapes.

- [ ] **Step 1 (feature tests):** suite `FeatureMatrix` —
```cpp
TEST(FeatureMatrix, ForwardLabel_IsForwardReturn_NaNAtTail) {        // §0.6 + M8
  auto panel = make_panel_close({ {10,20},{11,22},{12,24} });        // 3 dates x 2 insts, close field
  learn::FeatureSpec spec; spec.raw_fields={"close"}; spec.horizons={1};
  auto fm = learn::build_features(panel, /*pool*/{}, spec);
  // y_1 at (date0,inst0) = close[1]/close[0]-1 = 11/10-1 = 0.1
  EXPECT_NEAR(fm.Y[0][fm.row_of(0,0)], 0.1, 1e-12);
  // last date has no forward bar -> NaN, NOT zero (M8)
  EXPECT_TRUE(std::isnan(fm.Y[0][fm.row_of(2,0)]));
}

TEST(FeatureMatrix, Feature_ReadsOnlyUpToDate_TruncationInvariant) { // M2
  auto full  = make_panel_close({ {10,20},{11,22},{12,24},{13,26} });
  auto trunc = make_panel_close({ {10,20},{11,22},{12,24} });        // drop last date
  learn::FeatureSpec spec; spec.raw_fields={"close"}; spec.horizons={1};
  auto a = learn::build_features(full, {}, spec);
  auto b = learn::build_features(trunc, {}, spec);
  // feature value at (date1,inst0) must be identical with/without date3 present (features read <= d)
  EXPECT_EQ(a.X[a.row_of(1,0)*a.n_features + 0], b.X[b.row_of(1,0)*b.n_features + 0]);
}

TEST(FeatureMatrix, OutOfUniverseCell_Excluded_NotZeroed) {          // M8
  auto panel = make_panel_close_universe({ {10,20},{11,22} }, /*mask*/{ {1,0},{1,1} });
  learn::FeatureSpec spec; spec.raw_fields={"close"}; spec.horizons={1};
  auto fm = learn::build_features(panel, {}, spec);
  EXPECT_FALSE(fm.has_row(0,1));                                     // (date0,inst1) out-of-universe: no row
}

TEST(FeatureMatrix, RowOrder_IsDateThenInstrument_Deterministic) {  // M1
  auto panel = make_panel_close({ {10,20},{11,22} });
  auto fm = learn::build_features(panel, {}, learn::FeatureSpec{{"close"},{},{1},1});
  EXPECT_LT(fm.row_of(0,0), fm.row_of(0,1));
  EXPECT_LT(fm.row_of(0,1), fm.row_of(1,0));
}
```
- [ ] **Step 2:** Build → FAIL.
- [ ] **Step 3:** Implement `feature_matrix.hpp` (§4.1) + the `date_label_spans`/`expand_date_folds`/`seed_for`/`TrialCounter` scaffold in `train.hpp` (§4.8). Functions ≤60 lines; `// SAFETY:` on any span aliasing the panel.
- [ ] **Step 4:** `ctest --test-dir build -R FeatureMatrix` → pass.
- [ ] **Step 5 (fold-expansion tests):** suite `TrainScaffold` —
```cpp
TEST(TrainScaffold, DateFold_ExpandsToRows_NoTestDateInTrain) {     // §0.2
  auto fm = small_two_inst_fixture(/*dates*/6);
  eval::CpcvConfig cfg; cfg.n_groups=3; cfg.n_test_groups=1; cfg.embargo=0.0;
  auto folds = learn::expand_date_folds(eval::cpcv_folds(learn::date_label_spans(fm,1), cfg), fm);
  for (auto& f : folds)
    for (auto tr : f.train_rows)
      for (auto te : f.test_rows)
        EXPECT_NE(fm.row_date[tr], fm.row_date[te]);                 // no train row shares a test date
}
```
- [ ] **Step 6:** Build → FAIL. **Step 7:** Implement the fold expansion (§4.8). **Step 8:** `ctest --test-dir build -R "FeatureMatrix|TrainScaffold"` → pass; full suite green. **Step 9:** Commit + ledger row (`feat(s5-1): PIT feature matrix + multi-horizon labels + CPCV date-fold scaffold`).

### Task S5-2: Latent / hidden-feature extraction (PIT PCA + interactions)
**Files:** Create `learn/latent.hpp`; Test `tests/latent_test.cpp`.
**Scope:** §4.2 + §0.5 + M2/M4. Trailing-fit PCA factors via `core::pca` + bounded interaction terms — the user's "higher-order / hidden features." **First verify** `core::pca::pca` returns `PcaResult{mean,components,explained_variance,explained_ratio}` and `core::pca::transform(model,X)`; `core::linalg::as_matrix(span,rows,cols)` column-major.

- [ ] **Step 1 (latent tests):** suite `Latent` —
```cpp
TEST(Latent, PcaFactor_RecoversKnownDirection) {                    // M4 (vs known rank-1 structure)
  auto fm = collinear_feature_fixture();                            // feat1 = 2*feat0 + noise
  auto basis = learn::fit_latent(fm, /*t=*/fm.n_dates-1, /*embargo=*/0, /*k=*/1);
  EXPECT_GT(basis.model.explained_ratio[0], 0.95);                  // first factor dominates
}

TEST(Latent, PcaBasis_FitOnTrailing_TruncationInvariant) {          // M2 / §0.5
  auto fm = feature_fixture(/*dates*/8);
  auto t = 5u;
  auto b_short = learn::fit_latent(truncate(fm, t+1), t, 0, 2);     // data up to t only
  auto b_long  = learn::fit_latent(fm, t, 0, 2);                    // same trailing window, more future data present
  // basis fit on <= t-embargo must be identical regardless of > t rows
  EXPECT_EQ(hash_components(b_short), hash_components(b_long));
}

TEST(Latent, Interactions_SelectTopByIC_Deterministic) {           // M1 / §4.2(b)
  auto fm = feature_fixture(8);
  auto p1 = learn::select_interactions(fm, 6, 0, /*m=*/3);
  auto p2 = learn::select_interactions(fm, 6, 0, 3);
  EXPECT_EQ(p1, p2);                                                // same input -> same pairs, fixed order
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `latent.hpp` (§4.2): `fit_latent` (gather trailing valid rows → `as_matrix` → `core::pca::pca`), `apply_latent` (`core::pca::transform`), `select_interactions` (trailing |IC| via `core::algo::argsort_desc`). **Step 4:** `ctest --test-dir build -R Latent` → pass.
- [ ] **Step 5:** Commit + ledger row (`feat(s5-2): PIT latent factor + interaction feature extraction (hidden features)`).

### Task S5-3: Linear learned alpha (elastic-net + ridge) + horizon blend + ISignalSource
**Files:** Create `learn/elastic_net.hpp`, `learn/learned_source.hpp`, `learn/linear_alpha.hpp`; Test `tests/elastic_net_test.cpp`, `tests/linear_alpha_test.cpp`.
**Scope:** §4.3/§4.6 + §0.3/§0.6 + M1/M2/M3/M4/M5/M6. The Pattern-B elastic-net kernel + the cross-sectional walk-forward learned alpha + the per-horizon blend + the `ISignalSource` adapter. **First verify** `combine::ISignalSource` (the `Result<SignalView> evaluate(PanelView)` + `usize max_lookback()` signature in `loop/signal_source.hpp`), `combine::compute_metrics`, `eval::deflated_sharpe(sr,T,skew,exkurt,N,var)`.

- [ ] **Step 1 (elastic-net kernel tests):** suite `ElasticNet` —
```cpp
TEST(ElasticNet, AlphaZero_MatchesAtxCoreRidge) {                   // M4 (differential vs atx-core)
  auto [Xs, y] = standardized_design_fixture(/*n*/40, /*p*/5);
  learn::ElasticNetCfg c{ .lambda=0.1, .alpha=0.0, .max_iter=5000, .tol=1e-10 };
  auto b_en  = learn::elastic_net(Xs, y, c);
  auto X = atx::core::linalg::as_matrix(Xs.data(), 40, 5);
  auto r  = atx::core::regression::ridge(X, atx::core::linalg::as_vector(y), 0.1*40); // match penalty scaling
  ASSERT_TRUE(r.has_value());
  for (int j=0;j<5;++j) EXPECT_NEAR(b_en[j], r->beta[j], 1e-4);     // pure-L2 path == ridge
}

TEST(ElasticNet, OrthonormalX_MatchesSoftThreshold) {               // M4 (closed form)
  auto [Xs, y] = orthonormal_design_fixture(/*n*/8, /*p*/8);        // X^T X = I
  learn::ElasticNetCfg c{ .lambda=0.2, .alpha=1.0 };                // pure lasso
  auto b = learn::elastic_net(Xs, y, c);
  // for orthonormal X, lasso beta_j = soft_threshold(ols_j, lambda)
  for (int j=0;j<8;++j) EXPECT_NEAR(b[j], soft_threshold(ols_coef(Xs,y,j), 0.2), 1e-6);
}

TEST(ElasticNet, SameInput_Deterministic_ByteIdentical) {          // M1
  auto [Xs, y] = standardized_design_fixture(40,5);
  auto a = learn::elastic_net(Xs, y, {0.1,0.5});
  auto b = learn::elastic_net(Xs, y, {0.1,0.5});
  EXPECT_EQ(hash_vec(a), hash_vec(b));                              // CD is RNG-free + cyclic -> bit-identical
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `elastic_net.hpp` (§4.3): cyclic CD with soft-threshold, `simd::dot` partial residuals, fixed-order sweep. **Step 4:** `ctest --test-dir build -R ElasticNet` → pass.
- [ ] **Step 5 (learned-alpha tests):** suite `LinearAlpha` —
```cpp
TEST(LinearAlpha, NoEdgePanel_RejectedByDeflation) {               // M3 (non-vacuous anti-snooping)
  auto fm = pure_noise_feature_fixture(/*seed*/7);                  // labels independent of features
  auto model = learn::fit_linear(fm, /*aug*/{}, default_linear_cfg());
  auto dsr = learn::oos_deflated_sharpe(model, fm);                 // N = trial_count (§0.3)
  EXPECT_LE(dsr, 0.0);                                              // no real edge => deflated <= 0
}

TEST(LinearAlpha, RealSignalPanel_PositiveDeflatedSharpe) {        // M3 (non-vacuous: real edge survives)
  auto fm = planted_signal_feature_fixture(/*seed*/7);             // y = 0.3*f0 + noise
  auto model = learn::fit_linear(fm, {}, default_linear_cfg());
  EXPECT_GT(learn::oos_deflated_sharpe(model, fm), 0.0);
}

TEST(LinearAlpha, FitOnTrailing_TruncationInvariant) {             // M2
  auto fm = planted_signal_feature_fixture(7);
  auto pred_short = learn::predict_at(learn::fit_linear(truncate_dates(fm, 30), {}, cfg()), 25);
  auto pred_long  = learn::predict_at(learn::fit_linear(fm, {}, cfg()), 25);   // more future data
  EXPECT_EQ(hash_vec(pred_short), hash_vec(pred_long));            // prediction at date 25 unchanged
}

TEST(LinearAlpha, SameSeed_ByteIdenticalModelAndSignal) {         // M1 (params AND signal)
  auto fm = planted_signal_feature_fixture(7);
  auto m1 = learn::fit_linear(fm, {}, cfg_seed(123));
  auto m2 = learn::fit_linear(fm, {}, cfg_seed(123));
  EXPECT_EQ(hash_model(m1), hash_model(m2));
  EXPECT_EQ(hash_vec(learn::predict_at(m1,20)), hash_vec(learn::predict_at(m2,20)));
}

TEST(LinearAlpha, HorizonBlend_WeightsNonNegative_SumToOne) {     // §0.6
  auto fm = planted_signal_feature_fixture(7);                    // horizons {1,5,21}
  auto m = learn::fit_linear(fm, {}, cfg_horizons({1,5,21}));
  f64 s=0; for (auto w : m.blend_w){ EXPECT_GE(w,0.0); s+=w; }
  EXPECT_NEAR(s, 1.0, 1e-12);
}

TEST(LinearAlpha, EmitsAsISignalSource_CrossSectionLength) {     // M6
  auto fm = planted_signal_feature_fixture(7);
  learn::LearnedSignalSource src{ learn::fit_linear(fm, {}, cfg()), spec_of(fm), {} };
  auto sv = src.evaluate(panel_view_at(fm, /*date*/20));
  ASSERT_TRUE(sv.has_value());
  EXPECT_EQ(sv->values.size(), fm.n_instruments);                 // a cross-section, like any alpha
}
```
- [ ] **Step 6:** Build → FAIL. **Step 7:** Implement `learned_source.hpp` (§4.6: `LearnedSignalSource` with pre-sized `scratch_`, M7) + `linear_alpha.hpp` (§4.3: CPCV walk-forward per horizon, `eval::cpcv_folds` + `eval::deflated_sharpe`, horizon blend, `compute_metrics` for `AlphaMetrics`). **Step 8:** `ctest --test-dir build -R "ElasticNet|LinearAlpha"` → pass; full suite green. **Step 9:** Commit + ledger row (`feat(s5-3): linear learned alpha (elastic-net CD + ridge), per-horizon blend, ISignalSource adapter`).

### Task S5-4: Deterministic gradient-boosted-tree learned alpha
**Files:** Create `learn/gbt.hpp`; Test `tests/gbt_test.cpp`.
**Scope:** §4.4 + M1/M3/M4/M5. The Pattern-B histogram GBT — captures feature interactions (higher-order structure), strictest deflation. **First verify** the `LearnedModel` shape from S5-3 (so the GBT model reuses the same `LearnedSignalSource` emit path) and `core::random::Xoshiro256pp` draw API.

- [ ] **Step 1 (GBT tests):** suite `Gbt` —
```cpp
TEST(Gbt, DepthOneStump_MatchesBruteForceThreshold) {              // M4 (differential vs exact)
  auto [X,y] = one_feature_step_fixture();                         // y jumps at x=0.5
  auto stump = learn::fit_gbt_single_tree(X, y, /*max_depth*/1, /*n_bins*/256);
  auto exact = brute_force_best_threshold(X, y);                   // O(n^2) reference
  EXPECT_NEAR(stump.threshold(0), exact.threshold, /*one bin width*/ 1.0/256);
}

TEST(Gbt, SameSeed_ByteIdenticalTreesAndSignal) {                  // M1 (the make-or-break)
  auto fm = planted_interaction_fixture(/*seed*/9);                // y = sign(f0)*sign(f1) interaction
  auto a = learn::fit_gbt(fm, gbt_cfg_seed(55));
  auto b = learn::fit_gbt(fm, gbt_cfg_seed(55));
  EXPECT_EQ(hash_model(a), hash_model(b));
  EXPECT_EQ(hash_vec(learn::predict_at(a,30)), hash_vec(learn::predict_at(b,30)));
}

TEST(Gbt, CapturesInteraction_BeatsLinearOnXorFixture) {           // higher-order structure (user ask)
  auto fm = planted_interaction_fixture(9);                        // pure interaction, no marginal signal
  auto gbt = learn::fit_gbt(fm, gbt_cfg());
  auto lin = learn::fit_linear(fm, {}, cfg());                     // linear, NO interaction aug
  EXPECT_GT(learn::oos_ic(gbt, fm), learn::oos_ic(lin, fm));       // trees see what the linear model can't
}

TEST(Gbt, NoEdgePanel_RejectedByDeflation) {                       // M3 (overfits hardest -> must still reject)
  auto fm = pure_noise_feature_fixture(9);
  EXPECT_LE(learn::oos_deflated_sharpe(learn::fit_gbt(fm, gbt_cfg()), fm), 0.0);
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `gbt.hpp` (§4.4): fixed-bin histograms (train-quantile edges), seeded row/feature subsample, first-max tie-break, gradient/hessian on squared error, CPCV + deflation, reuse `LearnedModel`/`LearnedSignalSource`. **Step 4:** `ctest --test-dir build -R Gbt` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s5-4): deterministic histogram GBT learned alpha (interaction capture, strict deflation)`).

### Task S5-5: HMM regime detector (log-space Baum-Welch)
**Files:** Create `learn/hmm.hpp`; Test `tests/hmm_test.cpp`.
**Scope:** §4.5 + M1/M2/M4/M5. The Pattern-B log-space forward-backward + PIT regime posterior — RenTech's noisy-channel heritage. **First verify** nothing new (self-contained kernel); confirm `core::random::Xoshiro256pp` for seeded init.

- [ ] **Step 1 (HMM tests):** suite `Hmm` —
```cpp
TEST(Hmm, ForwardLogLik_EqualsBackwardLogLik) {                    // M4 (the algorithm's own identity)
  auto obs = two_state_toy_observations(/*T*/200, /*seed*/3);
  auto hmm = learn::baum_welch(obs, hmm_cfg(/*states*/2));
  auto [la, ll_fwd] = learn::forward_log(hmm, obs);
  auto ll_bwd = learn::backward_loglik(hmm, obs);
  EXPECT_NEAR(ll_fwd, ll_bwd, 1e-8);
}

TEST(Hmm, RecoversPlantedRegimes_AboveChance) {                    // M4 (vs known generator)
  auto [obs, truth] = planted_two_regime_series(/*T*/500, /*seed*/3);
  auto hmm = learn::baum_welch(obs, hmm_cfg(2));
  EXPECT_GT(regime_agreement(learn::viterbi_or_posterior(hmm, obs), truth), 0.8);
}

TEST(Hmm, Posterior_FitOnTrailing_Causal_TruncationInvariant) {    // M2 (PIT, the load-bearing one)
  auto obs = two_state_toy_observations(300, 3);
  auto hmm = learn::baum_welch(obs.first_n(200), hmm_cfg(2));       // fit on <= t only
  auto p_short = learn::regime_posterior_at(hmm, obs.first_n(150), 149);
  auto p_long  = learn::regime_posterior_at(hmm, obs, 149);         // future obs present
  EXPECT_EQ(hash_vec(p_short), hash_vec(p_long));                   // posterior at 149 uses obs[0..149] only
}

TEST(Hmm, SameSeed_ByteIdenticalParams) {                          // M1
  auto obs = two_state_toy_observations(200, 3);
  EXPECT_EQ(hash_hmm(learn::baum_welch(obs, hmm_cfg_seed(2))),
            hash_hmm(learn::baum_welch(obs, hmm_cfg_seed(2))));
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `hmm.hpp` (§4.5): log-space forward/backward (logsumexp), EM re-estimation, diagonal-Gaussian emissions, seeded init, `regime_posterior_at` forward-only filter. **Step 4:** `ctest --test-dir build -R Hmm` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s5-5): log-space Baum-Welch HMM + PIT regime posterior`).

### Task S5-6: Stacking mega-combiner (alpha-of-alphas) + regime-conditional + deflation gate
**Files:** Create `learn/ensemble.hpp`; Test `tests/ensemble_test.cpp`.
**Scope:** §4.7 + §0.4 + M3/M6. The nonlinear meta-model over pool alphas, admitted **only if it beats the P4 linear combiner OOS-after-deflation**; regime-conditional weighting; the optional live re-eval adapter. **First verify** `combine::AlphaCombiner::fit(pool, fit_begin, fit_end) → Result<Combination>`, `combine::CombinedSignalSource` ctor, `library::Library::admit(AlphaCandidate, AlphaGate)` + `AlphaCandidate`/`Provenance` fields.

- [ ] **Step 1 (ensemble tests):** suite `Ensemble` —
```cpp
TEST(Ensemble, NoNonlinearEdge_RejectedVsLinear) {                 // M3 / §0.4 (no ML-for-ML's-sake)
  auto pool = linearly_combinable_pool(/*seed*/11);                // best blend IS linear
  auto v = learn::fit_stack(pool, /*regime*/nullptr, stack_cfg());
  EXPECT_FALSE(v.admitted);                                        // nonlinear must not beat linear here
  EXPECT_EQ(v.reason, learn::AdmitKind::RejectFitness);
}

TEST(Ensemble, GenuineNonlinearEdge_AdmittedOverLinear) {          // M3 (non-vacuous: real edge admits)
  auto pool = nonlinear_interaction_pool(/*seed*/11);              // alphas combine nonlinearly
  auto v = learn::fit_stack(pool, nullptr, stack_cfg());
  EXPECT_TRUE(v.admitted);
  EXPECT_GT(v.oos_dsr_nonlinear, v.oos_dsr_linear);
}

TEST(Ensemble, RegimeConditional_ImprovesOosOnRegimeFixture) {     // §4.5 regime-conditional
  auto [pool, regime] = two_regime_pool_and_hmm(/*seed*/11);       // weights should differ by regime
  auto v_flat   = learn::fit_stack(pool, nullptr, stack_cfg());
  auto v_regime = learn::fit_stack(pool, &regime, stack_cfg());
  EXPECT_GT(v_regime.oos_dsr_nonlinear, v_flat.oos_dsr_nonlinear);
}

TEST(Ensemble, Admitted_PlugsIntoLibrary_AsLearnedAlpha) {         // M6
  auto pool = nonlinear_interaction_pool(11);
  auto lib  = library::Library::open(tmpdir(), combine::GateConfig{}, {/*seed*/42});
  auto cand = learn::stack_to_candidate(learn::fit_stack(pool, nullptr, stack_cfg()), pool);
  EXPECT_EQ(cand.prov.expr_source.rfind("learned:stack", 0), 0u);  // provenance tagged learned
  auto verdict = lib.admit(cand, combine::AlphaGate{combine::GateConfig{}});
  EXPECT_NE(verdict.kind, library::AdmitKind::Duplicate);          // a new learned alpha enters the pool
}

TEST(Ensemble, SameSeed_ByteIdenticalVerdict) {                    // M1
  auto pool = nonlinear_interaction_pool(11);
  EXPECT_EQ(hash_verdict(learn::fit_stack(pool,nullptr,stack_cfg_seed(7))),
            hash_verdict(learn::fit_stack(pool,nullptr,stack_cfg_seed(7))));
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `ensemble.hpp` (§4.7): meta-feature matrix from the pool, per-fold nonlinear (`fit_linear`/`fit_gbt`) vs `AlphaCombiner{ShrinkageMv}` benchmark, `deflated_sharpe` on both, admit iff `dsr_nl>0 ∧ sr_nl>sr_lin`, regime-conditional fit gated by the §4.5 posterior, `stack_to_candidate` (wrap `LearnedSignalSource`, set `Provenance.expr_source="learned:stack:<base>"`). Add the optional live re-eval adapter (re-parse `expr_source` → `alpha::Engine::evaluate`) behind a flag. **Step 4:** `ctest --test-dir build -R Ensemble` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s5-6): nonlinear stacking mega-combiner (alpha-of-alphas), regime-conditional, deflation-gated vs P4 linear`).

### Task S5-7: Integration proofs + bench + close
**Files:** Test `tests/learn_integration_test.cpp`; Create `bench/learn_bench.cpp`; edit `sprint-5-progress.md` (+ close ceremony).
**Scope:** §6 exit criteria end-to-end + close. The five load-bearing proofs in one suite. **First verify** the full S5-1…S5-6 public surface is stable (the integration test calls every unit).

- [ ] **Step 1 (integration tests):** suite `LearnIntegration` —
```cpp
TEST(LearnIntegration, DeterministicTrainingEndToEnd_ByteIdentical) {     // M1 headline
  auto fm = e2e_feature_fixture(/*seed*/100);
  auto run = [&]{ return learn::full_pipeline_digest(fm, pipeline_cfg_seed(100)); }; // features->latent->linear->gbt->hmm->stack
  EXPECT_EQ(run(), run());                                                 // same seed -> identical digest
}

TEST(LearnIntegration, NoEdge_AdmitsNothing_RealEdge_AdmitsSurvivors) {    // M3 the anti-snooping proof
  auto noise = pure_noise_e2e_fixture(/*seed*/100);
  auto real  = planted_signal_e2e_fixture(/*seed*/100);                    // SAME gate + deflation bar
  EXPECT_EQ(learn::admitted_count(noise, pipeline_cfg()), 0u);             // p0 ML-ban lift justified
  EXPECT_GT(learn::admitted_count(real,  pipeline_cfg()), 0u);
}

TEST(LearnIntegration, NonlinearCombiner_RejectedWhenLinearSuffices) {     // M3 / §0.4
  auto pool = linearly_combinable_e2e_pool(100);
  EXPECT_FALSE(learn::pipeline_admits_stack(pool, pipeline_cfg()));
}

TEST(LearnIntegration, RegimeConditional_ImprovesOos) {                    // spec exit criterion
  auto [pool, regime] = regime_e2e_fixture(100);
  EXPECT_GT(learn::oos_with_regime(pool, regime), learn::oos_without_regime(pool));
}

TEST(LearnIntegration, MultiHorizon_BlendBeatsBestSingleHorizon) {         // §0.6 user ask
  auto fm = multi_horizon_e2e_fixture(100);
  EXPECT_GE(learn::oos_ic_blend(fm, {1,5,21}), learn::oos_ic_best_single(fm, {1,5,21}) - 1e-9);
}

TEST(LearnIntegration, ThreadCountInvariance_DigestUnchanged) {           // M1 (if n_workers used)
  auto fm = e2e_feature_fixture(100);
  EXPECT_EQ(learn::full_pipeline_digest(fm, workers(1)),
            learn::full_pipeline_digest(fm, workers(4)));                  // determinism survives parallelism
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement any thin `full_pipeline_digest`/`admitted_count` harness helpers in `learn/train.hpp` (no new model logic — just wiring). **Step 4:** `ctest --test-dir build -R LearnIntegration` → pass; **full engine suite green (record the N/N count)**.
- [ ] **Step 5 (bench):** Create `bench/learn_bench.cpp` — register: feature-build rows/sec, elastic-net fit/sec at {p=16,64,256}, GBT fit/sec at {trees=50,200}, HMM Baum-Welch iters/sec, stacking fit/sec, and `LearnedSignalSource::evaluate` cross-sections/sec (the hot path, M7 zero-alloc). `BENCHMARK(...)` cases auto-globbed; build `-DATX_BUILD_BENCH=ON`.
- [ ] **Step 6 (close ceremony, per [`../docs/sprint.md`](../docs/sprint.md)):** fill `sprint-5-progress.md` (all rows ✅, commits table, **residuals → ROADMAP backlog**: elastic-net CD → atx-core L7, histogram GBT → atx-core L7, HMM forward-backward → atx-core L7, plus any deferred bench); flip `p1/ROADMAP.md` S5 row `⏳ → ✅ <sha>` + bump `Last reviewed`; mark spec `Status: ✅ closed`; create `sprint-5.md` user reference; write the **baton → S7** paragraph. **⚠️ `p1/ROADMAP.md` may carry the user's uncommitted edits — coordinate before staging it; stage only the S5 row change with an explicit pathspec.** **Step 7:** Commit (`docs(s5-7): close sprint-5 — 8 units, <M> tests; learned signals + ML combiner shipped`) + `git show HEAD --stat`.

---

## §6 — Exit criteria · invariants · dependencies · NOT-in-scope · baton

**Exit criteria (from the spec, made concrete):**
- **Feature-matrix truncation-invariance** — proven by `FeatureMatrix.Feature_ReadsOnlyUpToDate_TruncationInvariant` + `Latent.PcaBasis_FitOnTrailing_TruncationInvariant` + `LinearAlpha.FitOnTrailing_TruncationInvariant` + `Hmm.Posterior_FitOnTrailing_Causal_TruncationInvariant` (features/factors/regimes at `t` identical with/without `> t` data).
- **Deterministic training (the hard ML one)** — `ElasticNet.SameInput_Deterministic_ByteIdentical`, `LinearAlpha.SameSeed_ByteIdenticalModelAndSignal`, `Gbt.SameSeed_ByteIdenticalTreesAndSignal`, `Hmm.SameSeed_ByteIdenticalParams`, `Ensemble.SameSeed_ByteIdenticalVerdict`, `LearnIntegration.DeterministicTrainingEndToEnd_ByteIdentical` + `ThreadCountInvariance_DigestUnchanged` (byte-identical params **and** emitted signal; {1,4}-worker digest invariance).
- **Deflated, non-vacuous gate** — `LinearAlpha.NoEdgePanel_RejectedByDeflation` + `RealSignalPanel_PositiveDeflatedSharpe`, `Gbt.NoEdgePanel_RejectedByDeflation`, `LearnIntegration.NoEdge_AdmitsNothing_RealEdge_AdmitsSurvivors` (a no-edge model is rejected; a real-edge model survives the SAME bar — the anti-snooping proof that lifts the p0 ML ban).
- **Nonlinear admitted only if it beats linear OOS-after-deflation** — `Ensemble.NoNonlinearEdge_RejectedVsLinear` + `GenuineNonlinearEdge_AdmittedOverLinear` + `LearnIntegration.NonlinearCombiner_RejectedWhenLinearSuffices`.
- **HMM regimes PIT + regime-conditional improves OOS** — `Hmm.Posterior_FitOnTrailing_Causal_*` + `Ensemble.RegimeConditional_ImprovesOosOnRegimeFixture` + `LearnIntegration.RegimeConditional_ImprovesOos`.
- **Multi-horizon** — `LinearAlpha.HorizonBlend_*` + `LearnIntegration.MultiHorizon_BlendBeatsBestSingleHorizon`.
- **Differential correctness per kernel** — `ElasticNet.AlphaZero_MatchesAtxCoreRidge` + `OrthonormalX_MatchesSoftThreshold`, `Gbt.DepthOneStump_MatchesBruteForceThreshold`, `Hmm.ForwardLogLik_EqualsBackwardLogLik` + `RecoversPlantedRegimes_AboveChance`.
- `/W4 /permissive- /WX` + strict-FP clean; one test file per unit; full engine suite green (no regression).

**Invariants proven (M1–M8):** Determinism (M1) — seeded `Xoshiro256pp` from a fixed `(master_seed,unit,fold,horizon)` derivation, RNG-free cyclic CD, deterministic histogram + first-max tie-break, order-fixed reductions; two-builds-equal and same-seed-replay are tested per RNG-bearing unit, and the end-to-end digest is {1,4}-worker invariant. No look-ahead (M2) — every fitted object (standardization, PCA basis, coeffs, GBT splits, HMM `(A,B,π)`+posterior, horizon weights) is trailing-fit/apply-forward, truncation-invariance tested. No snooping (M3) — CPCV training + deflated Sharpe with `N`=the ML trial count; no-edge ⇒ rejected; nonlinear admitted only OOS-beats-linear. Pattern B (M5) — elastic-net/GBT/HMM ship engine-local-then-lifted; ridge/PCA/linalg/RNG/cross_section consumed from atx-core. A learned model is just an alpha (M6) — emits via `ISignalSource`, admits via `library::admit`. No hot-path alloc (M7) — `evaluate()` reuses pre-sized scratch. PIT/no-survivorship/NaN-verbatim (M8). **Differential correctness (M4): each new kernel is bounded against an obviously-correct reference, never a "returned something" check.**

**Dependencies:** Upstream **S1** (closed `2158a17`) — `eval::{cpcv_folds,deflated_sharpe,stats_ext}` (the CPCV + DSR + engine-local stats this sprint trains and gates inside). **S2** (closed `d7a1b75`) — optional `parallel::` fan-out of folds/horizons (the digest must stay {1,4}-worker invariant; single-thread fallback is correct, just slower). **S3** (closed `5f57a34`) — `factory::canonical_hash` (the dedup key on learned-alpha provenance), the `seed_for` derivation precedent. **S4** (merged) — `library::{Library,AlphaCandidate,Provenance,admit}` (the admit path), `combine::AlphaStore` (the pool / feature source). **P4** (closed `f2d22f4`) — `combine::{AlphaCombiner,CombinerConfig,CombineMethod,Combination,CombinedSignalSource,ISignalSource,AlphaGate,compute_metrics}` (the linear-combiner benchmark + the seam). **atx-core L7/L6/L1** — `regression::ridge`, `pca`, `linalg`, `solve`, `decompose`, `cross_section`, `rolling`, `random::Xoshiro256pp` (consumed directly). **Pattern-B requests (§2.1):** elastic-net CD, histogram GBT, log-space HMM forward-backward → atx-core L7 (engine-local this sprint). **S4b alignment:** S5 consumes the SAME `library::admit`/`AlphaCandidate`/`Provenance` contract S4b will use; **if S4b lands `PoolView`/`worst_corr_to_pool` first, S5's corr-to-pool should consume it (M6, no fork); until then S5 reads stored streams (§0.1).** **No `combine`/`eval`/`library`/`factory`/`alpha` source edits** — S5 is purely additive.

**Explicitly NOT in this sprint** (spec + ROADMAP anti-roadmap): **no neural nets / deep learning** (anti-roadmap #7 — stop at linear + GBT + HMM + PCA; revisit only with evidence the simpler combiners are saturated). **No alternative-data features** (anti-roadmap #3 — price-volume + classifications + DSL/pool-alpha outputs only). **No online/streaming model updates** (batch walk-forward only). **No GPU.** **No new general-purpose primitive in the engine** (M5 — solvers/GBT/HMM are atx-core requests). **No re-eval-on-new-panel as a training dependency** (the live re-eval adapter is built optionally in S5-6 for deployment only, §0.1). **No edits to the P4 linear combiner** (it is the benchmark, reused verbatim, §0.4).

**Baton → next:** S5 makes the pool's constituents and combiner **learned, not just formulaic** — completing the discover→predict→combine arc the module set out to build. **S7** (portfolio + lifecycle) then operates the combined book over time: it consumes the learned `ISignalSource`s and the stacking mega-combiner as ordinary pool members, runs them through the multi-period optimizer + decay monitor + capacity bound, and feeds dead learned alphas into the Kakushadze risk-factor extractor exactly as it does formulaic ones. The HMM regime posterior is available to S7's regime-aware rebalancing schedule. **Open at close:** the three Pattern-B atx-core L7 promotions (elastic-net CD, histogram GBT, HMM forward-backward) and — if S4b has not yet landed — the `PoolView`/`worst_corr_to_pool` consumption swap (M6).

---

## §7 — References (research foundation)

- **RenTech deep-dive** [`renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md): §3.2 (Baum-Welch / HMM lineage), §3.6 (noisy-channel / IBM speech statistics), §4.2 + §4.2.1 (HMM stated precisely — `λ=(A,B,π)`, forward-backward EM), §4.3 (noisy-channel `argmax`), §4.4 (many weak signals → one combination; `IR≈IC·√BR`), §9.3 (signal combination is the product; shrinkage→factor→regularized regression→ML ensembles; "prefer the simplest combiner the data supports"), §9.4 (OOS discipline enforced by the harness; deflate Sharpe for multiple testing).
- **WorldQuant deep-dive** [`worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md): fitness formula + correlation/turnover gating (the `AlphaGate` a learned alpha admits through), the datafield→alpha pipeline (features → signal).
- **In-repo grounding (read before implementing):** `combine/combiner.hpp` (the linear rungs the nonlinear combiner must beat + the `Combination` firewall), `eval/cpcv.hpp` + `eval/deflated_sharpe.hpp` (the CPCV + DSR this sprint trains and gates inside), `eval/stats_ext.hpp` (engine-local `norm_cdf`/`skewness`/`excess_kurtosis` — atx-core lacks them, reuse don't re-add), `library/library.hpp` + `library/record.hpp` (the admit/Provenance contract), `alpha/panel.hpp` + `alpha/streams.hpp` (PIT feature sources), `factory/search_driver.hpp` (the `seed_for` determinism precedent).
- **External algorithm references** (engine-local kernels, M4): elastic-net coordinate descent — Friedman, Hastie & Tibshirani, *Regularization Paths for GLMs via Coordinate Descent* (JSS 2010); gradient boosting — Friedman, *Greedy Function Approximation* (Ann. Stat. 2001) + the histogram/LightGBM split-finder construction; HMM — Rabiner, *A Tutorial on Hidden Markov Models* (Proc. IEEE 1989) with the log-space forward-backward of Mann, *Numerically Stable Hidden Markov Model Implementation* (2006); deflated Sharpe — Bailey & López de Prado, *The Deflated Sharpe Ratio* (2014).

---

## §8 — Self-review (against the spec)

- **Spec coverage:** S5.0 marker → **S5-0**; S5.1 PIT feature builder → **S5-1** (+ multi-horizon labels, §0.6); S5.2 linear learned alphas (ridge/lasso/elastic-net) → **S5-3** (elastic-net CD Pattern-B + ridge baseline); S5.3 GBT learned alpha → **S5-4** (deterministic histogram); S5.4 HMM + regime-conditional combination → **S5-5** (Baum-Welch) + **S5-6** (regime-conditional stacking); S5.5 nonlinear ensemble mega-combiner + close → **S5-6** (stacking, deflation-gated vs P4 linear) + **S5-7** (close). Every spec exit criterion maps to a named test in §6. ✅
- **User asks satisfied:** "integrate ML into the alpha pipeline" → learned alphas admit through the existing `ISignalSource`→`library::admit` seam (M6); "discover higher-order / hidden features" → **S5-2 latent PCA factors + interaction terms** and **S5-4 GBT interaction capture** (`Gbt.CapturesInteraction_BeatsLinearOnXorFixture`); "how alphas interact to produce new alphas" → **S5-6 stacking** (pool alphas as meta-features → a learned alpha-of-alphas); "optimize predictions over various time horizons" → **multi-horizon labels + per-horizon fits + OOS-IC horizon blend** (§0.6, `MultiHorizon_BlendBeatsBestSingleHorizon`). ✅
- **As-built reconciliation applied:** S4b is spec-only ⇒ feature source = stored streams, not re-eval (§0.1); CPCV runs on the date axis (§0.2); deflation `N` = ML trial count (§0.3); mega-combiner is additive `learn::StackingCombiner`, not a `CombineMethod` edit (§0.4); PCA/HMM/horizon objects are trailing-fit PIT (§0.5/§0.6); determinism seeded, no wall-clock/thread/map entropy (§0.7); Eigen-link retired at S5-0 (§0.8). ✅
- **Placeholder scan:** no "TBD/TODO/handle-edge-cases" — every coding step shows the test code and names the file/§-ref to implement; the only literal `<…>` placeholders are commit SHAs, the final test count, and the `<HEAD-sha>` base, all filled at execution time. ✅
- **Type consistency:** `FeatureMatrix`/`FeatureSpec`/`LearnedModel`/`LearnedSignalSource`/`ElasticNetCfg`/`LatentBasis`/`StackingVerdict` are used with the same fields/signatures across §4 and §5; the `LearnedModel`+`LearnedSignalSource` emit path defined in S5-3 is reused by S5-4 (GBT) and S5-6 (stacking); `fit_linear`/`fit_gbt`/`baum_welch`/`fit_stack` signatures match between pseudocode and tests. ✅
- **Anti-roadmap honored:** linear + GBT + HMM + PCA only (no NN); price-volume + DSL/pool outputs only (no alt-data); batch walk-forward (no streaming); no engine general-purpose primitive (Pattern B); no `combine`/`eval`/`library` source edits. ✅
- **Determinism is the dominant risk and is gated by construction** — every RNG-bearing unit carries a same-seed-byte-identical test on params AND signal, and the end-to-end pipeline is worker-count invariant; this is the make-or-break for ML in a deterministic engine and the §3 handoff names it first. ✅
