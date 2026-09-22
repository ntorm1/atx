# DP1 fix2: valid physical invalid-state fixtures

Root's integrated focused run reported 20 passed / 2 failed. Both parametrized
`test_keyset_reopens_preserve_full_history_stale_cleanup_and_budget` cases failed
inside `_seed_mixed_history`, before the derived refresh: the quarterly fixture
attempted to insert Python `None` into `fundamental_standardized.value`, whose
physical schema requires NOT NULL. The annual invalid revision used the same
invalid fixture value and would encounter the same constraint afterward.

Changed only the authorized live test file:
`atx-db/tests/test_derived_connection_capacity.py`. Both invalid input revisions
now use `float("nan")` rather than `None`, with a short explanation of the
physical-input versus canonical-output contract. This directly reuses the
existing AF1 precedent in
`tests/test_derived_annual.py::test_precedence_switches_and_null_never_resurrects`,
which uses nonfinite quarterly and annual standardized revisions to produce
explicit canonical NULL states. P1's
`test_invalid_recovery_fallback_api_coverage_and_release` likewise exercises
invalid canonical NULL states from nonnull physical inputs (zero denominators).

The original/amended quarterly values 460/510 and annual values 1000/1100, the
latest NULL assertions for both securities, full-history/count parity, traversal,
snapshot cleanup, one-time enumeration and settings-replay assertions are
unchanged. The schema's NOT NULL constraint and all production logic remain
unchanged. No source change is warranted by this fixture failure.

Root's exact rerun selector (two parametrized cases) is:

```text
tests/test_derived_connection_capacity.py::test_keyset_reopens_preserve_full_history_stale_cleanup_and_budget
```

Use the existing exclusive guarded runtime and `-n 0`. This correction was
authored using static reads/edits only: no Python/imports, test collection or
execution, database access, lint, typecheck, Git command, subagent or additional
review was performed. The root's prior 20 passes are reported evidence; the
two corrected cases remain pending root execution. Draft artifacts and the
previous integration patch were deliberately left unchanged under the narrow
live-test authorization.
