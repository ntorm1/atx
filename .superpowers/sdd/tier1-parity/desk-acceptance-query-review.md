# Desk acceptance-query review

Scope: static review of the prepared CF1 decile readout, its evaluator and
schema, and the committed CVX acceptance SQL, recorded JSON result, runner, and
desk documents. No database was opened and no query was executed. The recorded
CVX result remains accurate for its observed state: Q2 is `+321.3793%`, Q1 is
`-44.5%`, Q4 has neither raw input, and all three published values are NULL.

## Critical

No critical finding.

## Important

### 1. Published-EPS status is incorrectly gated by raw Company Facts presence

Evidence: `quarterly-eps-acceptance.sql` deliberately allows a published Q4
metric to be found when a direct Company Facts Q4 row is absent
(`eligible_cik_owner_ids`, lines 152-155).  However, the
`published_metric_status` CASE at lines 262-275 first returns
`not_evaluable_raw_input_missing` whenever either raw input is NULL, before it
examines `derived_value_id`, `value_status`, or the published value. A future
generic earnings-release repair can therefore correctly publish a valid
quarterly YoY state while this acceptance query reports it as not evaluable.
That conflates the independent questions of raw comparison availability and
published-metric validity.

Remedy: retain `raw_input_status` and add a separate raw-comparison/reconciliation
status, but make `published_metric_status` depend only on the selected derived
state: missing, unavailable/invalid, NULL, or valid. Preserve the existing
executed SQL and JSON under SHA-256
`fe886f41543f7bf42261dec61096a6935f2c608ee7e447e1ca6379b90c7af262`; publish a
versioned successor query and result for this correction. The present three-row
report need not change because its derived states are all absent.

### 2. CF1 readout does not pin the named feature and price sources

Evidence: the `requested` CTE in
`atx-db/sql/research/custom-feature-decile-acceptance.sql` pins run IDs,
version, source SHA-256, snapshot, cutoff, and horizon (lines 6-15), but has no
expected `feature_source` or `price_source`. The manifest checks only require
the evaluation source names to equal the build source names (lines 50-53).
Thus a build and evaluation from the same unintended named sources, carrying
the expected hash/version/date, can be reported as an ordinary research result
rather than a manifest mismatch. This falls short of the stated source pin and
weakens the readout's lineage claim.

Remedy: add expected `feature_source = 'atx_custom_price_liquidity_v1'` and
`price_source = 'tbltickerhistory3_10y'` fields to `requested`; require both
the build and evaluation manifests to equal them, and return their actual
values in the evidence columns. Keep the existing 8-by-3 outer expected grid
and its absent-result behavior unchanged.

## Minor

No material minor finding. The CF1 query's 24-row outer grid, NULL/absent
preservation, primary-horizon/evaluation manifest checks, and diagnostics
columns agree with the evaluator and migration schema. The CVX query ranks
visible derived states before inspecting nullable values, and the runner's
read-only connection, output budget, and immutable evidence output are
consistent with the recorded receipt.
