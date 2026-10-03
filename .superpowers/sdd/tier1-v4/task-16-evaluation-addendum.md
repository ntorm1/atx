# Task 16: Predictive evidence addendum (controller ruling R3)

Owner now prioritizes trustworthy golden panels with statistically significant IC and quantile returns at 5, 21 and 63 trading-session horizons. This supplements the original task's warehouse acceptance gates; those gates remain binding. Implement in new golden_evaluation.py (plus tests/docs), never foreign ic_eval/gold/labels modules.

## Freeze before measuring

Before first return-statistic run, persist evaluation-config.json in this workspace and commit its content/hash with the evaluation implementation. Record every subsequent run and configuration change; exploratory results do not become confirmatory by renaming them. Do not choose feature signs or delete failed candidates based on evaluation returns.

Primary candidate family and economic direction, chosen before this resumed session's evaluation: be_me (+), gp_at (+), ret_12_1 (+), ni_be (+), at_gr1 (-), oaccruals_at (-), rvol_21d (-), ivol_capm_252d (-), sue_eps_ann (+), guid_surprise (+). Use their registry definitions; missing/unsupported features are explicit non-testable outcomes, not filled or silently replaced. Evaluate the full available characteristic library separately as exploratory and disclose all tested candidates. No promise that this set passes significance.

Primary universe univ_all_common; univ_r3000p is a fixed robustness view, not a selection criterion. Main evidence period 2020-01-01 through 2022-12-31, split by year for stability. This history may have prior research/benchmark exposure; never call it untouched out-of-sample. Any tuning uses only a designated development subset recorded in config, with purging/embargo at least the maximum label horizon before remaining evaluation dates. 2023+ stays entirely excluded from all return statistics.

## Timing and computation

- Daily source features obey GC1 at the feature date; monthly features carry forward only after their documented monthly availability. Explicitly record source clocks and staleness.
- Use a reproducible next-session entry convention and 5/21/63 complete session holding periods. Document exact entry/exit/total-return and delisting conventions. Every return observation, including any benchmark or cost inputs, must end before 2023-01-01. Assert this in the evaluator before ranking, aggregation or inference, including boundary tests; filtering only feature dates is insufficient.
- Per date/horizon: Spearman IC; NYSE-breakpoint quintile forward returns with capped value weights using pre-entry ME; equal-weight sensitivity; signed top-minus-bottom spread, turnover and clearly labeled gross/cost-sensitivity results. Never assume absent delisting proceeds are zero. Publish return/label coverage and exclusions separately.
- Aggregate daily IC and spread with overlap-aware uncertainty. Primary inference: HAC/Newey-West with lag max(horizon-1,21); report means, standard errors, t statistics, two-sided p values, confidence intervals, and usable dates/names. Record minimum sample requirements before execution (at least 126 dates and 100 eligible names/date for primary inference). Block-resampling sensitivity may be added with fixed seed and blocks no shorter than 63 sessions if meaningful within memory limits.
- Correct the predeclared primary family across all candidate x horizon x endpoint (IC/spread) tests using Benjamini-Hochberg FDR at q=0.05; missing tests remain counted conservatively. Exploratory family has its own disclosed multiplicity correction. Do not report only winners.

## Honest acceptance

Report data-integrity, feature/label-coverage, and statistical outcomes separately. Predictive gate for each horizon: at least one predeclared candidate has direction-consistent IC and gross quantile spread with both primary adjusted q<=0.05. Report cost sensitivity and yearly sign stability alongside; do not convert gross significance into a claim of live net profitability. This gate can legitimately FAIL. Preserve the measured evidence, diagnose data issues, and distinguish engineering fixes from outcome-driven tuning. Any unachieved owner predictive objective remains explicitly open.

All real-data runs under <=1 GiB guard; bounded column/date scans. Tests use synthetic fixtures with known rankings, deliberate forward-clock leaks, horizon boundary crossings, delisting/missing labels, overlapping returns, null/tie handling and multiple-testing edge cases. Full reports are artifacts; controller receives concise gate summaries.
