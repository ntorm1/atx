# Daily risk bounded writer

## Implementation

- Replaced production `refresh_equity_price_metrics`' full-universe pandas load with a DuckDB SQL window pipeline. The pure `compute_equity_price_metrics` transform remains the small-input reference.
- Added optional `as_of_date`; eligible inputs must have both `trade_date` and `available_at` no later than the cutoff date's end. This supports operational backfills from recorded observations and makes no certified historical-vintage claim.
- Corrected SQL `pct_from_high_252d` to use a 252-row trailing high while retaining the expanding peak for drawdown and availability propagation.
- Preserved adjusted-price return endpoints, raw-dollar liquidity, existing scoped calculation-universe market peers/ranks, rolling history, late input clocks, empty-source replacement, and stable IDs.
- Added `equity_price_metrics` directly before activation `quality`, reporting source, explicit cutoff/run ID, row count, security count, date range, and availability watermark.

## Publication design and remaining limitation

The initial full staging-table plus indexed target delete/insert design was rejected.
The approved replacement uses migration 0314's primary-key-only physical target,
a persistent disk-backed stage, and a shadow whose DDL reproduces the table's primary
key, nullability, and defaults. It inserts the retained non-source rows and replacement
rows into that shadow in 256 contiguous SHA-256 metric-ID prefix ranges, ordered by key,
with checkpoints/reopens only between committed batch groups. The shared publisher then
renames the validated shadow inside one short transaction. A build failure leaves the
live table intact; the stage/shadow are retained for inspection. This avoids secondary
ART maintenance and bounds working memory per primary-key batch. The shared publication
path subsequently completed the guarded 1GB production refresh over 31,959,271
corrected bars through 2026-09-18.

The combined atomic-publication review closed the prior Critical architecture finding.
Its three implementation findings were repaired: the shadow DDL now follows the
migrated physical column order, the SQL emits the distinct propagated
`metrics_available_at` clock as the published availability value, and the
failure-preservation test injects shadow construction failure only after creating a
valid retained foreign row.

## Validation

- The earlier fixture-bootstrap failure used unsupported system Python with DuckDB
  1.5.1, rather than the locked project virtual environment (DuckDB 1.5.5), and is
  invalid as production evidence. The unchanged HEAD bootstrap passed under locked
  1.5.5.
- Root's guarded locked-venv focused batch passed the six daily-risk checks and the
  shared API snapshot. The initial five-plus-snapshot batch peaked at 0.708GiB
  (`daily-risk-focused-final.log`); the final parity rerun passed at 0.693GiB
  (`daily-risk-parity-squared-memory.json`, with matching `.log` and `.err`). The
  parity assertion normalizes only Python `date`/DuckDB `DATE` representation and
  compares squared idiosyncratic volatility at 3e-18 residual-variance tolerance for
  observed square-root cancellation; formulas, null handling, all other numerical
  tolerances, and availability-clock assertions remain intact.
- Locked-venv static compilation and `git diff --check` pass. Root owns all guarded
  executions; no production database scan or write was run by this task.

## Ownership release

Owned files are `atx-db/src/atx_db/equity_price_metrics.py`, `atx-db/src/atx_db/activation.py`, `atx-db/tests/test_equity_price_metrics.py`, and the narrow ordering assertion in `atx-db/tests/test_activation_ladder.py`. The shared publisher and migration 0314 are owned by the price-publication task. No jobs or registry edits were made. Commit: pending below.
