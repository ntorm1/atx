# Sprint S3 — Formulaic Alpha Factory (sprint spec)

**Status:** ✅ CLOSED 2026-06-08 (`feat/atx-core-stdlib @ 5f57a34`; ledger [`sprint-3-progress.md`](sprint-3-progress.md) · user ref [`sprint-3.md`](sprint-3.md)). Depended on **S1** (the scorer), **S2** (throughput), **P4** (pool + gates).
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md)
**Grounded in:** [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
§1 (formulaic DSL), §5 (correlation/overlap = marginal contribution), §9 (the alpha factory: 19→10⁷→10⁸
ambition; fractional window constants are search artifacts; DAG reuse is the throughput lever).

---

## Why this sprint

This is the **headline** — the first time the engine *discovers* alphas instead of merely *evaluating* the
ones a human wrote. WorldQuant industrialized this: 19 alphas in 2007 → 10 million by 2018, goal 100 million.
The tell that they search is everywhere in the 101-Alphas paper — fractional window constants like
`decay_linear(..., 8.22237)` and `delta(..., 2.25164)` are the fingerprints of grid/random/genetic
parameter search over the DSL.

We already own the perfect substrate: a **hash-consed DAG + CSE + columnar VM** (Phase 3) that makes
evaluating thousands of overlapping expressions cheap, **batch eval** (Phase 3c), **S2** parallelism, and
**S1** an honest deflated scorer. S3 is the search loop that sits on top: generate candidate ASTs, evaluate
them en masse, score them pool-aware, admit the survivors through the P4 gates.

The defining design choice — straight from the research — is the **fitness function**: a new alpha's worth
is **not** its standalone Sharpe, it is its **marginal contribution to a diversified pool** (WQ's 15.9%
mean-pairwise-correlation thesis). The factory optimizes diversification, not raw strength.

---

## Scope — units

### S3.0 — Marker + ledger
Open `sprint-3-progress.md`, freeze scope, base SHA.

### S3.1 — AST mutation operators
Type-safe mutations over the Phase-3 `Ast` that **always produce a valid, causal program** (the typecheck +
causality rail rejects the rest, but mutations should mostly stay in-grammar to avoid wasted generations):
**op-swap** (replace a node's operator with a shape/dtype-compatible one), **field-swap** (swap a leaf field
for another of the same kind), **window/const perturbation** (jitter a window or scale constant — the
fractional-constant search the research reveals). Each mutation reuses `analyze()` to validate before
admission.

### S3.2 — Subtree crossover + canonical-hash dedup
**Crossover:** swap a subtree between two parent ASTs at type-compatible cut points. **Canonical-hash
dedup:** normalize a candidate's DAG to a canonical form (commutative-operand ordering, constant-fold,
strength-reduction already in p0) and hash it, so a structurally-equivalent expression is **never
re-evaluated** — the dominant throughput lever at population scale (it is the same DAG interning Phase 3 uses
for CSE, lifted to dedup across *generations* and *the whole library*, prefiguring S4's index).

### S3.3 — Parameter optimizer
Given an AST *template* (fixed structure, free numeric constants), search the constant space:
**grid** (baseline), **random**, and **CMA-ES** (or a simpler evolutionary-strategy step) over the
fractional window/scale constants. Deterministic, **seeded** (the seed is part of the recorded artifact per
the determinism invariant). This is what turns `decay_linear(x, d)` into `decay_linear(x, 8.22237)`.

### S3.4 — Pool-aware fitness
The scoring function the search maximizes: **WQ fitness** (`sqrt(abs(returns)/max(turnover,0.125))·sharpe`,
reused from P4/S1) **×** a **marginal-correlation discount** (how much the candidate's PnL stream
*diversifies* the current pool — reuse the P4 correlation gate stats and S1 `PerfMetrics`). Optionally
fold in S6's cost-aware turnover penalty once S6 lands (hook left). Sub-universe robustness (re-score on a
weaker universe, WQ §4) as an anti-overfit multiplier. The result feeds S1's DSR/PBO so the **trial count**
is tracked and the reported fitness is **deflated**.

### S3.5 — Evolutionary search driver
The population loop: initialize (seed expressions or random valid ASTs) → evaluate (batch, across S2
workers) → select (tournament/truncation) → reproduce (S3.1 mutation + S3.2 crossover) → **elitism** (keep
the best) → **novelty/diversity pressure** (penalize candidates too close to the pool *and* to each other,
to avoid the population collapsing onto one motif). Deterministic and seeded; a run replays byte-identically.
Configurable budget (generations × population, or wall-clock via S2).

### S3.6 — Factory integration + bench + close
The end-to-end **mine → gate → admit** loop: the driver streams survivors through the **P4 gates**
(fitness/correlation/turnover floors) into the pool (S4 once it lands; the in-memory P4 `AlphaStore` until
then). Bench: **alphas evaluated/sec** and **admitted/hour** at a given population size, with the CSE/dedup
hit-rate (the throughput story). Report the trial count to S1 for DSR. Close ceremony.

---

## Exit criteria

- The factory mines a population over ≥ N generations and **admits** alphas that pass the P4 gates, fully
  deterministic (seeded run replays byte-identically — the digest invariant).
- Canonical-hash dedup demonstrably skips re-evaluating structurally-equivalent expressions (measured dedup
  rate on a real generation, reported in the ledger).
- Mined alphas' reported fitness is **deflated** by S1 (DSR/PBO consume the trial count) — a pure-noise
  population produces **no** admitted alphas after deflation (non-vacuous anti-snooping proof).
- Marginal-correlation fitness demonstrably prefers a diversifying weak alpha over a strong-but-redundant one
  (the WQ thesis, shown on a fixture).
- Throughput bench recorded (alphas/sec, admitted/hour, dedup %, CSE hit %); scales with S2 worker count.
- `/W4 /permissive- /WX`, clang-tidy + clang-format clean; test file per unit.

## Invariants this sprint must prove

Determinism (seeded search replays byte-identically; the seed is in the artifact), no look-ahead (every
candidate is scored through S1's fit/apply firewall — fitness is OOS, never in-sample), no snooping (DSR/PBO
deflation kills noise populations). Differential correctness for the mutation/crossover ops (a mutated AST
re-analyzes to a valid causal program or is rejected).

## Dependencies

- **Upstream:** S1 (`PerfMetrics`, DSR/PBO, CPCV), S2 (parallel batch-eval), P4 (`AlphaStore`, gates,
  correlation stats), p0 Phase-3 (`Ast`, `analyze`, `compile_batch`, DAG/CSE).
- **atx-core (Pattern B edge):** possibly a CMA-ES / evolution-strategy helper in L7 (S3.3) — or implement
  the ES step on existing linalg; decide at kickoff.

## Explicitly NOT in this sprint

No **learned** signals (S5 — S3 is purely formulaic search over the DSL). No persistent library store (S4 —
S3 admits into the in-memory pool; S4 gives it a home). No cost calibration (S6 — S3 prices turnover with the
existing model; a cost-aware hook is left for S6). No new operators (the DSL vocabulary is Phase-3b's; adding
ops is a one-row registry task, not a factory concern). No neural/DL search.

## Baton → next

S3 produces a **flood** of admitted alphas — which immediately motivates **S4** (a persistent, deduplicated,
lifecycle-managed home for 10⁵–10⁹ of them) and later **S5/S7** (combine and operate them).
