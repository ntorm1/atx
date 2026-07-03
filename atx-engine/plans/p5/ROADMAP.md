# p5 — Throughput & Real-Alpha Production

**Created:** 2026-06-24. Scoped against the north star ([../ROADMAP.md](../ROADMAP.md)): a robust,
profitable **mega-alpha** — low turnover, high capacity, deflation-surviving (DSR/PBO/CPCV),
honest cost.

p0→p4 built and merged the full discover→gate→combine→optimize→cost→report machine. Two facts
now define the work:

1. **The machine produces degenerate output on real data.** The canonical 10-year ORATS
   acceptance run (`work/accept/`) loaded 25.2M rows / 23,348 securities / 2,627 dates and, after
   790 evaluations, admitted exactly **one** alpha:
   `((ts_min(earnFlag, 45) ^ atmCenI_21d) / close)` — a near-constant, price-scale-tilted,
   ultra-sparse signal whose Sharpe is an artifact of sparsity, not edge. The robustness defenses
   that would have rejected it (`--typed-fields`, `--reject-price-scale`, non-vacuous first-alpha
   gate) exist but are **opt-in and were OFF**. → **Sprint 2.**

2. **The machine is too slow to compete on breadth.** WorldQuant mines on the order of millions of
   alphas; `IR ≈ IC·√BR` says breadth is the product. Our search evaluates on the order of
   hundreds per run. The eval kernels recompute O(window) per cell and the search ranks on an
   O(pool·T) pool-correlation that does not scale. → **Sprint 1.**

## The two sprints

| Sprint | Theme | Goal metric | Doc |
|---|---|---|---|
| **Sprint 1** | **Performance** — push alphas-evaluated-per-second by 10–100× | eval-ms/genome ↓, alphas/sec ↑, max pool size before O(pool·T) stall ↑ | [sprint-1-performance.md](sprint-1-performance.md) |
| **Sprint 2** | **Real-alpha production** — turn the defenses on, kill the degenerate class, scale breadth into a mega-alpha | OOS-DSR ↑, turnover ↓, %ADV capacity ↑, PBO ↓, # robust admits ↑ | [sprint-2-real-alpha-production.md](sprint-2-real-alpha-production.md) |

## Sequencing

They are **independent and parallelizable** (different files, different owners), but Sprint 2 is
the one that produces the deliverable and should not wait. Recommended:

1. **Sprint 2 Step 0** first (hours): re-run canonical with defenses ON and prove the degenerate
   alpha is rejected. This is the single highest-information action in the whole plan — it either
   works or exposes the next real bug.
2. Run **Sprint 1** and the rest of **Sprint 2** concurrently. Sprint 1 makes the breadth in
   Sprint 2 (Step 2–4) affordable; Sprint 2 defines the correctness contract Sprint 1 must not
   violate (the final admitted-alpha scorecard stays trustworthy regardless of how fast the search
   ran to get there).

## Shared invariant change

Sprint 1 deliberately **loosens byte-determinism** in the search hot path (the user-authorized
structural change). To keep the deliverable trustworthy, both sprints share one contract:

> **Two-tier evaluation.** `ResearchFast` mode (SIMD horizontal reductions, online variance,
> parallel reduction, fast-math, work-stealing order) is used for *search/ranking*. `AuditExact`
> mode (today's bit-identical, chronological-order, single-thread-equivalent path) is used to
> **re-evaluate every admitted alpha once before it is written to the library**, and for all
> published scorecards (DSR/PBO/CPCV/capacity). Fast and exact are kept in agreement by a
> tolerance-band differential test (relative ULP), not bit-exact equality.

This means "loosen determinism for speed" never contaminates the number a human or a deflation
test reads. See Sprint 1 §Determinism contract.
