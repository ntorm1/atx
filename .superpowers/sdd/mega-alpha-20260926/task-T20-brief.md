# T20 — fundamental events producer (v4 data, L; critical path)
Spec: T18 report (C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T18-report.md) §2 (sources, clocks, restatements, coverage), §7 row T20 and "T20 details"; rulings in
C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/v4-prereg.md §R2 (accepted_utc clock, FC1 fallback labelled, latest-clock-wins, seal < 2025-01-01).
Deliver atx-engine/tools/build_fundamental_events.py + test_build_fundamental_events.py (synthetic pytest).
FIRST commit (before the implementation): the output schema as a markdown/docstring contract
(atx-engine/tools/fundamental_events_schema.md): event table columns (cik, accepted_utc, clock_basis, form,
period_end, fiscal fields, every item incl. lag-4 / lag-1q items, ttm items, sue, fscore, noa) and the SIC event
table — T21 codes against it concurrently, so keep it stable and tell the controller if you must change it.
Items: exactly the T21 field list in the T18 report §6 "Field names" (be, at, at_lag4, lt, che, debt, sale_ttm,
gp_ttm, oi_ttm, ni_ttm, ni_q, ni_q_lag4, be_lag1q, cfo_ttm, capx_ttm, xrd_ttm, dvc_ttm, prstkc_ttm, sstk_ttm,
txt_q, txt_q_lag4, shrs_q, shrs_q_lag4, noa, noa_lag4, sue, fscore) + be_lag1q_lag4 needed by droe.
CIK scope from the T19 bridge (argument: a CIK list file). Chunked by CF-R batch with per-batch receipt, verified
resume, manifest last; each invocation <= 180 s and <= 1536 MiB (design for ~120 s); read-only on all sources.
Reuse atx-db helpers where they exist (import read-only, pin by code hash) rather than re-deriving concept maps.
