# Core metric breadth follow-on

User's production priority supersedes the earlier S3 growth breadth scope cut.
Build on the static `production-metric-surface-report.md`; catalogue presence is
not measured live coverage. Current source ingestion must run uninterrupted.

## Scope

Add declarative definitions for quarterly YoY and QoQ growth of revenue, gross
profit, operating income, net income, diluted EPS, CFO, FCF, capex and R&D (nine
series, eighteen outputs), preserving all existing TTM definitions/codes. Use
the existing quarterly compositions, dense quarter grid and DSL semantics;
document that unadjusted fiscal-quarter comparisons may be seasonal.

Add three-year CAGRs for trailing gross profit, operating income, EBITDA and
FCF, plus common equity. Preserve the current DSL domain guards; negative,
zero or missing endpoints cannot acquire invented meaningful CAGRs.

Complete basic EPS TTM and operating-efficiency DSO, DIO, DPO and cash conversion
cycle from existing standardized inputs/rollups. Use average opening/closing
balances over the same annual span and positive annual revenue/cost bases,
state the 365-day convention, retain NULLs for absent inputs. Do not conflate
reported normalized income with an invented recurring-earnings adjustment:
only add normalized-income/EPS variants if item semantics and denominator
basis are supported concretely; otherwise report the limit and defer them.

No changes to existing formulas, factors, market wide schema or source loaders.
No new API/schema-condition claims. Preserve all original 173 definitions.

## Ownership and delivery

Static wiring confirmed activation already reseeds derived definitions from
the CSV. No migration is needed for this additive wave; the provisional 0315
reservation and registry lock below are released. 0315 is available for the
separate PIT revision-preservation repair if required.

Fresh Codex implementer owns derived_metric_definitions.csv, a NEW focused
test file, and this task's report. Inspect seed refresh wiring: if a governed
metadata migration is needed for the existing warehouse, reserve 0315 for
this task (only its body/import/registration lines). Do not edit historic
migrations/checksums, activation.py/jobs.py/derived_metrics.py. Coordinate
any newly discovered shared-file need with root before editing it.

Prepare focused, meaningful mathematical and PIT-clock cases for the added
families, including missing quarter and nonpositive CAGR endpoints. Root
alone runs all DB/tests/probes after the active source writer finishes. No
runtime/test/subprocess workload by the agent; static inspection and edits
only. One independent Codex review; Important fixes accepted on report;
Critical fixes rereviewed. No commit until root validation and explicit
commit dispatch. Pathspec-only commits, exact requested coauthor trailer.

Report exact added codes/formulas, domain and denominator decisions, live
seeding plan, focused root command, remaining limits. No statistical claims.
