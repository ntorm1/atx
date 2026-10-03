### Task 13: Distributions + buybacks

Spec: gap-analysis §2 item 11 (binding). Files: `alpha_panel/{corporate_actions,events_buyback}.py`, new
`alpha_panel/distributions.py`, tests. Report: `task-13-report.md`.

Task4 downstream dependency: read final `atx-db/docs/datasets/ratios.requirements.json` and report. WRDS `divyield` needs an indicated dividend rate with source/class/currency/availability provenance; historical cash dividends paid are a separately named extension, not an equivalent input. Publish supported declared regular rate/cadence inputs when justified by sources, retain unknown cadence/special-dividend distinctions, and coordinate a ratios gate/handcheck refresh before gold/Task17. No future declaration or realized future payment may determine an earlier rate.

