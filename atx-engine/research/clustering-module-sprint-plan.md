# Sprint S11 — Unsupervised Return-Structure Clustering ("Covariance-Based Sectors") — Implementation Plan

**Status:** 📋 PLAN (frozen spec for kickoff — not yet opened)
**Proposed worktree:** `.claude/worktrees/s11-return-clustering`
**Proposed branch:** `worktree-s11-return-clustering`
**Base:** `main` after `feat/megaalpha-enrich-validate` lands (ROADMAP §"What's next" #0 — confirm at kickoff)
**Source research:** [`unsupervised-stock-clustering-deep-dive.md`](unsupervised-stock-clustering-deep-dive.md) (5-angle deep-research pass: 22 sources, 20 verified findings, 5 refuted)
**Prior progress:** `feat/megaalpha-enrich-validate` ships "GICS sector → DSL datafields/groups" — this sprint replaces that *static* group-id with a *data-driven, rolling* one consumed by the **same** operators.

> ⚠️ **Number is provisional.** p2 reached S10; p3 is at S1/S2/S3; persistence-v2 is "Tasks 1–9". S11 is the proposed next free cross-module sprint id. Confirm against the canonical [ROADMAP](../plans/ROADMAP.md) before opening; this is a **cross-module** sprint (atx-core + atx-engine + store + DSL), not a single-module unit.

> 🧭 **CIO-guardrail note.** ROADMAP says *"stop adding capability until the benchmark (p3-S2) runs."* Per kickoff decision this module is approved as a **standalone track now**, but it is deliberately framed as an **input-quality lever**, not a new pipeline stage: the headline feature reuses the *existing* `group_*` operators (§"Key code-review finding"), and the validation unit (S11-6) is a **head-to-head against GICS-grouping on the p3 real panel** — i.e., it is built to *feed and be judged by* the decisive experiment, not to bypass it. Profitability over GICS is **unproven** (research §9 refuted all ">10% / Sharpe>1" claims); S11-6 is the honest test, not an assumption.

---

## Why this sprint exists

The deep-research pass (source above) established the well-attested quant practice: **replace GICS/sector labels with clusters built directly from market data** — Winton's *"covariance-based sectors"*. The verified pipeline is: residualize returns (strip the market factor) → build a rolling correlation matrix → **denoise it with Random Matrix Theory** → partition with **hierarchical** clustering and/or **signed-graph (SPONGE)** community detection → use the resulting cluster ids exactly where you would use a sector.

A code review of the atx-engine alpha DSL, evaluation pipeline, and persistence-v2 store was mapped against this. **The headline result: the consumption side is already built.** The DSL has a full family of cross-sectional group operators that take a group-id field; the panel model accepts arbitrary f64 fields; the store has an external-binary-artifact pattern with deterministic fingerprinting. What is *missing* is the **producer**: a deterministic, point-in-time, rolling **cluster-panel builder** plus a place to **persist** its output. This sprint builds the producer and wires its output into the existing consumer.

### Key code-review finding — the headline feature needs ZERO new DSL operators

The DSL already ships cross-sectional group operators that reduce-and-broadcast within a group ([`registry.cpp:45-52`](../src/alpha/registry.cpp), kernels in [`cs_ops.hpp`](../include/atx/engine/alpha/cs_ops.hpp)):

| DSL function | OpCode | Behavior | Kernel |
|---|---|---|---|
| `group_mean(x, g)` | `CsMeanG` | per-group arithmetic mean, broadcast to members | `cs_group_count_mean_row` |
| `group_neutralize(x, g)` / `indneutralize` | `CsNeutG` / `CsDemeanG` | `x − group_mean(x, g)` (per-group demean) | `cs_group_demean_row` |
| `group_zscore(x, g)` | `CsZscoreG` | `(x − μ_g) / σ_g` within group | `cs_group_row` |
| `group_rank(x, g)` | `CsRankG` | ordinal percentile within group | `cs_group_row` |
| `group_scale(x, g)` | `CsScaleG` | rescale so Σ\|x\| per group = 1 | `cs_group_scale_row` |
| `group_count(x, g)` | `CsCountG` | member count per group | `cs_group_count_mean_row` |
| `cs_residualize(x, g[, z])` | `CsResidualize` | per-group demean or FWL partial-out | `cs_residualize_row` |

The group-id argument `g` is a `CrossSection` of f64-widened integer labels, recognized as a `Group` dtype **purely by field name** — `sector` or the `IndClass.*` prefix ([`typecheck.hpp:124-128`](../include/atx/engine/alpha/typecheck.hpp), `is_group_field()`). Grouping is `O(n)` via a reusable open-addressing hash table (`CsScratch`, [`cs_ops.hpp:151-234`](../include/atx/engine/alpha/cs_ops.hpp)) with ascending-instrument-index scan order for bit-for-bit determinism.

**Therefore, name the cluster field `IndClass.cluster` and the user's headline feature is already a one-liner:**

```text
# "stock X's deviation from its cluster's average 10-day return"
ret10 = close / delay(close, 10) - 1
dev   = group_neutralize(ret10, IndClass.cluster)     # ret10 − mean(ret10 over X's cluster)

# richer forms, all already supported:
group_zscore(ret10, IndClass.cluster)                 # cluster-relative z-score (deviation in σ units)
group_rank(volume / adv20, IndClass.cluster)          # within-cluster liquidity percentile
cs_residualize(close/delay(close,20)-1, IndClass.cluster)   # cluster-residual momentum
```

No `OpCode`, no kernel, no VM-dispatch, no oracle change is required for the headline feature. The entire sprint is **producer + persistence + wiring + honest validation.**

### Where the producer plugs in (code-review seams)

- **Panel injection**: `alpha::Panel::create()` accepts arbitrary `field_names`/`field_data` ([`panel.hpp:88-90`](../include/atx/engine/alpha/panel.hpp)) — append `IndClass.cluster` as a date-major f64 column. Derived-field path `with_datafields()` ([`datafields.hpp:154`](../include/atx/engine/alpha/datafields.hpp)) is the precedent.
- **Segment injection (zero-copy, persistent)**: cluster ids can be baked into a tsdb sealed segment at load and attached via `attach_multi_segment_panel()` ([`segment_panel.hpp:94-114`](../include/atx/engine/alpha/segment_panel.hpp)); the segment binary format (date-major F×T×N) is [`segment.hpp:53-95`](../../atx-tsdb/include/atx/tsdb/segment.hpp).
- **Online path**: `WeightPolicy::to_target_weights()` already takes a `group_map` for industry-neutralization ([`weight_policy.hpp:197-206`](../include/atx/engine/loop/weight_policy.hpp)) — the cluster map drops in there for the backtest loop.
- **Store artifact pattern**: panels are *not* DB BLOBs; the store registers external binary files keyed by id + `content_hash`, indexed in SQLite (`segment` table + `segment_index` module, [`segment_index.hpp:15-84`](../include/atx/engine/store/segment_index.hpp)), with deterministic FNV-1a fingerprints ([`fingerprint.hpp:16-71`](../include/atx/engine/store/fingerprint.hpp)) and cross-env promotion ([`promotion.hpp:19-111`](../include/atx/engine/store/promotion.hpp)). The cluster panel follows this exact pattern.
- **Constraint (anti-roadmap)**: *"no engine-local general-purpose primitives — atx-core requests = Pattern B edges."* RMT denoising + linkage + SPONGE eigen-solves are general numerical primitives → they live in **atx-core** (reusing `atx-core/linalg/pca`, already cited by S10-5), not engine-local.

---

## Gap analysis (research capability → current atx state → gap → unit)

| # | Research-verified capability | Current atx state | Gap | Unit |
|---|---|---|---|---|
| C1 | RMT denoise the correlation matrix (MP-fit, eigenvalue clip, RIE) before use (research §4) | `atx-core/linalg/pca` exists; S8 covariance has shrinkage/eigenfactor work but no MP-fit/RIE *cleaning* primitive exposed for clustering | No reusable RMT-cleaning primitive | **S11-1** |
| C2 | Hierarchical (Winton) + signed-graph SPONGEsym (Oxford-Man) partitioning (research §5–§6) | none — engine has no clustering | No clustering primitives | **S11-2** |
| C3 | Rolling, **point-in-time**, residual-return correlation → cluster panel (research §2.1, §2.3) | residual returns computable in DSL; no rolling cluster builder; PIT machinery exists (`bias_audit`, CPCV purge/embargo) | No PIT cluster-panel producer | **S11-3** |
| C4 | Persist a cache of rolling cluster panels to disk, keyed by config (research §2.3 cadence) | store persists external binary artifacts + FNV fingerprints; **no feature/panel cache layer** (every run recomputes) | No persisted cluster-panel artifact type | **S11-4** |
| C5 | Use cluster id as a grouping field / feature in alphas (research §1, §7) | full `group_*` operator family keyed on `sector`/`IndClass.*` | Field not produced/wired; (optional) bare-name support | **S11-5** |
| C6 | Validate clusters: stability + agreement vs GICS; profitability is *contested* (research §8, §9) | `bias_audit`, CPCV, eval metrics exist; no cluster-stability / ARI-vs-GICS / head-to-head | No cluster validation + GICS-bench harness | **S11-6** |
| C7 | IV/volume multi-feature clustering — **under-evidenced** in research (§2.2, open Q1) | options/IV present via ORATS enrich; no multi-feature cluster builder | (deferred) multi-feature inputs | **S11-7 (stretch / defer)** |

**Explicitly NOT in scope** (do not rebuild): the `group_*` operators (done — just feed them); the tsdb segment binary format (reuse); the store fingerprint/promotion machinery (reuse); PCA/eigensolvers in atx-core (extend, don't reinvent). Live re-clustering inside a running broker session is out of scope (batch/offline producer only).

---

## Realistic scope for this sprint

Eight units (incl. marker + close). Split per the existing `a`/`b`/`c` convention if compile fan-out exceeds the ~7-unit guideline:
- **S11a** {S11-1, S11-2} — atx-core numerical primitives (RMT cleaning + clustering).
- **S11b** {S11-3, S11-4} — engine PIT cluster-panel builder + persisted artifact.
- **S11c** {S11-5, S11-6} — DSL wiring + validation/GICS benchmark.
- **S11-7** — stretch (IV/volume features), defer unless cheap.

1. **S11-1** — atx-core RMT correlation-cleaning primitive (MP-fit cutoff, eigenvalue clip, optional RIE shrink).
2. **S11-2** — atx-core clustering primitives: (a) hierarchical linkage on the correlation distance; (b) signed-graph SPONGEsym.
3. **S11-3** — engine rolling **point-in-time** cluster-panel builder (residualize → correlate → denoise → cluster → emit `cluster_id[date][inst]`).
4. **S11-4** — persisted `cluster_panel` store artifact (binary on disk + SQLite registry + deterministic params hash + promotion hook).
5. **S11-5** — DSL/eval wiring: expose `IndClass.cluster` field to offline Panel + sealed segment + online `WeightPolicy` group_map.
6. **S11-6** — validation + GICS head-to-head benchmark (stability, ARI vs GICS, scorecard delta on the p3 real panel).
7. **S11-7** — *(stretch)* IV/volume multi-feature clustering inputs.

---

## Per-unit specifications

> Every unit obeys the existing discipline, quoted verbatim into each sub-agent brief:
> **"Marker-commit pattern is mandatory: commit before stopping or work is lost."**
> Plus the determinism contract (§"Determinism contract") and the [`plans/docs/implementation-quality.md`](../plans/docs/implementation-quality.md) standard. atx-core changes are **Pattern B edge requests** (anti-roadmap): general numerics go to atx-core, engine consumes.

### S11-0 — Marker + ledger + scaffold
- **Scope:** Open `sprint-11-progress.md` ledger (header per [`plans/docs/sprint.md`](../plans/docs/sprint.md) §required-structure). Fwd-decl scaffold: `atx-core` headers for `rmt_clean` + `cluster` (namespace `atx::core::linalg` / `atx::core::cluster`, Pattern B edge filed); engine header `alpha/cluster_panel.hpp` (namespace `atx::engine::alpha`); store header `store/cluster_panel.hpp` (namespace `atx::engine::store`). No logic.
- **Acceptance:** builds green under `/W4 /permissive- /WX`; `ClusterScaffold 1/1/0/0`.
- **Commit:** `docs(s11-0): open sprint-11 ledger + clustering scaffold (core/engine/store)`.

### S11-1 — atx-core RMT correlation-cleaning primitive
- **Rationale (C1, research §4):** sample correlation is mostly noise when `N≈T` (~94% of S&P spectrum indistinguishable from random); denoise *before* clustering or clusters fit noise.
- **Scope:** New `atx-core/linalg/rmt_clean.{hpp,cpp}` (Pattern B). `CleanedCorr rmt_clean(const SymMatrix& C, double q /*=N/T*/, RmtConfig cfg)` providing:
  - **MP-fit cutoff**: fit Marchenko-Pastur PDF to the empirical eigenvalue density to estimate `σ²`, derive `λ₊ = σ²(1+√q)²` (research §4.1 — minimize SSE between analytic MP PDF and KDE of eigenvalues; mirror de Prado's `findMaxEval`).
  - **Eigenvalue clipping** (default): replace sub-`λ₊` eigenvalues with their average so the trace is preserved; keep eigenvectors fixed.
  - **RIE / nonlinear shrink** (optional `cfg.mode = Clip | RIE`): rotationally-invariant eigenvalue reshaping (research §4.2). RIE requires `T>N` — guard and fall back to clip otherwise.
  - Re-symmetrize, restore unit diagonal, return PSD-repaired matrix.
- **Acceptance:** `RmtClean` suite ≥ 8 tests: planted signal-plus-noise matrix → clip removes bulk, keeps outliers (eigenvalue count match); trace preserved under clip (1e-9); cleaned matrix PSD (min eigenvalue ≥ −1e-12); `q→0` (T≫N) ≈ identity-cleaning (near no-op); RIE falls back to clip when `T<N`; two-runs-equal; FNV digest of cleaned matrix stable. `RmtClean total/0/0`.
- **Determinism:** ascending-eigenvalue order; fixed eigenvector sign convention (first nonzero component positive); Neumaier summation; no RNG.
- **Commit:** `feat(s11-1): RMT correlation cleaning (MP-fit clip + optional RIE) in atx-core`.

### S11-2 — atx-core clustering primitives (hierarchical + signed-graph SPONGE)
- **Rationale (C2, research §5–§6):** the two verified partitioners. Hierarchical = Winton "covariance-based sectors" (unsigned, dendrogram). SPONGEsym = the Oxford-Man stat-arb signed-graph method that keeps negative correlations.
- **Scope:** New `atx-core/cluster/{hierarchical,sponge}.{hpp,cpp}` (Pattern B). Common `Clustering cluster(const SymMatrix& C, ClusterConfig cfg)` → dense `cluster_id` per row + canonicalized labels.
  - **(a) Hierarchical**: distance `d_ij = √(2(1−ρ_ij))` (research §3), agglomerative **Ward** or **average** linkage (`cfg.linkage`), cut to `cfg.k` clusters (or by dendrogram-height threshold).
  - **(b) SPONGEsym** (research §6): decompose adjacency `A=C̃` into `A⁺=max(A,0)`, `A⁻=max(−A,0)`; form regularized Laplacians `L⁺, L⁻`; solve the generalized eigenproblem `(L⁺+τ⁻D⁻)v = λ(L⁻+τ⁺D⁺)v`; **deterministic** k-means++ on the bottom-`k` generalized eigenvectors.
  - **Label canonicalization** (both): relabel clusters by ascending smallest-member instrument index, so `cluster_id` is a stable, digestable function of the input (NOT of internal scan/allocation order).
- **Acceptance:** `Hierarchical` suite ≥ 7 + `Sponge` suite ≥ 7: planted block-correlation matrix (3 clean blocks) → both recover the 3 blocks exactly; hierarchical tie-break deterministic (equal merge distances broken by ascending index); SPONGE separates an anti-correlated pair into different clusters where the distance-collapse would not; canonical labels invariant under input row permutation+inverse; degenerate single-cluster (`k=1`) trivial; two-runs-equal; cluster-id digest stable. `Hierarchical total/0/0`, `Sponge total/0/0`.
- **Determinism:** **k-means++ init is the determinism hazard** — use deterministic D²-furthest-first seeding (no RNG on the result path), or a PRNG seeded *only* from the content hash of the eigen-embedding and isolated per the contract ("RNG only permitted in candidate init"). Order-fixed reductions; ascending-eigenvalue; canonical labels. Must pass two-runs-equal.
- **Commit:** `feat(s11-2): hierarchical + signed-graph SPONGEsym clustering in atx-core`.

### S11-3 — engine rolling point-in-time cluster-panel builder
- **Rationale (C3, research §2):** turn primitives into a `cluster_id[date][inst]` panel computed **without look-ahead**, on a rolling window, at a configured cadence. This is the crux unit and the #1 correctness risk.
- **Scope:** New `alpha/cluster_panel.{hpp,cpp}`. `ClusterPanel build_cluster_panel(const Panel& src, ClusterPanelConfig cfg)`:
  - For each **rebalance date** `t` (every `cfg.recluster_every` bars), using **only** bars in `[t−cfg.window+1, t]` (strictly ≤ t; reuse CPCV purge/embargo boundary from [`eval/cpcv.hpp`](../include/atx/engine/eval/cpcv.hpp)):
    1. compute returns; **residualize** vs market (CAPM, rolling β) when `cfg.residualize=CAPM` (research §2.1);
    2. correlation over the window (valid-set = in-universe ∧ non-NaN, mirroring `cs_ops` semantics);
    3. `rmt_clean` (S11-1); 4. `cluster` (S11-2);
    5. write the resulting `cluster_id` as the cross-section for dates `[t, next_t)` (step-function: hold between re-clusterings).
  - Symbols with insufficient history or out-of-universe at `t` → `NaN` cluster id (the group ops already treat NaN-group as out-of-set).
  - Output is a date-major f64 panel column ready for `Panel`/segment injection (§S11-5) **and** for persistence (§S11-4).
- **Acceptance:** `ClusterPanel` suite ≥ 9 tests: **no-look-ahead truncation-invariance** (cluster row at `t` byte-identical whether or not future bars exist — reuse [`validation/bias_audit.hpp`](../include/atx/engine/validation/bias_audit.hpp) style) — *the critical guard*; step-function hold between rebalances; insufficient-history symbol → NaN; planted regime-shift fixture → cluster membership changes after the shift, not before; `recluster_every=1` vs `=20` both deterministic; CAPM-residual path strips a planted common factor (clusters differ from raw-return clusters); two-runs-equal; panel digest stable; empty-universe degenerate guarded. `ClusterPanel total/0/0`.
- **Determinism:** lexicographic date/instrument enumeration; fixed window boundaries; inherits S11-1/S11-2 determinism; no RNG on result path.
- **Commit:** `feat(s11-3): point-in-time rolling cluster-panel builder (residualize→denoise→cluster)`.

### S11-4 — persisted `cluster_panel` store artifact
- **Rationale (C4, research §2.3):** the user's "cache of rolling cluster panels persisted to disk." Cache hits avoid recompute; keyed deterministically so identical config = identical artifact, promotable across envs.
- **Scope:** New `store/cluster_panel.{hpp}` mirroring [`segment_index.hpp`](../include/atx/engine/store/segment_index.hpp). Add a `cluster_panel` table to [`schema.hpp`](../include/atx/engine/store/schema.hpp) `create_all()`:
  - columns: `panel_id TEXT PK`, `universe_id`, `window_start`, `window_end`, `recluster_every`, `params_hash INTEGER`, `asof_date`, `binary_path TEXT`, `content_hash INTEGER`, `algo TEXT` (`hier`|`sponge`), `k`, `created_at`, `created_by_run_id`; index `(universe_id, asof_date, params_hash)`.
  - `register_panel()`, `lookup(universe_id, asof_date, params_hash)`, `locate()` (binary path). Binary stored on disk in the tsdb segment format (date-major), **not** a DB BLOB — consistent with `segment`.
  - `compute_params_hash(ClusterPanelConfig + universe_content_hash + snapshot_content_hash)` reusing the FNV-1a length-prefixed folding from [`fingerprint.hpp`](../include/atx/engine/store/fingerprint.hpp) (same offset/prime) so the cache key is replay-stable and cross-platform.
  - Promotion: extend [`promotion.hpp`](../include/atx/engine/store/promotion.hpp) ATTACH pattern to copy `cluster_panel` rows Dev→UAT→PROD; dual-write an `alpha_event` (`event_type='cluster_panel_built'`) for the timeline.
- **Acceptance:** `ClusterPanelStore` suite ≥ 7 tests: register→lookup round-trips; identical config → identical `params_hash` (replay), any field change → different hash (sensitivity, mirror `fingerprint_test`); `content_hash` mismatch detected; promotion copies row + writes ledger/event; **golden-schema test updated** ([`store_schema_golden_test.cpp`](../tests/store/store_schema_golden_test.cpp) — `cluster_panel` added to the table set); two-runs-equal on the hash. `ClusterPanelStore total/0/0`.
- **Determinism:** FNV-1a params hash (no wall-clock/RNG in the key); content_hash = CRC of the binary; schema idempotent.
- **Commit:** `feat(s11-4): persisted cluster_panel artifact (binary + SQLite registry + FNV key + promotion)`.

### S11-5 — DSL / eval wiring: expose `IndClass.cluster`
- **Rationale (C5):** connect producer to the existing consumer. After this unit the headline DSL one-liners (§"Key code-review finding") evaluate end-to-end.
- **Scope:**
  - **Offline Panel path**: helper to append the cluster column (from S11-3 or loaded from the S11-4 cache) as field `IndClass.cluster`; auto-typed `Group` by existing `is_group_field()` ([`typecheck.hpp:124-128`](../include/atx/engine/alpha/typecheck.hpp)) — **no typecheck change required**.
  - **Segment path**: allow the cluster column to be baked into a sealed segment at build and selected via `attach_multi_segment_panel(... fields ...)` ([`segment_panel.hpp:112`](../include/atx/engine/alpha/segment_panel.hpp)).
  - **Online path**: feed the per-date cluster cross-section as the `group_map` to `WeightPolicy::to_target_weights()` ([`weight_policy.hpp:197-206`](../include/atx/engine/loop/weight_policy.hpp)) so cluster-neutralization works in the backtest loop.
  - *(Optional, 1-line)* extend `is_group_field()` to also accept bare `cluster` if the team prefers `group_mean(x, cluster)` ergonomics — gated, default off to avoid namespace surprise.
- **Acceptance:** `ClusterWiring` suite ≥ 6 tests: `group_neutralize(ret10, IndClass.cluster)` compiles + evaluates, **VM matches oracle bit-for-bit** (the mandatory differential test, [`alpha_cs_test.cpp`](../tests/alpha/alpha_cs_test.cpp) pattern) on a fixture with a planted cluster column; NaN-cluster cell stays NaN; segment-baked cluster column round-trips; online group_map neutralizes a planted 2-cluster book; two-runs-equal. `ClusterWiring total/0/0`.
- **Determinism:** reuses the existing `CsScratch` ascending-index group reduction (already deterministic); no new RNG.
- **Commit:** `feat(s11-5): wire IndClass.cluster into Panel/segment/weight-policy for group ops`.

### S11-6 — validation + GICS head-to-head benchmark
- **Rationale (C6, research §8–§9):** clusters must be *stable* to be tradable, and the whole premise is they *differ usefully* from GICS — but profitability over GICS is **unproven** (all ">10%/Sharpe>1" claims refuted). This unit produces the honest evidence, not an assumption.
- **Scope:** New `eval/cluster_eval.{hpp,cpp}` + an `atx-impl` report wiring:
  - **Stability**: temporal label-churn + bootstrap Jaccard recovery per cluster across rolling re-estimations (research §8; `clusterboot` analogue). Flag clusters below `cfg.jaccard_floor` (default 0.6).
  - **Agreement vs GICS**: Adjusted Rand Index / NMI between `IndClass.cluster` and `IndClass.sector` per date (how far the data departs from sectors).
  - **Head-to-head**: run an identical alpha grouped two ways — `group_neutralize(sig, IndClass.sector)` vs `group_neutralize(sig, IndClass.cluster)` — over the **p3-S1 real panel** (golden digest `0x2a22a873483d9157`, ~5321 symbols); report the **scorecard delta** (OOS Sharpe/DSR, turnover, %ADV capacity) with research §9 caveats printed inline.
- **Acceptance:** `ClusterEval` suite ≥ 7 tests: stable planted clusters → Jaccard ≈ 1; shuffled-each-period clusters → Jaccard ≈ 0 + flagged; ARI = 1 when cluster≡sector, ≈ 0 when independent; head-to-head harness runs on a small real-shaped fixture and emits both scorecards; no-look-ahead in the rolling stability estimate; two-runs-equal; report lines present. `ClusterEval total/0/0`.
- **Determinism:** order-fixed metrics; PIT stability windows; eigen/perm order fixed; no RNG.
- **Commit:** `feat(s11-6): cluster stability + ARI-vs-GICS + GICS-grouping head-to-head benchmark`.

### S11-7 — *(stretch)* IV/volume multi-feature clustering inputs
- **Rationale (C7, research §2.2 — UNDER-EVIDENCED):** the research pass found **no surviving primary claim** that IV/volume *magnitude* features are production clustering inputs; only return-correlation and one co-trading network are verified. Treat as exploratory.
- **Scope:** Extend S11-3 with an optional **second, separate** clustering on a z-scored characteristic vector `[IV-level, IV-skew, term-slope, log-dollar-volume, turnover, realized-vol]` (research recommends *not* bolting these onto the correlation distance — keep it a distinct clustering for comparison). Emit as `IndClass.cluster_char`; bench against `IndClass.cluster` in S11-6.
- **Acceptance:** `ClusterChar` suite ≥ 5 tests: feature z-scoring cross-sectional + PIT; planted characteristic-separated groups recovered; NaN-feature symbol guarded; two-runs-equal; clearly documented as exploratory. `ClusterChar total/0/0`.
- **Commit:** `feat(s11-7): exploratory IV/volume characteristic clustering (separate from corr clusters)`.

---

## Sprint commits (template — fill SHAs during execution)

| Commit | Unit | Test counts |
|--------|------|-------------|
| `<sha>` | S11-0 | ClusterScaffold 1/1/0/0 |
| `<sha>` | S11-1 | RmtClean … |
| `<sha>` | S11-2 | Hierarchical … / Sponge … |
| `<sha>` | S11-3 | ClusterPanel … |
| `<sha>` | S11-4 | ClusterPanelStore … |
| `<sha>` | S11-5 | ClusterWiring … |
| `<sha>` | S11-6 | ClusterEval … |
| `<sha>` | S11-7 | ClusterChar … *(stretch)* |
| `<sha>` | close | docs(s11-close): close sprint-11 — N units, M tests |

**S11 adds N new tests on top of the prior total (full engine suite green under `/W4 /permissive- /WX`; atx-core net diff = the approved Pattern B edges only).** ← load-bearing close metric.

---

## Dependencies & dispatch order

```
S11-1 (RMT) ─► S11-2 (cluster) ─► S11-3 (PIT builder) ─┬─► S11-5 (DSL wiring) ─► S11-6 (validation/bench)
                                                       │
S11-4 (store)  ── parallel-eligible w/ S11-1/2 ────────┘   (S11-3 feeds binary; S11-4 schema independent)
S11-7 (IV/vol) ── stretch, depends on S11-3; parallel w/ S11-6
```

- **Sequential:** S11-1 → S11-2 → S11-3 → S11-5 → S11-6.
- **Parallel-eligible (disjoint files):** S11-4 (store schema + module) can be built alongside the S11-1→2→3 chain; it only *joins* at S11-3's binary output. S11-7 runs alongside S11-6.
- Each sub-agent brief carries: worktree+branch, verbatim unit scope (from this plan), acceptance criteria, the mandatory marker-commit line, expected commit message, predecessor SHAs, the ledger row to write, the `implementation-quality.md` standard, and — for S11-1/S11-2 — the **Pattern B atx-core edge request** note.

---

## Point-in-time correctness (the #1 risk)

Clusters re-estimate on a rolling window; the cluster id effective at date `t` **must be computable from data available at `t`** — a future-leaking cluster assignment is a silent, catastrophic look-ahead bug that would inflate every cluster-grouped backtest.

- The S11-3 builder uses bars strictly in `[t−window+1, t]`, reuses the CPCV purge/embargo boundary, and writes a **step-function** panel (cluster held between rebalances).
- **Truncation-invariance is a mandatory acceptance test** (S11-3): the cluster cross-section at `t` must be byte-identical whether or not bars after `t` exist in the source panel. This is the same guard the engine already applies to alphas via `validation/bias_audit.hpp`.
- The persisted artifact (S11-4) is keyed by `asof_date` so a cache hit never serves a panel built with later data.

---

## Determinism contract (applies to every unit)

Mirrors the engine's standing contract; clustering adds three specific hazards, each pinned:
- **Eigendecomposition** (S11-1, S11-2): ascending-eigenvalue order; fixed eigenvector sign convention (first nonzero component positive); Neumaier summation.
- **Linkage ties** (S11-2a): equal merge distances broken by ascending instrument index.
- **k-means++ init** (S11-2b): **no RNG on the result path** — deterministic D²-furthest-first seeding, or a PRNG seeded only from the eigen-embedding content hash and isolated per the contract. Two-runs-equal is mandatory.
- **Label canonicalization** (S11-2): cluster ids relabeled by ascending smallest-member index so the panel digest is a function of the *partition*, not of internal allocation order.
- **Params hash** (S11-4): FNV-1a-64, length-prefixed, no wall-clock/RNG — replay-stable across machines.
- Every unit: two-runs-equal; FNV-1a-64 digest stability on produced artifacts; **byte-identical guard** when a config disables new behavior (`k=1`, `residualize=NONE`, `mode=Clip` fallback, single-window).

---

## What S11 will prove (baton target)

S11 turns "the universe is grouped by a static, infrequently-revised GICS label" into "**the universe is grouped by a deterministic, point-in-time, RMT-denoised, rolling cluster of actual return co-movement** — persisted, cache-keyed, promotable, and consumed by the *existing* group operators with zero new DSL surface." It hands p3-S2 (and its iteration loop) a **better group-id to neutralize and rank against**, plus an honest head-to-head (S11-6) that answers — on real data, with the research's refutations carried — whether data-driven clusters actually beat GICS-grouping on the scorecard. It does **not** assume they do.

---

## Open questions to resolve at kickoff

1. Confirm sprint number / module home vs [ROADMAP](../plans/ROADMAP.md) (S11 proposed; cross-module — does it live under p2 deepening, p3, or a new clustering track?).
2. Confirm sequencing vs CIO guardrail: land **after** `feat/megaalpha-enrich-validate` merges (ROADMAP #0) so S11-6's benchmark runs on the consolidated real panel.
3. `recluster_every` default and `window` default — research cites 60d (stat-arb) and ~250d (sectoring); pick the default cadence (proposal: 250d window, re-cluster every 21 bars) and whether both are config-exposed.
4. `k` default — research: Winton used 12 for S&P 500, and RMT does **not** determine `k` (refuted §9). Proposal: `k=12` default + silhouette/eigengap diagnostic in S11-6; confirm.
5. Default algorithm for the *shipped* `IndClass.cluster` — hierarchical (sectoring) vs SPONGEsym (stat-arb). Proposal: hierarchical as the default cluster field; SPONGEsym available by config and benched in S11-6.
6. Whether S11-7 (IV/volume) is in-scope now or deferred (research flags it under-evidenced).
