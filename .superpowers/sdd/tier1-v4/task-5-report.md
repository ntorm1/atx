# Task5 recovery report

Status: IN_PROGRESS. Recovered prior uncommitted Task5 work without editing foreign paths. Runtime agent family: Codex; no independently exposed finer model identity. Controller requested GPT6.1 Sol. No children spawned.

## Implementation and verification

Recovered producer source-backed-only v11 rules, direct annual comparative history and catalog tax/debt/retained-equity vintages. Missing, nil and presentation omissions remain NULL; explicit source zero remains zero. Removed additional implicit missing-NCI parent-equity and NOA zeros. Source debt components are required; no balance plug, sign/size preference, issuer exception or unsupported notes_wide fallback.

The own-filing contractual-revenue preference uses FSDS income-statement presentation; issuer-custom tags cannot impersonate standard concepts. Existing taxonomy/concept IDs remain intact. Duration descriptors preserve actual source starts/ends and clocks, with exact reporting accession preceding older comparative metadata. Ratios require source-backed full-year duration and publish explicit annual-history status while retaining ordinary missing-history coverage denominators. v11 standardized IB does not infer missing discontinued/extraordinary components as zero.

Generated primitive/formula/gold requirements: 25 primitive families, 48 formula contracts. Economic-scope, currency, fiscal-transition, prior-history and share-basis qualifications are explicit; coverage and source certification remain pending. Files: docs/datasets/fundamentals_v11.{md,registry.json,requirements.json}.

Offline commands from C:/atx use `PYTHONPATH=C:/atx/atx-db/src`, `OPENBLAS_NUM_THREADS=1`, `.venv/Scripts/python.exe -m pytest -n0 -o addopts=''`:

|scope|result|
|---|---|
|Recovered depth/depth_std/compustat_std/ratios four files|56 PASS, 73.01 seconds total; includes retained Task3 delayed-component/cutoff tests|
|fund_items/catalog/ttm/fsds/validate five files|70 PASS, 5.36 seconds|
|Updated depth source/namespace/budget fixtures|8 PASS, 2.27 seconds|
|Updated depth_std and ratios fixtures|31 PASS, 32.22 seconds|

An intermediate new regression referenced internal `doq`, which is not a published fundq column: one fixture failed with KeyError. Corrected it to assert published NI and IB behavior; covering rerun passed. No data/source or threshold change addressed that test defect.

## Durable SEC acquisition reconciliation

Existing named allocation: 12 total HTTP adapter attempts including retries/redirects, 24MiB/document, 288MiB total, at most1Hz through sec_http. Counter and two receipts agree: **2 attempts, 8,299,830 bytes**. Two successful retained Flowserve primary documents corroborate 2018 sales 3.832666bn and 2020 sales 3.728134bn. Remaining 10 attempts unused; recovery issued zero new HTTP attempts. Original receipts/counter reside in data/raw/sec_fundamentals_depth; target inventory in t5-approved-targets.json. Retained CompanyFacts/FSDS and originals remain the sources. Namespace-stripped historical diagnostic captions are leads, not certified standardized economic equivalence.

## Publication and acceptance

No recovered publication accepted yet. Stage writes await controller's exclusive coordinated data-job slot after active Task2/Task9 work. New-only publication names: fundamentals_v11, compustat_std_v11, ratios_v11; old producer/consumer outputs are preserved for rollback. Task5 fund validation is isolated in validation/fundamentals_v11.json. t5-publish.py refuses existing completed outputs rather than overwriting another session's stage.

Each phase runs from C:/atx/atx-db under `../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60`, with distinct receipt/stdout/stderr under `.superpowers/sdd/tier1-v4/logs/t5-recovery-<phase>.*`, child `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t5-publish.py <phase>`. Phases: fund, fund-check, std, std-check, ratios, ratios-check. Controller is notified before every stage-writing phase. No network handcheck is implicitly authorized by these phases.

|gate|status|current measured evidence|
|---|---|---|
|105/117 coverage FY2015–25;110 stretch|PENDING / NOT PASS|new producer not built|
|Balance identity >=98.5%|PENDING / NOT PASS|historical v10 diagnostic245572/257687=95.3%;5482 mezzanine/4529 other exceptions; not v11 acceptance|
|Mega-alpha GP .90,SALE .96,CAPX .95,TXT_Q .95,XRD .95|PENDING / NOT PASS|new producer not built|
|FSDS source parity and cutoff zero differences|PENDING / NOT PASS|offline regressions PASS; actual publication audit pending|
|Task3 v11 coverage/sum4/balance/rdq/source/cutoff|PENDING / NOT PASS|versioned standardized consumer not built|
|Task4 acceptance|OPEN / FAIL|retained0/6 PASS; no waiver, denominator or threshold change|

Exact remaining dependencies: guarded complete v11 producer publication and output SHA/size integrity; standardized/ratio rebuilds; final coordinated universe publication for GC3 coverage; affected cache-only independent original-source checks including tax comparatives/debt/NI/COGS/RE and balance residuals; Task7 independent published comparison; Task13 indicated rate; market class/ADR/share/EPS/split/date alignment; composite original-model and literature reference certification. No return statistic or label-window evaluation was computed.

## Fix round1 — prebuild review

Addressed all four Important findings in review-T5-prebuild-findings.md before any real build:

1. CHE and deferred-revenue totals require reported aggregate or both source components in v11 event/history paths. Explicit component zero and legacy behavior remain intact; missing history values are not emitted as facts.
2. Direct reported TEQ enters standardized history/vintages and takes precedence over SEQ/MIB component derivation. Fixtures preserve the earlier missing-total snapshot, recover a delayed400total, then a restated410total, with exact source clocks and cutoff invariance.
3. t5-publish.py checks every child return and stops on nonzero. Check phases publish measured gate status, separate execution success from acceptance, and exit2 for measured failures or missing required measurements. Missing source handchecks remain NOT_MEASURED; exit0 alone never certifies acceptance. Fund gates retain105/117,98.5%balance and original mega-alpha targets, rather than reusing the unrelated historical99%diagnostic flag. Producer cutoff differences are explicitly included.
4. Requirements now cover27primitive families/68formula contracts, adding inventory, receivables, physical investment, free-cash-flow and basic/diluted share growth plus dilution. Quarterly/annual/TTM exact columns/item keys, prior periods and source tables are explicit. Unpublished diluted quarterly shares and unmatched total-versus-continuing EPS definitions remain source-gap/qualification dependencies.

Minor findings also addressed: IB source labels use the same v11 conditions as IB values. Extract reuse verifies each data SHA/size against its receipt and the approved legacy source-manifest inventory; verifies the pinned plan SHA; only immutable data is hardlinked, metadata is copied, unapproved parts/stats are excluded. Frozen parser WIP preserves qualified names/namespaces/units, rejects ambiguous namespace declarations and marks blank/dash numerics unqualified; it does not certify economic scope.

Exact commands from C:/atx, with PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1:

```powershell
atx-db/.venv/Scripts/python.exe .superpowers/sdd/tier1-v4/t5-requirements.py
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' atx-db/tests/test_alpha_panel_fund_depth.py atx-db/tests/test_alpha_panel_fund_depth_std.py atx-db/tests/test_alpha_panel_fund_depth_publish.py atx-db/tests/test_alpha_panel_compustat_std.py atx-db/tests/test_alpha_panel_fund_items.py atx-db/tests/test_alpha_panel_fund_catalog.py atx-db/tests/test_alpha_panel_fund_fsds.py -q --tb=short
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' atx-db/tests/test_alpha_panel_fund_depth.py atx-db/tests/test_alpha_panel_fund_depth_publish.py -q --tb=short
```

Results: generator27families/68contracts; covering7files **96PASS77.74s total**, including retained20Task3PIT/delayed-clock fixtures and the new TEQ cutoff. Final source-inventory/wrapper/matrix refinement rerun **16PASS2.59s**. Earlier fixture incorrectly indexed an absent missing-value history key; corrected the assertion to require no emitted fact, then reran successfully. No real-data scan, stage write, HTTP acquisition or threshold/denominator change occurred in this fix round. Actual producer/source/coverage gates remain unmeasured; Task4 acceptance remains OPEN0/6PASS. Await scoped code re-review and guarded slot.

## Fix round2 — history contracts and exact extract completeness

Read review-T5-prebuild-r1-findings.md fully. Addressed retained I4 and the new extract-completeness Important before any source reuse/build. ROA/ROE/accrual quality now bind actual current/prior stocks, source tables and same-fiscal-quarter four-quarter history. All seven margin/profitability changes bind the exact base metric inputs at both endpoints. ROA/ROE changes explicitly require averaged stock history at each endpoint, including the eight-quarter-old balance. Unsupported ROIC/ROCE capital-scope prerequisites also carry explicit prior dependencies and remain UNPROVIDED/UNSUPPORTED; they are not invented published columns. A regression asserts every formula referencing prior history declares a nonempty prior-input list and comparison period. Matrix remains27families/68contracts; no metric was dropped.

Extract reuse validates the mandatory pinned plan before linking anything; actual data filenames equal the approved set; receipt stems agree with their declared data filename; declared members are distinct and equal the approved set. Data SHA/size pins remain required. Missing/duplicate/stale/incomplete snapshots fail before creating the destination. Named regressions cover duplicate references, missing approved members, missing pinned plan and stale plan, plus existing hash mismatch rejection. I1/I2/I3 and IB fixes remain unchanged.

Exact commands from C:/atx, PYTHONPATH=C:/atx/atx-db/src, OPENBLAS_NUM_THREADS=1:

```powershell
atx-db/.venv/Scripts/python.exe .superpowers/sdd/tier1-v4/t5-requirements.py
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_fund_depth.py atx-db/tests/test_alpha_panel_fund_depth_publish.py atx-db/tests/test_alpha_panel_fund_depth_std.py::test_delayed_and_restated_reported_total_equity_preserves_prior_cutoff atx-db/tests/test_alpha_panel_fund_depth_std.py::test_v11_missing_discontinued_and_extraordinary_facts_do_not_certify_ib -q --tb=short
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_fund_depth.py -q --tb=short
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_fund_depth_publish.py -q --tb=short
```

Results: **23PASS6.58s**; final exact-data-set checks **16PASS2.21s**; final prior-contract assertions **5PASS1.24s**. No real-data scans, source writes/reuse, stage publication, HTTP, foreign edits or acceptance threshold/denominator changes. All actual acceptance measurements remain pending; Task4 remains OPEN0/6PASS. Await scoped re-review/data-slot grant.

## Interrupted fund reconciliation (2026-10-03)
Read-only guarded inspection completed exit 0 in 3.0s, peak 0.0439 GiB (.4 GiB cap); no retry or data mutation. Evidence and exact pins/recovery commands: t5-interruption-reconciliation-v1.md and logs/t5-interruption-inspection-v1.json. Source/target extract identity PASS (85 data batches); prepare cache payload authentication remains an unresolved operational-certification gap. Preserve interrupted _work then recompute preparation under root-owned .8 GiB guarded job; no skip-prepare. Original exit/cause unavailable. Data slot released; actual gates unmeasured, Task4 OPEN.
