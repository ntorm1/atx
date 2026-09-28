# T18 — fundamentals + industry (via CIK) for library v4: inventory and design (read-only)

Context: v3 (pure price/volume + SI/IV/earnings fields) failed validation (gross SR 2.74 TRAIN in-sample
-> 0.16 VAL). Root diagnosis so far: selection on 3 years of TRAIN had ~zero persistence (FIT-vs-HOLD
Sharpe Spearman .04 across 121 candidates). The declared next lever is alpha quality: fundamentals and
industry classification joined via SEC CIK, with signs fixed by published priors rather than by TRAIN
data. Another session ("tier1 v2", atx-db) has been building SEC CompanyFacts / fundamentals / identity
infrastructure; its work is merged in main (C:/atx-wt/pool-2 HEAD d63a7058 = main).

Read-only inventory and design. Work in C:/atx-wt/pool-2 READ-ONLY (code/docs). You may open data stores
ONLY for metadata (schema, table list, row counts, min/max dates, null rates, <= 20 sample rows) with
read-only connections; if a store is locked by another process, do not retry — report it. Never write
to any store or file other than your report. Do not modify the other session's files.

Answer, citing paths:
1. What PIT fundamentals exist today, where (paths/tables), with what availability stamp (filing
   acceptance datetime? period end?), restatement handling (first-reported vs latest), coverage for
   2019-06..2024-12 (the TRAIN role starts ~2018-06 for warmup): e.g. `atx-db/src/atx_db/` modules
   (asset_growth, cash_profitability, accruals-like, _derived_pit, companyfacts_*, estimates/), docs
   `atx-db/docs/research/FUNDAMENTALS_FAST_PATH.md`, handoffs
   `docs/superpowers/handoffs/2026-09-26-tier1-v2-alpha-priors.md` and the latest
   `2026-09-27-tier1-v2-session-7-handoff.md`, plan `docs/superpowers/plans/2026-09-27-tier1-v2-fundamentals-fast-path.md`.
   Which are READY (produced + qualified) vs PLANNED?
2. Industry classification: SIC (SEC submissions) or other; PIT history or latest-only?
3. Identity: how our role instrument IDs (u64 in `build-equity/recent-fast-*/` roles; see the role
   producer and `strategy_data`) map to CIK as of each date; existing identity-links tables; expected
   match rate for the top-3000-by-ADV universe.
4. How the current fields path works end-to-end: `atx-engine/tools/prepare_research_fields.py` ->
   fields manifest -> IC runner `--train-fields`/`--validation-fields` -> library DSL field names
   (what field names/types are allowed, per-field PIT semantics, memory model). What minimal changes
   add N fundamentals fields + an industry-code field (categorical) and what DSL/VM support exists for
   group operations (industry-demean, industry-rank, group mean) — list the VM operators available.
5. Recommend library v4 candidates (~20-40) with the prior sign and a citation for each, favouring
   anomalies with long published out-of-sample records and low turnover: value (B/M, E/P, CF/P, S/P),
   profitability (GP/A, operating profitability, ROE), investment (asset growth), accruals, net share
   issuance, earnings surprise / PEAD (SUE, if estimates or reported EPS history allow), F-score-style
   quality, industry momentum, industry-relative versions of existing price signals (industry-adjusted
   reversal, within-industry momentum). Mark which are computable from READY data.
6. A concrete implementation plan (tasks with files) for producing these fields for TRAIN role v2 and
   VALIDATION role v1 with the existing bounded producer pattern (<= 180 s, <= 1536 MiB per run,
   chunking if needed), and the size of each task.

Deliverable `task-T18-report.md` (same dir as this brief). Reply with: READY/PLANNED counts, match-rate
estimate, top blockers, and the proposed task list (one line each).
