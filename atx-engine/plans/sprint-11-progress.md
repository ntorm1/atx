# Sprint S11 — Unsupervised Return-Structure Clustering

**Status:** ✅ COMPLETE (S11-0..S11-6 + adversarial-review fixes landed; S11-7 deferred). All 80 S11 test cases green at `d33839b`.

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
| S11-5 | done   | `c819085` | Wired the rolling `ClusterPanel` into the DSL's existing group operators as an `IndClass.cluster` field — **no new operators / opcodes / VM dispatch / typecheck change**. The headline fact: `is_group_field()` already recognizes the `IndClass.` prefix, so a field literally named `IndClass.cluster` is group-typed by name and every `group_*` consumer (`group_neutralize`/`indneutralize`, `group_zscore`, `group_rank`, `group_mean`, `group_scale`, `group_count`, `cs_residualize`) reads it with zero machinery change. New header-only `atx/engine/alpha/cluster_field.hpp` (free functions, all `inline`): (1) **`broadcast_cluster_field(ClusterPanel, dates)`** → dense date-major f64 column (length `dates*instruments`) applying the S11-3 step-function hold (`snapshots[i].cluster_id` fills `[snapshots[i].date, snapshots[i+1].date)`; last snapshot holds to `dates`; pre-first-snapshot dates stay NaN). **Encoding: label `L` → `f64(L)`, `kUnclustered (-1)` → NaN** (`encode_cluster_label`), so an unclustered instrument is out-of-group exactly like a NaN-group cell in cs_ops — no kernel special case. (2) **`append_cluster_field(src Panel, ClusterPanel)`** → new OWNED Panel via `Panel::create` with `IndClass.cluster` appended as the last field (the headline OFFLINE path; instrument-count mismatch → `Err(InvalidArgument)`; source universe mask carried forward). Works on a freshly built OR an S11-4 `load_binary`-rehydrated `ClusterPanel` (same value type). (3) **Segment path**: bake the column into a sealed segment under the short tag `cluster` (the atx-tsdb `FieldEntry` field-name cap is **15 usable bytes** — the full 16-char `IndClass.cluster` is truncated to `IndClass.cluste` and would not resolve), attach via `attach_multi_segment_panel`, then `rename_field_to_cluster(attached)` re-labels `cluster` → `IndClass.cluster` (OWNED copy). (4) **Online path**: `cluster_group_map(ClusterPanel, date)` → `vector<u32>` per-universe-instrument group id (held-snapshot lookup: last snapshot with `date <= date`) for `WeightPolicy::to_target_weights(...)`; since WeightPolicy's `group_map` has NO NaN/no-group sentinel, an unclustered instrument is mapped to a DISJOINT SINGLETON id (`instruments + inst_index`, above the real-id space) so its per-group demean drives it to ~0 without contaminating a real cluster's mean. **Touched ONLY callers** — no edit to typecheck/registry/vm/oracle/segment_panel/weight_policy. `ClusterWiring 8/8/0/0` (new `atx-engine/tests/alpha/cluster_wiring_test.cpp`, auto-globbed into `atx-engine-alpha-tests`, mirrors the `alpha_cs_test.cpp` differential idiom): the **mandatory VM-vs-oracle differential** for `group_neutralize`/`group_zscore`/`group_rank`/`group_mean`/`indneutralize` over a planted `IndClass.cluster` column (bit-for-bit, NaN==NaN); typecheck-as-group-op (analyze accepts the group op end-to-end); kUnclustered→NaN excluded from its group + own output NaN; 2-snapshot broadcast step-function (date ranges + pre-first NaN); segment bake→attach→rename round-trip cell-for-cell + a group op on the attached panel; online group_map cluster-neutral (each cluster's weights sum to ~0, gross holds at leverage) + unclustered-is-singleton; two-runs-equal broadcast + pinned FNV-1a-64 digest (`5678576743557081587`, same offset `1469598103934665603`/prime `1099511628211` scheme as S11-1..S11-4, NaN normalized to canonical quiet-NaN). Full engine `alpha` suite green (501 passed) and `core`/loop suite green (294 passed) under `/W4 /permissive- /WX` clang-cl. |
| S11-6 | done   | `e0a2bec` | Implemented `atx::engine::eval::cluster_eval` (header-only/inline in `eval/cluster_eval.hpp`, matching the `deflated_sharpe.hpp` / `synthetic_alpha.hpp` header-only eval precedent — NO new engine-lib `.cpp`, NO CMake change; the test links `atx::engine` and reuses its already-compiled `compute_return_metrics` / `deflated_sharpe`). The HONEST-EVIDENCE unit: it MEASURES cluster-vs-GICS, it does not assume clusters win (research §8–§9 refuted every ">10%/Sharpe>1 over GICS" claim). Three instruments: (1) **`cluster_stability(snapshots, cfg)`** — per consecutive-snapshot transition it computes label CHURN and a per-cluster **best-match Jaccard recovery** (Hennig `clusterboot` analogue: `|A_c ∩ B_best| / |A_c ∪ B_best|` over jointly-clustered instruments, kUnclustered skipped), flagging any cluster below `cfg.jaccard_floor` (default 0.6). POINT-IN-TIME: transition i reads only snapshots i and i+1, so truncating the stream after i+1 leaves steps[0..i] byte-identical (asserted). Purely deterministic temporal Jaccard — **no resampling, no RNG**. (2) **`adjusted_rand_index` / `normalized_mutual_info`** — order-fixed Hubert-Arabie ARI and arithmetic-mean NMI between two integer labelings (NaN/negative excluded via a `std::map` order-fixed dense remap); ARI=1 / NMI=1 iff identical up to relabeling, ~0 when independent (ARI is 0 only in EXPECTATION — a finite table lands near, not at, 0). `agreement_vs_gics(panel, cluster_field, sector_field)` scores per-date ARI+NMI across the augmented panel. (3) **`run_head_to_head(panel, signal_expr, cfg) → Result<HeadToHead>`** — the fair contest: wraps `signal_expr` as `group_neutralize(sig, IndClass.sector)` vs `group_neutralize(sig, IndClass.cluster)` and runs BOTH through the EXISTING eval path (`parse→analyze→compile→Engine::evaluate→alpha::extract_streams→compute_return_metrics + deflated_sharpe`), emitting two `Scorecard`s (OOS Sharpe, DSR, max_dd, mean turnover, %ADV-capacity proxy = 1/turnover) plus the field-by-field `delta` (cluster − sector). Default frictionless `WeightPolicy{}`/`ExecutionSimulator{}` so the two arms differ ONLY in the grouping classifier. **`render_head_to_head(h) → vector<string>`** prints the metric lines AND the research §9 caveat (`kGicsCaveat`, "profitability over GICS is UNPROVEN … this delta is the test, not a result") INLINE as the last content line — the warning cannot be stripped from the numbers. Reused (no reinvention): `eval::compute_return_metrics`/`ReturnMetrics`, `eval::deflated_sharpe`/`DsrResult`, `eval::skewness`/`excess_kurtosis`/`mean_std_pop`/`MeanStd` (stats_ext), `alpha::extract_streams`/`AlphaStreams` (PnL + positions, no look-ahead w[t-1]·ret[t]), `alpha::append_cluster_field` (S11-5, builds the ONE panel carrying both classifiers). `ClusterEval 8/8/0/0` (new `atx-engine/tests/eval/cluster_eval_test.cpp`, auto-globbed into `atx-engine-eval-tests`): stable planted clusters → Jaccard≈1, nothing flagged; maximal shuffle → Jaccard 0.2 < floor AND all clusters flagged; ARI=1 when cluster≡sector (relabeled) + NMI=1, chance-level cross far below 1; head-to-head on a 24×8 real-shaped fixture emits BOTH scorecards (periods==dates, finite Sharpe, positive turnover) + delta == field-by-field difference; no-look-ahead (truncate-future-snapshots leaves transition-0 churn+Jaccard identical); two-runs-equal across stability+ARI+NMI+harness; render carries `kGicsCaveat` verbatim + the OOS Sharpe / %ADV capacity lines; per-date ARI/NMI strictly between 0 and 1 (cluster departs from but correlates with GICS). Whole engine `eval` suite green (65 passed / 12 suites) under `/W4 /permissive- /WX` clang-cl. **Did NOT wire into atx-impl** (its test group is not even built in this worktree; a report line would touch a different subsystem and risk the build — the optional clause says SKIP if it expands scope). The harness is READY to wire: `render_head_to_head` returns report lines a consumer can route into the atx-impl report with no change to this header. **DEFERRED real run (p3-S2)**: the operator builds the ORATS-backed p3-S1 real panel (golden digest `0x2a22a873483d9157`, ~5321 symbols), appends `IndClass.cluster` via `append_cluster_field` over a source that already carries `IndClass.sector`, and calls `run_head_to_head(real_panel, "rank(close)" /*or the candidate alpha*/, {})` — no code change. |
| S11-3 | done   | `5fcbb02` | Implemented `atx::engine::alpha::build_cluster_panel` (header-only/inline in `alpha/cluster_panel.hpp`, matching `panel.hpp` and the atx-core linalg headers). Frozen seam honored exactly: `ClusterPanelConfig{window=0, recluster_every=0, k=0, residualize=None}`, `ClusterPanel{Snapshot{DateIdx date, vector<int> cluster_id, i64 n_labels}, kUnclustered=-1, snapshots, instruments}`, `build_cluster_panel(const Panel&, ClusterPanelConfig) -> Result<ClusterPanel>`, signature unchanged. **ADDED two config fields** (both with spec-mandated defaults, so `ClusterScaffold` still sees `residualize==None`): `std::string return_field = "ret"` (FIELD-SELECTION CONTRACT: the single Panel field read as each instrument's per-date RETURN; unknown name propagates `Panel::field_id`'s `Err(NotFound)`) and `atx::core::cluster::Algo algo = Hierarchical` (exposes the partitioner; SpongeSym is selectable but deliberately not the default — its N-restart k-means is ≈O(N³) per window). Pipeline per rebalance date `t` (t = window-1, window-1+recluster_every, … while t < dates), using ONLY the strict window `[t-window+1, t]`: (1) valid set = instruments in-universe for the WHOLE window AND non-NaN at every window date (cs_ops valid-set semantics), M = |valid set|; M<2 → snapshot all `kUnclustered`, `n_labels=0`, no error; (2) materialize the M×window return block (rows = ascending dates, cols = ascending valid instruments); if `residualize==CAPM`, form an equal-weight cross-sectional market factor per date over the valid set, OLS-regress each column's window series on it (β=Cov/Var, flat market → β=0), replace with residual `r−β·mkt`; (3) order-fixed Neumaier-compensated Pearson correlation (ascending date, ascending pair; a zero-variance column → 0 off-diagonal, no divide-by-zero); (4) `rmt_clean(corr, q=M/window, {})`; (5) `cluster(cleaned, {.algo=cfg.algo, .k=min(cfg.k, M)})`; (6) scatter the M canonical labels back into a full-length `cluster_id` (kUnclustered outside the valid set), record `Snapshot{date=t}`. Config guards: window/recluster_every/k ≤ 0 or k>instruments → `Err(InvalidArgument)`; `window>dates` (no reachable rebalance date) → empty-snapshot panel (valid). Reuses the same local `detail::NeumaierSum` primitive as rmt_clean/cluster. **PIT/determinism**: each window is strictly ≤ t, so a snapshot at t is byte-identical with or without later bars (truncation-invariance), order-fixed reductions, no RNG, two-runs-equal. `ClusterPanel 13/13/0/0` (new `atx-engine/tests/alpha/cluster_panel_test.cpp`, auto-globbed into `atx-engine-alpha-tests`): no-look-ahead crux (per-snapshot truncate-to-t identity + the shared `validation/bias_audit.hpp` `check_no_lookahead` harness over the flattened snapshot stream); cadence (recluster_every=1 → 31 snapshots at 19…49, =20 → 2 at 19,39); out-of-universe + NaN instrument → kUnclustered for that window; planted regime shift (10 instruments, 2 blocks of 5, window 40, noise 0.15 — sized so both factor eigenvalues clear the MP edge) → switcher regroups after the shift, not before; clean 3-block recovery at k=3 (3 independent full-rank factor streams, blocks of 5 — sinusoidal factors collapse under rmt_clean and were replaced); algo field honored end-to-end (SpongeSym path exercised, deterministic, valid full-length partition); CAPM residual strips a dominant common market factor and recovers the masked block structure; two-runs digest-equal; pinned FNV-1a-64 panel digest (`4293541372186093814`, same offset/prime scheme as S11-1/S11-2); degenerate configs → InvalidArgument / guarded empty / unknown-field Err; empty-universe window → all-kUnclustered, n_labels 0. Full engine `alpha` suite green (445 passed) and `ClusterScaffold` still green under `/W4 /permissive- /WX`-equivalent clang-cl. |

## Review fixes (post-S11-6 adversarial review)

Four reviewer-confirmed defects fixed under strict TDD (`d33839b`). Each fix made the test
express the correct contract first, then green; cascaded golden digests were re-pinned ONLY after
the new output was independently confirmed correct (block recovery + two-runs-equal still green),
so the re-pin freezes verified-correct behavior.

1. **Ward linkage runs on SQUARED distances (Ward.D2, matching scipy `ward`).** `cluster.hpp`
   `hierarchical_partition`: the working matrix is initialized to `d²=2(1−ρ)` for Ward and the
   Lance-Williams recurrence `d'²=α_k·d_kc²+α_d·d_dc²+β·d_kd²` and the closest-pair selection both
   run on squared distances (no on-the-fly squaring of an already-Ward-updated distance, no √ on the
   result path). **Average (UPGMA) is unchanged** — its matrix stays in raw `d=√(2(1−ρ))` units and
   its size-weighted-mean recurrence is correct for ITS definition. Re-verified: `WardRecoversThreeBlocks`,
   `AverageRecoversThreeBlocks`, the equicorrelation tie-break, and two-runs-equal all stay green.
2. **SPONGEsym embedding: removed the `1/√(λ+eps)` inverse-eigenvalue weighting** (it put the LARGEST
   weight on the smallest-eigenvalue axis and at λ≈0 scaled one coordinate by ~1/√eps, a blow-up).
   The fix uses the **raw bottom-k generalized eigenvectors** with the canonical Ng–Jordan–Weiss
   **unit-row normalization** before k-means (a zero row is left at the origin, so no `1/√0` blow-up).
   Investigation (Eigen diagnostic on the pencils) showed the discriminative signed-cut signal lives
   in the lowest mode while the higher modes inside the degenerate eigenvalue bulk are arbitrary
   within-cluster axes whose raw magnitude otherwise dominates Euclidean k-means; row-normalization
   makes k-means key on the ANGLE between embedding rows (co-grouped point together, anti-correlated
   groups point apart), which is exactly the SPONGE contract. The bottom mode here is NOT a trivial
   near-constant mode (the constant mode sits at the TOP eigenvalue), so skip-trivial would discard
   the signal — raw-bottom-k + row-norm is the correct choice. **Fixture change:** `Sponge.AntiCorrelatedPairSplits`
   was 2-members-per-block, which is under-powered for the canonical embedding (two noise-free
   members cannot resolve the bottom eigenvectors; it only passed because the removed weighting
   suppressed the bulk modes). Strengthened to 3-members-per-block (`block_corr({0,0,0,1,1,1},0.8,-0.8)`),
   exercising the real contract — cf. S11-3's under-powered-fixture fix. `AntiCorrelatedMemberSeparated`
   and `RecoversThreeBlocks` now pass on the canonical embedding.
3. **store `cluster_panel.asof_date` / `created_at` declared TEXT, not INTEGER**, matching the ISO
   strings `ClusterPanelRecord`/`ParamsKey` store and bind/read as TEXT. Added `StoreSchema.ClusterPanelDateColumnsAreText`
   (reads `pragma_table_info('cluster_panel')` and asserts both columns are `TEXT`) so the corrected
   types are guarded against regression. `StoreSchemaGolden` + `ClusterPanelStore` green.
4. **eval `agreement_vs_gics` averages ARI/NMI only over SCORED dates** — dates with ≥1 labeled
   (non-NaN) instrument in BOTH cross-sections. A warm-up / fully-unlabeled date yields the degenerate
   1.0 ("trivially identical"); counting it pulled `mean_ari`/`mean_nmi` toward 1.0 and manufactured a
   false "cluster ≈ GICS" agreement. `per_date` keeps every date for inspection; a new `scored_dates`
   field exposes the effective sample size. New `ClusterEval.AgreementVsGics_WarmUpDatesExcludedFromMean`
   (cluster NaN for the first 4 of 10 dates) proves the mean reflects only the 6 scored dates, stays
   below 1.0, and is strictly below the warm-up-inflated naive all-dates mean.

**Re-pinned digests (verified-correct, not whatever-fell-out):**

| Test | Old digest | New digest | Confirming assertions |
|------|-----------|-----------|-----------------------|
| `ClusterPanel.BuildClusterPanel_PinnedDigest_MatchesGolden` | `4293541372186093814` | `1151214793510920852` | Changed by the Ward.D2 fix. Re-pinned only after `BuildClusterPanel_ThreeBlockFixture_RecoversThreeBlocks` (block recovery), `BuildClusterPanel_TruncatedPanelAtT_SnapshotIdentical` + the bias-audit harness (no look-ahead), and `BuildClusterPanel_TwoRuns_DigestEqual` (two-runs-equal) all stayed green on the new output. |
| `Hierarchical.DigestStability` | `15156715062202025513` | unchanged | Ward.D2 leaves the well-separated 3-block partition `{0,0,0,1,1,1,2,2,2}` identical (digest is over labels, not merge distances), so no re-pin — block recovery + two-runs still green. |
| `Sponge.DigestStability` | `15156715062202025513` | unchanged | The 3-block partition is unchanged under the new embedding (`RecoversThreeBlocks` green), so no re-pin. |
| `ClusterWiring.BroadcastTwoRunsEqual_AndPinnedDigest` | `5678576743557081587` | unchanged | The broadcast digest is over a hand-built `ClusterPanel` fixture, not the clustering algorithm, so it does not drift. |

Affected suites all green under `/WX`: atx-core 860/1-skip, atx-engine-alpha 501, atx-engine-store 30,
atx-engine-eval 66. Cluster-specific: `RmtClean`+`Hierarchical`+`Sponge` 37/37, `ClusterEval` 9/9,
`ClusterPanel`+`ClusterWiring` 21/21, `StoreSchema`+`ClusterPanelStore` 13/13.

## Sprint S11 commits

| Commit  | Unit | Test counts |
|---------|------|-------------|
| `9845626` | S11-0 | ClusterScaffold 1/1/0/0 |
| `2b12a33` | S11-1 | RmtClean 12/12/0/0 |
| `ba7cb53` | S11-2 | Hierarchical 12/12/0/0, Sponge 12/12/0/0 |
| `5fcbb02` | S11-3 | ClusterPanel 13/13/0/0 |
| `a952b7f` | S11-4 | ClusterPanelStore 11/11/0/0, StoreSchemaGolden green |
| `c819085` | S11-5 | ClusterWiring 8/8/0/0 |
| `e0a2bec` | S11-6 | ClusterEval 8/8/0/0 |
| `d33839b` | review fixes | Hierarchical 12/12/0/0, Sponge 12/12/0/0, ClusterPanel 13/13/0/0, ClusterWiring 8/8/0/0, ClusterEval 9/9/0/0, StoreSchema 2/2/0/0, ClusterPanelStore 11/11/0/0 |

## What Sprint S11 proves / Next sprint priorities

S11-0 freezes the clustering seams (Pattern B: numerics in atx-core, consumed by the engine and
indexed by the store) so S11-1 (RMT cleaner) and S11-2 (clusterer) can be implemented against stable
`MatX`-typed signatures, and S11-3/S11-4 can bind to the engine result type and store record without
re-litigating the shapes. The determinism contract is recorded in each header as TODO seams. S11-1
landed the RMT cleaner (MP-fit clip + guarded RIE) against the frozen seam. S11-2 landed the
clusterer: hierarchical (Ward/Average on the `sqrt(2(1-rho))` distance, ascending-index tie-break)
and SPONGEsym signed-graph spectral clustering (regularized signed-Laplacian generalized
eigenproblem + deterministic k-means on the raw bottom-k generalized eigenvectors with
Ng–Jordan–Weiss unit-row normalization — see "Review fixes" §2; the original inverse-eigenvalue
weighting was removed), both emitting canonical ascending-smallest-member labels. S11-3 landed the engine rolling cluster panel
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

S11-5 wired the labels into the DSL group operators as an `IndClass.cluster` field with NO new
operators (the `group_*` family keys the classifier by name via `is_group_field`). The offline path
(`append_cluster_field`) is the headline: build/load a `ClusterPanel`, broadcast it with the
step-function hold (label→f64, `kUnclustered`→NaN→out-of-group), and append it to a Panel the existing
VM/oracle evaluate bit-for-bit.

**S11-5 contract S11-6 (validation + GICS head-to-head) must honor** — when it runs
`group_neutralize(sig, IndClass.sector)` vs `group_neutralize(sig, IndClass.cluster)` on a
real-shaped panel:
- **Construct the cluster field with `append_cluster_field(src, cluster_panel)`** (in
  `atx/engine/alpha/cluster_field.hpp`). The source Panel must already carry the GICS `IndClass.sector`
  (or `sector`) column and the signal/return columns; the helper appends `IndClass.cluster` as a new
  last field, so ONE augmented Panel carries both classifiers for an apples-to-apples comparison.
- **Encoding**: a real label `L` is `f64(L)`; `kUnclustered (-1)` is **NaN** (out-of-group, matching
  cs_ops / the GICS missing-sector sentinel). So an instrument outside the point-in-time cluster valid
  set is excluded from its group reduction exactly like a missing GICS sector — the two classifiers are
  directly comparable cell-for-cell.
- **Instrument alignment**: `cluster_panel.instruments` MUST equal `src.instruments()` (mismatch →
  `Err(InvalidArgument)`); the cluster `cluster_id[i]` is universe-aligned to the SAME instrument axis
  as the source Panel (the S11-3 build contract), so no re-keying is needed.
- **Date coverage**: the broadcast holds each snapshot to the next rebalance; dates before the first
  snapshot (the warm-up `< window-1`) are NaN for EVERY instrument. A head-to-head over those early
  dates will see all-NaN cluster groups (no cluster opinion yet) while GICS sectors are already
  populated — compare only over `>= window-1` for a fair contest, or expect the cluster arm to be inert
  in the warm-up.
- **Online / loop arm** (if S11-6 drives a backtest rather than a static panel): use
  `cluster_group_map(cluster_panel, date)` for `WeightPolicy::to_target_weights`, but note its
  no-sentinel model maps unclustered names to disjoint SINGLETON groups (per-group demean → ~0), which
  differs from the offline NaN→out-of-group semantics; pick the arm that matches the GICS baseline's
  treatment of missing groups.

Note for any future segment-baked classifier: the atx-tsdb on-disk field-name cap is **15 usable
bytes**, so `IndClass.cluster` (16) cannot be a segment field name verbatim — bake under a ≤15-char tag
(S11-5 uses `cluster`) and `rename_field_to_cluster` on attach.

S11-6 landed the HONEST-EVIDENCE validation layer (`eval/cluster_eval.hpp`): temporal per-cluster
Jaccard stability (clusterboot analogue, flags clusters below the floor; PIT, no RNG), per-date ARI /
NMI between `IndClass.cluster` and `IndClass.sector`, and a `run_head_to_head` harness that runs the
SAME alpha neutralized by sector vs by cluster through the existing eval (`extract_streams` →
`compute_return_metrics` + `deflated_sharpe`) and emits both scorecards plus the delta, with the
research §9 caveat ("profitability over GICS is UNPROVEN — this delta is the test, not a result")
printed inline. Unit-tested on a small real-shaped fixture; NO 11GB data dependency.

**DEFERRED real p3-S2 head-to-head (data-gated, operator-driven):** the harness is callable as-is.
The operator must (1) build the ORATS-backed p3-S1 real panel (golden digest `0x2a22a873483d9157`,
~5321 symbols) — that ORATS data build is the deferred manual step — then (2) append the cluster
classifier via `alpha::append_cluster_field(real_panel_with_IndClass.sector, built_or_loaded_ClusterPanel)`
and (3) call `eval::run_head_to_head(augmented_panel, "<candidate alpha>", {})` and render it with
`render_head_to_head(...)`. No code change is required — only the real panel and the call site.

Optional follow-up: wire a `render_head_to_head` line into the atx-impl report (skipped here to avoid
expanding scope / risking the build; the harness already returns report-ready lines).

## Sprint close

**Status: COMPLETE.** Units S11-0..S11-6 implemented (subagent-driven, one marker commit + ledger
follow-up per unit), then four adversarial-review defects fixed (`d33839b`). Final gate: **80/80 S11
test cases pass** in one ctest run at fix HEAD (`RmtClean`+`Hierarchical`+`Sponge`+`ClusterPanel`+
`ClusterWiring`+`ClusterPanelStore`+`StoreSchema`+`ClusterEval`+`ClusterScaffold`); every touched
group binary green under `/W4 /permissive- /WX` clang-cl (atx-core 860, alpha 501, store 30, eval 66).

**S11-7 (IV/volume characteristic clustering) — DEFERRED, not built.** Per the source plan it is a
stretch unit and the deep-research pass found NO surviving primary claim that IV/volume *magnitude*
features are production clustering inputs (only return-correlation is verified). Building a second,
under-evidenced clustering would add unproven capability against the CIO guardrail ("stop adding
capability until the benchmark runs"). It is intentionally left for after the p3-S2 head-to-head
verdict, which S11-6 is built to feed.

**Known pre-existing issue (NOT introduced by S11):** a full-tree `cmake --build` ICEs clang-cl
18.1.8 (frontend crash / signal) while compiling `atx-engine/tests/data/data_e2e_byo_capstone_test.cpp`
(DATA group). S11 changed only core/alpha/store/eval and that TU includes `data/dataset_schema.hpp`
with no include path to any S11 header, so S11 cannot be the cause (an ICE is a compiler bug, not a
result of adding a struct + CREATE TABLE). Flagged for the data/build owner; out of scope for S11.

**Next:** the module is the input-quality lever the ROADMAP frames it as — a deterministic,
point-in-time, RMT-denoised rolling cluster id consumed by the existing `group_*` operators with zero
new DSL surface. It hands p3-S2 a better group-id to neutralize/rank against AND the honest
`run_head_to_head` harness to decide — on the real ORATS panel, with the research's refutations carried
inline — whether data-driven clusters actually beat GICS grouping. It does not assume they do.
