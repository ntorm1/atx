# Sprint 5 — Learned Signals & ML Combiner — Implementation Progress

**Status:** 🚧 OPEN (S5-0 done; S5-1…S5-7 pending)
**Branch:** `feat/atx-core-stdlib` (SHARED — explicit pathspecs only, never `git add -A`/`-u`/`-a`; never pushed; S5 is purely additive under `learn/`, `tests/`, `bench/`, this ledger)
**Base:** `feat/atx-core-stdlib` @ `1a64c55` (S4b close `1a64c55`; S4 library management merged; P4 combine/risk + S1 eval/CPCV/DSR present)
**Started:** 2026-06-09
**Source plan:** [`sprint-5-learned-signals-ml-combiner-implementation-plan.md`](sprint-5-learned-signals-ml-combiner-implementation-plan.md)
**Spec:** [`sprint-5-learned-signals-ml-combiner.md`](sprint-5-learned-signals-ml-combiner.md)
**ROADMAP:** [`../p0/ROADMAP.md`](../p0/ROADMAP.md)

**Thesis:** A learned model is a deterministic, deflation-gated, firewall-fit alpha that plugs into the existing pool through the existing seams. The engine learns higher-order structure (latent factors, tree interactions, regimes, alpha-of-alphas) WITHOUT ever learning to look ahead or to fool its own scorer.

---

## §0 as-built amendment summary

The implementation plan's §0 reconciles the frozen spec against the as-built S4/S4b engine (§0 overrides the spec on conflict). The eight corrections, one line each:

1. **Feature source is stored streams + raw fields, not DSL re-eval (§0.1).** The re-parse `expr_source` → compile → eval path the spec implies does not exist (S4b is spec-only, zero code). S5-1 sources features from raw `alpha::Panel` fields plus already-computed `combine::AlphaStore`/`alpha::AlphaStreams` pnl/positions streams (both PIT-aligned, both exist today). The live re-eval adapter is built once, optionally, in S5-6 — never on the training path.

2. **CPCV folds run on the date axis; feature rows are (date × instrument) (§0.2).** Purge/embargo are temporal, so S5 feeds `eval::cpcv_folds` one `LabelSpan{date, date+horizon}` per DATE; the trainer expands each date-fold to all instruments at those dates. `eval::cpcv` is consumed unchanged — the no-look-ahead firewall is inherited, not re-implemented.

3. **Deflation N for ML = the ML trial count (§0.3).** A learned model has no population; the trial analog is a distinct fit. The trainer maintains a monotone `trial_count` (per CV config × horizon × ensemble candidate) passed as `N` to `deflated_sharpe`. More configs tried ⇒ higher deflation bar — a no-edge search winner is correctly killed; `N=1` reduces to plain PSR.

4. **The mega-combiner is a new `learn::StackingCombiner`, not a `CombineMethod` enum value (§0.4).** Adding a sixth enum value would edit `combine/combiner.hpp` (forbidden) and break the linear-blend contract. S5 adds a separate nonlinear `learn::StackingCombiner` benchmarked against the P4 `combine::AlphaCombiner{ShrinkageMv}` via OOS-after-deflation. `combine/` untouched.

5. **Latent/PCA features are fit on a trailing window, applied forward (PIT) (§0.5).** PCA on the whole tensor leaks the future. S5-2 fits the `core::pca` basis on rows with `date ≤ t − embargo` only, then projects forward via `transform`. Latent factors are truncation-invariant — hidden features without look-ahead.

6. **Multi-horizon is first-class in the label/predict path (§0.6).** S5-1 builds forward-return labels `y_h` for a horizon set `H` (default `{1,5,21}`; `{1}` recovers single-horizon). Each model trains one fit per horizon; a horizon blend weights per-horizon predictions by each horizon's OOS information coefficient (fit-on-trailing). The blend is itself deflated + gated.

7. **Determinism is seeded from a fixed derivation; no wall-clock / thread / map-order entropy (§0.7).** Every RNG draw seeds from `(master_seed, unit, fold, horizon)` via a fixed mix (mirrors S3 `seed_for`); GBT = deterministic histogram + first-max tie-break; elastic-net = RNG-free cyclic CD; HMM init seeded; reductions order-fixed. Every RNG-bearing unit ships a "two-builds-equal / same-seed byte-identical params AND signal" test.

8. **atx-core L7 is header-only Eigen-backed; confirm the link at S5-0 (§0.8).** `core::regression`/`pca`/`linalg`/`decompose`/`solve` are header-only but instantiate Eigen (`MatX = Eigen::MatrixXd`). P4's `ShrinkageMv` already does eigendecomp + ridge, so Eigen is likely transitive — but a build risk to RETIRE FIRST, not an assumption. **RETIRED at S5-0 (see Kickoff risk (b)).**

---

## §0 supplementary — as-built atx-core API confirmed at S5-0

The plan's S5-0 test sketch GUESSED the header paths and namespaces. Confirmed as-built (the path/namespace S5-1+ must use):

| What | Guessed (plan sketch) | AS-BUILT |
|---|---|---|
| ridge header | `atx/core/regression.hpp` | `atx/core/linalg/regression.hpp` |
| pca header | `atx/core/pca.hpp` | `atx/core/linalg/pca.hpp` |
| RNG header | `atx/core/random.hpp` | `atx/core/random.hpp` ✓ |
| ridge namespace | `atx::core::regression::ridge` | `atx::core::linalg::ridge` |
| pca namespace | `atx::core::pca::pca` | `atx::core::linalg::pca` |
| `as_matrix`/`as_vector` | `atx::core::linalg` | `atx::core::linalg` ✓ |
| ridge signature | — | `ridge(const MatX&, const VecX&, f64 lambda) -> Result<OlsResult>` |
| ridge accessor | `.beta` | `.beta` ✓ (also `.r2`, `.residuals`) |
| pca signature | — | `pca(const MatX&, i64 k = -1) -> Result<PcaResult>` |
| pca accessor | `.explained_ratio` | `.explained_ratio` ✓ (also `.mean`, `.components`, `.explained_variance`) |
| `as_matrix` arg | `std::span` vs ptr+size | `as_matrix(std::span<const double>, Eigen::Index rows, Eigen::Index cols)` → returns `MatMapConst` (zero-copy Map); materialize into `MatX` to bind `ridge`'s `const MatX&` |
| `as_vector` arg | — | `as_vector(std::span<const double>)` → `VecMapConst`; materialize into `VecX` |
| Result success | — | `Result<T> = tl::expected<T,Error>`; success = `.has_value()`, value via `->` |
| RNG type | `atx::core::random::Xoshiro256pp` | `atx::core::Xoshiro256pp` (NOT in a `random` sub-namespace) |
| RNG next-u64 | `.next_u64()` | `.next_u64()` ✓ (also `.uniform01()`, `.normal()`, `.bernoulli()`, `.jump()`) |
| `f64` | `atx::f64` | `atx::f64` ✓ (in `atx/core/types.hpp`) |

Column-major storage: a flat `{1,2,3,0,0,0}` viewed as 3×2 has col0 = `{1,2,3}`, col1 = `{0,0,0}`.

---

## Kickoff risks

### (a) Pattern-B atx-core L7 edges (three kernels atx-core lacks — ship engine-local-then-lifted, M5)

- **Elastic-net (L1+L2) coordinate-descent solver → atx-core L7.** atx-core ships `regression::{ols,ridge,wls}` (L2-only); no L1/elastic-net. Ships engine-local in `learn/elastic_net.hpp` (cyclic CD + soft-thresholding, RNG-free), differential-tested vs `core::ridge` at `α=0` + the soft-threshold closed form (M4). [S5-3]
- **Deterministic histogram gradient-boosted-tree → atx-core L7.** No tree/boosting/split-finder in atx-core. Ships engine-local in `learn/gbt.hpp` (fixed-bin histogram split finder, seeded subsampling, first-max tie-break), differential-tested vs an exact brute-force stump (M4). [S5-4]
- **Log-space HMM forward-backward / Baum-Welch → atx-core L7.** No HMM/EM/forward-backward in atx-core. Ships engine-local in `learn/hmm.hpp` (log-space forward-backward + EM, seeded init), differential-tested vs a reference impl + the fwd==bwd log-likelihood identity (M4). [S5-5]

### (b) Eigen transitive link — RETIRED at S5-0

atx-core L7 (`regression`/`pca`/`linalg`/`decompose`/`solve`) is header-only but instantiates Eigen (`MatX = Eigen::MatrixXd`, column-major). §0.8 named this a build risk to retire before any model unit. **Verdict: RETIRED.** `tests/learn_scaffold_test.cpp` (suite `LearnScaffold`, 3/3 PASS) proves `atx::core::linalg::ridge` and `atx::core::linalg::pca` INCLUDE, COMPILE and LINK from the `atx-engine-tests` target under `/W4 /permissive- /WX`, and recover correct values (`beta[0]≈2.0` on `y=2·x1`; rank-1 `explained_ratio[0]>0.99`). The Eigen include path (`-imsvc …/eigen3-src`) is present on the engine test target transitively (via the P4 combine layer's existing `ShrinkageMv` eigendecomp + ridge usage) — no CMake change needed. **The §0.8 risk does NOT block S5-3.**

### (c) Shared-branch / explicit-pathspec discipline

This branch (`feat/atx-core-stdlib`) is SHARED — a concurrent effort leaves foreign uncommitted/untracked files here. Build against them, but stage ONLY this unit's files: `git add -- <exact paths>`; NEVER `git add -A`/`-u`/`.`; NEVER `git commit -a`. After each commit run `git show HEAD --numstat` to confirm only the intended files appear. NEVER touch `atx-core/*`, any p0 file, `.agents/*`, `.clang-tidy`, `.clangd`, `.vscode/*`, `research/*`, or other sprints' ledgers. Do not push.

### (d) DOMINANT RISK: non-determinism / look-ahead in a fitted model

ML is exactly where the two carried-forward invariants die quietly:
- **Non-determinism (M1).** RNG seeded from wall-clock/address/thread, parallel-reduction float non-associativity, hash-map iteration order, or a non-fixed tie-break ⇒ same data + same seed produces DIFFERENT params/signal across builds — a silent reproducibility break. Mitigation: every RNG-bearing unit ships a "two-builds-equal / same-seed byte-identical params AND emitted signal" test (params hashed via `core::hash::hash_bytes`).
- **Look-ahead (M2).** A fitted object (standardization stats, PCA basis, coeffs, GBT splits, HMM params, blend weights) trained on data `> t` and applied at `t` ⇒ an inflated OOS Sharpe that evaporates live. Mitigation: every fitting unit ships a truncation-invariance test (fit on `[0,t)` vs `[0,t+k)` ⇒ identical output at dates `< t`), and the whole stack trains inside S1 CPCV with a deflated-Sharpe gate (M3) so a no-edge / over-searched model is rejected non-vacuously.

These two failures are silent, not loud — the suite must catch them by construction, per unit.

---

## Per-unit status

| Unit  | Title                                                                 | Status | Commit SHA(s) | Tests | Notes |
|-------|-----------------------------------------------------------------------|--------|---------------|-------|-------|
| S5-0  | Marker + ledger + learn scaffold + atx-core L7 link smoke-test        | ✅ done | _this commit_ | 3/3 (LearnScaffold) | `learn/fwd.hpp` (M1–M8 doc block + seam map + determinism contract + the three Pattern-B edges; forward-decls `struct FeatureMatrix; struct FeatureSpec; class LearnedSignalSource; struct LearnedModel;`) + this ledger + `learn_scaffold_test.cpp`. **§0.8 Eigen-link risk RETIRED** (ridge/pca INCLUDE+LINK from `atx-engine-tests`, 3/3 PASS, /WX clean). As-built API confirmed (see §0 supplementary table): headers under `atx/core/linalg/`, `ridge`/`pca` in `atx::core::linalg`, RNG is `atx::core::Xoshiro256pp` (no `random` sub-ns), Result success = `.has_value()`. |
| S5-1  | PIT feature matrix + multi-horizon forward-return labels              | ✅ done | _this commit_ | 5/5 (FeatureMatrix 4 + TrainScaffold 1); full engine suite 1840/1840 (1 pre-existing Databento smoke skipped) | `learn/feature_matrix.hpp` (`FeatureSpec`, `FeatureMatrix`, `build_features`) + `learn/train.hpp` (`date_label_spans`, `expand_date_folds`, `RowFold`/`Folds`, `seed_for`, `TrialCounter`); raw Panel fields + stored `AlphaStore::positions` streams; forward labels `close[d+H]/close[d]-1`, NaN-verbatim at the tail (M8/§0.6); truncation-invariant (M2); deterministic (date,instrument) emit order, no map iteration on the hot loop (M1); out-of-universe cells emit NO row (M8); date-axis CPCV expanded to feature rows (§0.2). /WX clean. |
| S5-2  | PIT latent / hidden-feature extraction (trailing-fit PCA + interactions) | ⏳ todo | —          | —     | `learn/latent.hpp`; `core::pca` fit on `date ≤ t−embargo`, projected forward; truncation-invariant (M2/§0.5). |
| S5-3  | Elastic-net CD + linear learned alpha + LearnedSignalSource           | ⏳ todo | —             | —     | `learn/elastic_net.hpp` (Pattern-B edge 1), `learn/linear_alpha.hpp`, `learn/learned_source.hpp`; ridge+elastic-net, per-horizon CPCV, horizon blend. M4 differential vs `core::ridge` at α=0. |
| S5-4  | Deterministic histogram GBT model + GBT learned alpha                 | ⏳ todo | —             | —     | `learn/gbt.hpp` (Pattern-B edge 2); fixed-bin histogram split finder, seeded subsampling, first-max tie-break. M1 byte-identical; M4 vs brute-force stump. |
| S5-5  | Log-space Baum-Welch HMM + PIT regime posterior                      | ⏳ todo | —             | —     | `learn/hmm.hpp` (Pattern-B edge 3); log-space forward-backward + EM, seeded init. M4 vs reference + fwd==bwd log-likelihood identity. |
| S5-6  | `StackingCombiner` nonlinear meta-model + optional live re-eval adapter | ⏳ todo | —          | —     | `learn/ensemble.hpp`; alpha-of-alphas, regime-conditional + horizon-aware blend, admitted only if it beats the P4 linear combiner OOS-after-deflation (M3/§0.4). |
| S5-7  | Integration proofs + bench + close                                   | ⏳ todo | —             | —     | `learn_integration_test.cpp` (determinism + anti-snooping + nonlinear-beats-linear + regime-improves + multi-horizon) + `bench/learn_bench.cpp`; residuals → ROADMAP; baton → S7. |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| _this commit_ | S5-0 | docs(s5-0): open sprint-5 learned-signals ledger + scaffold + atx-core L7 link smoke-test |
| _this commit_ | S5-1 | feat(s5-1): PIT feature matrix + multi-horizon labels + CPCV date-fold scaffold |

---

## Close residuals → p1 ROADMAP future-work backlog

To be populated at S5-7 close. Expected lifts: the three Pattern-B kernels (elastic-net CD, histogram GBT, log-space HMM forward-backward) → atx-core L7; `learn_bench.cpp` throughput curve.

## Baton → next

S5-7 hands to **S7 (portfolio lifecycle)**: a learned model admits through `library::Library::admit` exactly like a mined alpha (M6), so the portfolio book operates learned and formulaic alphas through one lifecycle journal.
