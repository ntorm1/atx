### Spec Compliance — ❌ Scoped operational prebuild needs fixes; actual acceptance remains pending

Independently reviewed commit 755d7c8249727a52df6908b4fe0d207c8c1f4a5a, Task5 report, interruption reconciliation and retained REVIEWER/global/resume contracts. This reviewer did not author the change. Scope is preparation authentication/recovery, not financial rule certification. No actual data reads/scans/jobs, HTTP, implementation edits, full suites, staging or commits occurred. Only this assigned findings file was written.

✅ The new unit cache binds FSDS manifest/file byte identities, module code bytes, SQL, explicit writer options, source selections and dependency payloads. Reuse checks output SHA/size and footer rows. Legacy sidecars cannot certify reuse. Unique attempt files and preserved prior outputs/receipts support conservative interruption recovery. Source startup/completion/prepublication checks stream hashes in 1 MiB blocks; quarter execution hashes its selected PRE/NUM files rather than unrelated quarter payloads.

❌ `fundamentals.py:400–409` omits dependency/linkage revalidation and accepts incomplete preparation output inventories at final certification. The promised endpoint drift/unsupported receipt refusal is therefore incomplete.

⚠️ Actual Task5 coverage/item/balance/FSDS/cutoff/mega-alpha/consumer gates remain unmeasured. Task4 remains OPEN **0/6 PASS**. Offline recovery engineering cannot close production acceptance or certify real resumed byte equivalence/resources. The earlier interrupted run's exit/cause remains unknown. No recovery job, forensic move, publication or acquisition grant follows from this review.

### Strengths

The new format replaces size/mtime fingerprints and readable-footer trust with streamed content hashes. SQL/options/code changes invalidate reuse, and output receipt creation follows candidate verification and replacement. Interrupted candidates and superseded receipts remain available for diagnosis. The supplied tests assert values, preserved artifacts, cache admission and quarter/global equivalence rather than execution success alone. The report distinguishes quiescent endpoint checks from a filesystem lock and keeps actual acceptance pending.

### Issues — Critical / Important / Minor

Critical: none.

**Important I1 — Linkage dependency drift after preparation escapes final verification** (`atx-db/src/atx_db/alpha_panel/fundamentals.py:400`, `:406`; scope dependency declaration at `:496`). The scope unit's sidecar records linkage identity, but the preparation summary retains only each output's identity. `_verify_preparation` rebuilds only FSDS/code binding and verifies outputs; it never reloads dependency pins or checks linkage bytes. A tiny offline scope fixture prepared linkage CIK=1, then changed linkage to CIK=2. Final verification still returned the expected binding while scope remained CIK=1. This is unreverted endpoint drift, so the documented quiescent-contract limitation does not excuse it. Fix: retain dependency identities in the completed preparation receipt and recheck them at successful preparation completion and immediately before publication, including linkage; bind the publication's preparation identity to those dependencies. Add after-prepare linkage mutation and unchanged-linkage positive controls.

**Important I2 — Final verifier accepts an incomplete or empty preparation inventory** (`atx-db/src/atx_db/alpha_panel/fundamentals.py:404–409`). Final certification trusts whatever paths happen to appear in `preparation_units`; it does not require the expected global/quarter/union/scope outputs or complete unit receipts. A JSON summary containing the valid FSDS/code binding plus `preparation_units: {}` passes without any prepared file. Removing an entry likewise disables corruption checking for that required output. Unit-level recomputation correctly refuses malformed/incomplete sidecars, but final certification bypasses those completeness guarantees. Fix: derive the exact required unit/product inventory from the pinned quarter set and fixed preparation steps, require equality, and validate each complete unit receipt/binding and output before publication. Add empty/missing/extra summary-member negatives and unit receipt corruption at final verification.

Minor: no additional scoped finding. Dataset-scale runtime/resource evidence remains pending; fixtures do not establish it.

### Focused verification

Five named offline tests passed, **5 passed in 10.65 seconds**, no warnings. Environment: PYTHONPATH=C:/atx/atx-db/src, OPENBLAS_NUM_THREADS=1; cwd C:/atx.

```powershell
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_fund_prepare_cache.py::test_interruption_resume_fresh_equivalence_and_legacy atx-db/tests/test_alpha_panel_fund_prepare_cache.py::test_interruption_after_output_before_receipt_refuses_reuse atx-db/tests/test_alpha_panel_fund_prepare_cache.py::test_prepublication_rechecks_exact_source_and_prepared_output atx-db/tests/test_alpha_panel_fund_prepare_cache.py::test_quarter_verification_does_not_rehash_unrelated_corpus atx-db/tests/test_alpha_panel_fund_pipeline.py::test_per_quarter_matches_global -q --tb=short
```

Two tiny stdin Python checks with the same interpreter/environment and temporary synthetic files reproduced the findings. I1 used `PreparationCache.copy(..., dependencies=(linkage,))`, wrote the returned units and preparation binding to prepare.json, replaced linkage with a different readable parquet, and called `_verify_preparation` with work_dir/FSDS_DIR pointed at the temporary fixture. Output: `final_verify_accepts_changed_linkage True`; scope CIK=1, linkage CIK=2. I2 wrote only valid `preparation_binding` and an empty `preparation_units` mapping under a temporary empty approved FSDS fixture; output: `empty_unit_summary_passes True`. Neither check opened actual FSDS or warehouse files.

Source mutation window audit: FSDS bytes are checked globally at snapshot creation, preparation completion and final verification; affected source selections are checked before unit admission and after execution. Dependency bytes are checked before admission and after recomputation, but cached returns have no second dependency check and completion/final verification omit those pins. I1 covers that missing dependency endpoint guarantee. Output bytes are checked on unit reuse/execution and for every recorded final member; I2 covers missing inventory membership. Changes reverted between endpoint checks remain outside the explicitly documented quiescent contract. No additional reproducible scoped defect was found beyond I1/I2.

### Assessment — Task quality: Needs fixes

Scoped operational spec: Needs fixes. Scoped quality: Needs fixes because final certification can miss changed required inputs or omit required outputs. Unit recovery is substantially stronger, but both final-verification gaps should close before a guarded recovery run is trusted. Actual Task5 gates remain unmeasured and Task4 remains 0/6; this independent code review grants no production acceptance or data-job authorization.

