# CF1 implementation report

Status: code complete; independent source review `ab65da79` reports no Critical or
Important findings. Six custom-feature cases, seven module-boundary checks and
two migration-relevant schema checks passed in the controller-authorized guarded
slot. No live source or warehouse reads/writes were performed by
this task. Migration 0313 was registered after controller ownership transfer;
only its own body/import lines and the custom_features public module line changed.

## Delivered surfaces

- `atx_db.custom_features.CustomFeatureOptions` and
  `refresh_custom_features(store, options) -> dict`: sequential SQL partitions,
  one source/version-scoped atomic replacement, wide daily rows and eight pinned
  immutable definitions. Inputs are corrected `equity_daily_bars`; source digest
  is explicit operator-supplied provenance, not an independently verified hash.
- `CustomEvaluationOptions` and `evaluate_custom_features(store, options) -> dict`:
  daily deciles before label joins, date exclusion/coverage/terminal diagnostics,
  72 fixed feature/horizon/split summary rows, explicit run manifests. Missing
  required source horizons cause a prerequisite error. Old build snapshots that
  have been replaced or whose observed source calendar changed cannot be evaluated.
- Migration body0313 creates `custom_features_daily`,
  `custom_feature_definitions`, `custom_feature_runs`, `custom_feature_deciles`,
  and `custom_feature_evaluations` with catalog/PIT research snapshot metadata.
- `scripts/build_custom_features.py` exposes explicit `build` and `evaluate`
  commands. It opens an existing database without auto-migration, checks the full
  pending migration set, and uses one thread/1GB by default.
- `docs/CUSTOM_FEATURE_RESEARCH.md` documents exact formulas, source limitations,
  cost accounting, methods, resource limits and guarded commands.

## Research contract

The eight definitions are five-session reversal,126-session momentum excluding
the latest21, volatility-scaled63-session momentum,5-vs63 dollar-volume shock,
21-session close-location pressure,5-vs63 intraday-range compression,
liquidity-conditioned reversal, and compression-times-accumulation. Each has a
fixed positive direction, minimum observation requirements and finite/domain
guards; exact formulas are persisted in definition JSON and the public document.

At T22UTC, selected data end on the previous observed market session. Entry is
the next observed market session's close. Observation availability is floored at
noon UTC on the following calendar day and maxed with stored clocks and across
the127-session input window. Late/unknown clocks exclude formation eligibility.
This is a modeled backfill contract and not certified historical delivery.

Controller corrected the initial proposed20/60 horizons **before any outcomes
were inspected** to match the existing production label contract: primary21,
secondary5/63, embargo63 market sessions. No AR1b producer/global IC constants
were changed. Train<=2020, validation2021–2023, holdout>=2024; purge labels crossing
split boundaries. Labels join at entry with exact calendar endpoints; newest
eligible revision precedes validity checks. Policy terminals are explicitly
counted and missing/invalid labels cannot change prior feature ranks.

The cohort is transparently named bar-observed price>=5 and ADV63>=1m, requiring
>=50 observations. It is not certified historical US-common membership. Python
receives only aggregated spread series. Daily-calendar Bartlett/Newey–West mean
SE uses horizon-1 lags and preserves gaps; p-values use the asymptotic normal
approximation. Holm retains all eight primary hypotheses within each split.
Degenerate/insufficient results remain null; unsuccessful features remain stored.

The reported return is a Q10-Q1 horizon spread, not daily PnL or trading Sharpe.
10/25/50bp per-side cost sensitivities subtract four times the assumed cost (two
legs, entry and exit). Production eligibility remains false until actual
historical listing, source adjustments/identity/vintages, terminal completeness
and execution prerequisites are certified. Statistical/economic screens remain
separate; no significance or profitability is claimed without a measured run.

## Resources and validation

The wide daily table deliberately has no large composite ART index. Approximate
full-source output is on the order of32 million rows minus final two sessions and
excluded duplicate keys. SQL output is staged, partitions execute sequentially,
and atomic publication is source/version-scoped. Daily deciles are at most about
0.9 million aggregate rows for14 years, plus72 summaries. Full-universe performance
and actual live results remain unmeasured.

Completed static validation:

- `python -m ruff check src/atx_db/custom_features.py src/atx_db/migrations/bodies_0313.py scripts/build_custom_features.py tests/test_custom_features.py`: passed.
- `python -m mypy --follow-imports=silent src/atx_db/custom_features.py src/atx_db/migrations/bodies_0313.py scripts/build_custom_features.py`: passed3 source files.

Passed focused package: `tests/test_custom_features.py`, six compact cases for
causal calendar windows/entry clocks, missing sessions, scoped idempotence and
publication rollback, ranks before label loss and invalid revisions, split
purge/embargo/constant dates, known synthetic HAC/complete-family Holm, and missing
label horizon prerequisite failure. Uses only minimal DuckDB schemas,128MB and
one thread. The controller authorized a guarded2GiB process job in the natural
production gap. Six feature cases plus seven module-boundary checks passed;
native peak job memory 0.624 GiB, receipt `cf1-focused-memory.json`. The two
migration-relevant schema-v2 checks also passed against an initialized warehouse,
native peak job memory 0.944 GiB, receipt `cf1-schema-memory.json`. Fifteen checks
passed in total, with no failures or reruns. Final Ruff also included both edited
migration registry files and passed. No full-suite or large synthetic benchmark run.

Commands actually run:

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 2 --receipt ../.superpowers/sdd/tier1-parity/cf1-focused-memory.json -- .venv/Scripts/python.exe -m pytest -n0 -q tests/test_custom_features.py tests/test_module_boundaries.py
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 2 --receipt ../.superpowers/sdd/tier1-parity/cf1-schema-memory.json -- .venv/Scripts/python.exe -m pytest -n0 -q tests/test_schema_contract_v2.py -k 'bootstrapped_warehouse_has_zero_pit_column_presence_after_s2_0 or schema_contract_version_table_pins_v2_manifest_hash_and_is_catalogued'
```
