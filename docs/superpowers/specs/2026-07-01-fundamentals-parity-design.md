# Design Spec — Fundamentals Parity (FactSet / S&P GMI Compustat)

**Date:** 2026-07-01
**Author:** warehouse-parity re-alignment session
**Operational plan:** [../../../atx-impl/plans/pf1/ROADMAP.md](../../../atx-impl/plans/pf1/ROADMAP.md)
(8 sprint docs: `atx-impl/plans/pf1/sprint-1..8-*.md`)
**Ledger:** [../../../atx-impl/WAREHOUSE_PARITY_TRANCHES.md](../../../atx-impl/WAREHOUSE_PARITY_TRANCHES.md)

## Problem

The warehouse (`atx-impl/db`, 122 tables, migrations `0060`, ~465 offline tests) reached Compustat
*breadth* over S3–S45 — 1,395 securities, 933k ratio rows, ten domains — but sprawled across domains
without closing fundamentals to true provider *depth and trust*. The north star is a point-in-time
warehouse that rivals FactSet and S&P GMI Compustat for **US equity fundamentals**: every metric, every
ratio, every derived ratio, linked through a strong schema, driven by real background job management.

## Decisions (this session)

1. **Scope: fundamentals-first + spine.** Drive the fundamentals spine to parity, plus the linkage
   spine it needs — identifiers (FIGI/LEI), corp-action adjustment, and pricing overlap for valuation
   multiples. Park ESG, supply-chain, licensed estimates, and the non-fundamental derived domains
   (13F/insider/macro/short-interest) in maintenance.
2. **Coverage target: full canonical dictionary.** Materialize the ~480-row cross-vendor field map
   (`archive/research/schemas/cross_vendor_field_map.md`) as a normalized `fundamental_item` dimension
   mirroring Compustat (~300 annual / ~100 quarterly) + FactSet + IBES cross-walk. "All metrics" means
   the dictionary, not a curated subset.
3. **Valuation multiples: load modern pricing.** Ingest 2015+ daily bars to intersect the 2017–26
   fundamentals window and unlock P/E, P/B, P/S, EV/EBITDA, market cap — core derived ratios every
   provider ships, currently impossible on the 2012–14 price sample.
4. **Job management: in-repo orchestrator.** A pure-Python DAG (dependency inference, watermark-driven
   incremental, retry/resume, run manifests + audit) — no Airflow/Prefect dependency; fits the
   single-host DuckDB design and the light/offline test constraint.

## Architecture

The pf1 series adds four structural assets on top of the existing PIT warehouse, without disturbing the
bitemporal system of record (`as_of_date` / `available_at` / `run_id` / `is_latest_revision`):

- **A governed metric dictionary** (`fundamental_item` + alias + vendor map). The single source of truth
  every loader and the ratio engine resolve through — replaces hard-coded metric strings. Everything
  links to `item_id`. (PF-S1; fed by PF-S3 concept expansion.)
- **A declarative derived layer** (`formula_registry`). Ratios and composite scores become
  definition-as-data with citations, not lambdas; the engine reads the registry. Valuation multiples are
  new registry families joined through price×shares×fundamentals. (PF-S4, PF-S6.)
- **A sticky identifier spine** (FSYM-style entity/security IDs; FIGI/LEI/ISIN, CUSIP internal-only).
  Makes "everything linked together" real and enables the price×fundamental join. (PF-S5.)
- **An orchestrated, trustworthy rebuild** (DAG orchestrator + dimension-aware XBRL validation +
  restatement vintage lineage). Reproducible, resumable, auditable, and provably PIT-correct. (PF-S2,
  PF-S7, PF-S8.)

Each unit has one clear purpose, communicates through the existing dataset/registry/as-of interfaces,
and is unit-testable offline. Sprint boundaries follow module boundaries; the ROADMAP's ownership matrix
and reserved migration ranges keep parallel-safe sprints disjoint.

## Approach chosen

**Foundation-first, disjoint-ownership** (over value-first slices or a big two-track parallel). Laying
the item dictionary, orchestrator, and identifier spine first means every later sprint compounds on
governed infrastructure instead of reworking hard-coded strings. Where module ownership is disjoint,
sprints still run concurrently in isolated worktrees (S1‖S2, S3‖S5, S4‖S7).

## Non-goals (pf1)

ESG, supply-chain graph, licensed IBES/FactSet estimate feeds, international (IFRS/ESEF/EDINET),
external schedulers, and a REST/GraphQL API gateway. These remain documented in `PARITY_GAP.md` for a
later cycle.

## Success criteria

The pf1 north star (see ROADMAP §North star): offline rebuild yields the full canonical item set with
vendor cross-walk, every fact/ratio linked to `item_id` + `security_id` + accession vintage, the full
statement + valuation ratio set PIT-safely, dimension-aware validation with zero unexplained standing
failures, and resumable job runs with manifests + audit. Verification stays offline: `python -m pytest
atx-impl\db\tests -q` green each tranche; live smoke recorded in the ledger with exact counts + run_id.
