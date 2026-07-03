# Sprint Plan — Phase C2/C3 (throughput + crash-safety) → STOP → Phase D (conviction + regime depth)

**Date:** 2026-06-21
**Status:** PLAN (not started). Authored after Phase A landed on `main` (mega-alpha OOS Sharpe deliverable
complete, 02e21e7..d070f1f). Supersedes the C2/C3/D sequencing in
`2026-06-21-mega-alpha-production-roadmap.md` §3 with a concrete, dependency-ordered task breakdown.

**Shape (as requested):** TWO sprints with a hard STOP between them.
- **Sprint 1 = Phase C2 + C3** — infrastructure: make large multi-seed sweeps fast (parallel substrate +
  redundant-eval reuse) and crash-safe (resumable discover). All off-path byte-identical; determinism-preserving
  plumbing only. No research-behavior change.
- **── STOP / human gate ──** — validate the infra (parallel seq==parallel digest, resume byte-identical
  admitted set, measured speedup, zero default-path drift) before building research features on top of it.
- **Sprint 2 = Phase D** — research depth: conviction→Kelly sizing (D1), the regime-label combiner (D2, with its
  panel-calendar prerequisite D0), breadth instrumentation (D3). Opt-in, higher-risk (changes sizing / admission
  / blending), and each needs the Sprint-1 throughput to run its heavier experiments.

**Why this split:** C de-risks and accelerates the search loop that D's experiments consume; it is mechanical and
determinism-safe, so it lands fast with low review surface. D changes the numbers the strategy trades on, so it
deserves a clean gate after the plumbing is proven, plus a real prerequisite (D2's panel-axis persistence) that
should not be rushed under a feature task.

---

## Global Constraints (binding — every task inherits these)

- **Determinism is sacred.** GA search F1 digest byte-identical across substrates/worker-counts and twice-run
  identical. `oracle.hpp` MUST NOT be touched. The ONLY sanctioned digest re-baseline is the library manifest
  `version_id`. No pinned golden digest literal edited to pass a test.
- **Off-path / default byte-identical.** Every new capability is opt-in behind a flag whose absence reproduces
  today's bytes exactly: no `--run-db` → no store I/O; no `--executor`/parallel flag → sequential as today; no
  `--conviction`/`--combine-method regime` → today's combine output. Each task ships a test pinning its off-path.
- **Engine purity.** No wall-clock / RNG / filesystem-order in the engine determinism path. Time and DB I/O live
  impl-side only (the resumable-discover spec already enforces this).
- Backwards-compat breaks allowed where worth it, but NEVER to the search digest / `oracle.hpp` / `version_id`.
- Build: clang-cl + Ninja, warm `build-rel` (Release `/W4 /WX`). Targets per task below. Pre-existing failures
  NOT in scope: `AtxImplPanel.BuildsPanelFromSegments`, `Alpha101Orats.*` (data-dependent), any `*DeathTest*`
  in NDEBUG.

---

# SPRINT 1 — Phase C2 + C3 (throughput + crash-safety)

## C2 — Redundant-eval reuse + parallel OOS in the sweep

**Verified current state (investigation 2026-06-21):**
- Within-run eval dedup EXISTS and is correct: `CanonSet` (search_driver.cpp:53) + `fitness_cache`
  (search_driver.cpp:58, keyed by `canon_hash`, per-run local), cross-gen skip at search_driver.cpp:481/492,
  cache hit at search_driver.cpp:674-676.
- Persistent `DedupIndex` (library/dedup_index.hpp) skips re-ADMISSION across runs — but NOT re-EVALUATION.
- The parallel OOS substrate `mine_into_oos_parallel` (factory.cpp:1151) EXISTS and is proven bit-identical to
  serial (`FactoryOos.WalkForwardSeqParallel`), reached only when an `IExecutor` of `Substrate::MultiProcess` is
  passed to `Factory::mine_into` (dispatch at factory.cpp:538-557).
- The sweep (stage_sweep.cpp → ResearchDriver::run, research_driver.cpp:66) constructs NO `IExecutor` → runs the
  sequential InProcess path. `sc.n_workers` (stage_sweep.cpp:101) controls only the in-process `DetPool` search
  threads, NOT the process substrate.

### C2.1 — Wire the parallel OOS substrate into the sweep (+ confirm seq==parallel)
- **Goal:** let a K-run sweep run each mine on the proven-deterministic `ProcessExecutor` substrate, selectable
  by a flag; default stays sequential (byte-identical).
- **Files:** `atx-engine/include/atx/engine/factory/research_driver.hpp/.cpp` (thread an optional
  `IExecutor*`/substrate selector through `ResearchConfig` → each `factory.mine_into(..., exec)` call);
  `atx-impl/src/stage_sweep.cpp` (construct a `ProcessExecutor` when a new `--executor process`/`--workers>1`
  intent is set; pass it); `config.{hpp,cpp}` (flag).
- **Determinism guard:** the sweep's research_digest with `--executor process` MUST equal the sequential
  research_digest (the engine already guarantees `mine_into_oos_parallel` == serial; this task only proves the
  sweep call-path preserves it). Default (no flag) byte-identical to today.
- **Tests:** extend `sweep_test.cpp` — `SweepParallelEqualsSequentialDigest` (same seed/panel, process vs
  in-process → identical `research_digest` + `library_size`); off-path unchanged.

### C2.2 — Cross-run redundant-eval cache (skip re-evaluating already-scored structures)
- **Goal:** in a multi-seed sweep, a canonical structure scored in run i and re-generated in run j is currently
  fully re-evaluated (VM cost) before the `DedupIndex` rejects re-admission. Add a persistent scored-cache so
  re-evaluation is skipped (the expensive part), not just re-admission.
- **DECISION (surface at the stop if non-trivial):** two designs —
  (a) **Library-adjacent scored sidecar:** persist `{canon_hash → CachedScore}` next to the library; `SearchDriver`
      consults it before evaluating (a cross-run analog of `fitness_cache`). Must be threaded into the per-run
      `SearchDriver` without breaking the "per-run clean slate (F1)" invariant — i.e. the cache may only SHORT-
      CIRCUIT a deterministic re-computation, never change the result, and must be proven to produce a
      byte-identical `research_digest` with vs without the cache populated.
  (b) **Scope C2.2 to measurement only** this sprint: instrument the redundant-eval rate (how many canon_hashes
      re-evaluated across runs) and DEFER the cache build if the rate is low. Recommended if C2.1 + C3 already
      fill the sprint — a wrong cross-run cache is a determinism risk for marginal gain.
- **Files (if building (a)):** new `atx-engine/include/atx/engine/factory/scored_cache.hpp` (header-only) +
  optional pointer param on `SearchDriver::run` (defaulted nullptr, like the resumable sink); impl glue to
  open/persist it alongside `--library-dir`.
- **Determinism guard (headline):** sweep `research_digest` with the cache warm == cold. This is the make-or-break
  test; if it cannot be guaranteed cleanly, fall back to design (b).
- **Tests:** `ScoredCacheShortCircuitsButByteIdentical` (warm-cache run digest == cold), cache hit-rate counter.

**Recommendation:** do C2.1 (cheap, high value, low risk) firmly; treat C2.2 as design-(a)-if-clean / design-(b)
otherwise, decided during implementation and reported at the stop.

## C3 — Resumable discover (crash-safe genetic search)

Fully specified in `docs/superpowers/specs/2026-06-19-resumable-discover-design.md` (approved design) with task
breakdown `docs/superpowers/plans/2026-06-19-resumable-discover.md` (T1–T7). This sprint EXECUTES that spec
verbatim — it is the largest, best-specified piece. Summary of the three components (all off-path byte-identical
via defaulted `nullptr`):

### C3.1 — Engine progress-sink interface (factory)
- New header `atx-engine/include/atx/engine/factory/search_progress.hpp`: `GenerationSnapshot`,
  `SearchProgressSink` (abstract), `SearchResumeState`. `SearchDriver::run` gains two defaulted trailing params
  (`SearchProgressSink* = nullptr`, `const SearchResumeState* = nullptr`) + private
  `serialize_population`/`deserialize_population` helpers (faithful genome round-trip via `unparse`/`parse_expr`).
- **Off-path:** `run(cfg, pool, nullptr, nullptr)` digest == legacy `run(cfg, pool)` digest (pinned).
- **Tests (factory):** `PopulationRoundTrip`, `SinkCalledPerGeneration`, `OffPathByteIdentical`,
  **`ResumeProducesIdenticalSearch`** (the discriminating correctness proof — resumed admitted-set canon_hashes ==
  full-run; do NOT assert `SearchResult.digest` across a resume boundary, per the spec's invariant note).

### C3.2 — Store recorder + schema v2 (atx::engine::store)
- Bump `kSchemaVersion` 1→2; add `pipeline_run` / `pipeline_checkpoint` / `pipeline_iteration` / `pipeline_event`
  / `pipeline_log` tables (idempotent `CREATE TABLE IF NOT EXISTS`; v1→v2 upgrade adds them + bumps the stamp).
  New header-only `atx-engine/include/atx/engine/store/pipeline_progress.hpp` (`PipelineRecorder`: begin /
  find_resumable / resume / save_checkpoint [ONE BEGIN IMMEDIATE txn] / latest_population_blob / heartbeat / log /
  event / complete / mark_failed) + blob helpers.
- **Tests (store):** schema-v2 golden-guard update, `PipelineRecorderLifecycle`,
  `FindResumableReturnsLatestCheckpoint`, `SaveCheckpointAtomic`.

### C3.3 — Factory + impl wiring
- Factory: defaulted sink/resume params on `mine_into` / `mine_into_oos`, forwarded to `SearchDriver::run`;
  `MultiProcess` + sink ⇒ `Err(InvalidArgument, "checkpointing requires InProcess workers")`.
- impl config: `--run-db <path>` (string, "" = off) + `--resume` (bool, requires `--run-db`; reject otherwise).
- impl glue: new `atx-impl/src/store_progress_sink.{hpp,cpp}` (`StoreProgressSink` + `compute_discover_fingerprint`);
  gated discover (stage_discover.cpp) opens the DB / begins-or-resumes / passes `&sink`+resume / completes or
  marks-failed. `--run-db` empty ⇒ existing call verbatim (no store code runs).
- **Tests (impl):** `ConfigParsesRunDbResume` + round-trip, `ResumeWithoutRunDbRejected`,
  `DiscoverOffPathByteIdentical`, `DiscoverWithRunDbWritesProgress`, `DiscoverResumeEndToEnd`.

**Note — C2.1/C3 substrate interaction:** C3 v1 is InProcess-only (the OOS path runs sequentially; checkpointing
on `MultiProcess` errors out). C2.1 adds the parallel substrate to the SWEEP. These do not conflict — resumable
discover targets the single gated `discover` 30-min crash risk; the parallel sweep is a separate throughput path.
But the sprint should land C2.1 and C3 such that a sweep on the process substrate does not silently expect
checkpointing — document the matrix (parallel sweep = fast but not yet resumable; gated discover = resumable but
InProcess). Resumable parallel sweep is explicitly out of scope (a future wire-format task).

---

## ── STOP / HUMAN GATE (between Sprint 1 and Sprint 2) ──

**Do not start Phase D until these are green and reviewed:**
1. **Determinism preserved.** Full factory + parallel + impl suites pass. Parallel-sweep `research_digest` ==
   sequential (C2.1). Off-path discover digest + `_manifest.txt` byte-identical with no `--run-db` (C3). If C2.2
   built the cross-run cache: warm-cache `research_digest` == cold (else C2.2 shipped as measurement-only).
2. **Resume correctness.** `ResumeProducesIdenticalSearch` green: an interrupted-then-resumed gated discover
   yields a byte-identical admitted alpha set. DB lifecycle/atomicity tests green.
3. **Measured benefit.** Report (a) the parallel-sweep wall-clock speedup vs sequential on a representative
   multi-run sweep, and (b) the cross-run redundant-eval rate (C2.2 measurement), so the human can decide whether
   the deferred cross-run cache is worth a follow-up.
4. **Whole-branch review of Sprint 1** (opus) — READY-TO-MERGE, zero Critical/Important, determinism section
   explicitly verified. Land Sprint 1 on `main`.
5. **Decision inputs for Sprint 2** surfaced: D0 panel-axis design choice (format bump vs sidecar), D1
   conviction-input carrier choice (sidecar vs persisted record_conviction vs library-field), and confirmation
   the human wants the regime/HMM path at all (research guardrail: HMM is one tool, opt-in, not mandatory).

The stop exists because everything after it changes the strategy's traded numbers and depends on a real
serialization prerequisite (D0). Resolve the three decision forks at the gate, not mid-flight.

---

# SPRINT 2 — Phase D (conviction + regime depth)

## D0 — Prerequisite: persist the panel date axis (gates D2)

**Why first:** the regime combiner needs per-period `regime_labels` of length `pool.n_periods()` joined to the
combine panel by CALENDAR. Today the serialized panel (`atx-impl/src/serialize_panel.cpp`) stores only a date
COUNT `D` — no timestamps; `alpha::Panel` has no calendar accessor. The regime macro segment carries a real
unix-nanos FRED-business-day axis. With no shared join key, ANY positional map is PIT-incorrect. (Full analysis:
`docs/superpowers/plans/2026-06-20-combine-regime-pipeline.md`.)

- **DECISION (resolve at the stop):**
  1. **Persist the calendar in the `.bin`** (recommended): bump `serialize_panel kVersion`; thread the `D`
     unix-nanos dates `build_history_panel → stage_panel → .bin`; add a calendar accessor to the reconstructed
     `alpha::Panel`. Update EVERY panel reader for the new version (format-compat is the review surface). A
     cross-cutting serialization change — its OWN task + review.
  2. **Date sidecar** (`panel.bin.dates`): written by `stage_panel`, read by `stage_combine`. Smaller blast
     radius, no format bump, but a parallel artifact to keep in sync.
- **Determinism guard:** the panel `.bin` (or sidecar) is deterministic; existing panel digests either re-baseline
  via `version_id`-style bump (option 1, documented) or are untouched (option 2). DEFAULT combine output unchanged.
- **Tests:** round-trip the calendar; assert a panel reader recovers the exact `D` unix-nanos; off-path combine
  byte-identical.

## D1 — Conviction score → fractional-Kelly position sizing

**Verified current state:** `conviction()` (combine/conviction.hpp:121, weights dsr 0.40 / pbo 0.35 / stability
0.25, tested) and `kelly_size()` (risk/kelly_sizing.hpp:73, quarter-Kelly default) are BUILT but DORMANT (no
caller in `atx-impl/src`). The schema-v2 `conviction` table exists but `record_conviction` is unimplemented.
**Blocker:** per-alpha conviction inputs are TRANSIENT — `FitnessReport::dsr` (fitness.hpp:200) and
`sharpe_h1/h2/split_stable` (fitness.hpp:213-215) are per-candidate but NOT persisted in the library
`AlphaDirEntry` (record.hpp:107; `AlphaMetrics` has only sharpe/turnover/returns/drawdown/margin/fitness/
holding_days). And `oos_pbo` (factory.hpp:263) is a RUN-level set statistic, NOT per-alpha.

### D1.1 — Carry per-alpha conviction inputs from discover to combine
- **DECISION (resolve at the stop):** (a) write a discover→combine sidecar (per-alpha `canon_hash, dsr,
  split_stable, sharpe_h1, sharpe_h2`) alongside the library/manifest; (b) implement `RunRecorder::record_conviction`
  + read the conviction table at combine; (c) add the fields to `AlphaDirEntry` (library format bump). Recommend
  (a) for the smallest blast radius unless the persistence DB is already in the path (then (b)).
- **Determinism:** the carrier is deterministic; combine default (no `--conviction`) ignores it → byte-identical.

### D1.2 — Apply conviction to per-alpha combine weights (opt-in)
- **Insertion point (verified):** `stage_combine.cpp` between the `combiner.fit()` return (~:240) and the crowding
  step (~:252): multiply `combo.weights[a] *= conviction_score[a]`, then renormalize with the existing
  `renorm_abs_sum` (combiner.hpp:190). Behind a `--conviction` flag.
- **Run-level-PBO nuance (must resolve in the brief):** PBO is per-RUN, not per-alpha — it cannot size an
  individual alpha. Options: feed `conviction()` the per-alpha DSR + per-alpha split-stability with a SET-level
  PBO term applied uniformly (a single multiplier on the whole book), OR drop the PBO term from the per-alpha
  score and keep PBO as a book-level gate only. Pick one explicitly; do not silently pass a run-level PBO as a
  per-alpha input.
- **Optional D1.3 — Kelly book sizing:** feed per-alpha conviction as the `conviction` vector into `kelly_size()`
  / book `AllocationConfig.fractional_kelly` in optimize, so gross leverage scales with conviction. Opt-in;
  report conviction-weighted vs equal-weight OOS Sharpe.
- **Tests:** `ConvictionScalesWeightsMonotonic` (higher per-alpha DSR/stability → larger |weight| share),
  off-path (`--conviction` absent) byte-identical combo.bin, twice-run identical.

## D2 — Regime-label pipeline + per-date regime combiner (opt-in)

Depends on D0 (panel calendar). Interfaces verified in `2026-06-20-combine-regime-pipeline.md` (§9.3/9.4).

### D2.1 — Regime labels joined to the panel calendar
- Read the regime macro segment (`--regime-segment` / reuse `cfg.regime_out`); build `hmm_lin::MatX obs`
  (T × n_macro_dims) JOINED to the panel calendar (forward-fill, PIT-safe); **assert `obs.rows() ==
  pool.n_periods()`**, fail closed on misalignment. Fit `learn::baum_welch(obs, HmmCfg{n_states=--n-regimes
  (default 3), master_seed=cfg.seed})` (seeded, byte-identical); `learn::posterior_decode` → `vector<u32>` labels.

### D2.2 — Per-date regime blend
- Add a `regime` combine method; `fit_regime_combiner(pool, labels, n_regimes, fit_begin, fit_end, cfg)`. For the
  regime method ONLY, the static step-9 blend becomes PER-DATE: per date `t`,
  `w_t = rc.blend(regime_posterior_at(hmm, obs, t))` (PIT posterior), apply to `streams.positions(a, t)`; assert
  `w_t.size() == streams.n_alphas()`. All non-regime methods unchanged.
- **Determinism:** whole regime path twice-run byte-identical (HMM `master_seed = cfg.seed`); DEFAULT combine
  (non-regime method) byte-identical. Works over `.dsl` and `--library-dir` pools.
- **Tests:** mirror `combine_regime_combiner_test.cpp` / `combine_crowding_test.cpp`; off-path byte-identical;
  misalignment fail-closed; twice-run identical.

## D3 — Breadth instrumentation + walk-forward re-fit harness (recorded-only)

- IR = IC·√N_eff breadth metric + a walk-forward re-fit harness (S10c). Recorded-only / diagnostic; off-path
  byte-identical (pure additive telemetry, like W5). Lowest risk in Sprint 2 — can run in parallel with D1/D2 or
  serve as the Sprint-2 warm-up.
- Also fold in the carried combine-regime minor: `--capacity-floor` is a no-op placeholder under the constant-1.0
  capacity stub (combine-regime plan §"Task 9 minor") — either wire real capacity (overlaps Phase B1) or document
  it as a placeholder.

---

## Sequencing summary

```
SPRINT 1 (infra, low-risk, off-path byte-identical)
  C2.1 parallel sweep wiring  ──┐
  C2.2 cross-run cache (or measure-only)
  C3.1 engine sink ─► C3.2 store v2 ─► C3.3 impl wiring   (per the approved spec)
        │
        ▼
  ── STOP: determinism + resume + speedup proven; whole-branch review; resolve D forks ──
        │
        ▼
SPRINT 2 (research depth, opt-in, higher-risk)
  D0 persist panel calendar  ──►  D2 regime pipeline (needs D0)
  D1 conviction→sizing (needs D1.1 carrier)   [independent of D0]
  D3 breadth instrumentation  [independent; lowest risk]
```

## Acceptance

- **Sprint 1 exit:** large multi-seed sweeps run on the deterministic parallel substrate with a measured speedup
  AND a gated discover that resumes from the last generation after a crash with a byte-identical admitted set —
  all opt-in, default bytes unchanged, F1 digest sacred. Whole-branch review READY-TO-MERGE.
- **Sprint 2 exit:** combine can (opt-in) (1) size per-alpha weights by a continuous conviction score from
  per-alpha DSR + split-stability (+ book-level PBO/Kelly), and (2) blend per-date by HMM regime posterior over a
  PIT-correct panel-calendar join — both twice-run byte-identical, default output unchanged. Report the
  conviction-weighted and regime-blended mega-alpha OOS Sharpe vs the Phase-A equal-weight baseline.
- **Global (unchanged):** every new path opt-in/flagged with a byte-identical default; determinism sacred
  (F1 byte-identical across substrates/worker-counts; twice-run identical); `oracle.hpp` untouched; only
  sanctioned re-baseline is the library manifest `version_id`.

**North-star (unchanged):** net-of-cost mega-alpha OOS Sharpe at target AUM — now (post-Sprint-2) reported for the
conviction-weighted and regime-aware book alongside the equal-weight Phase-A baseline, with the single reconciled
run-level PBO (A3) feeding conviction at the book level.
