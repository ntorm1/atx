# Daily risk bounded writer — independent source review

Reviewed 2026-09-20. One-pass source review only; no implementation edits, test reruns, or database operations.

## Critical

1. **The production publication architecture remains an unbounded full-table replacement and is not credible at 32M rows under the fixed 1GB DuckDB / 3GiB process-tree budget.** `CREATE OR REPLACE TEMP TABLE eqpm_refresh_stage AS ...` materializes every output row with the full 34-column width before publication (`equity_price_metrics.py:298-430`). Publication then retains that complete staging table and the preceding successful target while inserting another complete copy into `equity_price_metrics` (`:455-479`). The target has three ART indexes (`bodies_0001_0137.py:2985-2990`), so the insert must also maintain large index state; describing window spill does not bound the materialized stage, replacement storage, WAL, or ART construction. This is the same large replacement/index footprint that already failed for the other price-table publisher at both 1GB and 2GB, now behind a SQL wrapper. Replace this with a physically bounded design whose peak working set is independent of the 32M-row output (for example, sequential bounded derivation into a disk-backed shadow artifact, with publication that avoids maintaining the old indexed table plus stage plus indexed replacement concurrently). Root should obtain the guarded production-scale receipt at the mandated limits after accepting the repaired architecture; that receipt is production validation, not a prerequisite for committing the reviewed code repair. Do not activate this stage until that validation succeeds.

## Important

1. **`pct_from_high_252d` is not formula-equivalent to the pure transform.** SQL computes `running_high` with `ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW` (`equity_price_metrics.py:322-327`) and publishes it as `pct_from_high_252d` (`:423`), while the reference uses `adj.rolling(252, min_periods=1).max()`. Results diverge after the historical peak ages past 252 rows. Add a distinct 252-row high window for this column while retaining the expanding peak required by drawdown/availability semantics. Extend parity data beyond 252 observations with an early high that leaves the window; the current 130-row test cannot detect the defect.

2. **The as-of test does not prove availability eligibility before calculation.** It constructs all retained bars with `available_at` no later than the day-125 cutoff; the sole delayed bar is day 80 plus three days, so no row is excluded by the availability predicate. Its expected frame is also produced by computing all bars and filtering only on `trade_date`, which would encode the wrong expectation if a late-known pre-cutoff bar were present. Add a pre-cutoff trade-date bar whose `available_at` is after the cutoff and compare SQL against the pure transform run on inputs filtered by both clocks before any lag, peer, rolling, or rank calculation.

## File hashes reviewed

- `atx-db/src/atx_db/equity_price_metrics.py`: `64FD2EE91779B0005F07CF05BE7A89EAAB29C1251B936379FE2EA7FDFB1A3F01`
- `atx-db/src/atx_db/activation.py`: `28C9CBA792A20E9DCB0E2FAB6A472FF6AF0BA49CD6C0B2238EDF1A303FC9E864`
- `atx-db/tests/test_equity_price_metrics.py`: `1ADBEF714BCB7F82F70BF9E19B58B65BBC36460ACAEEDD0817EBFBEE18037829`
