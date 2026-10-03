### Task 7: JKP-layout monthly characteristic library `char_monthly/`

Spec: gap-analysis §2 item 6, deliverable and coverage gate (binding). ≥ 150 JKP characteristics where inputs exist,
plus the gold T5 extras listed in the item. A machine-readable registry (name, JKP name, family, formula, inputs,
clock, staleness, sign convention). Do not edit `characteristics.py` (foreign). Files: new
`alpha_panel/char_monthly.py` (+ a registry module), tests. Report: `task-7-report.md`.

Owner extension: read `owner-coverage-addendum.md`; the registry and coverage reports must include the required profitability/growth families, their primitive dependencies, history sufficiency and quality/null reasons.

R15 explicit release interfaces supersede any implicit universe fallback. The
reviewed universe precursor publishes universe_release_v1/generations/<g>/year=YYYY/
universe.parquet; pointer checking alone is insufficient: require full
validate_generation and explicit manifest/file pins. Actual first-year build is
only starting, so no accepted final universe exists. Independent fundamentals
admission is being implemented in fund_batch_release.py, with proposed candidate
fundamentals_release_v1/generations/<g>/fundamentals_v11/ and semantic producer
fundamentals_v11. Preserve downstream standardized/ratio v11 identities and bind
their eventual actual source generation explicitly. Do not edit these adapters
or manufacture legacy fallback while their owning agents are active.

Release recovery binding (R14): use explicit `fundamentals_v11`, `compustat_std_v11`, and `ratios_v11` producer stage names and final actual-file identities. The preexisting unversioned publications are provisional references only. Task6 adapters are under separate ownership; do not edit them while their agent is active. Keep the monthly registry in a new private module if changing the old shared char_registry would affect the foreign characteristics pipeline. Implement native adapters, complete CLI, bounded/resumable atomic publication, validators and docs; an offline kernel or a registry of unimplemented formulas is only an intermediate checkpoint. Preserve the full requested field and coverage denominators, with unknown inputs and unsupported formulas explicitly excluded/missing. Freeze field definitions and trial/evaluation configuration before return-based comparison; no post2022 return evaluation or forward-label windows. No independent new HTTP or real-data job without controller slot/source-budget allocation.

Controller R10: Task4 has no published `characteristics/year=*/characteristics.parquet` comparator. Its reconstructed registry-formula correlation is diagnostic only. After publishing char_monthly, remeasure the binding same-date bm/ep/sp/gpa Spearman gate against the final ratios panel. Match formula, universe, currency, issuer market-cap definition and strict information cutoff; report keyed disagreement causes. Do not run/edit the foreign characteristics chain. Carry the actual comparison result back to Task4 acceptance and Task17. Unsupported fixed numeric ratio-tail bands are not literature evidence; unresolved reference gaps remain explicit.

