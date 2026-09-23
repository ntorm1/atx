# Daily fundamental signal research panel

Migration 0324 adds a run-versioned, long-form warehouse panel. The builder
accepts an existing migrated DuckDB warehouse, an explicit bounded date range,
a UTC run timestamp and a unique run ID. It does not load source data, migrate a
database, join outcomes, evaluate alpha, or mark a signal production eligible.

```powershell
python scripts/research_fundamental_signals.py build --db-path PATH `
  --start-date 2024-01-02 --end-date 2024-03-29 --as-of-date 2024-04-01 `
  --run-at 2024-04-01T22:00:00+00:00 --run-id research_2024q1 `
  --memory-limit 256MB --threads 1
```

The default five specifications contain diluted EPS year-over-year growth,
year-over-year operating margin change, low total accruals, low net debt to
EBITDA, and a complete-case equal-weight percentile-rank combination. Negative
finite values remain valid. The combination ranks each input after applying
its declared direction, on the same complete-case cohort. Equal input values
get equal ranks and a constant cohort receives a neutral percentile of 0.5.

`--signals-json` accepts a JSON array of objects with `signal_id` and `terms`.
Each term has `metric_code`, `metric_window`, finite nonzero `weight`, and
`transform` (`identity` or `cs_rank`). Only the four reviewed dimensionless
metrics in the module allowlist are accepted. At most 32 signals and eight
terms per signal are allowed; duplicate IDs and duplicate terms are rejected.
The source identifiers and exact publisher definitions and hashes are pinned
before any panel rows are built.

For an observed decision session T, the cutoff is T at 22:00 UTC. Membership
must be a visible dated US common listing; overlapping visible intervals are
quarantined. Exactly one dated normalized CIK must be visible and must agree
with the membership CIK when present. Derived states are selected within
publisher buckets, then across fiscal periods, before status or hash checks.
The newest visible NULL invalidation therefore suppresses the previous numeric
revision. Only actual selected fiscal operands within the age limit may be
eligible. The next observed market session is the entry date. No forward
return labels are read by this builder.

Readers must join `fundamental_signal_runs` and require `status='complete'`.
Research evaluators call `validate_fundamental_signal_panel(con, run_id)` before
reading scores. This verifies the completed manifest, frozen definitions,
session calendar, row grains, score/input clock contracts and the exact shared
panel digest, all six diagnostic counts, and the exact static and dynamic
blockers; it returns the frozen specifications and `(decision_date,
entry_date)` session pairs.
Rows from `building`, `failed`, and `blocked_empty` runs are diagnostics only.
The six tables are `fundamental_signal_runs`,
`fundamental_signal_definitions`, `fundamental_signal_values`,
`fundamental_signal_inputs`, `fundamental_signal_proofs`, and
`fundamental_signal_coverage`. The values table
contains one row per `(run_id, signal_id, decision_date, security_id)`, a
nullable score, eligibility and reason, decision cutoff, entry date, and input
clock. Inputs contain selected derived state IDs, hashes, fiscal operand dates,
origin, statuses and per-term reasons. Coverage retains every observed decision
session and signal, including empty and blocked sessions. The manifest seals a
digest over canonical specs, resolved definitions, observed calendar, values,
inputs, proofs and coverage, independent of run ID.

Migration 0323 supplies each derived state with exact selected-input references
and a hash of their canonical JSON. The builder uses its bounded lineage
resolver once per unique selected state per run and stores the result in
`fundamental_signal_proofs`. Only a single verified selected leaf CIK can link
an issuer-owned derived state to a listed security's decision-date CIK. Multiple
matching owners are quarantined. The current accounting anchor is the selected
root fiscal end together with the newest selected leaf fiscal end. Both must
be within `max_age_days`; older declared YoY comparison leaves remain valid if
their clocks and offsets are correctly selected. The oldest/newest leaf fiscal
ends remain in the input record for audit.

An invalid/NULL root remains a blocking event. If its selected references
still prove a unique CIK, it participates in owner ambiguity checks but can
never produce a score; a previous numeric state is not revived. When the
current root cannot prove a CIK, the builder searches its full visible prior
owner/metric history for the most recent verified CIK. This proof only blocks
an otherwise ambiguous owner; it cannot make the invalid root eligible or
assign a security to an earlier date. More than 100,000 prior state rows for a
metric/session fail the run rather than silently drop older evidence.

The run manifest records modeled filing clocks, source backfill,
ownership/terminal label and transaction/borrow limits. These blockers keep
`production_eligible=false` even for a complete research panel.
