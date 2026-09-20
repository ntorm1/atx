# AF1: Annual statement fallback for core production metrics

## Concrete requirement

Read `annual-filer-metric-path-report.md`, the current P1 implementation/report,
and the fiscal-year fallback contract in the Tier-1 design. Annual duration facts
are standardized but do not reach TTM core metrics when a complete quarterly
history is absent. Close that production gap without inventing quarters.

## Ownership and delivery while the source writer runs

Fresh Codex implementer, static work only. No tests, Python imports, database
connections, source acquisition, or production commands. Root runs verification
after the sole companyfacts writer finishes. P1 and Core source changes are
reviewed, uncommitted, and frozen until root validates and commits them.

Prepare the implementation in an isolated draft directory:
`.superpowers/sdd/tier1-parity/annual-fallback-draft/`. Preserve repository-relative
paths there. Copy only files actually needed for this task, including a pristine
baseline copy for changed existing files. Do not copy the warehouse, archives,
venv, or full repository. Do not modify actual `atx-db/` files. Deliver an ordinary
unified integration patch plus new file contents, a complete path inventory,
baseline SHA-256s, and a concise implementation report. Root will apply it only
after P1 then Core commit; regenerate against actual committed files if necessary.
No commits, stash, checkout, reset, restore, clean, or writes to another task.

## Behavioral contract

- Preserve a single canonical historical state stream per source/security/metric
  and fiscal endpoint. At an event, prefer a valid complete four-quarter result
  over a comparable direct annual fallback. Evaluate changes in that precedence
  as events; future quarterly facts cannot alter an earlier annual as-of value.
- An annual fallback must have actual annual source evidence and a matching
  fiscal endpoint. Do not carry a stale FY total forward as a later quarter's TTM,
  mix different annual spans, or accept short/stub durations as full years.
- Reuse the existing formulas and their sign/domain rules. Cover direct TTM flows
  and composed gross profit, EBITDA, positive capex and FCF. Existing descendants
  should then obtain margins, per-share, payout, accrual/leverage and market
  measures where their own dependencies are present. Annual weighted-average
  shares may supply an annual endpoint per-share denominator, but must never be
  relabeled as a quarterly reported value.
- Keep quarterly flow outputs and their QoQ/YoY unavailable without quarterly
  evidence. Annual endpoints can support comparable annual YoY/CAGR and annual
  balance averages; preserve real missing years. No synthetic fractional quarters
  and no broad relaxation of the four-quarter DSL.
- Preserve P1 whole-state selection, explicit NULL invalidations, deterministic
  revision identities/lineage, state and arithmetic clocks, atomic per-security
  publication, scope closure, and bounded SQL work. No Python fact panel or whole
  universe event cross product. Existing 1 GB / one-thread limits stay.
- Selected origin must be inspectable, not concealed solely in a hash. Use the
  smallest coherent schema/consumer change needed. If a migration is necessary,
  reserve 0316 in the draft only; do not edit live registry or old migration bodies.
  No source/SLO/coverage condition flips and no historical-vintage certification.
- Prefer a small annual-input/fallback helper feeding the existing canonical
  dependency path over a second public metric catalog. The exact integration
  seam is implementer's choice after reading current P1. Enumerate supported
  catalog definitions and material limitations in the report.

## Focused acceptance cases to prepare, not run

Annual-only FY revenue1000, GP400, income100, CFO160, capex60, shares50 and
year-end equity/current assets/liabilities should produce the values in the
path report. Include annual weighted diluted shares/EPS and FY2023 revenue900
to demonstrate YoY; real absent quarters must stay absent/invalid.

Cover FY-minus-9M complete-quarter preference, missing-9M direct FY fallback,
annual restatement history, later quarterly completion/invalidations switching
precedence at the correct event, NULL versus zero, comparable endpoint/span,
and direct daily/API consumption. Include deterministic rerun and bounded/atomic
failure behavior where touched, using meaningful fixtures rather than broad
suite expansion. Root will run touched tests once under the guard, then one
fresh independent Codex review. Re-review only for Critical fixes.

## Report

Write `annual-fallback-implementation-report.md`: implementation result, paths,
integration instructions, source evidence, exact supported metric semantics,
pending focused command, resource bounds and any unresolved issue. Runtime tests
and production coverage are unmeasured. Do not claim the task complete until
root applies, verifies, reviews and commits it.
