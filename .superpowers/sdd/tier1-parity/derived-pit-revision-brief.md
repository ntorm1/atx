# PIT revision preservation repair (P1)

Read `derived-pit-revision-audit.md` first. This is a confirmed Critical
production defect, not an optional optimization. The user's full PIT objective
requires preserving original and amended derived states. A cutoff applied to
today's latest-only computation is insufficient. Complete before run5's derived
and market stages; leave the active Companyfacts source pass running.

## Required outcome

Implement the audit's event/interval design end to end: historical standardized
inputs and dependency states; affected target-bucket/event generation through
the metric DAG; bounded dense local frames; deterministic full state selection;
clocked invalid derived states; historical raw/DEI and derived daily joins;
quarterly export and factor/publication/schema consumer closure. Preserve the
frozen legacy parity definitions and all existing metric formulas. Core breadth
is a concurrent CSV-only addition and must work without per-metric hardcoding.

Audit example: Q2 revenue110->160 amendment changes Q4 TTM from460 (Feb10) to510
(Mar10). February20 must remain460 and price/sales10 with cap4600, even after
the later amendment is loaded. The same day's raw revenue still uses newer
Q4=130. Required invalidation example: A100/B10 -> B0 -> B20 must yield valid10,
NULL invalid, valid5 at successive events; readers rank states before filtering.

Store state transition time including selection/control-flow changes, not just
max arithmetic input clock. Preserve same-value lineage changes. Same-event
ties, missing-quarter arrivals, stub/bucket representative dates and older-period
amendments must be deterministic and cannot use future evidence. Do not invent
withdrawals from concepts omitted in filings; upstream admitted numeric history
and modeled availability remain explicit limits.

## Scope and resource contract

- Prefer evolving the existing canonical derived table to nullable value plus
  explicit state/status/revision metadata, with governed migration0315. Preserve
  physical public tables and required keys. Do not change historic migrations.
  If a separate canonical state table is materially safer, explain that decision
  before edits and ensure no PIT consumer remains on a valid-only compatibility
  table. Do not claim old legacy rows are complete historical states.
- Generate only affected target/event keys from each expression's bounded lag
  frame and its dependency state events. No all-universe or per-security
  event-times-whole-history expansion. SQL owns values/frames; Python owns
  bounded control metadata only. Event chunks must retain predecessor states.
- One DuckDB thread,1GB budget,3GiB root process guard. Bounded security/event
  blocks and scoped transactions/staged publication; no all-universe indexed
  transaction, no RAM escalation. Do not add optional full-panel ART indexes.
- Stable state IDs incorporate typed source/security/metric/window/period/
  definition/event identity; lineage hashes include actual selected input states.
  Runtime/run IDs and mutable revision counters are not stable identifiers.
- Preserve scoped rebuild atomicity/rollback and unrelated sources/securities.
  No partial generation may appear as a successfully rebuilt canonical scope.
  Invalidate/rebuild downstream metric closure when requested dependencies change.
- Update affected public schema/version/description and coverage semantics
  honestly. No measured-threshold flip or production certification.

## Exclusive ownership

Fresh Codex implementer owns derived_metrics.py (and new private helper if
needed), relevant derived_dsl context seam only, market_daily.py,
panel_export.py, derived_factor_projection.py, affected API/catalog/publication
and quality consumer lines, new focused tests, task report, and only migration
0315's body/registry/facade entries. Root grants registry lock0315. Do not edit
derived_metric_definitions.csv or the new core_metric_breadth test/report.
Serialize any activation.py/jobs.py change with root; no other source-owner
currently needs them, but first explain a concrete requirement. Do not alter
fundamentals.py or other active source loader code. No commits until root's
validation and dispatch. Never stash/checkout/reset/restore/clean.

## Verification and delivery

Agents do static implementation/review only while source ingestion is active.
Root alone runs all DB/tests/probes after the live writer exits. Prepare one
focused root command for audit examples, dependency effects, invalidation and
fallback clocks, missing-quarter arrival, deterministic ties, chunk equivalence,
scope/rollback, consumer exports/daily PS, and bounded candidate/frame growth.
Reuse existing fixtures; no broad test loop. Report exact files/contracts,
algorithmic bounds, untested runtime concerns and any necessary departures.

One independent Codex implementation review must explicitly assess closure of
the original Critical audit finding. A further scoped rereview is needed only
if that implementation review raises a Critical requiring fixes. Important
findings are fixed and accepted on implementer report. No test reruns by
reviewers. Pathspec-only commit with the user's exact coauthor trailer after
root approval to commit.
