# atx-engine — LIVE Master Roadmap

**Maintained by:** CIO ([`../../.agents/cio/agent.md`](../../.agents/cio/agent.md)). Single source of cross-module truth. Per-module ROADMAPs ([p0](p0/ROADMAP.md) · [p1](p1/ROADMAP.md) · [p2](p2/ROADMAP.md) · [p3](p3-impl/ROADMAP.md)) are canonical for **per-unit SHAs + test counts**; this file is canonical for **what's done, what's merged, and what's next**. Last CIO review: **2026-06-19**.

**North star:** a robust, profitable **mega-alpha** — **low turnover, high capacity**, surviving deflation (DSR/PBO/CPCV) and honest cost. Defended by the 7 invariants ([p1 ROADMAP](p1/ROADMAP.md) §Carried-forward). Every move below is scored against: OOS-DSR ↑, turnover ↓, %ADV capacity ↑, PBO ↓.

---

## The headline (read this first)

**The machine is built. It has not yet been run on real data at scale.** p0→p2 + p3-S1/S3 are merged into `main` (`c317c33`): the full discover→gate→combine→optimize→cost→size→report pipeline, including S10's conviction/Kelly/regime/breadth/walk-forward layers that closed the verified RenTech gaps (G1–G6, [improvement plan](../research/rentech-improvement-sprint-plan.md)). The deep "why no profitable mega-alpha yet" analysis resolved to: **not a missing capability — a missing experiment.** Nothing has been pushed through the unchanged pipeline on the real ORATS liquid universe to produce a deflation-surviving, capacity-bounded scorecard. **That experiment (p3-S2) is the next decisive milestone.** Everything else is in service of running it on the best possible inputs.

---

## Current state (truth, not hopes)

| Module | Scope | Status |
|---|---|---|
| **p0** — trustworthy backtester | event spine · loop+portfolio+exec-sim · alpha DSL/VM · mega-alpha + Barra risk + optimizer | ✅ **merged** ([p0](p0/ROADMAP.md)) |
| **p1** — alpha factory | S1 eval/validation · S2 parallel · S3 factory · S4 library · S4b automated engine · S5 learned+ML combiner · S6 cost+capacity · S7 portfolio/lifecycle · S8 vendor-grade covariance | ✅ **merged** ([p1](p1/ROADMAP.md)) |
| **p2** — robust alpha engine (v3) | S1 multi-horizon · S2 meta-book · S3 deepened DSL · S4 genetic+robust pipeline · S5 DL sequence alphas · S6 BYO-data layer · S7 distributed scale-out · S8 production optimizer (Ruiz/ADMM/cone) · S10 conviction+Kelly+regime+breadth+walk-forward | ✅ **merged** ([p2](p2/ROADMAP.md)) |
| **p3** — real-data validation (v4) | S1 real-data hardening (corp-actions/total-return/universe) ✅ · **S2 real-data benchmark ⏳ PENDING** · S3 single-file ORATS history loader ✅ | 🟡 **S1+S3 merged; S2 open** ([p3](p3-impl/ROADMAP.md)) |
| **persistence-v2** — SQLite lifecycle DB | Tasks 1–9: schema/StoreDb · alpha catalog · universe registry · fingerprint/replay · run recorder · event log · segment index · env config · cross-env promotion (Dev/UAT/PROD) · E2E + schema-drift guard | 🟢 **complete on branch** `feat/megaalpha-enrich-validate` ([spec](../../docs/superpowers/specs/2026-06-19-atx-persistence-v2-design.md)) |
| **megaalpha enrich/validate** | ORATS options/earnings + GICS sector → DSL datafields/groups · real OOS-holdout in gated discover (IS/OOS manifest) · %ADV capacity footprint in report | 🟢 **in progress on branch** `feat/megaalpha-enrich-validate` (23 ahead of `main`) |

> **Consolidation note:** all the p2 "pending-merge" branches are in fact **already in `main`** (verified `git rev-list main..<branch> == 0`). The only un-merged work is the current feature branch (persistence-v2 + enrich/validate). The tree is consolidated, not fragmented.

---

## What's next — prioritized (CIO directive)

### 0. Land the current branch → `main`  ·  *gate, do first*
`feat/megaalpha-enrich-validate` (23 commits): persistence-v2 foundation is feature-complete (Tasks 1–9, golden schema guard `ebaf44e`) and the enrich/validate work is the input-quality lever for the benchmark. **Run the done-gate, merge, so p3-S2 runs on one consolidated tree.** Owner: PM. Blocks #1 (a benchmark on a side branch isn't reproducible-of-record).

### 1. p3-S2 — real-data benchmark  ·  **THE decisive experiment**
Run the **unchanged** p2 mine→validate→robust→combine→optimize→cost→size→report pipeline over the expanded ORATS liquid universe (p3-S1 real Panel, golden digest `0x2a22a873483d9157`; ~5321 symbols). Produce a **scorecard**: post-deflation survivors, realized OOS Sharpe/DSR, **turnover**, **%ADV capacity**, cost drag, with caveats. This is the literal answer to "can we build a robust profitable mega-alpha." Currently **outline-grade** ([p3 ROADMAP](p3-impl/ROADMAP.md) S2) — needs: (a) freeze the S2 implementation plan, (b) the **operator data-build** (full ORATS ~11 GB, deferred at S3 close — manual `OratsE2ESmoke.OperatorOratsZip`), (c) the benchmark harness + report. Owner: CIO freezes intent → PM dispatches.

### 2. Iterate on the benchmark's verdict  ·  *evidence-driven, do NOT pre-commit*
The scorecard diagnoses where the edge leaks, and that — not a guess — sets the next sprint:
- **Survivors weak / overfit** → signal-quality lever: more enrichment (the current branch's direction — options-implied, earnings, cross-sectional fundamentals), richer learned features.
- **Edge real but turnover too high** → combination/sizing lever: tighten S10 crowding/de-correlation + multi-horizon turnover penalty.
- **Edge real but capacity-capped** → cost/capacity lever: re-calibrate S6 impact, breadth (S10-5) to spread into more independent bets.
- **Edge real and clean** → operate it: wire persistence-v2 promotion (Dev→UAT→PROD) + S7 decay monitor into a standing book.

---

## Standing guardrails (CIO)

- **Deflate everything.** No survivor accepted on raw Sharpe — DSR/PBO/CPCV + capacity curve or it isn't an edge.
- **Turnover & capacity are first-class objectives**, not post-hoc filters. A high-Sharpe / high-turnover / low-capacity alpha is a non-goal.
- **Stop adding capability until the benchmark runs.** The machine is feature-complete for the north star; the binding constraint is real-data evidence, not more layers. New sprints justify themselves against the scorecard.
- **Anti-roadmap holds** ([p1 ROADMAP](p1/ROADMAP.md) §v2 anti-roadmap, [p2](p2/ROADMAP.md)): no live broker, no LOB matching engine, no alt-data beyond price/vol/classifications/options, no cross-machine distributed (single-box multicore is the ceiling), no engine-local general-purpose primitives (atx-core requests = Pattern B edges).

---

## Canonical references

- Per-module ROADMAPs: [p0](p0/ROADMAP.md) · [p1](p1/ROADMAP.md) · [p2](p2/ROADMAP.md) · [p3](p3-impl/ROADMAP.md)
- Latest strategic mapping: [rentech-improvement-sprint-plan.md](../research/rentech-improvement-sprint-plan.md) · [rentech-structure-signals-domain-mapping.md](../research/rentech-structure-signals-domain-mapping.md)
- Research north-stars: [renaissance-technologies-systems-deep-dive.md](../research/renaissance-technologies-systems-deep-dive.md) · [worldquant-systems-deep-dive.md](../research/worldquant-systems-deep-dive.md)
- Persistence-v2: [spec](../../docs/superpowers/specs/2026-06-19-atx-persistence-v2-design.md) · [plan](../../docs/superpowers/plans/2026-06-19-atx-persistence-v2.md)
- Discipline: [sprint.md](docs/sprint.md) · [implementation-quality.md](docs/implementation-quality.md) · code gate [cpp/agent.md](../../.agents/cpp/agent.md) · engine map [atx-engine/agent.md](../../.agents/atx-engine/agent.md) · dispatch [pm/agent.md](../../.agents/pm/agent.md)
