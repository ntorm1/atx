# FQ3 independent static review

Scope: one pass over the FQ3 brief, current readiness script, isolated tests, operator note, and FQ1/FQ2 table and writer contracts. No tests, warehouse connection, or runtime checks were performed. Codebase graph tools were unavailable, so discovery used `rg` and source reads.

## Important

1. **A future build can make an older evaluation's candidate summaries appear linked.** `atx-db/scripts/measure_tier1_readiness.py:574-590` accepts a linked build when its `as_of_date` is no later than the *report* date; it does not require `build_as_of_date <= e.as_of_date`. For example, an evaluation dated September 19 that links to a build dated September 20 will pass on a September 20 report if the recorded hashes agree, and lines 590-605 will expose its candidate rows. The FQ2 writer explicitly rejects this chronology (`atx-db/src/atx_db/fundamental_signal_evaluation.py:332-337`). Check the build date against the selected evaluation date as well as the report snapshot, and add a fixture for the mismatch.

2. **Missing FQ1 schema is reported as absent FQ2 evaluation evidence.** `atx-db/scripts/measure_tier1_readiness.py:561-571` skips selection entirely when `build_gap` is present, and lines 606-607 then state that no complete evaluation with an eligible build exists. A partial warehouse can contain complete FQ2 manifests while FQ1 columns are missing; the current wording asserts absence from a query that was never run, and hides the selected FQ2 run's date and link diagnostics. Preserve the FQ2 inventory, but distinguish `linkage_unmeasured` because FQ1 schema is missing from a genuinely absent complete evaluation. This is part of the brief's requirement to report missing FQ1/FQ2 surfaces separately.

## Minor

1. `atx-db/tests/test_fundamental_signal_readiness.py:72-99` tests a later hash mismatch, but does not test the complete evaluation with a future-dated linked build, missing FQ1 schema with existing FQ2 manifests, or blocked-only inventories. The existing test at lines 101-121 covers coverage truncation, not run-inventory truncation. Add focused fixtures when fixing the Important findings.

## Checks with no finding

- All new variable-size manifest, coverage, and summary results pass through `Measurement.rows()` (`atx-db/scripts/measure_tier1_readiness.py:102-108`), retaining caps and truncation flags. The selected-run lookups are SQL-limited to one row. The new queries do not scan values, inputs, proofs, labels, or deciles.
- The feature is opt-in; both report-wide and FQ section certification remain `unmeasured`. Candidate is named `recorded_research_candidate` and is suppressed when the recorded hash/link check fails. Selected FQ1 and FQ2 snapshot dates, FQ1 date range, and FQ2 linked build range are displayed.
- The new fields do not transfer arbitrary manifest JSON, blocker text, source URLs, or contact values. Unknown identifiers and statuses are reduced to fixed safe labels. Blocker count is only computed for JSON no longer than the configured detail limit.
- `markdown(report)` indexes the new key at `atx-db/scripts/measure_tier1_readiness.py:635-636`. Its only in-repository caller is `main()` at line 722, whose report always supplies that key at lines 704-705; the new isolated test also supplies it. I found no current caller regression. If `markdown()` is intended as an external API accepting older report dictionaries, a default lookup would preserve that use, but the repository gives no evidence of such a contract.
