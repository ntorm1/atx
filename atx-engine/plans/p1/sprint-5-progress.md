# Sprint 5 — Learned Signals & ML Combiner — Implementation Progress

**Status:** 🚧 OPEN (S5-0…S5-5 done; S5-6…S5-7 pending)
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
| S5-2  | PIT latent / hidden-feature extraction (trailing-fit PCA + interactions) | ✅ done | _this commit_ | 3/3 (Latent); full engine suite green (no regression) | `learn/latent.hpp` (`LatentBasis`, `LatentAugmentation`, `fit_latent`, `apply_latent`, `select_interactions`, `interaction_value`); `core::linalg::pca` fit on trailing valid rows `date ≤ t−embargo` only, projected forward via `core::linalg::transform` ((X−mean)·components); truncation-invariant (M2/§0.5) — pinned by hashing the fitted components+mean f64 bytes via `core::hash_bytes` (NB: it's `atx::core::hash_bytes`, NOT `core::hash::hash_bytes` — the S5-0 seam map was wrong). Interactions ranked by trailing \|Spearman IC\| vs Y[0] (Spearman = Pearson on `core::stats::rank`), top-m with ascending-index tie-break, crossed pairs (a<b) in fixed order (M1). Edge: `k=0`/`m=0`/empty-or-<2-row trailing window/`t<embargo` → empty result, `k` reported 0, no UB. `core::linalg` PCA `components` = n_features×k (COLUMNS are eigenvectors); `transform()` EXISTS. /WX clean (fixed one `-Wunused-lambda-capture` in the test fixture). |
| S5-3  | Elastic-net CD + linear learned alpha + LearnedSignalSource           | ✅ done | _this commit_ | 9/9 (ElasticNet 3 + LinearAlpha 6); full engine suite 1854/1854 (1 pre-existing Databento smoke skipped) | `learn/elastic_net.hpp` (Pattern-B edge 1, RNG-free cyclic CD), `learn/linear_alpha.hpp` (`fit_linear`/`predict_at`/`oos_deflated_sharpe`), `learn/learned_source.hpp` (`LearnedModel`+`LearnedSignalSource` M6/M7). ridge+elastic-net, per-horizon CPCV OOS walk, §0.6 horizon blend. M4 differential vs `core::ridge` at α=0 + the soft-threshold closed form. **ADOPTED from a concurrent effort and FIREWALL-HARDENED before first commit:** a spec-compliance review + code read found two CRITICAL look-ahead defects which were fixed under TDD. (1) **M2 standardization leak** — the CPCV OOS folds standardized test rows with stats fit over ALL dates (incl. the test + future rows); now each fold fits a FOLD-LOCAL standardization on its TRAIN rows only and applies it forward to both the train and OOS test design. (2) **In-sample M3 gate** — `oos_deflated_sharpe` re-predicted IN-SAMPLE with the deployed (all-dates) coeffs; now the genuine per-date OUT-OF-FOLD IC series is assembled DURING fitting from the fold-local OOF predictions (horizon-0, averaged across folds for any row covered by >1 test fold) and frozen on the model in a new `LearnedModel::oos_score_series` field (defaults `{}` so S5-4/S5-6 model shells still construct), and the gate simply DSRs that stored series. The M2 truncation test was strengthened from a vacuous same-matrix predict_at check to a genuine fitting-firewall test (the stored OOS series is byte-identical to an independent fold-local OOF reconstruction; a single fold's OOF prediction is invariant to truncating the matrix to its [train ∪ test] rows) — it FAILS under the standardization leak (red-proof confirmed) and passes under the fix. M3 contrast holds same-bar at seed 7: noise dsr=0.0, planted-signal dsr≈1.0; signal robust (≈0.999–1.0) across 15 seeds, noise=0.0 at 11/15 with small low-N leaks (≤0.23) at a few seeds (a property of DSR at trial_count=5 / ~39 points, not a firewall break — the planted signal did NOT need strengthening). M1 determinism now also pins `oos_score_series` byte-identical. /WX clean. |
| S5-4  | Deterministic histogram GBT model + GBT learned alpha                 | ✅ done | _this commit_ | 4/4 (Gbt); full engine suite green (no regression) | `learn/gbt.hpp` (Pattern-B edge 2: histogram split finder, train-quantile bin edges, seeded row/feature subsample, first-max tie-break, squared-error grad/hess boosting, CPCV walk-forward, deployed full-trailing refit, §0.6 horizon blend) + GBT deployed params in `learn/learned_source.hpp` (`ModelKind::Gbt`, `GbtNode`/`GbtTree`/`GbtForest`, allocation-free `gbt_tree_predict`/`gbt_forest_predict` (M7), `LearnedModel::forests` (defaults `{}` so Linear/S5-6 shells still construct), the `Gbt` arm of the exhaustive-no-default `predict_blended` switch). Reuses the model-kind-agnostic `oos_deflated_sharpe`/`predict_at` from `linear_alpha.hpp` by populating `oos_score_series` from GENUINE out-of-fold tree predictions (mirrors the S5-3 OOF construction). Tests: `DepthOneStump_MatchesBruteForceThreshold` (M4, vs independent O(n²) best-threshold within ~2 bin widths), `SameSeed_ByteIdenticalTreesAndSignal` (M1, forest-node-byte hash + signal hash), `CapturesInteraction_BeatsLinearOnXorFixture` (trees beat no-interaction linear on pure XOR), `NoEdgePanel_RejectedByDeflation` (M3). **ADOPTED from a concurrent effort, TWO-STAGE REVIEWED before commit.** Spec review: SPEC-COMPLIANT, no look-ahead — confirmed per-fold TRAIN-only standardization + TRAIN-only bin edges (the S5-3 firewall defect NOT reintroduced), genuine-OOF series, one trial per fold fit (§0.3). Two as-built deviations from §4.4, both judged SOUND (no M2 leak): (1) **root-split gain-floor exemption** — the root explores with no `min_split_gain` floor (a pure interaction has zero marginal root gain; a root floor would prune before the depth≥1 interaction split can appear); the floor applies below the root so noise trees stay shallow. (2) **OOF dispersion floor** (`kOofDispersionFloor=0.16`) — a per-date OOF prediction cross-section whose std is below `0.16 × horizon-0 OOF LABEL std` scores IC 0 (label-side-scaled ⇒ fit-legal, no look-ahead; the tree analog of L1's exact-zero on noise). Spec review flagged the `0.16` constant as swept-to-fit + tested at a single seed; **resolved by strengthening the M3 test to a 24-seed sweep** (each draws a distinct noise panel) — all 24 reject (dsr ≤ 0), so the gate is seed-ROBUST, not pinned. Quality review CHANGES-REQUESTED → fixed: corrected an inaccurate SAFETY comment (copies into a fresh buffer, does NOT alias column-major X), dropped a dead `<span>` include, added the direct `atx/core/macro.hpp` include + `ATX_CHECK` guards on the non-obvious fit-path bin/feature indices, fixed the stale "one bin width" tolerance comment. M1 determinism: seeds from `seed_for(master,"gbt-rows"/"gbt-feat"/"gbt-fold"/"gbt-deploy",…)`, ascending-order subsets, fixed nested split scan — no clock/thread/map/address entropy. /WX clean. |
| S5-5  | Log-space Baum-Welch HMM + PIT regime posterior                      | ✅ done | _this commit_ | 5/5 (Hmm); full engine suite 1014/1014 (1009 baseline + 5) | `learn/hmm.hpp` (Pattern-B edge 3) + `tests/hmm_test.cpp`. A SELF-CONTAINED kernel over a raw `obs` (T×d) MatX — NO Panel/FeatureMatrix dep. Public surface: `HmmCfg`, `Gaussian` (diag mean/var), `Hmm` (`logA`/`emit`/`logpi`), `baum_welch` (EM), `forward_log`→`ForwardResult{log_alpha,loglik}`, `backward_loglik`, `posterior_decode` (argmax smoothed γ), `regime_posterior_at` (PIT filter). **Everything in LOG-SPACE** (one `logsumexp` with the standard max-shift; all-(-inf)→-inf, never NaN). Diagonal-Gaussian emission log-pdf; EM = log-space forward-backward + re-estimation (π=γ₀, A=Σξ/Σγ, μ/σ² from γ responsibilities). **M4 (two proofs):** (1) `forward_log.loglik == backward_loglik` to 1e-8 — the two are INDEPENDENT recursions (forward α vs backward β; `backward_loglik` never calls the forward pass), so the identity is a genuine cross-check. (2) recovery vs an independent planted 2-regime generator decoded via `posterior_decode`, permutation-robust agreement. **Two-stage reviewed; spec review flagged the recovery bar as not-load-bearing (the original ~5σ fixture auto-passes any `>0.8`), RESOLVED:** the easy fixture (mean 0 σ0.3 / mean +3 σ0.6, run-length 25) now asserts a discriminating **0.95** bar (measured 1.0000), and a SECOND harder fixture (`RecoversWeaklySeparatedRegimes_AboveChance`, regime B only mean +1.2 σ0.6 ≈ 2σ overlap, T=400) asserts `>0.75` — a non-trivial separation where a single-threshold classifier struggles but the HMM's temporal self-transition prior still recovers above chance, making the recovery claim genuinely load-bearing. Quality review APPROVED (IWYU clean, accurate SAFETY notes, ATX_CHECK density matches siblings, no vocab leaks). **M2 (load-bearing):** `regime_posterior_at(fitted,obs,d)` is a FORWARD-ONLY filter — it copies obs[0..d] into a length-(d+1) prefix and runs the forward recursion with `T_use=d+1`, so causality is STRUCTURAL (it cannot read rows > d); the truncation test is byte-EXACT (`hash_vec` of the posterior at d=149 is identical whether obs has 150 or 300 rows). **M1 determinism:** seeded init from `Xoshiro256pp{seed_for(master_seed,"hmm",0,0)}` in a FIXED state-major/dim-minor draw order (means jittered by global-std·normal, var=floored global var, A near-uniform 0.9 self-bias, π uniform); every reduction order-fixed (forward t↑ s'↑, backward t↓ s'↑, M-step Σ_t↑); no clock/thread/address/map entropy → same-seed byte-identical `hash_hmm`. **Numerical guards:** `var_floor` (max(var,floor)) on every emission variance; M-step log(0)→`-inf` via the -inf-safe logsumexp; a DEAD state (Σγ≤0 / non-finite) keeps its previous row/emission deterministically (no div-by-~0). Caveat: EM finds a local optimum from the seeded init — M1 only requires same-seed→identical (which a fixed-order seeded init + deterministic EM gives); different seeds may converge differently. /WX clean (no warnings under /W4 /permissive- /WX + strict-FP); IWYU strict. |
| S5-6  | `StackingCombiner` nonlinear meta-model + optional live re-eval adapter | ⏳ todo | —          | —     | `learn/ensemble.hpp`; alpha-of-alphas, regime-conditional + horizon-aware blend, admitted only if it beats the P4 linear combiner OOS-after-deflation (M3/§0.4). |
| S5-7  | Integration proofs + bench + close                                   | ⏳ todo | —             | —     | `learn_integration_test.cpp` (determinism + anti-snooping + nonlinear-beats-linear + regime-improves + multi-horizon) + `bench/learn_bench.cpp`; residuals → ROADMAP; baton → S7. |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| _this commit_ | S5-0 | docs(s5-0): open sprint-5 learned-signals ledger + scaffold + atx-core L7 link smoke-test |
| _this commit_ | S5-1 | feat(s5-1): PIT feature matrix + multi-horizon labels + CPCV date-fold scaffold |
| _this commit_ | S5-2 | feat(s5-2): PIT latent factor + interaction feature extraction (hidden features) |
| _this commit_ | S5-3 | feat(s5-3): linear learned alpha (elastic-net CD + ridge), per-horizon blend, ISignalSource adapter; genuine-OOS deflation gate + per-fold standardization firewall |
| _this commit_ | S5-4 | feat(s5-4): deterministic histogram GBT learned alpha (interaction capture, genuine-OOS deflation) |
| _this commit_ | S5-5 | feat(s5-5): log-space Baum-Welch HMM + PIT regime posterior |

---

## Close residuals → p1 ROADMAP future-work backlog

To be populated at S5-7 close. Expected lifts: the three Pattern-B kernels (elastic-net CD, histogram GBT, log-space HMM forward-backward) → atx-core L7; `learn_bench.cpp` throughput curve.

## Baton → next

S5-7 hands to **S7 (portfolio lifecycle)**: a learned model admits through `library::Library::admit` exactly like a mined alpha (M6), so the portfolio book operates learned and formulaic alphas through one lifecycle journal.
