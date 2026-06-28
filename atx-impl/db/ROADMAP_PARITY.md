# Warehouse Parity Roadmap — FactSet / S&P-Compustat rival on open data

**Branch:** `feat/warehouse-parity`
**Goal:** Bring `atx-impl/db` (DuckDB, PIT/bitemporal, custom `JobManager` DAG) to feature parity with
state-of-the-art fundamental + public alt-data providers for US equities, for a quant L/S shop.
**Blueprint:** `archive/research/` (89 CREATE TABLE across 10 datasets, 147 us-gaap concepts, cross-vendor field map).
**Gap source of truth:** [PARITY_GAP.md](PARITY_GAP.md).

## Constraints (global, bind every sprint)
- **No C++.** Work only in `atx-impl/db/` (Python) + `atx-impl/scripts/` (thin CLI wrappers). Never touch
  `atx-core/`, `atx-engine/`, `atx-impl/src/`, `atx-impl/tests/`, `atx-tsdb/`, `python/src/_bindings/`, any `CMakeLists.txt`.
- **Stack:** Python 3.12, DuckDB (single file `atx_impl.duckdb`), pandas, requests, pyarrow. SQL = DuckDB dialect.
- **PIT-correct:** every fact table carries `as_of_date` (DATE), `available_at` (TIMESTAMP), `source_loaded_at`, `run_id`, `source`.
- **Identifier spine:** FIGI/LEI/CIK/ticker only. **CUSIP is non-redistributable** — never store CUSIP as a public key; map to internal `security_id`.
- **House pattern:** logic = `XxxDataset(Dataset).run(store, XxxOptions(...))` in `db/xxx.py`; export via `db/__init__.py`;
  register in `db/jobs.py DATASET_REGISTRY` + DAG; thin `scripts/<verb>_xxx.py` argparse/JSON wrapper; add a `db/quality.py` check
  and a `db/lake.py` export object.
- **Every sprint delivers:** schema DDL + loader + connector(s) + DAG wiring + quality checks + lake export + pytest tests + a `parity.py` ledger row update. No sprint is "done" until pytest is green and a task review passes.

## Sprint sequence (ordered by parity-impact × effort, per PARITY_GAP.md)
| # | Domain | Headline deliverable | Status |
|---|--------|----------------------|--------|
| S0 | Foundation | pyproject/requirements, pytest harness, versioned migrations, this roadmap | in progress |
| S1 | Reference classifications | `taxonomy`/`taxonomy_node`/`entity_classification`/`taxonomy_mapping` + NAICS/SIC/Fama-French loaders | pending |
| S2 | Estimates | `est_broker`/`est_analyst`/`est_detail`/`est_consensus`/`est_actual`/`est_recommendation`/`est_guidance` | pending |
| S3 | Insider ownership | Form 3/4/5 (`insider_transaction`, 28-code), 13D/G beneficial ownership | pending |
| S4 | Fundamentals depth | statement_map 16→147 concepts, bank/insurance/REIT overlay, capture `rdq` | pending |
| S5 | Corp actions + pricing | CRSP-grade: DISTCD/CAEV dim, `delisting`/`dlret`, CFACPR/CFACSHR, shares-outstanding history | pending |
| S6 | ESG / sustainability | `esg_metric_dim`/`esg_metric`/`ghg_emission`/`esg_score`/`esg_controversy` (raw disclosed metrics) | pending |
| S7 | 13F + off-exchange | `filer_13f` entity-resolution alias, FINRA ATS/OTC transparency tables | pending |
| S8 | Supply-chain graph | `sc_node`/`sc_edge` from Exhibit-21 + 10-K Item 1 + CBP AMS + GLEIF | pending |
| S9 | QA hardening | cross-vendor reconciliation (`fact_disagreement`), DERA bulk backfill, final whole-branch review | pending |

## Execution model
Subagent-driven (per `superpowers:subagent-driven-development`): fresh implementer per sprint → task review (spec + quality)
→ fix loop → mark complete in `.superpowers/sdd/progress.md`. Controller (this session) curates briefs, never re-reads bulk artifacts.

## Progress ledger
Durable record: `.superpowers/sdd/progress.md`. Trust it + `git log` over conversation memory after any compaction.
