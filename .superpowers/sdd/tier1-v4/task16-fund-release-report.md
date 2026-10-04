# Task16 independent fundamentals release dependency: initial engineering checkpoint

2026-10-03. **DONE_WITH_CONCERNS for the authorized offline implementation checkpoint; full Task16 remains open.** Independent review and all actual-source/build/use grants remain required. No production source, warehouse value, actual source hash, source build, HTTP acquisition or return evaluation was run. No existing Task5, Task6, release_inputs or universe_release module was edited. Legacy batches0000/0001, including the controller's successful guarded operational run, remain NOT_ADMITTED and must be recomputed in an isolated generation.

## Scope and implemented behavior

New `fund_batch_release.py` supplies explicit bindings and frozen contracts, actual source/code/reader/144-unit preparation admission, all original85 extraction/source units, new-only native batch computation, authenticated continuation, independent source archive reconstruction/native batch replay, typed output keys/clocks/populations, explicit admitted-unit merges, independent native financial gates, immutable-generation publication, pointer resolution/full validation, writer locking, validated rollback and transaction recovery, and offline/runtime CLI. The separate registry is a stub, not shared registration.

Physical and logical names both remain `fundamentals_v11`; the leaf has its own native-compatible manifest, including source-backed missing/zero/balance/sales/history rules and all four native output identities. `compustat_std` can bind the explicit relative generation path ending `/fundamentals_v11`, retains its source-backed branch and native manifest dependency, and persists this path for validators. The controller must independently rebuild standardized v11 and ratios v11 from their explicit final bindings before use. No silent legacy binding exists in this adapter.

Frozen artifact `atx-db/docs/datasets/fundamentals_release_v1.contract.json`: 1865bytes, SHA256 **ca8449b281a86401e16f9220c04362f6040d50b9f1fed363921f058df7c58d4c**; canonical contract digest **82572faf9304cdcc8c73b29ff9e895c7530e90d3fb913b9033514eb00e706070**. Runtime freeze requires its exact current semantic content and includes its physical hash in the actual code inventory. This is frozen before any actual build/comparison; there have been no real trials or tuning.

Consumer interface: `resolve_current(stage)->generation` checks pointers only; `validate_generation(stage,generation,con)->outer_manifest` performs full actual admission. Exact stage is root/fundamentals_release_v1; exact native leaf is generations/g/fundamentals_v11. Outer/native manifest, validation, snapshot, all85 unit receipts, start receipts, native counters and four output payloads remain pinned and rechecked. Final validation rejects abbreviated reader/code/preparation/unit inventories and unsupported status/qualification/types; it preserves `NOT_PRODUCTION_READY` and source_quality_certified=false.

## Complete native reader/writer map

Native calculation reader code: fundamentals/fund_items/fund_extract/fund_fx/fund_catalog/common/fund_prepare_cache, fund_validate/fund_asof, reference_classifications.py, statement_map_seed.py; this adapter and frozen contract also pinned. Private cloned function globals bind prepared getters, FSDS, source extract85 paths, optional FX/8-K, root, calendar and price selectors. Native writer classes retain native formulas/schema and receive an isolated destination. Shared modules/environment/common roots are never mutated.

Actual reads: source ZIP; extract plan/data/receipt/statistics all85; approved preparation summary plus exact144 authenticated units and their FSDS/dependency/source inventory; identity/links.parquet; identity/link_table.parquet; calendar.parquet; native statement seed; explicit optional FX and8-K; full prices/year=*/*.parquet; full prices_history/year=*/prices.parquet; all six2020–2025 panel_member files; identity/sec_filings/reference manifests and all declared payloads. Source/clock/admission descriptions remain lineage, never economics certification. Authentic external sideproduct release evidence and qualified membership/identity remain controller dependencies.

Native final helper calls: coverage, benchmark, balance, cutoff, fx_check, ttm_gaps, exit_measure(fullsixyears), fundamentals._stats and coverage_by_year, plus native fund_asof through cutoff. Original mathematical formulas, fiscal coverage windows, structural handling, failure populations and source-backed v11 branches are retained. Financial level coverage after2022 does not authorize return construction or evaluation; no return helper exists in this component.

Writes: new generation snapshot/units/UUID attempts/start/native parts/admissions; four native final Parquets plus native manifest; generation validation and outer manifest; root/_tmp/fund-release-verify and root/_tmp/fund-release-extract-verify temporary reconstruction directories; stage/_spill scratch DB/spill; stage writer lock/current/previous/pointer-transaction files. Native unsafe `_batch_done`, run_batches, finalize, validator.run and shared write_stage_manifest are never invoked. No legacy outputs are imported, replaced or blessed.

## Focused offline verification

All fixtures are synthetic; each fixture connection sets DuckDB128MB, one thread and UTC. OPENBLAS_NUM_THREADS=1 was set. Named runs below cover27 distinct cases in the new test file. No broad suite or actual-source validation was run.

| Command suffix (all `python -m pytest atx-db/tests/test_fund_batch_release.py`) | Result |
|---|---|
| first `-q` | 18passes; mistakenly inherited repository `-n4` default; worker pool completed/exited. No reliable elapsed summary was printed. This violated the intended single-worker fixture lane; causal connection to any controller memory stop is unproven. |
| `-n0 -o addopts='' -q -k 'full_extract or cutoff_timing or frozen_config or complete_snapshot'` | 4passed17deselected22.49s |
| `-n0 -o addopts='' -q -k 'private_native_v11 or legacy_no_start or original_gates'` | 8passed1failed12deselected9.86s; the deliberately abbreviated legacy receipt now failed earlier with ValueError after receipt tightening. Fixture was corrected to contain all receipt fields, isolating its intended missing-start defect. |
| `-n0 -o addopts='' -q -k legacy_no_start` | corrected1passed20deselected3.04s |
| `-n0 -o addopts='' -q -k 'full_extract or cutoff_timing or contract_cli or public_validation'` | 8passed19deselected46.71s, including archive replay/value forgery/error-population controls and typed public qualification |
| `-n0 -o addopts='' -q -k cutoff_timing` | 1passed26deselected1.51s after persisted tuple normalization assertion |
| `-n0 -o addopts='' -q -k pointer_pin` | 1passed26deselected1.42s after prepublication manifest-pin tightening |
| `python -m py_compile <new-module> <new-tests>` | PASS |

Tests prove: actual private native v11 calculation keeps unavailable treasury stock missing; new-only computation and authenticated continuation; independent replay rejects same-schema value tampering even with rewritten identity; counter/error population replay rejects forged receipts; NULL/earlier clock, source-unsupported CIK, duplicate key and wrong schema fail; no at-compute start prevents legacy admission; all85 source extraction inventory/full ZIP reconstruction is mandatory; a rewritten legacy extraction SHA does not admit a changed value; one selected new batch cannot publish an84-unit partial cache; exact full-year/member/sample denominators prevent empty PASS; exact code/readers/144-unit contracts are mandatory; current pointer byte pin, writer exclusion, journal recovery and full-target rollback calls operate; explicit native/standardized source-backed names and manifest dependency remain; CLI contract is offline. Fixtures mocking orchestration/admission are labeled as such and do not establish real-source correctness.

## Actual gates: FAIL / unmeasured

| Criterion | PASS/FAIL | Measurement / command still required |
|---|---|---|
| Actual reader/code/source/preparation85-unit admission | FAIL_PENDING | zero actual snapshots/generations admitted; controller guarded build/validate after independent review |
| FY2015–2025 top3000 90% coverage,105minimum/110stretch items | FAIL_UNMEASURED | no actual native coverage run |
| 10000FSDS stratified cells,0.5% tolerance/native mapping+match gates | FAIL_UNMEASURED | no actual benchmark run |
| Balance98.5% and native99% diagnostic | FAIL_UNMEASURED | no actual balance run |
| Exactly200seeded cutoff events/zero differences | FAIL_UNMEASURED | no actual cutoff replay |
| Full2020–2025 exit field targets/all member and linked denominators | FAIL_UNMEASURED | no actual full-six-year exit run |
| Final standardized/ratios explicit binding | FAIL_PENDING | no final candidate or dependent rebuild |
| Runtime memory/time/disk <=1GiB guard | FAIL_UNMEASURED | no actual adapter job; configured300MB DuckDB is not total-process proof |
| Whole Task16, Task4, economic source qualification | FAIL_OPEN | source_quality_certified=false and NOT_PRODUCTION_READY retained |

0 actual PASS /9 grouped actual FAIL or pending. Source acquisition0bytes; actual published rows/files/generations0; actual date span/coverage0 measured. Engineering fixtures are not source evidence and do not change the French/task acceptance failure states.

## Cost, limitations and self-review

Whole-inventory SHA and replay cost is deliberate and substantial. Initial full freeze reconstructs all85 source extractions directly from ZIP; continuation independently replays every existing admitted financial unit; bounded new computation additionally replays its native output before sealing. Final public validation repeats whole-source extraction and all85 native financial units, exact admitted merges and original final gates. Before/after source/preparation/code/output identity checks occur around work. `max-new-batches` limits only new computations, not full inventory admission/replay or elapsed time. Source JSON/native Arrow/financial validation memory is not empirically bounded here; separate guard approval and measurements remain necessary. Repeated controller slots are not assumed.

Independent semantic checks use SQL EXCEPT ALL and exact native schemas, retain nullable native fiscal_period and pre-event history warmup, compare actual source CIK/accession support and filing availability, preserve all failure counters, and never use recorded PASS/counters as proof. Deterministic gate evidence removes cutoff wall time and normalizes native tuple stats to JSON to avoid false mismatches. Publication checks unchanged outer manifest before pointer write and rechecks payload/provenance after full validation; rollback/recovery reject changed or incomplete targets. Hard-kill locks require explicit controller inspection; no automated lock deletion or old-generation deletion is provided.

Final-generation CLI/native financial checker integration has not been exercised on actual data. Authentic final membership/economic qualifiers, external admission evidence, complete sideproduct lineage and real gates remain dependencies. Existing source files, preparation cache and Task5 outputs remain untouched. Full Task16 universe component remains separately reviewed/owned and is not claimed complete by this dependency checkpoint.

## Owned files and handoff

Only: new fund_batch_release.py; test_fund_batch_release.py; fundamentals_release_v1.md/.registry.json/.contract.json; this separate Task16 fund release report. No live exec job or subprocess remains at handoff. Required next step is independent review of this initial component before a controller-exclusive actual-source run; no actual build or consumer use is authorized by this report.
