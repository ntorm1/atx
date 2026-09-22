# Task 4 fix1 report — per-exchange Shumway policy

Status: complete. All review findings addressed; focused verification passed.

## Addressed findings

- Important finding 1: completed the interrupted per-exchange implementation. The default
  `ShumwayPerformancePolicy()` applies -0.55 to Nasdaq and -0.30 to other or unresolved
  exchanges. `None` still opts out; explicit floats retain the original uniform override.
  Observed returns and corporate-action policy retain their higher priority. Nasdaq rows
  have their own `performance_unknown_nasdaq` policy and `shumway_nasdaq_default` basis.
- The refresh path resolves exchange from `universe_us_listed_membership` (`us_listed_v1`),
  then `exchange_listings`, then the Nasdaq directory. Membership/listing validity must
  cover the delist date; snapshot dates cannot exceed it; real input availability cannot
  exceed that date at 22:00. Latest-revision eligibility does not replace these PIT bounds.
  The result carries `max(event.available_at, selected_exchange.available_at)`; lower-priority
  records do not delay the result. No exchange timestamp or event availability is invented.
- Minor finding 2: all four new evidence-code reason categories now have reconciliation
  family mappings. Bankruptcy accepts dropped and liquidation families. A conflicting
  known family remains a `mismatch`, rather than becoming `unmapped`.
- Informational finding 3: the runbook now explicitly says the error-severity companion
  check degrades a run to `partial`; only critical failures halt execution. It documents
  per-exchange dispatch, PIT lookup, opt-out, overrides, and a NULL-safe filter retaining
  observed returns while excluding both imputation variants.
- Known module-boundary failure: migration 0306 no longer imports the private
  `quality.checks_survivorship` leaf. Its registry insertion pins the stable check-name
  literal, preserving the quality facade without expanding its API or editing shared files.

## Verification

- Focused pytest: **78 passed, 1 skipped**, exit 0, run once after implementation:
  `.venv/Scripts/python.exe -m pytest tests/test_delisting_terminal_policy.py
  tests/test_delisting_returns.py tests/test_module_boundaries.py -n 0 -q`.
  This covers mixed-exchange dispatch, all three PIT lookup sources and their precedence,
  future snapshots/availability/validity exclusions, expired intervals, revision filtering,
  maximum selected-input availability, observed-return priority, explicit scalar override,
  None opt-out, all four reconciliation categories, and the real package-boundary checks.
- Ruff checked the four touched Python files. Migration 0306 and the terminal-policy test
  file are clean. The existing delisting module and returns tests retain 26 pre-existing
  findings, all outside the fix hunks; no broad cleanup was applied.
- `mypy --strict --follow-imports=silent` checked migration 0306 and `delisting.py`.
  Migration 0306 has no findings; the delisting module retains its 27 existing errors,
  all outside the fix hunks. The new policy object, dispatcher changes, and loader have
  no strict typing findings.
- `git diff --check` passed for the five owned implementation/documentation files.
- No full suite, live database, jobs/activation changes, external calls, or changes to the
  T3 evidence module, migration registry, migrations facade, CLI, or publication code.

## Scope and handoff

The pre-existing uncommitted policy constants/dataclass/dimension edits were reconciled
into this fix. Migration 0306 now seeds eight policy rows; its existing idempotency test
pin was updated. The controller owns the schema-contract check, ledgers, and generated
data-dictionary refresh. No Critical concern or scope expansion was identified.
