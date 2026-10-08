# Alpha panel validation and reproducible build

The research build generates gold feature panels for 2020-2026, keeps admission independent of later outcomes,
and measures the frozen scores at 5, 21 and 63 exchange-session horizons. It publishes evidence and source
limitations; a positive discovery IC does not certify a tradable strategy.

Admission uses fixed registry signs, 2019 confirmation and purged 2020-2021 selection. A selection decision at
session d qualifies for horizon h only when its entire d+2 through d+1+h label ends before 2022. Redundancy uses
selection month ends only. The 2022-2023 validation window does not choose signs, horizons, variants or weights.
Future feature coverage is diagnostic and does not decide historical admission.

The owner explicitly authorized measurement of frozen features on 2024-2026. This occurs in a separate final
phase, after `gold/frozen_recipe.json`, the physical gold/source files, selection evidence and evaluation code
are pinned in an immutable freeze receipt. Every frozen `g_*` score and fixed `c_*` composite is measured at all
three predeclared horizons. There is no OOS ranking, admission, horizon choice, sign fitting or weight fitting.
The separate OOS files cannot be consumed by the gold admission step. Native consumer roles retain their
existing pre-2024 loader seal; gold Parquet panels retain all feature years.

Strict labels require an observed, tradable d+1 closing anchor and all h observed returns, or a complete
observed terminal payoff. They exclude partial windows, unknown terminal proceeds, Shumway-imputed proceeds,
and repaired identities at the decision, anchor or any forward-return session. Reason codes and NULL censored
tail rows remain available. Unknown terminal outcomes make IC conditional on observed outcomes, so survivor
bias remains possible. The pipeline also measures legacy-label sensitivity at the admitted choices frozen
from strict selection; this sensitivity never changes admission.

Daily Spearman IC uses at least 100 eligible names. Reports include mean names, usable and eligible sessions,
calendar-year coverage, industry and risk variants, and Newey-West confidence intervals on actual exchange
session distances. Missing sessions are not compressed. The HAC lag is h, with lag 5 for the diagnostic
one-session horizon. The discovery scorecard uses approximate normal-HAC Holm corrections across frozen
feature choices. Inspect OOS results across all horizons and years; 2026 is incomplete and long horizons end
earlier because unavailable outcomes are censored.

Run from `C:/atx/atx-db` after the registered silver refresh has published its complete, current manifest:

```powershell
.venv/Scripts/python.exe scripts/build_gold_alpha_panels.py --skip-silver --years 2020-2026 --evaluate-frozen-oos --dry-run

& 'C:/Program Files/Python312/python.exe' ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 1.0 --wait-minutes 30 --quiet --receipt data/alpha_panel/v1/_logs/gold_pipeline_guard.json -- .venv/Scripts/python.exe scripts/build_gold_alpha_panels.py --skip-silver --years 2020-2026 --evaluate-frozen-oos
```

The second command is for the controller after source completion and code review. No real OOS measurement was
run during implementation. `--skip-silver` verifies publication status, all physical file hashes, complete
exchange-session coverage, PIT identity tiers and the current frozen source signature. It refuses an active
silver scratch writer. The pipeline runs one child process at a time, pins each step's inputs and transitive
code, and resumes only when its artifacts remain unchanged. A failed or interrupted operation leaves a
durable log and failure receipt. Export replacement uses a completed temporary directory and removes the
superseded pipeline-owned output after publication.

Important outputs under the build root:

- `characteristics/manifest.json`: raw prior-signed features, coverage and repair quarantine.
- `labels_goldready/manifest.json`: strict development outcomes ending by 2023-12-29 and quality attrition.
- `validation/gold_pipeline/characteristics_strict_ic.json`: purged selection, validation and yearly IC.
- `gold/selection.json`, `gold/frozen_recipe.json`, `gold/manifest.json`: admitted candidates and exact recipes.
- `validation/gold_pipeline/selected_legacy_ic.json`: sensitivity at strict-frozen admitted choices.
- `export/gold-qualified-native/role/` and `gold-fields/`: a sealed 2020-2023 market role and aligned gold fields.
- `validation/gold_pipeline/gold_freeze.json`: immutable feature, recipe, evidence, source and code pins.
- `labels_holdout/manifest.json`: explicitly authorized strict OOS labels and attrition through the data ceiling.
- `validation/gold_pipeline/frozen_oos_ic.json` and `frozen_oos_scorecard.md`: frozen 2024/2025/2026 and aggregate evidence.
- `validation/gold_pipeline/quality.json` and `orchestration.json`: overall status, consumer acceptance and step receipts.

Vendor historical price/IV vintages and lagged vendor shares remain uncertified. Identity repair quarantine
removes known future-bracket contamination; it does not certify historical vendor vintages. Any later promoted
bronze tail must receive a fresh source build and reviewed freeze; the existing recipe and OOS evidence remain
immutable.
