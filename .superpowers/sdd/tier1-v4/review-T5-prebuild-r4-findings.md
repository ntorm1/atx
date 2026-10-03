### Spec Compliance — ❌ Scoped operational prebuild needs one further fix; actual acceptance pending

Independently reviewed commit 55d718d8362bfbc058ee48b676272bc078b720df, its complete scoped diff and updated Task5 report against retained REVIEWER/global/resume contracts and round3 I1/I2. No real data reads/scans/jobs, HTTP, implementation changes, full suites, staging or commits occurred. Only this assigned artifact was written.

✅ Prior I1 addressed: completion and final verification reload full complete unit receipts, reconstruct exact expected SQL/options/source/code bindings, compare live dependency identities including linkage, and retain those dependencies in the published preparation identity. Cached admission rechecks dependencies after output verification. ✅ Prior I2 addressed: required-unit inventory is derived from equal nonempty pinned SUB/NUM/PRE quarter sets, exactly six fixed outputs plus two products per pinned quarter. Missing/extra/empty units fail. Sidecar-summary equality, output bytes and footer rows are independently checked. The v2 format excludes unsupported v1 certification.

❌ A new source-list ordering mismatch rejects a valid unchanged pinned snapshot at completion (`fundamentals.py:434`, `:510`, `:524–528`). Scoped quality therefore still needs fixes.

⚠️ Actual Task5 item/balance/FSDS/cutoff/mega-alpha/coverage/consumer gates remain NOT_MEASURED. Task4 remains OPEN **0/6 PASS**. No actual operational recovery, real resumed-output equivalence or production memory measurement is established by this review. Root-owned recovery still requires reviewed code and the coordinated guarded slot; final producer/universe/source/consumer evidence remains required.

### Strengths

The repair checks an exact expected inventory rather than trusting recorded membership. It independently reconstructs SQL, keys, selected sources and writer options, so editing both summary and sidecar together cannot certify a changed unit. Endpoint dependency hashing memoizes each path: a quarter payload used by its union is hashed once at each endpoint. FSDS remains globally checked at startup/completion/final publication and selectively before/after unit execution or cache admission. Endpoint checks retain the explicit quiescent contract; reverted-between-check changes are outside that contract, and no filesystem lock is claimed.

### Issues — Critical / Important / Minor

Critical: none.

**Important I3 — Creation and certification disagree on source ordering** (`atx-db/src/atx_db/alpha_panel/fundamentals.py:434`, `:510`, `:524–528`). `_required_preparation_units` sorts selected SUB/NUM paths; `prepare` records them by `snapshot.pins` insertion order inherited from the manifest's quarter mapping. `verify_units` compares lists exactly. JSON mapping order is not a quarter-order qualification, and SourceSnapshot accepts either order. A two-quarter offline fixture with unchanged file hashes and valid manifest entries ordered 2020q2 then 2020q1 completes unit writes but fails certification with `ValueError: uncertified preparation unit or dependency drift: .../_work/sub_clock.parquet`. Repeating preparation cannot repair this deterministic mismatch; no source drift occurred. Fix: use one canonical ordered source selection for both cache creation and required-unit reconstruction (prefer deriving execution selections directly from the required specs or a common helper). Add reversed/nonchronological manifest-quarter order positives that certify, then resume unchanged; retain actual drift negatives. Do not change hashes, inventories or thresholds to work around it.

Minor: no additional scoped finding. Dataset-scale operational/resource evidence remains pending.

### Focused verification

Five selected offline cases passed, **5 passed in 12.74 seconds**, no warnings, using PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1 from C:/atx:

```powershell
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_fund_prepare_cache.py::test_after_prepare_linkage_drift_refused_with_unchanged_control atx-db/tests/test_alpha_panel_fund_prepare_cache.py::test_preparation_completion_rechecks_linkage_before_summary atx-db/tests/test_alpha_panel_fund_prepare_cache.py::test_cached_return_rechecks_live_dependency 'atx-db/tests/test_alpha_panel_fund_prepare_cache.py::test_final_required_inventory_refuses_omitted_or_extra_units[empty]' 'atx-db/tests/test_alpha_panel_fund_prepare_cache.py::test_final_unit_receipt_corruption_refused[options]' -q --tb=short
```

The original concerns are closed by code inspection plus targeted linkage completion/final/cache-admission, empty-inventory and corrupted-options controls. Remaining parametrized fixtures were read rather than rerunning the reported 31-case covering suite.

One tiny offline stdin Python check with the same interpreter/environment imported `prepared_fixture` from the test module under `pytest.MonkeyPatch.context()`, used a temporary directory, reversed `metadata['quarters']` in its synthetic fsds-staging-manifest.json, and called `fu.prepare()`. It reproduced I3 with the error above. Only temporary synthetic parquet/JSON files were read/written; no actual source files were inspected.

### Assessment — Task quality: Needs fixes

Scoped spec: Needs fixes. Scoped quality: Needs fixes for I3; prior I1/I2 are addressed and no other reproducible scoped defect was found. Actual Task5 acceptance remains unmeasured and Task4 remains 0/6. This review grants neither production acceptance nor a recovery/data-job authorization.
