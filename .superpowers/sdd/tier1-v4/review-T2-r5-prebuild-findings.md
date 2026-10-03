### Spec Compliance — ❌ Scoped operational prebuild needs fixes; final Task2 remains pending/FAIL

Independently reviewed commit 8205fd60, updated Task2 report/brief and retained global/resume/REVIEWER contracts. This reviewer did not author Task2. Scope is authenticated bounded-year continuation, not source/class/French acceptance. This is round **5/5**, the binding local cap; no sixth round or reset is presumed. No actual source/data reads/hashes/scans/jobs, HTTP, implementation edits, full suites, staging or commits occurred. Only this assigned findings file was written. The live root fund job was left untouched; focused checks used tiny synthetic Arrow files or mocked connections, with no DuckDB connection opened.

✅ Nonnegative max-new-years bounds only newly completed years; zero builds none and validates existing units. Required inventory remains configured YEARS independently of work selection. Missing required years return partial_cache_only/published=false before output promotion, gates, validation and stage manifest writes. Narrow selection cannot certify shortened acceptance. New isolated scratch leaves legacy artifacts untouched. Incomplete/stale/corrupt cache entries fail and remain available for inspection. Before whole-inventory promotion and after measurements, source/code identities are rechecked; promoted payload SHA/size is compared with the year cache.

❌ Exact consumed-source inventory and year key integrity are not established by the new helper (`universe_checkpoint.py:15`, `:39–54`). These are Important authentication gaps.

⚠️ Actual final Task2 source/universe/hand/coverage/gate verification remains pending; French remains the historical **10/60 FAIL**, I4 remains open and R9 descendants stay held. Physical identity of old publication files does not refresh their source snapshot. No actual bounded-year continuation, latest membership/sample certification or guarded resource result was produced by this review. SEC remains closed; no acquisition or data-job grant follows from code review.

### Strengths

The incomplete-inventory branch cleanly separates scratch completion from publication. Each sealed receipt pins code/source identity, payload SHA/size/footer rows and expected-date daily counters/checksum, and cache reuse is validated rather than admitted by existence. Tests preserve a failing French gate rather than equating process success with acceptance. Legacy and interrupted artifacts are not silently deleted. Final promotion interruption remains explicitly subject to R9 integrity verification.

### Issues — Critical / Important / Minor

Critical: none.

**Important R5-1 — Required consumed files can be absent from authenticated source inventory** (`atx-db/src/atx_db/alpha_panel/universe_checkpoint.py:14–20`). `identity` verifies only whatever members appear in manifest files, accepting absent/empty/subset files mappings. It never requires known consumed files to be listed or establishes an exact physical/product inventory. For example `universe.load_issuer_tables` reads security_master/finra_names.parquet (`universe.py:175–176`), but a security_master manifest with files={} passes identity. A tiny fixture changed that readable payload from security_id=1 to 2 while keeping its empty manifest unchanged; both identities were identical. Endpoint verification therefore cannot detect drift in an omitted consumed file. Fix requirement: require every consumed product and appropriate exact declared/physical inventory, refusing missing/unsupported declarations; do not turn an incomplete manifest into an authenticated snapshot. Add omitted-required-member, empty-inventory and extra/substituted-file negatives, plus actual declared-member mutation positives. The fifth-round cap requires controller disposition; this report does not authorize an automatic sixth repair.

**Important R5-2 — Authenticated year receipts certify duplicate or unsupported payload keys** (`atx-db/src/atx_db/alpha_panel/universe_checkpoint.py:39–54`, seal at `:58`). Validation checks hash/footer row count and daily metadata, but never verifies (session_date, security_id) presence, uniqueness, nonnullness, year/date membership or equality with expected panel keys. A tiny payload with two identical (2021-01-04, 1) rows and a syntactically valid one-date counter receipt seals successfully with rows=2. The supplied continuation fixture itself uses a keyless payload containing only year; it also passes, demonstrating the tests do not assert actual universe key integrity. A self-consistent hash protects bytes, not their required keys. Fix requirement: bounded key/schema/date validation before sealing and on admission/promotion, with an exact expected panel-key identity or equivalent comparison. Add duplicate/missing/null/wrong-year/date/key-set refusal fixtures and a valid-key interrupted/resumed equivalence control. This is engineering incompleteness, not evidence that the actual retained universe has duplicate keys; no actual payload was inspected.

Minor: no additional scoped defect. Full-scale checkpoint hashing/runtime/resource evidence remains unmeasured. Narrow work bounds completed years but do not bound whole-input identity hashing or issuer-table loading; this limitation should remain visible during controller scheduling.

### Focused verification

Three selected mocked offline tests passed, **3 passed in 6.12 seconds**, no warnings. Environment PYTHONPATH=C:/atx/atx-db/src, OPENBLAS_NUM_THREADS=1; cwd C:/atx.

```powershell
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_universe_checkpoint.py::test_narrow_year_selection_cannot_publish_shorter_acceptance atx-db/tests/test_universe_checkpoint.py::test_interrupted_year_leaves_no_receipt_or_publication atx-db/tests/test_universe_checkpoint.py::test_resume_only_refuses_input_change_before_partial_return -q --tb=short
```

Two tiny temporary-file checks using the same interpreter/environment reproduced the Important findings without DuckDB. R5-1 wrote a security_master manifest with files={}, one small finra_names.parquet, calendar and one panel partition; `Q.identity({'security_master': manifest_sha}, (), [2021])` was identical before/after changing finra_names bytes. Output: `undeclared_consumed_input_change_invisible True`. R5-2 constructed YearCache with a synthetic fixed identity, wrote duplicate date/security keys, and sealed rows=2 with all DAILY_COLS counters=1; output: `duplicate_keys_accepted 2`. No warehouse or actual source path was opened.

### Assessment — Task quality: Needs fixes

Scoped spec: Needs fixes. Scoped quality: Needs fixes for R5-1/R5-2; bounded exits and preservation behavior are sound but the intended authentication does not yet certify required files and keys. This is the fifth-round cap: controller disposition is required for unresolved engineering issues, with no sixth round presumed. Final Task2 stays pending/FAIL, French FAIL/I4 remain open, and R9 descendant hold remains binding.
