# Task 6 offline checkpoint (incomplete)

Status: NEEDS_CONTEXT / implementation remains incomplete. Agent: GPT-6.1-Sol.
No real-data reads, scans, builds, evaluations or acquisitions performed.
No production factor/risk stage is claimed. Parent must not accept this checkpoint
as satisfying full Task6 implementation or fixture results as real gates.

Built: new factors.py/risk_inputs.py; bounded offline formation/regression kernels;
exact explicit release manifest/output SHA checks; streaming prepared-input CLI;
immutable generation/publish-last JSONL; full-window benchmark gate function;
documentation and intentionally non-registerable stubs. No data landed.

Test command from C:/atx/atx-db:
`$env:PYTHONPATH='C:/atx/atx-db/src'; $env:OPENBLAS_NUM_THREADS='1';
.venv/Scripts/python.exe -m pytest -n 0 -q tests/test_alpha_panel_factors.py
tests/test_alpha_panel_risk_inputs.py` => 15 passed, exit 0.

| Criterion | Status | Measured | Command |
|---|---|---|---|
| Offline boundary/PIT/missingness/SHA/regression checks | PASS | 15 tests | pytest above |
| Mkt .99/SMB .90/UMD .90/HML .85/RMW .75/CMA .75 | FAIL unmeasured | 0 real months evaluated; 120 required | Real command deferred under code-only phase |
| q IA/ROE .75 | FAIL unmeasured | 0 real months evaluated; 120 required | deferred |
| Exposure coverage .99 on universe | FAIL unmeasured | 0 real cells; full denominators pending | deferred |

Incomplete engineering/source gaps: native lake source adapters, independently
verified original share economic basis (CIK77159 discrepancy), historical universe
eligibility, correct fixed formation carry, q EG model and independently reviewed
q definitions, complete style construction, FF49/liquidity/borrow source joins,
production Parquet, publisher locking/resumable bounded units, production CLI
gate measurement/manifest validation. Prepared input certification assertions do
not establish any underlying source truth. No acceptance denominator is shortened.

For a tiny guarded schema-only read once producer slot is free: obtain Parquet
schema names (no value reads) for final prices_history files, universe output,
compustat_std_v11 funda/fundq, ratios_v11 fund_state, market/liquidity and borrow
proxy/classification; exact final manifest paths/hashes from controller. Need
source-authoritative histories/units and dates, not numerical-fit inference.

Self-review: no upstream/shared/foreign path edited. Snapshot verification is
streaming, but immutable inputs/exclusive producer slot remain necessary. Existing
modules are unchanged. Complete source definitions must be frozen before any real
return comparison; current CONFIG is an initial offline proposal and is not yet
approved as the full literature formula freeze.
