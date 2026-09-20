# AF1 fix2: focused runtime fixture/seam corrections

Date: 2026-09-20. Status: static edits in the applied test tree, awaiting root rerun.

Root reported 76/79 passing from
`annual-fallback-focused-056478ff7e8048afb38c18198385edf1`. I read that stdout
and the existing P1 invalidation fixture before editing. This follow-up changes
only the two test files below and this report. No production source, schema,
migration, catalog, draft patch or resource setting changed. No tests, imports,
DB connections, probes, runtime work, lint or commits were executed by me.

## NULL-state fixture contract

P1's `test_invalid_recovery_fallback_api_coverage_and_release` inserts nonnull
standardized numbers: numerator100 and denominator10, then0, then20. The DSL
produces canonical derived states10, NULL, then5. It does not insert NULL into
`fundamental_standardized.value` or relax that column's NOT NULL constraint.

The two AF1 fixtures incorrectly inserted Python `None` into the standardized
table. They now insert explicit nonfinite DOUBLE revisions with `float("nan")`
for the invalid source cases. These are nonnull numeric fixture inputs; the
existing evaluator's finite-result gate produces the canonical NULL states.
No source/schema accommodation was added to production code.

`test_precedence_switches_and_null_never_resurrects` retains the same chronology:
annual1000, restated annual1100, completed-quarter950, quarterly invalidation
selecting annual1100 at the later event with its original arithmetic clock,
then an invalid annual revision and a later real annual0. Its one-group,
available-at/valid-to, API latest/first-reported, daily invalid-state, and
deterministic chunk-rerun assertions are unchanged. The three source revisions
that previously used NULL across the two failing tests now use nonfinite values.

`test_annual_composition_and_weighted_share_invalidations` retains weighted
shares50, then0, then a nonfinite revision. It still asserts EPS2 before the
zero, `zero_denominator` after zero, and a later canonical NULL with the later
event clock. A new explicit `value_status == 'nonfinite'` assertion distinguishes
the final invalidation from the earlier zero-denominator state. It cannot
silently resurrect the old positive denominator.

These cases exercise canonical NULL invalidation, nonfinite source-state
selection, and valid zero separately. They do not establish upstream NULL
standardized admission, concept-withdrawal behavior, or nonfinite ingestion
coverage. The physical standardized-value NOT NULL contract remains intact.

## Frame instrumentation seam

P1's bounded-frame test wrapper now accepts `annual_plan` as its fourth argument
and passes that exact object through to the original `pit.frame_sql`. Its
`max_lag + 1 <= 8` assertion, recorded frame widths, expected maximum5,
unrelated-security preservation and prepublication input/candidate failure
assertions are unchanged.

## Exact changed paths and root rerun selectors

* `atx-db/tests/test_derived_annual.py`
  * `tests/test_derived_annual.py::test_precedence_switches_and_null_never_resurrects`
  * `tests/test_derived_annual.py::test_annual_composition_and_weighted_share_invalidations`
* `atx-db/tests/test_derived_pit_revisions.py`
  * `tests/test_derived_pit_revisions.py::test_bounded_frames_scope_preservation_and_prepublication_failure`
* `.superpowers/sdd/tier1-parity/annual-fallback-fix2-report.md`

Post-edit test-file SHA-256s:

* `test_derived_annual.py`:
  `7aacfa0e208a756e5abbd1290ad95261258341bb425018b9c81d2600ccc89ce7`
* `test_derived_pit_revisions.py`:
  `da21c08e7e132046955c2d412a2730f79827f8296b9c7c69b683cdaf4939cbd9`

Root owns the focused rerun and subsequent independent AF1 review. Run only in
the existing single-workload guarded slot with the unchanged memory/thread
limits. No rerun success is claimed here.
