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
| S11-4 | done   | `a952b7f` | Implemented `atx::engine::store::cluster_panel` (header-only/inline in `store/cluster_panel.hpp`, matching `segment_index.hpp`). Persists a built `ClusterPanel` as an external binary artifact registered in a new `cluster_panel` SQLite table — the "cache of rolling cluster panels persisted to disk," keyed so identical config ⇒ identical artifact, promotable across envs. **Schema** (`schema.hpp` `create_all`, idempotent `CREATE TABLE IF NOT EXISTS`): `cluster_panel(panel_id TEXT PK, universe_id TEXT, window_start INTEGER, window_end INTEGER, recluster_every INTEGER, params_hash INTEGER, asof_date INTEGER, binary_path TEXT, content_hash INTEGER, algo TEXT, k INTEGER, created_at INTEGER, created_by_run_id TEXT)` + index `ix_cluster_panel_lookup(universe_id, asof_date, params_hash)`. **Binary artifact (external file, NOT a DB BLOB)** — version-1 little-endian layout, documented in the header: header `u32 magic=0x31504341 ('A','C','P','1')`, `u32 version=1`, `u64 instruments`, `u64 n_snapshots`; then per snapshot `u64 date`, `i64 n_labels`, `instruments × i32 label` (kUnclustered=-1 encoded as `0xFFFFFFFF`). `save_binary(path, ClusterPanel)→Result<u64 content_hash>` / `load_binary(path)→Result<ClusterPanel>` are an exact lossless round-trip; save→load→save reproduces byte-identical bytes + content_hash. `content_hash` / `content_hash_of_file` = FNV-1a-64 over the exact file bytes (store fingerprint offset/prime). **Registry** (mirrors segment_index): `register_panel` (INSERT OR REPLACE, idempotent on panel_id), `lookup(db, universe_id, asof_date, params_hash)→Result<optional<ClusterPanelRecord>>` (Ok(nullopt) on miss, not Err), `locate(...)→Result<string>` binary_path (Err(NotFound) on miss). **NOTE — the scaffold `lookup(db, panel_id)` declaration was REPLACED** with the spec'd `(universe_id, asof_date, params_hash)` signature, and `compute_params_hash` now takes a new `cluster_panel::ParamsKey` struct (not `ClusterPanelRecord`). **`compute_params_hash(ParamsKey)`** folds via `store::fingerprint` (same FNV-1a-64 offset/prime, length-prefixed strings) in FROZEN order: `universe_id` (str), `universe_content_hash` (u64), `source_content_hash` (u64), `window` (u64), `recluster_every` (u64), `k` (u64), `residualize` (str), `return_field` (str), `algo` (str) — pure, no wall-clock/RNG. **Promotion** (`promotion.hpp`): the Dev→UAT→PROD ATTACH copy now also `INSERT OR IGNORE INTO dest.cluster_panel SELECT * FROM main.cluster_panel` (cluster_panel rows are universe-scoped, not canon_hash-keyed, so the whole registry is carried forward) and dual-writes an `alpha_event(event_type='cluster_panel_built')` onto the dest timeline. **Golden schema guard updated** for the new table (the intended reviewed change). `ClusterPanelStore 11/11/0/0` (new `atx-engine/tests/store/cluster_panel_store_test.cpp`, auto-globbed into `atx-engine-store-tests`): register→lookup round-trip (all fields), lookup-miss→nullopt, register idempotent on panel_id, locate returns binary_path / Err on miss, binary save→load round-trip (snapshots + kUnclustered cells), save→load→save byte-stable + equal content_hash, params_hash identical-inputs-equal, params_hash two-runs-equal, params_hash differs on every one of the 9 fields, content_hash detects a flipped byte, promotion copies the row Dev→UAT→PROD AND writes the `cluster_panel_built` event. `StoreSchemaGolden` green; full store suite green (31/31) including `ClusterScaffold` and `Promotion`, under `/WX`. Header-only — no .cpp / no CMake change. |
| S11-3 | done   | `5fcbb02` | Implemented `atx::engine::alpha::build_cluster_panel` (header-only/inline in `alpha/cluster_panel.hpp`, matching `panel.hpp` and the atx-core linalg headers). Frozen seam honored exactly: `ClusterPanelConfig{window=0, recluster_every=0, k=0, residualize=None}`, `ClusterPanel{Snapshot{DateIdx date, vector<int> cluster_id, i64 n_labels}, kUnclustered=-1, snapshots, instruments}`, `build_cluster_panel(const Panel&, ClusterPanelConfig) -> Result<ClusterPanel>`, signature unchanged. **ADDED two config fields** (both with spec-mandated defaults, so `ClusterScaffold` still sees `residualize==None`): `std::string return_field = "ret"` (FIELD-SELECTION CONTRACT: the single Panel field read as each instrument's per-date RETURN; unknown name propagates `Panel::field_id`'s `Err(NotFound)`) and `atx::core::cluster::Algo algo = Hierarchical` (exposes the partitioner; SpongeSym is selectable but deliberately not the default — its N-restart k-means is ≈O(N³) per window). Pipeline per rebalance date `t` (t = window-1, window-1+recluster_every, … while t < dates), using ONLY the strict window `[t-window+1, t]`: (1) valid set = instruments in-universe for the WHOLE window AND non-NaN at every window date (cs_ops valid-set semantics), M = |valid set|; M<2 → snapshot all `kUnclustered`, `n_labels=0`, no error; (2) materialize the M×window return block (rows = ascending dates, cols = ascending valid instruments); if `residualize==CAPM`, form an equal-weight cross-sectional market factor per date over the valid set, OLS-regress each column's window series on it (β=Cov/Var, flat market → β=0), replace with residual `r−β·mkt`; (3) order-fixed Neumaier-compensated Pearson correlation (ascending date, ascending pair; a zero-variance column → 0 off-diagonal, no divide-by-zero); (4) `rmt_clean(corr, q=M/window, {})`; (5) `cluster(cleaned, {.algo=cfg.algo, .k=min(cfg.k, M)})`; (6) scatter the M canonical labels back into a full-length `cluster_id` (kUnclustered outside the valid set), record `Snapshot{date=t}`. Config guards: window/recluster_every/k ≤ 0 or k>instruments → `Err(InvalidArgument)`; `window>dates` (no reachable rebalance date) → empty-snapshot panel (valid). Reuses the same local `detail::NeumaierSum` primitive as rmt_clean/cluster. **PIT/determinism**: each window is strictly ≤ t, so a snapshot at t is byte-identical with or without later bars (truncation-invariance), order-fixed reductions, no RNG, two-runs-equal. `ClusterPanel 13/13/0/0` (new `atx-engine/tests/alpha/cluster_panel_test.cpp`, auto-globbed into `atx-engine-alpha-tests`): no-look-ahead crux (per-snapshot truncate-to-t identity + the shared `validation/bias_audit.hpp` `check_no_lookahead` harness over the flattened snapshot stream); cadence (recluster_every=1 → 31 snapshots at 19…49, =20 → 2 at 19,39); out-of-universe + NaN instrument → kUnclustered for that window; planted regime shift (10 instruments, 2 blocks of 5, window 40, noise 0.15 — sized so both factor eigenvalues clear the MP edge) → switcher regroups after the shift, not before; clean 3-block recovery at k=3 (3 independent full-rank factor streams, blocks of 5 — sinusoidal factors collapse under rmt_clean and were replaced); algo field honored end-to-end (SpongeSym path exercised, deterministic, valid full-length partition); CAPM residual strips a dominant common market factor and recovers the masked block structure; two-runs digest-equal; pinned FNV-1a-64 panel digest (`4293541372186093814`, same offset/prime scheme as S11-1/S11-2); degenerate configs → InvalidArgument / guarded empty / unknown-field Err; empty-universe window → all-kUnclustered, n_labels 0. Full engine `alpha` suite green (445 passed) and `ClusterScaffold` still green under `/W4 /permissive- /WX`-equivalent clang-cl. |

## Sprint S11 commits

| Commit  | Unit | Test counts |
|---------|------|-------------|
| `9845626` | S11-0 | ClusterScaffold 1/1/0/0 |
| `2b12a33` | S11-1 | RmtClean 12/12/0/0 |
| `ba7cb53` | S11-2 | Hierarchical 12/12/0/0, Sponge 12/12/0/0 |
| `5fcbb02` | S11-3 | ClusterPanel 13/13/0/0 |
| `a952b7f` | S11-4 | ClusterPanelStore 11/11/0/0, StoreSchemaGolden green |

## What Sprint S11 proves / Next sprint priorities

S11-0 freezes the clustering seams (Pattern B: numerics in atx-core, consumed by the engine and
indexed by the store) so S11-1 (RMT cleaner) and S11-2 (clusterer) can be implemented against stable
`MatX`-typed signatures, and S11-3/S11-4 can bind to the engine result type and store record without
re-litigating the shapes. The determinism contract is recorded in each header as TODO seams. S11-1
landed the RMT cleaner (MP-fit clip + guarded RIE) against the frozen seam. S11-2 landed the
clusterer: hierarchical (Ward/Average on the `sqrt(2(1-rho))` distance, ascending-index tie-break)
and SPONGEsym signed-graph spectral clustering (regularized signed-Laplacian generalized
eigenproblem + deterministic k-means on the inverse-eigenvalue-weighted bottom-k eigenvectors), both
emitting canonical ascending-smallest-member labels. S11-3 landed the engine rolling cluster panel
(`build_cluster_panel`): per rebalance date it selects the point-in-time valid instrument set from the
strict window `[t-window+1, t]` (never reads beyond t), optionally CAPM-residualizes against an
equal-weight market factor, builds the order-fixed windowed correlation, cleans it (`rmt_clean`), and
partitions it (`cluster`, Hierarchical default), scattering canonical labels back into a full-length
`cluster_id` with `kUnclustered` outside the valid set. The crux — truncation-invariance (a snapshot
at t is byte-identical with or without future bars) — is proven both directly and through the shared
`bias_audit.hpp` harness.

**S11-3 contract S11-4 (store persistence) and S11-5 (DSL wiring) must honor:**
- **Return field**: `ClusterPanelConfig.return_field` (default `"ret"`) names the single Panel field
  read as each instrument's per-date return; the correlation and the CAPM market factor are computed
  on that one field. CAPM residualization (`residualize==CAPM`) is self-contained — the equal-weight
  cross-sectional mean of the valid set per window date is the market factor, so no extra market field
  is needed. `algo` (default `Hierarchical`) selects the partitioner; both fields are part of the
  params identity S11-4's `compute_params_hash` should fold (alongside window / recluster_every / k /
  residualize / universe).
- **Snapshot → date-range broadcast (the S11-5 step-function hold)**: S11-3 emits exactly ONE snapshot
  per rebalance date `t`, in ascending-date order. A snapshot's labels are the partition that holds
  AS OF `t` and remain valid (point-in-time) until the next rebalance — S11-5 broadcasts snapshot `i`'s
  `cluster_id` to the date range `[snapshots[i].date, snapshots[i+1].date)` (the last snapshot holds to
  the panel end) when materializing `IndClass.cluster`. Dates before the first snapshot (`< window-1`)
  have no labeling and read `kUnclustered`. `kUnclustered=-1` is the sentinel for an instrument outside
  the valid set; S11-5 must map it to "no group" rather than a real cluster id 0.

**S11-4 contract S11-5 (loading a persisted panel) must honor:**
- **Lookup key**: a cached panel is resolved by `cluster_panel::lookup(db, universe_id, asof_date,
  params_hash) → Result<optional<ClusterPanelRecord>>` (a miss is `Ok(nullopt)`, NOT an error), or
  `locate(...)` for just the `binary_path`. `params_hash` is `compute_params_hash(ParamsKey{...})` —
  S11-5 MUST build the `ParamsKey` with the SAME field values used at build time (universe_id +
  universe/source content hashes + window + recluster_every + k + residualize + return_field + algo)
  or it will not collide with the registered row.
- **Loading the artifact**: `load_binary(record.binary_path) → Result<ClusterPanel>` reconstructs the
  exact panel (snapshots in stored ascending-date order, `kUnclustered=-1` preserved). Before trusting
  it, S11-5 should verify integrity via `content_hash_of_file(path) == record.content_hash`
  (the registry pins the bytes). The binary format is version-1 (`magic 0x31504341 'ACP1'`); `load_binary`
  rejects a bad magic / unsupported version / size-mismatch with `Err(InvalidArgument)`.
- The snapshot→date-range broadcast / `kUnclustered`→"no group" rules from the S11-3 contract above
  still apply to a loaded panel — it is the same `ClusterPanel` value type, just rehydrated from disk.

Next: S11-5+ wire the labels as cluster-aware alpha / risk grouping (loading the persisted panel via the
contract above). S11-3 binds to the as-built `cluster(const MatX&, ClusterConfig)` and
`rmt_clean(const MatX&, f64 q, RmtConfig)` signatures.
