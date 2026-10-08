# Golden research panel native contract

This original Task16 delivery uses the separate `golden_panel` stage. It does
not modify the foreign alpha-admission panel or frozen source producers.
No actual golden generation has been built or qualified by this delivery.

`golden_panel.build(plan_path, stage, generation_id, max_new_units=None)`
materializes bounded daily/year and monthly/month units, freezes the complete
independently reviewed binding plan, and publishes `current.json` only after
complete public replay. `validate_generation` repeats native admission, all
unit inventories/receipts, every cell including NULLs, unique keys, source-clock
checks and exact all_common population comparison. Its deterministic 200-cell
coordinates are supplemental to complete native cell replay, which also
covers event/amendment/restatement outcomes. Changed sources or code reject a
restart. `rollback` validates the selected generation before changing the
pointer. `load_golden(start, end, columns, universe, stage=stage)` validates the
immutable generation then yields bounded batches; allowed universes are
member_equity, univ_all_common and univ_r3000p.

Daily output starts 2020-01-02; monthly output starts 2020-01. Inputs retain
warmup and complete native vintage history. Daily GC1 uses 22:00 UTC of the
preceding calendar day and strict `<`; monthly uses month-end 22:00 UTC and
strict original source clocks. A native monthly publication can first be used
at its publication clock only where its own underlying clocks are already
strictly earlier. Later historical return predictors remain permitted by R21.
Labels and statistics retain the independent pre2023 seal.

`golden_field_registry.json` contains every field's formula, units, source,
primitive lineage, clock, structural NULL and history requirements. `std_`
names retain the exact Compustat mnemonic in source_columns/native_name;
`ratio_` names preserve unchanged native WFRS formulas, including composites.
`daily_` names distinguish the separate daily registry from monthly JKP
features. Raw fields, normalized `score_` fields, controls and per-field
`_source_at`, `_quality`, `_qualified` columns remain distinct. Normalized
scores use average tie ranks of at least 100 source-qualified all_common
names, with frozen economic directions and the maximum participating clock.
They never normalize unknown or NULL values to zero. Coverage is recorded for
each field/year and all three universe denominators.

Bindings have schema `atx.golden-bindings/v1`, independent_review, exact
code pins and registry SHA, plus fixed native source specs. Universe admission
calls `universe_release.validate_generation`; final fundamentals call the
unchanged full `fund_batch_release.validate_generation` through an independent
R23 256MB runtime/proof connection. Monthly characteristics call
`char_monthly.validate_generation`, `char_monthly_sources.admit` and replay
each `native_month`. Guidance calls `guidance_native.validate` and
`guidance_publication.validate`, binds the complete native source identity,
pins retained original documents/independently selected receipt inventories,
and rejects source/code changes. Review of the Task10 R1 code does not close
its seven source/precision gates or manual 0/300 evidence.

The four accounting/earnings adapters delegate only to separately reviewed
`tier1_native_readers`, VERSION `tier1-native-readers-v1`; names are
standardized_v11, ratios_v11, ea_actuals and earnings_calendar_v2. The native
ratio context holds `ratio_cells(spec, con, session_sql, cutoff)` open through
materialization/replay. Each source-close ratio is consumed strictly after its
source clock rather than backdating a same-close mark. Final qualified fund
lineage is mandatory; no legacy standardized/fundamental stage fallback exists.
Those new reader modules require independent review before actual admission.

Unsupported family readers remain explicit exclusions and physically NULL
fields, never generic Parquet admission. R12 notes enforce only the nine
reviewed curated source identities, original period/proof pins and strict
clocks; automatic list proofs, external selections, unverified geographic
sales and ambiguous tax_dtl are excluded. HHI additionally requires original
compatible revenue contexts and bridge arithmetic. The committed curated
inventory leaves unprovided independent period/context proof identities NULL.

Operational replay can pass while source admission, coverage, predictive and
consumer gates remain FAIL. A diagnostic value is not economic certification.
Actual release/source scans and publications belong exclusively to root after
independent review; these modules perform no import-time warehouse work.
