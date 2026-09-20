# AF1 fix3: two Important review findings

Date: 2026-09-20. Status: actual-tree source/tests stable for root verification.

Read the single independent review, `annual-fallback-review.md`: 0 Critical,
2 Important, 0 Minor. Both Important findings are addressed below. Only
`atx-db/src/atx_db/_derived_pit.py`, `atx-db/tests/test_derived_annual.py`, and
this report changed in this follow-up. `_derived_annual.py`, migrations,
catalog/schema definitions, public snapshots, runbook, other tests and CF5
work were not edited. The old isolated draft remains historical delivery.

No tests, imports, DB connections, probes, runtime work, lint/typecheck or
commits were executed by this implementer. Root owns verification. No further
independent review was requested or performed: the review had no Critical
finding, and the accepted gate is this issue-by-issue report plus root execution.

## I1: retain complete quarters when this metric has no usable annual alternative

Previously, a global annual span from an unrelated concept could prevent a
finite quarterly result from being emitted even when that metric had no annual
alternative. The new quarterly retention condition requires all of:

* This metric's own direct annual alternative is unusable under the existing
  actual-span, arithmetic-clock and finite-value checks.
* The quarterly result is finite.
* Its four actual quarter spans are coherent under the existing complete-span
  checks.
* Its actual quarterly span endpoint equals the event-visible target endpoint.

This condition is an additional path in quarterly preference. Existing
comparability behavior remains for usable annual alternatives. Actual annual
operands still join by exact start/end, and annual-backed descendants still
undergo the existing span checks. Missing/incoherent/nonfinite quarterly
results cannot use the new retention path. Therefore the annual fallback's
later invalidation still produces a canonical invalid state when no valid
quarterly result exists, and stale fiscal-year values cannot move to a new
endpoint through this fix.

Exactly one regression was added for the review's counterexample:

`tests/test_derived_annual.py::test_unrelated_annual_span_keeps_complete_quarterly_revenue`

The fixture first rebuilds four revenue quarters100/200/300/400 covering
January1--December31, with Q4 available February10. It then adds the unrelated
February1--December31 annual COGS600 at March10 and rebuilds. It asserts
revenue1000/quarterly before and after the new event, the unchanged earlier
as-of tuple, the original actual span, a valid later state at March10, and the
unchanged February10 arithmetic clock. There is no annual revenue row.

## I2: coherence follows the selected annual share denominator

Weighted-share `Span.coherent` now uses the same `choose_annual` CASE as its
value, availability, start, end and annual flag. The selected exact-span annual
branch supplies `true`; the quarterly branch keeps its prior coherence check.
An unselected legacy quarterly row without a start can no longer invalidate
the correctly selected annual denominator. The surrounding numerator/share
span combination still verifies matching starts/endpoints. Missing, zero or
nonfinite annual share values retain the existing numeric/domain invalidation
behavior.

Exactly one regression was added for this counterexample:

`tests/test_derived_annual.py::test_annual_eps_ignores_unselected_legacy_quarterly_share_span`

Annual income100 and annual diluted weighted shares50 first produce EPS2 at
February20. A later quarterly shares40 row at the same endpoint has a NULL
start and arrives March10. After rebuilding, the test asserts EPS2 with
`annual_dependency` origin, the unchanged earlier as-of tuple and fiscal span,
a retained March10 state, and the original February20 arithmetic clock. It
does not replace the annual denominator with the quarterly value40.

## Root verification selectors and limits

The two new selectors above are the precise counterexample regressions.
Existing focused assertions relevant to preserving the fixed paths are:

* `tests/test_derived_annual.py::test_precedence_switches_and_null_never_resurrects`
* `tests/test_derived_annual.py::test_annual_composition_and_weighted_share_invalidations`
* `tests/test_derived_annual.py::test_mismatched_annual_spans_do_not_compose`
* `tests/test_derived_annual.py::test_no_stale_fy_at_new_quarter_or_wrong_endpoint`

Root previously reported the three fix2 selectors passing with peak process
memory0.790688GiB. That is prior root evidence, not verification of these two
new regressions. Their runtime and lint/typecheck outcomes remain pending here.

The changes add only scalar SQL selection conditions; no relation, join,
candidate event, frame width, input panel, publication row or transaction was
added. Existing input/candidate/scope bounds, per-security atomic publication,
1GB DuckDB limit and one-thread setting remain unchanged. SQL scalar byte/work
cost and production coverage were not measured by this implementer. Root must
keep the existing one-workload guarded execution policy without RAM escalation.

Post-edit SHA-256s:

* `_derived_pit.py`:
  `5c9a5dd444257181acf0c7c7645e3d6e3efc92ebd5421c78835ebc0534041f9f`
* `test_derived_annual.py`:
  `fbc494e482d96817bdbd846979b76e07f694d15abfa26005703f3008034da7cf`

No source/test edits are pending from this implementer. Root can accept the
Important fixes after its focused execution, without a second review.
