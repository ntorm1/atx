### Task 7: JKP-layout monthly characteristic library `char_monthly/`

Spec: gap-analysis §2 item 6, deliverable and coverage gate (binding). ≥ 150 JKP characteristics where inputs exist,
plus the gold T5 extras listed in the item. A machine-readable registry (name, JKP name, family, formula, inputs,
clock, staleness, sign convention). Do not edit `characteristics.py` (foreign). Files: new
`alpha_panel/char_monthly.py` (+ a registry module), tests. Report: `task-7-report.md`.

Owner extension: read `owner-coverage-addendum.md`; the registry and coverage reports must include the required profitability/growth families, their primitive dependencies, history sufficiency and quality/null reasons.

Controller R10: Task4 has no published `characteristics/year=*/characteristics.parquet` comparator. Its reconstructed registry-formula correlation is diagnostic only. After publishing char_monthly, remeasure the binding same-date bm/ep/sp/gpa Spearman gate against the final ratios panel. Match formula, universe, currency, issuer market-cap definition and strict information cutoff; report keyed disagreement causes. Do not run/edit the foreign characteristics chain. Carry the actual comparison result back to Task4 acceptance and Task17. Unsupported fixed numeric ratio-tail bands are not literature evidence; unresolved reference gaps remain explicit.

