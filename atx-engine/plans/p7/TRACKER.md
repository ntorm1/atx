# p7 — Live Sprint Tracker

**Last updated:** 2026-06-28 · **main:** `d95ce04` (+ this doc-close)

Status legend: ☐ not started · ◐ in progress · ☑ merged

| # | Sprint | Status | Branch | Merge commit | Notes |
|---|---|---|---|---|---|
| S1 | Deflation gates & honest selection | ☑ merged | `feat/p7-s1` | `914ae7f` | DSR/PBO/split gates in `AlphaGate::admit`; cumulative-N trial-count |
| S2 | Information breadth | ☑ merged | `feat/p7-s2` | `60547ff` | FINRA + IV + liquidity families + multi-family seeds (engine/core only) |
| S3 | Eval-VM hot path + bench | ☑ merged | `feat/p7-s3` | `4a2113b` | Welford online variance (ResearchFast) + column parallelism (AuditExact) |
| S4 | Turnover & capacity realism | ☑ merged | `feat/p7-s4` | `a149156` | EmaDecayPolicy + real capacity vector + CapacityScorecard + tradeable fitness helpers (engine layer; driver wiring → S7) |
| S5 | Conviction-scaled sizing | ☑ merged | `feat/p7-s5` | `d95ce04` | Kelly wiring + conviction KV + WF conviction-aware test (config fields only; `--kelly-fraction` arg-parse → S7) |
| S6 | Incremental panel + provenance | ☐ | — | — | Wave 3 |
| S7 | Wire + dev-panel validate | ☐ | — | — | Wave 3 (CLI hub; absorbs Wave-1 + Wave-2 carry-forwards below) |
| V1 | Operator prod validation | ☐ (unblocked) | — | — | S1–S5 all merged; the only full-panel run — operator-driven, after carry-forwards wired in S7 |

## Wave 1 — merged 2026-06-28 (`2eaf3da..4a2113b`)

Method: 3 disjoint worktrees, one background sprint-executor each (opus, TDD per
`scratchpad/p7-wave1-protocol.md` against `.agents/cpp/agent.md`), independent whole-branch review
per branch, fixes, then `--no-ff` merge to main. Controller ledger: `.superpowers/sdd/progress-p7-wave1.md`.

Integration-verified on merged main: **alpha 602/602**, **factory oracle/golden/digest 18/18**.
Determinism held end-to-end (inert defaults byte-identical; `oracle.hpp` untouched; no golden re-baseline).

Per-sprint records: `phase-{1,2,3}-progress.md` (ledgers) + `phase-{1,2,3}-report.md` (reports) in this dir.

### Reviews
- **S1** Spec ✅ / reconciled. 0 Crit · 1 Imp (S1-4 direction) · 2 Minor. `GateDeflation` carrier
  confirmed correct (AlphaMetrics is a frozen 56-byte serialized record).
- **S2** Spec ✅ / Approved. 0 Crit · 0 Imp · 4 Minor (all optional hardening, accepted). Derivations
  verified vs engine `cs_ops`/`ts_ops`; FINRA causal; byte-identity default path literally unchanged.
- **S3** Spec ✅ / Approved. 0 Crit · 1 Imp (I-1) · 4 Minor → all fixed (`9f9b549`). AuditExact
  byte-identity intact; Welford math correct; parallelism race-free by construction + {null,2,4} digest.

### Carry-forward → S7 (CLI hub)
1. Wire `GateDeflation` into `library::verdict_for` — else S1's DSR/PBO/split screens are dead code on
   every live caller (gate capability shipped + unit-tested, but no production path exercises it yet).
2. Thread S2's augment CLI: `--short-interest` / `--augment-out` / `--si-publication-lag` flags +
   `augment` subcommand + the `run_augment` stage (S2 deferred all CLI per decision D1).
3. Fix the stale `atx-impl/src/stage_discover.cpp` reject-histogram comment ("0..5" → "0..7").

### Open accepted Minors (non-blocking, optional future hardening)
- S2: `rolling_sample_std` per-bar in-universe check stricter than `ts_std` (inert under
  out-of-universe⇒NaN invariant); `iv_term` returns all-NaN (not Err) when `atmCenI_126d` absent
  (documented contract).
- S3: TSan run pending (no TSan toolchain in the Windows clang-cl build; no-race holds by construction
  + the seq==parallel digest gate is the empirical check).

### Key decisions
- **D1:** S2 lands FINRA engine + pure augment core only; all CLI deferred to S7.
- **D2:** Merge all 3 Wave-1 branches to main, then stop (no Wave 2 yet). Not pushed.
- **S1-4 direction:** ship safe/looser-with-N (only byte-identity-compatible option); cumulative-N
  deflation is enforced at the holdout DSR gate, not the cascade pre-gate.

## Wave 2 — merged 2026-06-28 (`e0659e4..d95ce04`)

Method: 2 disjoint worktrees, one background sprint-executor each (opus, TDD per
`scratchpad/p7-wave2-protocol.md` against `.agents/cpp/agent.md`), independent whole-branch review
per branch, S4 fix, then `--no-ff` merge to main. Controller ledger: `.superpowers/sdd/progress-p7-wave2.md`.
Merge commits: `a149156` (S4), `d95ce04` (S5). Branches disjoint (pairwise file overlap empty;
merge-tree both into main = no conflict). `C:\atx` untouched (feat/warehouse-parity); merge done in a
dedicated `C:\atx-wt\p7-merge` worktree. **Not pushed.**

Integration-verified on merged main (configure+build clean, 7 targets — S4 engine headers + S5
atx-impl coexist): **S4 core 10/10**, **S4 factory + byte-identity slice 116/116** (oracle/golden/digest
**18/18**), **S5 risk/eval 19/19** (KellySizing + ConvictionWF), **S5 atx-impl 42/42** (ConvictionSizing
15 + Combine off-path 27), **alpha coexistence 481/481**. Determinism held (inert defaults byte-identical;
`oracle.hpp` untouched; no golden re-baseline).

Per-sprint records: `phase-s4-progress.md`, `phase-s5-progress.md` (ledgers) in this dir; reports
`phase-s4-report.md` / `phase-s5-report.md` live in the worktrees (untracked).

### Reviews
- **S4** Spec ✅ / Approved. 0 Crit · 2 Imp · 2 Minor. I-1 (`compute_capacity_vector` non-positive
  `target_aum` → log NaN/-inf, not clean ≤0) **fixed** (`0336485`: `ATX_CHECK(target_aum>0.0)` + comment).
  I-2 (`combiner.hpp` `capacity_scale_weights`) is an undecomposed wiring-map artifact (no task/accept/test)
  → **carry-forward to S7**. `WeightPolicy` body unchanged; `kTruncateIters=8` intact; D3 honored.
- **S5** Spec ✅ / Approved. 0 Crit · 0 Imp · 3 Minor (all optional, accepted). Off-path byte-identity
  sound (Kelly block fully gated; `config.cpp` empty-diff per D4). Kelly-over-ALPHAS interpretation
  adjudicated **acceptable** (plan Accept criteria operate on per-alpha `combo.weights`). Kelly math
  non-vacuous (1e-12 vs hand-solved `apply_inverse`).

### Carry-forward → S7 (CLI hub)
4. Thread S5 Kelly CLI: `--kelly-fraction` / `--kelly-max-gross` arg-parse in `config.cpp` (D4 split —
   S5 landed the inert `config.hpp` fields + `stage_combine` wiring only).
5. Add `combiner.hpp` `capacity_scale_weights` helper when threading the S4 capacity vector into
   `stage_combine` (S4 review I-2; undecomposed in the S4 plan).

### Before V1 (main-level test debt, NOT a Wave-2 regression)
- 2 risk `RobustPipelineE2E` failures (`NoiseGrowsRobustLibraryByZero`,
  `SyntheticPanelAdmitsRobustSurvivors`) pre-exist on base `e0659e4` (reproduced on the S5 branch; S5
  changed no engine code). The Wave-1 integration build did not run the risk E2E suite. Investigate —
  likely predates p7; confirm not introduced by the Wave-1 merge before the V1 prod run.

### Key decisions (Wave 2)
- **D3:** S4-4 may edit `factory/fitness.hpp` (header-only helpers); the plan's Must-NOT-touch fence on
  it was a stale Wave-1 artifact ("S1 owns it"; S1 merged, not parallel). `src/factory/fitness.cpp` stays
  untouched.
- **D4:** S5 config = SPLIT. S5 lands the inert struct fields (`kelly_fraction=0.0`/`kelly_max_gross=1.0`)
  in `config.hpp` + `stage_combine` wiring; the `--kelly-fraction` arg-parse in `config.cpp` is deferred
  to S7 (respects the ROADMAP "CLI hub = S7" binding reconciliation + D1).
- **D5:** Merge both Wave-2 branches to main, then stop (no Wave 3 yet). Not pushed.
