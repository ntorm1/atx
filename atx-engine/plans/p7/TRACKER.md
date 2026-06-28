# p7 — Live Sprint Tracker

**Last updated:** 2026-06-28 · **main:** `4a2113b`

Status legend: ☐ not started · ◐ in progress · ☑ merged

| # | Sprint | Status | Branch | Merge commit | Notes |
|---|---|---|---|---|---|
| S1 | Deflation gates & honest selection | ☑ merged | `feat/p7-s1` | `914ae7f` | DSR/PBO/split gates in `AlphaGate::admit`; cumulative-N trial-count |
| S2 | Information breadth | ☑ merged | `feat/p7-s2` | `60547ff` | FINRA + IV + liquidity families + multi-family seeds (engine/core only) |
| S3 | Eval-VM hot path + bench | ☑ merged | `feat/p7-s3` | `4a2113b` | Welford online variance (ResearchFast) + column parallelism (AuditExact) |
| S4 | Turnover & capacity realism | ☐ | — | — | Wave 2 |
| S5 | Conviction-scaled sizing | ☐ | — | — | Wave 2 (wires S10 infra) |
| S6 | Incremental panel + provenance | ☐ | — | — | Wave 3 |
| S7 | Wire + dev-panel validate | ☐ | — | — | Wave 3 (CLI hub; absorbs Wave-1 carry-forwards below) |
| V1 | Operator prod validation | ☐ | — | — | After S1–S5 merge; the only full-panel run |

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
