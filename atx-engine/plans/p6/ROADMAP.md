# p6 — Tradeable-Alpha Uplift

**Created:** 2026-06-27. Scoped against the north star ([../ROADMAP.md](../ROADMAP.md)): a robust,
profitable **mega-alpha** — low turnover, high capacity, deflation-surviving (DSR/PBO/CPCV),
honest cost.

**Predecessor:** [p5](../p5/ROADMAP.md) (Throughput & Real-Alpha Production) framed the two
structural problems — the machine is slow and admits degenerate alphas. p6 is the
**post-"close-discovery-loop" successor**: it is grounded in a full code review of the
alpha-generation hot path + downstream pipeline (2026-06-27) and the close-discovery-loop session
that produced the first end-to-end admitted real alpha. Where p5 named the problems in the
abstract, p6 names the **exact defects with file:line** and ships the fixes. p6's perf sprints
(1–2) are the concrete, code-reviewed realization of p5 Sprint 1; they adopt p5's **two-tier
EvalMode determinism contract** (see below) rather than re-deriving it.

**Source:** `atx-impl/research/2026-06-27-close-discovery-loop-findings.md` + 5 subsystem code
reviews (hot path, downstream portfolio, gates/metrics/cost, data/panel/fields, search/diversity).

---

## The facts that define the work (truth, measured this session)

The discover loop now CLOSES on real data. Alpha101 #42 `rank(vwap-close)/rank(vwap+close)` admits
through the full strict gated discover: gross OOS Sharpe **1.93**, fitness **1.077**, IS Sharpe
**+0.89**, 12.3%/yr. So the earlier zero-admit was the gates working on a marginal seed, not a
pipeline failure. But four concrete walls now block a *tradeable* mega-alpha:

1. **Net-of-cost collapses the edge.** a42 nets only **~0.37** Sharpe at 10 bps (turnover
   0.39/day). The admission gate screens on FRICTIONLESS fitness — it never sees cost or turnover.
   Low-turnover alternatives (iv_hi, illiq) net better but are not in-sample robust. → **Sprints 3, 4.**
2. **The downstream pipeline deploys a wrong book.** combine→optimize→report inverts the alpha's
   sign (MVO `V⁻¹` tilt, report −1.86 vs +1.93), zeros the book under capacity, and reports
   8,464,812% participation. Never validated end-to-end with a real admit. → **Sprint 6.**
3. **The GA mines junk; only seeds produce tradeable structure.** The search signal is
   turnover-blind, the one structure-growing mutation (`wrap_in_op`) is OFF, novelty rewards
   failing genomes, and seeds lose survival pressure after gen 0. → **Sprint 3.**
4. **Most of the factor catalog is unreachable.** The CLI panel lacks `returns`/`cap`/`IndClass.*`/
   multi-adv, so ~60+ of the 101 alphas cannot even be evaluated. The augment exists but is
   test-only. → **Sprint 5.** And the 21-minute wall-clock caps breadth. → **Sprints 1, 2.**

---

## The seven sprints

| Sprint | Theme | Goal metric | Doc |
|---|---|---|---|
| **S1** | **Eval/VM performance** — drive eval-ms/genome down (compile cache, alloc kill, ts-kernel transpose, SIMD) | eval-ms/genome ↓, alphas/sec ↑ | [sprint-1-eval-vm-performance.md](sprint-1-eval-vm-performance.md) |
| **S2** | **Factory admission** — early-abort doomed holdouts, single-pass sub-windows, collapse the 4× admit ladder | holdout-evals/run ↓, admission digest stable | [sprint-2-factory-admission.md](sprint-2-factory-admission.md) |
| **S3** | **Search net-cost selection** — turnover penalty in `raw`, structure-growing mutation, seed-elitism, viable novelty | net-DSR ↑, GA admits with IS>0 ↑ | [sprint-3-search-netcost-selection.md](sprint-3-search-netcost-selection.md) |
| **S4** | **Cost-aware gates** — net-of-cost fitness floor, holding-period floor, net-of-cost DSR | admitted net-Sharpe ↑, turnover ↓ | [sprint-4-cost-aware-gates.md](sprint-4-cost-aware-gates.md) |
| **S5** | **Panel augmentation** — production returns/cap/IndClass/multi-adv; full catalog reachable | # evaluable factors ↑ (≈20 → 101) | [sprint-5-panel-augmentation.md](sprint-5-panel-augmentation.md) |
| **S6** | **Downstream portfolio** — fix sign-flip, capacity-zeroing, participation overflow | sign-correct non-empty book; sane %ADV | [sprint-6-downstream-portfolio.md](sprint-6-downstream-portfolio.md) |
| **S7** | **Wire + build tradeable alphas** — thread every flag, run the real-panel hunt | ≥1 alpha: WQ-fit + IS>0 + net-10bps OOS Sharpe > 0.8 + sign-correct book | [sprint-7-wire-and-build.md](sprint-7-wire-and-build.md) |

---

## Disjoint file-ownership (the parallelism contract)

S1–S6 own DISJOINT file sets and may run in PARALLEL. S7 owns the shared integration files and runs
LAST. The four hub files — `atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}` —
are **reserved for S7 only**; feature sprints expose new behavior via engine config-struct fields
(SearchConfig / FitnessCfg / GateConfig / FactoryConfig) with inert defaults, and S7 threads the CLI
flags. `atx-engine/tests/factory/oracle.hpp` is untouchable by every sprint.

| Sprint | Owns (exclusive) |
|---|---|
| S1 | `alpha/{vm,streams,bytecode,dag,cs_ops,ts_ops}.hpp`, `loop/weight_policy.hpp` + tests |
| S2 | `src/factory/factory.cpp`, `factory/factory.hpp`, `tests/factory/factory_oos_test.cpp` |
| S3 | `src/factory/{fitness,search_driver,mutation,genome}.cpp`, `factory/{fitness,search_driver,generate,genome,behavior,mutation,pool_view}.hpp` + tests |
| S4 | `combine/{gate,metrics}.hpp`, `library/library.hpp`, `cost/cost_aware.hpp`, `eval/deflated_sharpe.hpp` + tests |
| S5 | NEW `alpha/augment.hpp`, `alpha/datafields.hpp`, `atx-impl/src/stage_panel.cpp`, `atx-impl/tests/alpha101_support.hpp` + tests |
| S6 | `atx-impl/src/{stage_combine,stage_optimize,stage_report}.cpp`, `atx-impl/src/{book_shape,diag_risk}.hpp` + tests |
| S7 | `atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}`, NEW `scripts/build-tradeable-alphas.ps1`, NEW research doc |

---

## Shared determinism contract (every sprint)

p6 inherits two established contracts. Each sprint's tasks state which applies.

**(A) Opt-in / default-byte-identical** (the B3 / P-seed pattern — for every behavior-changing
sprint: S3, S4, S5, S6, S7). Any output-changing capability is gated behind a new engine-config
field defaulting to today's value. Pinned goldens UNCHANGED on the default path:
`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, MultiObjective/ScalarRaw digests,
`FactoryOos.MineIntoOffPathDigestUnchanged`, OOS goldens. `oracle.hpp` untouched. Each opt-in ships
three test classes — (a) off-path byte-identity, (b) on-path RED→GREEN, (c) twice-run — plus (d)
seq==parallel where an admission path is touched. S7's build profile turns the opt-ins on as an
explicit non-default profile, never a golden re-baseline.

**(B) Two-tier EvalMode** (inherited from [p5 Sprint 1](../p5/sprint-1-performance.md#determinism-contract-the-structural-change) —
for the perf sprints S1, S2). `AuditExact` = today's bit-identical, chronological-order,
single-thread-equivalent path; the system of record. `ResearchFast` = SIMD horizontal reductions,
online variance, parallel reduction, fast-math; search/ranking only. **Every admitted alpha is
re-evaluated once in AuditExact before it is written to the library**, and all published scorecards
(DSR/PBO/CPCV/capacity) are the AuditExact number. A tolerance-band differential test keeps the two
in agreement. Byte-identical wins (compile cache, alloc reuse, the 4× ladder refactor, monotonic
Min/Max) ship in AuditExact and need no mode flag; determinism-breaking wins go behind ResearchFast.

**Process (all sprints):** never `git add -A` (the tree has many unrelated dirty/untracked files —
stage explicit paths); never push; commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. New unit ⇒ new test file under the
auto-globbed test dir. Performance claims require a recorded before/after bench line (per
[implementation-quality.md](../docs/implementation-quality.md)).

---

## Sequencing

1. **Parallel wave:** S1, S2, S3, S4, S5, S6 — disjoint files, independent owners. Dispatch all in
   one message. S2's holdout-Engine reuse consumes S1's reusable-Engine API; if S1 has not landed
   it, S2 implements the reuse locally and notes the dependency.
2. **S7 last** — depends on all six; owns CLI wiring + the tradeable-alpha build.

**If you can only do a subset:** S6 (the book is wrong — nothing downstream is trustworthy without
it) → S4+S3 (net-of-cost is the binding constraint on tradeability) → S5 (unlocks the catalog) →
S1+S2 (breadth) → S7 (compose). S6 alone makes the existing a42 deployable; S6+S4+S3 is the minimum
that changes *which* alphas the machine prefers toward tradeable ones.

## North star (S7 acceptance)

On `work/accept/panel.bin`, end-to-end produce ≥1 admitted alpha that is simultaneously: WQ-fit
(fitness ≥ 1.0), in-sample robust (IS Sharpe > 0), NET-of-10bps OOS Sharpe > 0.8 (vs a42's 0.37),
deployed with a SIGN-CORRECT non-empty book and a sane participation footprint — or a documented
frontier naming the binding constraint.

Sprint discipline: same as p0 — see [../docs/sprint.md](../docs/sprint.md). Implementation quality:
[../docs/implementation-quality.md](../docs/implementation-quality.md) is mandatory for every coding
sprint.
