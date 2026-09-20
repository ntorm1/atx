# AF1 independent static review

Date: 2026-09-20. Reviewer: fresh Codex reviewer. Branch: `feat/tier1-parity`.
AF1 comparison base: `a3b007d9`.

**Result: 0 Critical, 2 Important, 0 Minor findings.** Address both Important
findings before production publication. This is the single independent AF1
review; no additional reviewer or agent was used.

## Important AF1-I1: unrelated annual evidence can invalidate complete quarters

Evidence: `atx-db/src/atx_db/_derived_pit.py:246-256`, together with
`atx-db/src/atx_db/_derived_annual.py:103-118` and `:253-268`.

The annual target span is selected across **all** annual concepts for a
security/bucket. Every eligible TTM metric receives that span, even when none
of its own annual operands exists. `comparable_quarters` requires the quarterly
span to equal this global annual span. The final result CASE returns the
quarterly result only when that comparison passes; it has no branch preserving
a valid complete quarterly result when the metric's annual alternative is
unavailable.

Concrete static counterexample:

1. Four revenue quarters cover 2024-01-01 through 2024-12-31 contiguously, with
   values 100, 200, 300, 400 and Q4 available on 2025-02-10. There is no annual
   revenue row. `revenue_ttm` is 1000 with complete quarter evidence.
2. On 2025-03-10 an unrelated annual cost-of-revenue fact arrives for
   2024-02-01 through 2024-12-31. Its 335-day duration is admitted.
3. The global annual start becomes February 1. Revenue's coherent quarter
   start remains January 1, so `comparable_quarters` is false. Its annual
   alternative is NULL. All three result CASE branches fail, publishing a
   canonical NULL and invalidating revenue descendants despite unchanged,
   complete revenue quarters.

This is an unintended loss of the existing quarterly result; accepting it does
not combine different annual spans. The report's conservative annual-composition
policy does not require an unrelated concept to veto independent quarterly
arithmetic.

Concrete fix: separate the test for an available, comparable annual alternative
from the validity of the four-quarter result. When the metric has no usable
annual alternative, retain its finite, coherent four-quarter result at the
actual matching target endpoint. Preserve exact-span checks for operands that
actually participate in annual arithmetic and preserve the endpoint check that
prevents stale FY values moving to another period. Add one focused regression
with the chronology above, asserting 1000/quarterly before and after the
unrelated annual event and unchanged earlier as-of selection.

## Important AF1-I2: selected annual shares inherit rejected quarterly coherence

Evidence: `atx-db/src/atx_db/_derived_pit.py:201-205`, `:215-224`, and `:229-234`.
The affected catalog formula is `eps_ttm = safe_div(net_income_common_ttm,
weighted_avg_shares_diluted)` in
`atx-db/src/atx_db/seeds/derived_metric_definitions.csv:48`.

When `choose_annual` is true, the denominator value, availability, start, end
and annual flag switch to the annual share state. Its `Span.coherent` is still
unconditionally `old.coherent`, however. That expression describes the
unselected quarterly share row. A quarterly row whose period start is NULL
sets it false, and the annual arithmetic guard then suppresses EPS.

Concrete static counterexample: annual common income 100 and annual diluted
weighted shares 50 both cover 2024-01-01 through 2024-12-31 and arrive on
2025-02-20. EPS is 2. A later quarterly diluted-share row at the same endpoint,
with value 40 and NULL period start, is admitted by the unchanged quarterly
input rules. The annual numerator still forces selection of annual shares 50,
but the unselected quarter changes coherence to false. EPS therefore becomes
NULL at that event. Its correct selected operands have not changed. Missing
quarterly start dates are explicitly supported as legacy inputs by this
implementation; adding such evidence must not poison an independent, fully
identified annual denominator.

Concrete fix: select denominator coherence using the same `choose_annual`
condition. The admitted exact-span annual branch can supply `true`; the
quarterly branch retains `old.coherent`. The surrounding numerator/denominator
span combination still checks their matching endpoints and starts. Add one
focused regression for annual EPS followed by the legacy quarterly share row,
asserting EPS 2 and annual dependency origin at both events. Keep the existing
annual zero/nonfinite invalidation checks.

## Reviewed behavior and limits

Read the AF1 brief, implementation report, fix1 and fix2 reports, annual-filer
path report, and P1 implementation report. Reviewed the eight AF1 production
paths and three AF1 test paths in the actual tree, including the new untracked
helper, migration and test file. Read the DSL, catalog definitions and relevant
standardizer source for context. Migration registry/facade review was limited
to 0316. Disjoint CF5 and unrelated user changes were excluded.

The remaining inspected structure is consistent with the intended design:

- Annual inputs stay separate from quarterly items, require actual 330-380-day
  source spans, and are joined by exact start/end and as-of event time. Expanded
  scalar ASTs reuse the existing gross-profit, EBITDA, positive-capex and FCF
  arithmetic without publishing annual values under quarterly flow codes.
- Annual revisions and span changes enter event keys; downstream dependencies
  propagate their own later events. Whole-state structs retain invalid states;
  finite-result selection does not search backwards for an older valid annual
  revision. Event and arithmetic clocks remain separate.
- Annual comparisons use dense year offsets and span checks, preserving real
  missing years. Annual-backed quarter-count operations are withheld. No broad
  relaxation of the four-quarter DSL was found.
- Canonical origin/span fields are published and compressed with the state;
  migration 0316 preserves legacy rows with `legacy_unspecified`. API version
  2.1 exposes the fields, and daily input lineage includes them. Consumers use
  the canonical state stream, including NULL transitions.
- Input counting includes annual rows before materialization. Annual event
  counts extend the candidate upper bound; local frames, scope limits and the
  per-security publication transaction remain bounded. No Python fact panel,
  universe/event cross product, resource-limit increase or extra thread was
  introduced in the reviewed diff. SQL byte cost, execution plans and live
  resource usage were not independently measured.

Final fix2 test edits were reread: the three invalid source revisions now use
nonnull NaN inputs, preserving the standardized table's NOT NULL constraint;
the frame wrapper forwards the fourth argument. These exercise canonical NULL
invalidation, not admission of SQL NULL upstream. Root reported strict mypy
passing, an initial focused result of 76/79 passing, and all three previously
failing selectors passing after fix2 (peak 0.790688 GiB), covering the original
79 cases after those fixture corrections. These are root-owned results, not
tests executed by this reviewer. Neither finding above is covered by the
current focused fixtures.

This review used source/diff reads only. No tests, package imports, database
connections, probes, production commands, Claude or other external LLM calls, commits,
stash, checkout, reset, restore or clean were performed. The only written file
is this report. Production coverage and historical-vintage certification remain
outside this review. The two counterexamples are derived directly from the
generated SQL branches; they have not been executed here. Root owns focused
verification of their fixes. No Critical finding requires another review.
