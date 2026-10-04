# Independent fundamentals release foundation (R15)

Engineering checkpoint only. No actual source snapshot or generation has been admitted. This component remains `NOT_PRODUCTION_READY`, `source_quality_certified=false`; whole Task16, Task4, economic qualification and actual financial acceptance remain open. Legacy Task5 batch outputs, including the controller's two operational batches, are not importable. A matching receipt created after a legacy computation cannot establish its provenance.

`atx_db.alpha_panel.fund_batch_release` owns new-only candidates under `<root>/fundamentals_release_v1/generations/<generation>/fundamentals_v11/`. Both the logical producer name and physical leaf remain exactly `fundamentals_v11`. Native `fund_items.issuer_events`, validation cutoff rebuild, and standardized derivation select their source-backed branches using these names. No environment, imported module, shared common root, or legacy output is mutated. The native `run_batches`, `_batch_done`, `finalize`, validation `run`, and shared stage-manifest writer are not used.

## Consumer interface

`resolve_current(stage: Path) -> Path` checks the current pointer's manifest SHA/size and refuses an unfinished pointer transaction. It is **pointer resolution only**. Consumers must subsequently call `validate_generation(stage: Path, generation: Path, con)` and retain its outer manifest identity and exact output SHA/size. `stage` is `<root>/fundamentals_release_v1`; the generation must be directly below its `generations/` directory. No default or legacy fallback is admitted.

The returned manifest uses schema `atx.fundamentals-release/v1`, `status=complete`, `semantic_producer=fundamentals_v11`, `source_backed_only=true`, explicit snapshot/code/preparation/reader lineage, all 85 admission receipts, the four native output identities, native leaf manifest and validation identities, and original gate statuses. `complete` concerns the required unit inventory and admission, not production or economic acceptance. The leaf's `manifest.json` is mandatory and carries native schema/rules, source-backed overrides, exact files and release lineage. This preserves `compustat_std._manifest_sha` and `Inputs.fund.name` behavior. The controller must pass the explicit root-relative generation path ending in `/fundamentals_v11` to the standardized build and rebuild `ratios_v11` from that exact standardized output. Those builds and final consumer integration remain held.

## Binding contract and actual readers

The supplied JSON has `version=fund-batch-release-v1`, `semantic_producer=fundamentals_v11`, and absolute `root`, `prep_root`, `extract_root`, `fsds_root`, `archive: {path, sha256, bytes}`, plus an exact `products` dictionary. A non-null product contains `source`, `clock`, `admission` (nonempty provenance descriptions) and `files` mapping each canonical absolute path to `{sha256, bytes}`. These descriptions are lineage, not certification: they never establish economic quality, PIT correctness, or controller approval. Their actual bytes and reader membership are independently checked. A final controller binding must retain external admission evidence; this foundation does not manufacture it.

The exact native readers are:

| Product | Exact selection |
|---|---|
| links_prepare | root/identity/links.parquet |
| links_validation | root/identity/link_table.parquet |
| calendar | root/calendar.parquet |
| statement_seed | native statement_map_seed.STATEMENT_MAP_SEED_PATH |
| fx | one explicitly selected FX file, or explicit null only when the native default is absent |
| eight_k | root/sec_filings/eight_k_items.parquet, or explicit null only when absent |
| prices | all root/prices/year=*/*.parquet, nonempty exact inventory |
| prices_history | all root/prices_history/year=*/prices.parquet, nonempty exact inventory |
| members | all six root/_tmp/panel_member/year=2020..2025.parquet |
| identity_manifest | exact Stage I identity-links-v1 outputs/sources adapter; links output and all three native source pins rechecked |
| filings_manifest / reference_manifest | exact native manifest plus every declared file rehashed |

Native preparation verification preserves the existing 144 units: exact FSDS source inventory/full SHA, actual preparation code, dependencies, SQL and output SHA are verified through the reviewed `fund_prepare_cache` machinery before and after work. Missing, abbreviated or changed preparation fails. Source archive membership is replanned using native ZIP metadata; all original 85 batches and all 256 extraction files (plan plus data/receipt/statistics for each batch) are mandatory. Full admission independently reconstructs both extraction Parquets directly from the pinned archive and compares typed values, all member/error populations and native counters. This avoids blessing a legacy extraction receipt based on size/mtime or rewritten SHA alone.

The identity adapter accepts only stage `I`, code_version `identity-links-v1`, rule `r4-links-asof-v1`, exactly `outputs['links.parquet']`, and exactly sources `security_company_links`, `security_permanent_ids`, `r4_manifest` at current native producer paths. Output absolute path, SHA/size/mtime and Parquet row count must match the declaration and independently pinned preparation reader. Every declared source path, SHA/size/mtime is checked. Independently selected `identity/link_table.parquet` remains mandatory; the Stage I outputs declaration does not cover or substitute it. Unknown schemas, empty/subset/extra outputs or sources, content/path/count changes and reader mismatch fail. Producer validation/clock descriptions do not certify source clocks or economics; qualification remains explicit and false. Generic `outputs` fallback is forbidden.

Actual code inventory includes the native seven `fundamentals.MODULES`, `fund_validate`, `fund_asof`, this adapter, `identity_links.py` (known source path/schema contract), `reference_classifications.py`, `statement_map_seed.py`, and the frozen contract JSON. Any source, selection, preparation, code, configuration or binding-file change invalidates continuation.

## Units, types, clocks and population

Each batch gets a new UUID attempt and a start receipt written before calculation. Native calculation writes isolated `events`, `sic`, `history`, `catalog` parts and its own receipt. Admission requires an exact native schema, nonnull typed source keys, unique keys, actual source CIK/accession population, valid available/filing clocks, event/catalog population agreement, and full native replay equality of all four Parquets and counters. Timing fields are excluded from semantic receipt equality. No `_batch_done` or existence-only skip is consulted. Interrupted unsealed attempts are retained and recomputed into a new attempt; an admitted unit is immutable and rechecked rather than overwritten.

Event/catalog/SIC keys are `(cik, accession)`; history keys retain native `(cik,item,period_end,fiscal_period,accession)` including genuinely nullable fiscal_period. History warmup before the event emission floor is preserved. Exact schemas and replay prevent changing the availability clock, inventing a zero, changing currency, inserting a source-unsupported CIK/accession, shrinking missingness, or editing a value while preserving a valid footer. Final outputs merge explicit admitted file lists; cross-batch duplicate keys and exact native final schemas are checked, then final Parquets are independently compared to the admitted-unit merge including 8-K ASOF nonreliance clocks and native classifications.

## Native final gates and reader/writer isolation

The owned orchestration supplies the explicit leaf to `fund_validate.coverage`, `benchmark`, `balance`, `cutoff`, `fx_check`, `ttm_gaps`, `exit_measure`, native `_stats`, and `coverage_by_year`. Private function globals bind the actual root, FSDS, calendar, optional FX/8-K, prepared getters and explicit price inventory. `fund_asof` is called through the native cutoff checker with the explicit candidate. Native writer classes retain only their native schema/formulas; their destination is supplied by the isolated batch call. No broad producer or validator copy is introduced.

Original gates remain: FY2015–2025 item coverage, top3000 observations, 90% target and 105 minimum/110 stretch items; 10000 stratified FSDS cells with 0.5% tolerance and native 95% mapped/98% match criteria; balance coverage 98.5% plus the native 99% diagnostic; exactly 200 seeded cutoff events with zero differences; full six-year 2020–2025 exit measurement and native field targets. Each required year/member denominator must be present and nonempty. Coverage retains native full member-all and linked/USD views; no missing year, zero sample, absent field or structural exclusion permits silent gate shrinkage. Cutoff wall time is not part of deterministic evidence, and native tuple stats are normalized to persisted JSON. An empty required population causes gate failure or checker failure, never admission by an empty PASS. Whole-dataset, Task4 and economic statuses stay failed.

The helper reads financial levels after2022 for the original financial coverage checks; it does not compute or evaluate return statistics. This component grants no exception to strict pre2023 return-product constraints.

Writers are confined to new generation attempts, leaf outputs/manifests, snapshot/admission/validation evidence, generation-local scratch reconstruction directories under root/_tmp/fund-release-{verify,extract-verify}, stage/_spill, and stage current/previous/pointer-transaction files. Single-writer exclusive creation retains a lock after hard process termination for controller inspection. Publication is immutable-generation, manifest-last then journaled pointer-last. `rollback` and `recover` hold the writer lock. Recovery fully admits the intended current generation and verifies both pointers' rooted generation locations and manifest identities before and after that admission. The retained previous generation may legitimately have stale sources or code after a refresh; recovery preserves its pinned identity without admitting it for use. Actually rolling back still requires full admission of that previous generation against current sources/code and refuses a stale or corrupt target. Resolution refuses an interrupted journal; recovery completes it after these checks. Old generations are not deleted.

## Cost, CLI and limitations

Offline contract: `python -m atx_db.alpha_panel.fund_batch_release contract`. Runtime commands `build --bindings <explicit-json> --generation <new-id> --max-new-batches N`, `validate --root <explicit-root>`, `rollback`, and `recover` require `ATX_GUARD_JOB` plus a separate controller-granted exclusive slot; that environment marker alone is not user authorization. Runtime DuckDB uses300MB, one thread, isolated disk spill. No actual job is authorized by this checkpoint.

`max-new-batches` bounds new unit computation, **not admission cost or elapsed time**. Initial freeze hashes all readers/archive/extract/preparation and reconstructs all 85 extraction batches; continuation replays every admitted unit, and final public validation reconstructs extraction and all 85 native batches again before checking full financial gates. Source/code/preparation/output rehashing also occurs around computations and final validation. This intentionally expensive audit has no measured production runtime/memory bound. Native per-issuer JSON/Arrow and validator memory remain subject to the external <=1GiB guard; no real measurements justify success yet. Partial required inventories return `partial_cache_only` and cannot create a final manifest or publish a pointer. Full85/gate execution, authentic final sideproducts/admission, actual peak/time/disk, standardized/ratios rebuild and independent review remain outstanding.

The registry is a stub; Task17 shared registration is pending. Fixture checks validate engineering behavior only. No source was acquired and no actual source values or returns were read during this checkpoint.
