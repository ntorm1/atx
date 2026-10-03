### Spec Compliance — ❌ Needs fixes (prebuild fix round 1/5)

Reviewed commit 9221b5d0b7e1b9778c6f146647bb099ec97280ad through the supplied 2,615-line review-T5-prebuild-r1.diff, against the original findings, Task5 brief, binding owner/GC/resume contracts and Task5 fix-round-1 report. Scope is the four Important findings, IB provenance and exact extract reuse pins, including fix-induced behavior. No real-data commands, full suites, acquisition, commits, children or producer writes were run. Package paths remain outside the foreign list and shared registries.

| Original finding | Round 1 disposition | Verified evidence |
|---|---|---|
| I1 unsupported CHE/DREV component zeros | ADDRESSED | fund_items.py:949,1508,1834 require both source components under v11; aggregate sources and explicit zero remain valid; event/history fixture passed. |
| I2 delayed reported TEQ discarded | ADDRESSED | compustat_std.py:270 adds direct TEQ to consumed history; :717 gives reported TEQ precedence over derived components. Fixture preserved NULL at t0 and emitted 400 at t1 and 410 at t2; cutoff before t1 retained NULL. |
| I3 command/gate failure propagation | ADDRESSED for coordinated entrypoint | t5-publish.py:8 propagates nonzero child returns; :29 fills missing required checks with NOT_MEASURED, writes manifest gate_status and separate execution/acceptance status, and returns 2 on non-PASS. Threshold and stop-before-next-call fixtures passed. Individual legacy module CLIs remain completion-only, but the authorized wrapper now evaluates gate results explicitly. |
| I4 incomplete owner matrix | PARTIALLY ADDRESSED | 27 primitive families and 68 formula contracts cover the previously omitted inventory, receivables, investment, FCF and basic/diluted share growth/dilution families. Growth rows bind exact inputs and periods, with diluted-quarterly-share and EPS-definition gaps explicit. Prior dependencies remain absent from average/change rows; see Important R1. |
| Minor IB source label divergence | ADDRESSED | compustat_std.py:579 applies the same v11 XIDO requirement to ibq_src; named fixture passed with both IB value and source label NULL. |
| Minor immutable extract validation | PARTIALLY ADDRESSED; concrete completeness defect established | fund_depth.py:120 pins each encountered data hash to the legacy manifest and receipt; data alone is hardlinked and metadata is copied. It does not enforce exact distinct membership or require the approved plan; see Important R2. |

⚠️ Actual fundamentals_v11, compustat_std_v11 and ratios_v11 publication, SHA/size integrity, source checks, cutoff parity, coverage deltas, 105/117 item gate, ≥98.5% balance identity and mega-alpha exit gates remain unmeasured. Task4 remains OPEN with 0/6 hard gates PASS. This checkpoint does not establish warehouse spec completion or final acceptance. Independent source/composite/published comparison qualification remains open even if every measured wrapper gate eventually passes.

### Strengths

The component and TEQ fixes directly address the reproduced failures and retain earlier visible snapshots. Gate failures are now machine-visible without overriding existing thresholds or equating process completion with acceptance. The matrix distinguishes physical capital growth from gross investment/capex and explicitly retains unsupported diluted quarterly shares. Receipt hashes and copied metadata substantially improve extract isolation.

### Issues — Critical

None established.

### Issues — Important

**R1 — retained I4, plan-mandated: average and change contracts still omit required prior bindings.** .superpowers/sdd/tier1-v4/t5-requirements.py:55–59 initializes prior_input_columns=[] and only fills it for growth/dilution cases. Therefore ROA, ROE and accrual_quality formulas explicitly reference prior_at/prior_seq/prior_txditc while their generated contract declares no prior inputs or prior period. All seven margin/profitability change rows also retain formula=current-prior, prior_input_columns=[] and no prior-period/input binding. Their current fields are the union of quarterly and TTM primitive families rather than the exact selected metric inputs. This leaves the original per-metric history/formula requirement unresolved and cannot guide a consumer without inventing period choices. Bind exact current/prior fields, source tables and declared comparison periods for these rows; for nested ROA/ROE changes also specify each endpoint's denominator-history requirement. Regenerate the JSON and add focused assertions that prior-dependent formulas cannot declare empty history. Coverage/source certification can remain pending; do not fabricate data.

**R2 — exact approved extraction inventory can silently omit a batch and its pinned plan.** fund_depth.py:114 checks receipt count, :117 trusts record['file'], :120 validates only that encountered member's pin, and :126 makes plan validation conditional on the file existing. It does not require receipt/data stem agreement, unique declared members, exact equality with the approved manifest keys, or the manifest-approved plan's presence. Focused offline reproduction used a manifest approving batch-0000.parquet and batch-0001.parquet plus a pinned plan, with two receipts both declaring batch-0000.parquet, one actual data file, and no plan. reuse_extract returned normally with files=4; the target contained only batch-0000.parquet and the two receipts. This defeats the claimed exact source snapshot check and can leave an approved issuer batch absent before the 35-minute build. Require the distinct declared member set to equal expected keys and each receipt stem to match its member; require the pinned plan and verify its hash before linking any member. Add duplicate-reference, missing-approved-member and missing-plan regressions. Existing hash mismatch rejection should remain intact.

### Issues — Minor

None newly established beyond the incomplete obligations above. No scoped evidence justifies reopening the component, TEQ, wrapper or IB fixes.

### Focused verification

Seven selected offline tests passed in 7.89 seconds, with no warnings in captured final output. Command from C:/atx with PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1:

```powershell
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_fund_depth.py::test_cash_and_deferred_revenue_require_both_components_in_event_and_history atx-db/tests/test_alpha_panel_fund_depth_std.py::test_delayed_and_restated_reported_total_equity_preserves_prior_cutoff atx-db/tests/test_alpha_panel_fund_depth_std.py::test_v11_missing_discontinued_and_extraordinary_facts_do_not_certify_ib atx-db/tests/test_alpha_panel_fund_depth_publish.py -q --tb=short
```

The separate exact-inventory reproduction ran synthetic JSON/byte fixtures under tempfile.TemporaryDirectory via atx-db/.venv/Scripts/python.exe stdin; it read no retained source data and demonstrated successful reuse despite an omitted approved data member and plan. The report's broader 96/16 passing test counts are producer evidence, not independently rerun suite certification.

### Assessment — Task quality: Needs fixes

Spec checkpoint: Needs fixes for the remaining matrix history contract and exact reuse completeness validation. Quality checkpoint: Needs fixes before publication; I1/I2/I3 and IB provenance are independently supported, and a narrow follow-up can close R1/R2 without a real-data scan. Real acceptance remains pending complete guarded publication and measured source/coverage gates.
