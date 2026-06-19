# Sprint S11 — Unsupervised Return-Structure Clustering

**Worktree:** `.claude/worktrees/s11-return-clustering`
**Branch:** `worktree-s11-return-clustering`
**Base:** `feat/megaalpha-enrich-validate` @ `44d79882ef4a005c2999e2f773faab87d26f4aaa`
**Started:** 2026-06-19
**Source plan:** [`research/rentech-improvement-sprint-plan.md`](../research/rentech-improvement-sprint-plan.md)
**Prior progress:** sprint-10 (conviction sizing & regime-adaptive integration; [`p2/sprint-10-progress.md`](p2/sprint-10-progress.md))

## Plan adjustments vs. the source plan

S11 builds the unsupervised return-structure clustering spine: a random-matrix-theory
correlation cleaner and a clusterer in atx-core (Pattern B numerics edges), consumed by an
engine rolling cluster panel and indexed by a store record. S11-0 freezes the seams as
declaration-only scaffold headers so the later units land against stable signatures.

Two conventions are noted at kickoff (matching the sprint-10 ledger):

1. **Test framework is GoogleTest**, not the `ATS_TEST` macro the generic
   [`docs/sprint.md`](docs/sprint.md) mentions. atx-engine tests use `TEST(Suite, Case)` /
   `EXPECT_*` in `tests/<group>/*_test.cpp` (auto-globbed via `CONFIGURE_DEPENDS`, one exe per
   group: `atx-engine-<group>-tests`). Test counts are reported as `Suite total/passed/failed/skipped`.
2. **The S11-0 scaffold test rides the existing `store` group** (`atx-engine-store-tests`) rather
   than adding a new test target. The engine library links atx-core, so a store-group test can
   `#include` all four new headers (two atx-core, two engine) and exercise the compile/link seam
   with no new CMake target. The per-group `*_test.cpp` glob picks the file up automatically.

Realistic scope for this sprint:

1. **S11-0** — Marker + ledger + declaration-only scaffold headers across core/engine/store, plus a
   ClusterScaffold compile/link test. No algorithm logic.
2. **S11-1** — RMT correlation cleaner (`atx::core::linalg::rmt_clean`): Marchenko-Pastur clipping +
   rotationally-invariant estimator.
3. **S11-2** — Clusterer (`atx::core::cluster::cluster`): hierarchical (Ward/Average) + signed-graph
   SPONGE, canonical labels.
4. **S11-3** — Engine rolling cluster panel (`atx::engine::alpha::build_cluster_panel`): windowed
   correlation → clean → cluster → per-date labelings, optional CAPM residualization.
5. **S11-4** — Store cluster-panel record + index (`atx::engine::store::cluster_panel`): register /
   lookup / locate + canonical FNV-1a-64 params hash.
6. **S11-5 … S11-7** — Downstream consumers (cluster-aware alpha signals, risk grouping, report
   wiring). Scoped when S11-0..S11-4 land.

## Determinism contract (applies to every S11 unit)

No RNG on any result path. Order-fixed reductions (ascending index throughout). Eigen steps take
eigenpairs in **ascending-eigenvalue order** with a fixed eigenvector sign convention (first nonzero
component made positive). Cluster labels are **canonicalized by ascending smallest-member index**.
Any parameter hash is **FNV-1a-64** over a canonical, length-prefixed byte stream (no wall-clock, no
seed). S11-0 records these as TODO seams in the scaffold doc comments so S11-1..S11-4 inherit them.

## Per-unit ledger

| Unit  | Status | Commit  | Notes |
|-------|--------|---------|-------|
| S11-0 | done   | `9845626` | Opened this ledger; laid down four declaration-only scaffold headers with no logic. atx-core: `linalg/rmt_clean.hpp` (`RmtConfig{Mode::Clip\|RIE, ridge, eps}`, `CleanedCorr{MatX corr, i64 clipped}`, `rmt_clean(const MatX&, f64 q, RmtConfig)`) and new dir `cluster/cluster.hpp` (`Linkage{Ward,Average}`, `Algo{Hierarchical,SpongeSym}`, `ClusterConfig{algo,linkage,k,eps}`, `Clustering{vector<int> cluster_id, i64 n_labels}`, `cluster(const MatX&, ClusterConfig)`). Engine: `alpha/cluster_panel.hpp` (`ClusterPanelConfig{window,recluster_every,k,Residualize{None,CAPM}}`, `ClusterPanel{Snapshot{date,cluster_id,n_labels}, kUnclustered=-1, snapshots, instruments}`, `build_cluster_panel(const Panel&, ClusterPanelConfig)`). Store: `store/cluster_panel.hpp` mirroring `segment_index.hpp` — `ClusterPanelRecord` (panel_id, universe_id, window_start/end, recluster_every, params_hash, asof_date, binary_path, content_hash, algo, k, created_at, created_by_run_id) + `register_panel` / `lookup` / `locate` / `compute_params_hash` declarations. All four free functions are DECLARED only (TODO(S11-1..S11-4)). The atx-core symmetric-matrix type targeted is `MatX` (column-major `Eigen::MatrixXd`), matching pca/spd/solve; failures travel via `Result<T>`. `ClusterScaffold 1/1/0/0` (rides `atx-engine-store-tests`; constructs every scaffold config/result type and asserts the frozen enum + numeric defaults). Builds green under `/W4 /permissive- /WX`-equivalent clang-cl. |
| S11-1 | done   | `2b12a33` | Implemented `atx::core::linalg::rmt_clean` (header-only/inline in `linalg/rmt_clean.hpp`, matching pca/spd/decompose). Frozen seam honored exactly: `RmtConfig{Mode::Clip\|RIE, ridge=0.0, eps=1e-12}`, `CleanedCorr{MatX corr, i64 clipped=0}`, signature unchanged. Pipeline: symmetrize → optional `ridge`·I → ascending `symmetric_eig` → canonical eigenvector signs (first non-trivial component positive) → MP noise-variance fit (de Prado `findMaxEval`: Gaussian-KDE-vs-MP-PDF SSE minimized by deterministic golden-section search on σ², 80 iters, no RNG) → λ₊ = σ²(1+√q)² → reshape. **Clip** (default): replace every λ≤λ₊ by the Neumaier-averaged sub-edge block (trace-preserving), keep factors. **RIE**: Ledoit-Péché/Bouchaud bulk contraction toward σ²(1+q) with (1−q) damping, renormalized to conserve bulk mass; **guarded to q<1 (T>N)** else falls back to Clip with an identical `clipped` count (no extra flag). Output re-symmetrized, rescaled to unit diagonal, PSD-repaired (clamp negatives, no eigenvalue < −1e-12). Added a local `detail::NeumaierSum` (compensated summation; no shared atx-core helper existed) and `detail::canonicalize_signs`. No fields ADDED to the frozen structs. `RmtClean 12/12/0/0` (new `atx-core/tests/rmt_clean_test.cpp` wired into `atx-core-tests`): non-square/empty/q≤0 → InvalidArgument, planted-factor clip keeps the outlier (clipped==N−1), trace preserved (≤1e-9), PSD min-eig ≥ −1e-12, unit diagonal, identity near-no-op at q→0, RIE q≥1 byte-identical to Clip, RIE q<1 PSD+trace≈N, two-runs byte-identical, pinned FNV-1a-64 digest (`17595468881287217971`, same offset/prime as the store fingerprint scheme — atx-core `hash.hpp` wyhash is not cross-platform-stable so a local FNV-1a-64 over column-major double bytes is used). Full atx-core suite green (836 passed / 1 env-skipped) under `/W4 /permissive- /WX`-equivalent clang-cl. |
| S11-2 | done   | `ba7cb53` | Implemented `atx::core::cluster::cluster` (header-only/inline in `cluster/cluster.hpp`, matching pca/rmt_clean). Frozen seam honored: `Linkage{Ward,Average}`, `Algo{Hierarchical,SpongeSym}`, `Clustering{vector<int> cluster_id, i64 n_labels}`, signature unchanged; existing `ClusterConfig` defaults (`algo=Hierarchical, linkage=Ward, k=1, eps=1e-12`) kept. **ADDED two fields** to `ClusterConfig`: `double tau_plus = 1.0` and `double tau_minus = 1.0` (SPONGEsym signed-Laplacian regularizers; consulted only by `Algo::SpongeSym`). Dispatch on `cfg.algo`. **Hierarchical**: correlation→distance `d=sqrt(2(1-rho))` (rho clamped to [−1,1]), Lance-Williams agglomeration (Ward/Average), cut after exactly N−k merges; equal merge distances broken by the lowest surviving (rep_lo, rep_hi) representative-index pair (a cluster's representative is its smallest member). **SpongeSym**: off-diagonal correlations form signed adjacency A=A⁺−A⁻ (diagonal zeroed); signed Laplacians L±=D±−A±; solves the generalized eigenproblem `(L⁺+τ⁻·D⁻) v = λ (L⁻+τ⁺·D⁺) v` via `Eigen::GeneralizedSelfAdjointEigenSolver` (RHS made SPD by τ⁺·D⁺ + eps·I); takes the bottom-k generalized eigenvectors, **weighted by 1/sqrt(λ+eps)** (diffusion-map scaling so the discriminative low modes dominate and degenerate within-cluster modes do not mis-split), and clusters them with deterministic k-means. **k-means++ determinism**: NO RNG — D²-furthest-first seeding run once per possible first center (n deterministic restarts), each refined by Lloyd, keep the lowest-inertia partition (ties → lexicographically smallest assignment). Both algorithms canonicalize labels by ascending smallest-member index. Added a local `detail::NeumaierSum` (compensated reductions, matching rmt_clean) and `detail::canonicalize_signs` (first non-trivial eigenvector component positive). `Hierarchical 12/12/0/0` + `Sponge 12/12/0/0` (new `atx-core/tests/hierarchical_test.cpp`, `atx-core/tests/sponge_test.cpp` wired into the explicit `atx-core-tests` list): non-square/empty/k<1/k>N → InvalidArgument; both linkages recover 3 planted blocks at k=3; hierarchical ascending-index tie-break on an equicorrelation matrix; SPONGE splits a strongly anti-correlated pair/member into different clusters where the distance metric would merge it; k=1 all-zero / k=N identity; canonical labels invariant under row/col permutation + inverse; two-runs-equal; pinned FNV-1a-64 label digest (`15156715062202025513`, same offset/prime as the store fingerprint scheme; both fixtures yield labels `0,0,0,1,1,1,2,2,2`). Full atx-core suite green (1359 passed / 1 env-skipped) under `/W4 /permissive- /WX`-equivalent clang-cl. |

## Sprint S11 commits

| Commit  | Unit | Test counts |
|---------|------|-------------|
| `9845626` | S11-0 | ClusterScaffold 1/1/0/0 |
| `2b12a33` | S11-1 | RmtClean 12/12/0/0 |
| `ba7cb53` | S11-2 | Hierarchical 12/12/0/0, Sponge 12/12/0/0 |

## What Sprint S11 proves / Next sprint priorities

S11-0 freezes the clustering seams (Pattern B: numerics in atx-core, consumed by the engine and
indexed by the store) so S11-1 (RMT cleaner) and S11-2 (clusterer) can be implemented against stable
`MatX`-typed signatures, and S11-3/S11-4 can bind to the engine result type and store record without
re-litigating the shapes. The determinism contract is recorded in each header as TODO seams. S11-1
landed the RMT cleaner (MP-fit clip + guarded RIE) against the frozen seam. S11-2 landed the
clusterer: hierarchical (Ward/Average on the `sqrt(2(1-rho))` distance, ascending-index tie-break)
and SPONGEsym signed-graph spectral clustering (regularized signed-Laplacian generalized
eigenproblem + deterministic k-means on the inverse-eigenvalue-weighted bottom-k eigenvectors), both
emitting canonical ascending-smallest-member labels. Next: implement S11-3 (engine rolling cluster
panel), which composes `rmt_clean` → `cluster` over rolling correlation windows. S11-3 binds to the
as-built `cluster(const MatX&, ClusterConfig)` signature; note `ClusterConfig` now also carries
`tau_plus`/`tau_minus` (both default 1.0) used only by `Algo::SpongeSym`.
