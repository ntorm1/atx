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

## Operational fix round 3/5 ? authenticated resumable preparation

Implemented after interruption reconciliation, with the controller selecting GPT-6.1 Sol for this agent; no independent runtime model attestation is available. This agent implements this checkpoint and does not independently review its own change. Financial rules, thresholds, legacy publications, immutable CF extract reuse and the approved extraction plan are unchanged.

Preparation now validates the approved FSDS manifest and exact physical parquet inventory against per-file SHA-256/byte pins. Streamed 1 MiB hashing checks the complete source at startup, successful preparation completion and immediately before v11 manifest publication. Quarter units check only their own PRE/NUM files before and after work; global SUB/NUM units check their own relevant files. Source drift fails before a completion receipt or publication manifest can certify the affected run. SQL, source manifest/file identities, preparation module bytes, writer options and dependency payload identities bind every unit receipt. Completed output SHA/size/row count must match before reuse. Missing/malformed/incomplete/stale/corrupt receipts and legacy `.sha256`-only outputs recompute; attempts and superseded units/receipts are preserved. Receipt publication occurs atomically after output verification/replacement. A stop between output replacement and receipt creation cannot certify that unit on restart. The scope unit binds linkage bytes; derived unions bind exact part payloads. v11 prepublication rechecks source and all recorded preparation outputs.

Quiescent source contract: FSDS and linkage inputs must remain unchanged throughout preparation/build/publication. Endpoint checks are evidence under this contract, not a lock or protection against changes reverted between checks. Global units require additional table-wide hashing around execution; quarter units do not repeatedly hash the entire corpus. No real FSDS parquet was opened or hashed in this implementation session: only the approved JSON metadata schema was inspected. No data scan/job, HTTP, publication, forensic move or unsupported legacy cache reuse occurred. The prepared forensic script remains unrun.

Exact final offline command from C:/atx, with PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1:

```powershell
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_fund_prepare_cache.py atx-db/tests/test_alpha_panel_fund_pipeline.py -q --tb=short
```

Results: **15 PASS in 5.94s**. Fixtures cover interruption before replacement and after replacement/before receipt, fresh resume and byte/value equivalence, preserved legacy/superseded units, readable payload corruption, missing/incomplete/malformed receipts, code/SQL changes, same-size/restored-mtime source changes at startup/during execution/prepublication, physical inventory/manifest drift, prepublication preparation corruption, and quarter hashing locality. Existing quarter-union versus global-query equivalence remains covered. Initial test-only relative import issue produced 2 failures/10 passes, corrected; subsequent runs 12 passes/5.93s and 14 passes/6.88s.

Changed owned files: alpha_panel/fundamentals.py, new alpha_panel/fund_prepare_cache.py, focused new cache fixtures, existing preparation pipeline fixtures, fundamentals_v11.md and this report. Self-inspection found and corrected a mechanical insertion into timed() rather than cached_copy() before testing. Independent checkpoint review remains required. Actual item/balance/FSDS/cutoff/mega-alpha/coverage gates remain NOT_MEASURED; no warehouse acceptance is claimed, Task4 remains OPEN 0/6 PASS and GC3/consumer rebuild gates remain pending. Dataset operational recovery is not yet executed.

## Operational fix round 4/5 — exact preparation certification

Read review-T5-prebuild-r3-findings.md in full and addressed both Important findings from independent review of 755d7c82. I1: completed preparation stores full unit receipts and dependency identities; completion and prepublication reload/check every complete sidecar against its exact expected SQL, key, writer options, source selection, module/source binding, output SHA/size/footer rows and current dependency bytes, including identity linkage. Publication preparation identity includes these certified units/dependencies. Cached returns also recheck selected sources and dependencies after output admission. I2: certification derives exact required paths from fixed global/union/scope products and the pinned quarter set, requiring equal nonempty SUB/NUM/PRE quarter inventories, six fixed outputs and both PRE products per configured quarter. Empty, omitted, extra or unsupported units cannot certify. Receipt format advanced to v2, so v1 summaries cannot silently reuse unsupported certification.

Whole-corpus source hashing remains at startup/completion/prepublication, with exact per-unit selected-source checks. Completion/prepublication use one memoized streamed identity per shared output/dependency; unions do not rehash their already checked part dependencies. Build progress omits the expanded authentication inventory/binding from stdout; durable prepare/manifest evidence retains it. No real data read/hash/scan/job, HTTP, forensic move, unrelated edit or source/threshold/denominator change occurred. No operational recovery or actual acceptance is claimed.

Exact offline command from C:/atx, with PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1:

```powershell
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_fund_prepare_cache.py atx-db/tests/test_alpha_panel_fund_pipeline.py -q --tb=short
```

Final result: **31 PASS in 45.68s**. Named covering regressions: `test_after_prepare_linkage_drift_refused_with_unchanged_control`, `test_preparation_completion_rechecks_linkage_before_summary`, `test_cached_return_rechecks_live_dependency`, `test_final_required_inventory_refuses_omitted_or_extra_units` (empty/global/quarter/extra), `test_final_unit_receipt_corruption_refused` (incomplete/missing/SQL/source/dependency/options/rows/malformed), `test_required_quarters_refuse_missing_configured_table`, retained exact source/output endpoint drift, interruption recovery and quarter/global equivalence. Initial run was 29 PASS/1 fixture failure in 40.21s: the options-corruption case assigned the same empty options it already had, so it did not actually corrupt the fixture. Changed it to an unexpected option, then final covering run passed. `git diff --check` covered owned changed paths. Actual Task5 gates remain NOT_MEASURED; Task4 OPEN **0/6 PASS**; independent round4 review is required before root-owned rebuild, followed by final Task2 universe, integrity/source and consumer qualification dependencies.

## Operational fix round 5/5 — canonical preparation source ordering

Read independent review-T5-prebuild-r4-findings.md fully. Prior I1/I2 remained closed; addressed I3 by one shared `ordered_sources` contract for cache creation, expected-unit construction and certification. Global preparation calls take their source lists from the required specs; quarter source lists use the same sorted/unique helper. Reviewed remaining construction: quarter execution and expected specs both sort quarter filenames, union SQL uses the same helper and sorted part sequence, dependency maps compare by exact path/payload identity, and fixed SQL/options remain shared or identical. Duplicate selections fail rather than being silently deduplicated. Approved FSDS JSON rejects duplicate keys before quarter mappings can be silently collapsed; empty or incomplete configured quarter inventories retain fail-closed behavior.

Exact offline command from C:/atx with PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1:

```powershell
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_fund_prepare_cache.py atx-db/tests/test_alpha_panel_fund_pipeline.py -q --tb=short
```

Result: **34 PASS in 59.41s**, no failing intermediate run. New `test_permuted_manifest_quarters_certify_identical_outputs_and_resume` covers reversed two-quarter and nonchronological three-quarter manifests with reversed table keys: outputs, SQL, source lists and dependency pins equal the canonical-order run; unchanged reordered manifests certify and resume all units with identical complete bindings. Raw manifest SHA/size pins deliberately still differ when manifest bytes are rewritten, preserving the existing source identity policy rather than claiming byte identity for different JSON. `test_empty_quarter_inventory_and_duplicate_quarter_keys_fail_closed` covers empty quarter sets, duplicate JSON quarter keys and duplicate selected paths. Retained linkage drift, exact inventory/receipt/source/output corruption, interruption and quarter/global fixtures all passed. Owned-path `git diff --check` passed.

No actual source/data read/hash/scan/build/job, HTTP, forensic move, foreign edit or threshold/denominator change. Fifth local fix round is the cap; no reset or sixth round is claimed. Independent final scoped review remains pending. Actual Task5 gates remain NOT_MEASURED; Task4 remains OPEN **0/6 PASS**. Root-owned guarded recovery, final Task2 universe, publication integrity and independent source/consumer acceptance dependencies remain outstanding.
