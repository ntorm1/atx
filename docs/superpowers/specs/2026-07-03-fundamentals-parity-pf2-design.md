# Design Spec — Fundamentals Depth + Production Warehouse (pf2)

**Date:** 2026-07-03
**Author:** warehouse-parity pf2 design session
**Predecessor:** pf1 (fundamentals spine) —
[docs/superpowers/specs/2026-07-01-fundamentals-parity-design.md](2026-07-01-fundamentals-parity-design.md)
**Operational plan:** [../../../atx-impl/plans/pf2/ROADMAP.md](../../../atx-impl/plans/pf2/ROADMAP.md)
(10 sprint docs: `atx-impl/plans/pf2/sprint-1..10-*.md`)
**Ledger:** [../../../atx-impl/WAREHOUSE_PARITY_TRANCHES.md](../../../atx-impl/WAREHOUSE_PARITY_TRANCHES.md)

## Problem

pf1 drove the US-equity fundamentals **spine** to Compustat/FactSet parity on six structural axes —
a governed item dictionary, an in-repo DAG orchestrator, full XBRL concept coverage, a declarative
formula registry, a FIGI/LEI identifier spine, a valuation-multiple scaffold, dimension-aware XBRL
validation, and restatement vintage lineage — but proved most of it as **schema + engine on a 5–40
security sample**. The parity matrix reads fundamentals as *"BUILT (polish remaining)."*

Two provider capabilities remain, and they are exactly what the northstar names but pf1 did not
deliver: **(A) content depth** — the analyst-grade normalization, point-in-time vintages, industry
templates, segment/footnote detail, calendarization, preliminary capture, populated multiples, and
cross-vendor reconciliation that separate a real fundamentals product from a raw-XBRL extraction; and
**(B) a production platform** — schema governance, migration/backup safety, quality gating, and
observability, which the 14.1 GB single-file DuckDB warehouse (two hand-recovered WAL crashes, ungated
quality checks) lacks.

## Decisions (this session)

1. **Two interleaved tracks, foundation-first.** pf2 runs a Content-depth track and a
   Production-platform track. The platform foundation (schema-as-contract + drift, migration/backup
   governance) lands *first* so content sprints churn the schema on governed, backed-up rails; a
   production capstone (quality-SLO gating + observability + storage) lands *last*, gating everything
   built. Depth sprints parallelize on disjoint modules (pf1's proven model).
2. **Standardization is the flagship.** A deterministic standardization engine — filer-specific tags +
   custom extensions → one fixed comparable template (~300 annual / ~100 quarterly items) with governed
   sign/scale/combination/missing-value rules as definition-as-data — is the single highest-value parity
   lever. Raw XBRL never achieves cross-company comparability without it.
3. **Point-in-time vintages, done properly.** Add Compustat-style monthly as-of-snapshot reconstruction
   and an `as_first_reported` (unrestated) vs `most_recently_restated` split, and extend restatement
   lineage from facts into **ratio-vintage** history. This is what defeats look-ahead/survivorship bias.
4. **Data posture: prove, don't backfill.** Content sprints ship injectable loaders + engines + offline
   fixtures, then an operator-run **~1-year recent proof slice** with live counts in the ledger. No
   large historical backfills in this module.
5. **Production hardening is co-equal, not a footnote.** Three dedicated platform sprints (schema
   contract; migration/backup/DR; quality-SLO/observability/storage) plus new contract clauses (E)
   schema-as-contract, (F) backup-before-migrate, (G) quality-gated. The ungated-quality-checks and
   no-backup facts are treated as production defects, not polish.

## Architecture

pf2 adds structural assets on top of pf1's governed spine without disturbing the bitemporal system of
record (`as_of_date` / `available_at` / `run_id` / `is_latest_revision`):

- **A standardization layer** (`standardization.py` + `fundamental_standardized` + rule seed). The
  normalized template every comparability query and downstream ratio reads. (PF2-S3.)
- **A point-in-time vintage layer** (`pit_snapshot.py` + as-of-month reader + ratio vintages). Monthly
  reconstruction + first-reported/restated split. (PF2-S4.)
- **Industry + calendar + segment/footnote depth** (`industry_templates.py`, `calendarization.py`,
  `segments.py`, `footnotes.py`). Full per-industry templates, FYE-normalized periods, and the
  dimensional facts pf1 discards. (PF2-S5/S6/S7.)
- **Preliminary + reconciliation + populated multiples** (`press_release.py`, `fact_disagreement.py`,
  populated `valuation_multiples`). Flash earnings, cross-vendor agreement, real multiples over the
  price×fundamental overlap. (PF2-S8/S9.)
- **A production platform** (`schema_contract.py`, `migration_admin.py`, `observability.py`,
  `storage_admin.py`, quality-gate wiring). Schema governance, backup/DR, gated quality, observability,
  managed storage. (PF2-S1/S2/S10.)

Each unit has one clear purpose, communicates through the existing dataset/registry/as-of interfaces,
and is unit-testable offline. Sprint boundaries follow module boundaries; the ROADMAP's ownership matrix
and reserved migration ranges (`0097–0131`) keep parallel-safe sprints disjoint.

## Approach chosen

**Platform-foundation-first, then depth, then production capstone** (over two fully-parallel tracks or
a value-first content-led order). Laying the schema contract + backup governance first means every later
content sprint reshapes the schema with drift-detection and real backups protecting the fragile 14 GB
file — the direct lesson of the S5g/S5k WAL crashes. Where module ownership is disjoint, depth sprints
still run concurrently in isolated worktrees (S5→S6 chain ‖ S7 ‖ S8).

## Non-goals (pf2)

ESG, supply-chain graph, licensed IBES/FactSet/CIQ estimate *feeds* (the injectable schema stays;
licensed data does not land), international (IFRS/ESEF/EDINET), large historical backfills, external
schedulers (cron/Airflow), and a REST/GraphQL API gateway. These stay documented in `PARITY_GAP.md`
for a later cycle.

## Success criteria

The pf2 north star (ROADMAP §North star): offline (+ operator ~1yr proof slice) rebuild on a governed,
backed-up, drift-checked platform yields standardized comparable financials, point-in-time as-of-month
vintages with first-reported/restated split, industry-templated + calendarized statements, segment +
footnote sub-ledgers, preliminary/basis-tagged earnings, populated valuation multiples with a
cross-vendor >99% agreement gate, and quality checks gated in the orchestrator with freshness-SLA +
anomaly observability and managed storage — such that the warehouse is reproducibly rebuildable and
operationally trustworthy. Verification stays offline: `python -m pytest atx-impl\db\tests -q` green
each tranche; live smoke recorded in the ledger with exact counts + run_id.
