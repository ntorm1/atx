# Core metric breadth report

Root verification update,2026-09-20 22:19UTC: combined P1/Core focused run
completed65passed/1failed; the sole failure was this task's off-by-one expected
TTM denominator at quarterindex4. The fixture correction is documented in
core-metric-breadth-fix1-report.md. Root reran that one affected case and it
passed under2.5GiBguard, peak0.751171GiB. All other cases passed the initial run
(peak0.922688GiB). Touched-file Ruff passed after one mechanical import-spacing
cleanup. Production definitions remained unchanged. Accepted for separate Core
commit after P1/0315 commit5519d1ac; no extra review required for fixture/spacing
fixes. This is runtime fixture evidence, not live metric coverage.

## Delivered catalog wave

`atx-db/src/atx_db/seeds/derived_metric_definitions.csv` adds 28 version-1
definitions, raising the declarative catalog from 173 to 201 definitions while
leaving every existing row unchanged.

| Family | Codes and formulas |
| --- | --- |
| Quarterly growth | `revenue`, `gross_profit`, `operating_income`, `net_income`, `eps_diluted`, `cfo`, `fcf`, `capex`, and `rd_expense` each receive `*_q_growth_yoy = yoy(<quarterly series>)` and `*_q_growth_qoq = qoq(<quarterly series>)`. The direct standardized inputs are `revenue`, `operating_income`, `net_income_total`, `eps_diluted`, `cash_flow_from_operations`, and `r_and_d_expense`; existing quarterly compositions supply `gross_profit_q`, `fcf_q`, and `capex_q`. |
| CAGRs | `gross_profit_cagr_3y = cagr(gross_profit_ttm, 3)`, `operating_income_cagr_3y = cagr(operating_income_ttm, 3)`, `ebitda_cagr_3y = cagr(ebitda_ttm, 3)`, `fcf_cagr_3y = cagr(fcf_ttm, 3)`, and `common_equity_cagr_3y = cagr(common_equity_q, 3)`. |
| Basic EPS | `eps_basic_ttm = ttm(eps_basic__1034)`. |
| Operating efficiency | `dso_days = safe_div(receivables_avg2 * 365, max(revenue_ttm, 0))`; `dio_days = safe_div(inventory_avg2 * 365, max(cost_of_revenue_ttm, 0))`; `dpo_days = safe_div(payables_avg2 * 365, max(cost_of_revenue_ttm, 0))`; `cash_conversion_cycle = dso_days + dio_days - dpo_days`. |

Quarterly comparisons use reported unadjusted fiscal quarters and can be
seasonal. `yoy` and `qoq` retain the DSL's absolute nonzero base convention.
The CAGR DSL emits a value only when current and three-year-ago endpoints are
positive. The DSO/DIO/DPO `max(base, 0)` guard means zero, negative, or absent
trailing revenue/cost bases remain NULL. The balance components use the existing
`avg2` definitions, which average current and four-quarter-prior balances over
the same annual span. Day-based measures use a 365-day convention.

The wave intentionally does not add normalized-income or normalized-EPS
metrics. Although `normalised_income` exists, the catalog does not establish a
concrete recurring-earnings semantic or a compatible denominator basis, so
deriving one would invent an adjusted-earnings meaning.

## Seeding and validation plan

No migration is required. Migration 0302 already owns the catalog table and
normal activation invokes `seed_derived_metric_definitions`, which replaces the
seeded catalog before the derived build. The next normal derived stage therefore
loads these definitions and materialises them after standardized facts are
available.

The pre-existing dead `derived`-statement registry codes `dso_days`,
`dio_days`, `dpo_days`, and `cash_conversion_cycle` are now explicitly reclaimed
by `atx-db/src/atx_db/derived_registry.py`, allowing the declarative engine to
own the canonical metric names without changing any source loader or schema.

The new focused test file is
`atx-db/tests/test_core_metric_breadth.py`. It covers catalog count/dependency
presence, the explicit dead-derived code reclamation, growth values and availability propagation, 3-year CAGR values,
basic-EPS TTM, DSO/DIO/DPO/CCC arithmetic, a missing quarter, nonpositive
annual bases, and nonpositive CAGR endpoints.

Root-run command only after the source writer has released its workload and the
0315 PIT changes are stable. It uses the locked project interpreter for both
the guard and its test child. The receipt name must be fresh.

```powershell
cd C:\atx\atx-db
$testPython = "C:\atx\atx-db\.venv\Scripts\python.exe"
$memoryGuard = "C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py"
$receipt = "C:\atx\.superpowers\sdd\tier1-parity\core-metric-breadth-tests-0315-memory.json"
$log = "C:\atx\.superpowers\sdd\tier1-parity\core-metric-breadth-tests-0315.log"
& $testPython $memoryGuard --job-gb 2.5 --receipt $receipt -- `
  $testPython -m pytest tests/test_core_metric_breadth.py -n 0 -q *>&1 | Tee-Object -FilePath $log
```

That test is queued behind migration 0315's PIT invalid-state repair. Its
missing-quarter and nonpositive-endpoint assertions require the latest explicit
NULL state to be `missing_input_or_domain` at the target event clock. Its
nonpositive DSO/DIO/DPO/CCC base assertions require an explicit NULL state with
a non-`valid` status, because the guarded denominator can classify as
`zero_denominator`. These checks select the latest state by its event clock and
identity rather than assuming invalid outputs are dropped.

This task performed static edits only; it ran no application or runtime
workload and makes no live materialisation-coverage claim.
