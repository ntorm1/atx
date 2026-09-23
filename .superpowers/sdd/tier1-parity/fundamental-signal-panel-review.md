# FQ1 fundamental signal panel: one-pass review

Read-only review of the current post-lint FQ1 consumer, migration 0324, CLI,
focused fixtures, and documentation against `fundamental-signal-panel-brief.md`.
No code, database, test, network, or Git mutation was performed by this
reviewer. FQ2's evaluator and DL1's publisher were outside scope. The
implementer reports 12 isolated checks passing in `fresh-fq1-tests3` (0.654
GiB peak) and scoped Ruff passing in `fresh-fq1-ruff2`; the earlier guarded
run stopped on host headroom after nine dots and was not a test failure.

## Critical

None found.

## Important

1. **The 16-state prior proof window can silently drop a known issuer owner.**
   `fundamental_signal_research.py:660-683` limits `_fs_prior_candidates` to
   the 16 most recent earlier events for an unqualified current root. If an
   owner had a verified selected-leaf CIK and then more than 16 invalid or
   legacy revisions before a run's first decision date, that proof is never
   loaded into `fundamental_signal_proofs`. `_fs_prior_owner_cik` finds no
   association, `_fs_owner_matches` omits the current invalid owner, and a
   different older qualified owner with that dated CIK may score. The current
   root must remain ineligible, but its prior verified CIK must still cause
   suppression or ambiguity. Resolve the bounded prior history to the most
   recent verified association or fail the affected partition closed at a
   hard cap; add a fixture with 17+ unqualified revisions after a verified
   state and another qualified owner.

2. **Completed manifest diagnostics and dynamic blockers are not fully
   verified.** `fundamental_signal_research.py:465-469` checks only the
   `sessions`, `values`, and `eligible_values` entries of `diagnostic_json`.
   The completed manifest also records `coverage_rows`,
   `common_membership_rows`, and `unmatched_owner_states` (lines 967-982),
   while `_panel_digest` (lines 262-296) omits `diagnostic_json` and
   `blockers_json`. The validator requires only the fixed blocker subset
   (lines 320-327). Changing those remaining counts or removing a dynamic
   blocker from a completed run leaves validation successful, so its sealed
   coverage explanation is mutable. Recompute and compare all stored
   diagnostics and dynamic blockers during validation, or include them in a
   versioned manifest digest and independently check their derivation. Add a
   tampered-count/blocker fixture.

## Checks with no finding

The builder freezes exact publisher definition hashes for the transitive
dependency closure before session processing, then uses DL1's selected leaf
CIK proof rather than publisher owner text for positive issuer qualification.
It selects whole derived event states before rejecting NULL values, enforces
root and newest-leaf freshness while retaining older YoY comparison leaves,
quarantines multiple associated owners, and ranks complete-case cohorts with
equal-score ties. Decision sessions come from observed priced dates and use a
22:00 UTC cutoff with next observed-session entry. The CLI opens an existing
warehouse directly, without calling the store's migration initializer. The
shared panel digest and read-only validator cover the score, input, proof,
coverage, and observed-calendar grains, subject to Important finding 2.

The full-schema regression gate remains pending host capacity; the reported
isolated checks do not establish production eligibility or historical vintage
coverage.
