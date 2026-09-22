# Fundamental availability clock audit

Date: 2026-09-20 local / 2026-09-21 UTC. Branch: `feat/tier1-parity`.

**Finding: the full activation path can admit a filing's facts and their descendants at the 22:00 UTC decision cutoff before that filing was accepted by EDGAR.** The ordinary standard-time filing window supplies a concrete counterexample. This is established from code plus official SEC rules, not a measured occurrence in the current warehouse. No DB connection, Python/import, test, lint, probe, data processing, source modification, download, install, commit, process control, or external paid API was performed. Only static source/document reads and small official SEC documentation lookups were used. This report is the only file written; archive4 keeps the exclusive runtime slot and its existing budgets.

The finding is upstream of the P1 revision-preservation repair. It does not dispute that repair or reopen AF1 arithmetic. The current implementation reconstructs events consistently using a modeled clock; that consistency cannot make an incorrectly early input clock safe. A bounded downstream clock policy can correct eligibility without interrupting or replaying raw Company Facts ingestion.

## Contract and evidence boundary

The design's exact contract is explicit: `filed_at` is the SEC acceptance datetime in UTC, and filing-fact `available_at` equals it (`docs/superpowers/specs/2026-09-19-tier1-parity-design.md:44-45`). The implementation instead derives a timestamp from the date-only Company Facts `filed` value.

The daily 22-hour convention itself is documented in the S3 plan (`docs/superpowers/plans/2026-09-19-tier1-s3-derived-engine.md:7,3157`) and `market_daily.py:1-8,42-44`: fundamental inputs must be visible at `trade_date + 22 hours`. This supports the decision grid, not the inference that every filing bearing that date had been accepted by then.

The later program ruling (`program.md:75`), P1 brief (`derived-pit-revision-brief.md:25-30`), P1 report ("Upstream limits remain explicit"), migration0315, and public API descriptions explicitly retain modeled-availability and observation-vintage limitations. These disclosures are useful and should remain. They support describing the existing output as reconstructed modeled filing history, but do not supply a source guarantee for same-day 22:00 eligibility. I found no explicit ruling accepting pre-acceptance use as meeting the design's exact clock contract.

No conclusion here certifies historical source delivery, local observation vintages, identity assignment, source completeness, or production coverage. Also distinguish **EDGAR acceptance** from **first public availability**: the former can be an explicit modeled source-event contract; it is not proof of the latter.

## Actual activation path and clocks

Line references are to the live source tree inspected for this audit, beneath `atx-db/src/atx_db/`.

| Step | Source evidence | Clock behavior |
| --- | --- | --- |
| Activation sequence | `activation.py:46-63` | Submissions load precedes Company Facts, then statement points, periods, TTM, calendarization, standardization, reconciliation, derived metrics and daily market. Having submissions available does not itself gate facts. |
| Submissions ingestion | `activation.py:541-553`; `sec_submissions.py:45-51,81-88` | All forms are requested. `acceptanceDateTime` is parsed with `utc=True` and converted to a timezone-naive UTC representation. Date and acceptance are retained separately. This describes the parser, not a verified upstream timezone contract. |
| Company Facts normalization | `activation.py:561-584`; `fundamentals.py:645-731,1214-1217` | `filed` becomes `filed_date`; `accn` becomes `accession_number`; `available_at = Timestamp(filed_date) + 22 hours` at line701. The same timestamp goes to both `sec_company_facts` and `fundamental_points`. There is no acceptance lookup. Activation disables inline downstream refresh and runs the separate stages. |
| Raw publication and resolution | `fundamentals.py:1249-1263,1472-1473` | Identifier resolution already uses each raw fact's modeled availability; facts and points are then published. The clock audit does not reopen that identity design. |
| Revision reconstruction | `activation.py:610-618`; `fundamentals.py:741,848-858,875-895` | `refresh_fundamental_fact_revisions` reads `sec_company_facts.available_at` unchanged. Revision ordering uses that timestamp, filed date, load-time tie information and accession. No `sec_submissions` join supplies an acceptance floor. |
| Statement points | `fundamental_statements.py:982,1120-1127` | `refresh_fundamental_statement_points` copies `r.available_at`, keeping the filed date separately. |
| Periods and RDQ | `fundamental_statements.py:1352-1354,1384-1416,1452-1457` | Period availability is the maximum statement-point availability. Acceptance becomes `rdq_available_at` only for 8-K item 2.02 candidates. It orders candidate earnings-release dates; the output selects `rdq`/`pdate`. It neither matches the fact's own accession nor raises its availability. |
| TTM/calendarization | `fundamental_statements.py:1576,1646,1770-1782`; `calendarization.py:321,505,668` | Source availability is propagated or maximized across inputs. Calendar mappings inherit period availability. None of these paths introduces an acceptance lookup or a protective day delay. |
| Standardization | `activation.py:658-670`; `standardization.py:815-829`; `_standardization_set_based.py:163-205,329,550-607,670-748` | The active facade uses the set-based engine. Direct statement candidates carry `src.available_at`; direct outputs preserve it. Discrete/composed outputs derive events from input clocks and select inputs visible by those events. |
| Derived metric events | `activation.py:793-799`; `derived_metrics.py:156,192`; `_derived_pit.py:53-86,317-324`; `_derived_annual.py:80-95` | Standardized clocks become state events. Output `available_at` is the event time; arithmetic availability remains separate. Annual inputs use the same source clocks. No later SEC acceptance floor appears. |
| Daily selection | `activation.py:812-820`; `market_daily.py:116-134,256-258,275-305,328-336` | Raw standardized and derived whole states are selected with inclusive `cutoff >= available_at`, where cutoff is exactly `trade_date + 22 hours`. A value stamped at 22:00 is eligible on that same day. |
| Daily output clock | `market_daily.py:220-234` | Output availability includes the bar and selected fundamental/metric clocks. A maximum of incorrectly early input clocks does not recover the missing acceptance timestamp. |

The DEI branch is relevant to repair scope, without implying that its table has been populated in this activation: `shares_outstanding.py:107,119` derives its clock from statement points; `market_daily.py:319-335` selects that history at the same cutoff. Rebuilding corrected statements can feed corrected share history when that surface is refreshed.

There is also a raw bypass to consider: `asof/fundamentals.py:19-38` reads `fundamental_points` directly and checks its stored `available_at`. Fixing only derived/daily selection would leave this advertised as-of surface using the old date approximation.

## Official SEC time semantics

These are direct primary-source documentation findings, not inferences from third-party libraries or discussion posts.

1. EDGAR gives an accepted live submission the same day's filing date when transmission starts by 17:30 Eastern. Most submissions beginning later receive the next business day's filing date and are not disseminated until then. Therefore filing date is not an exact acceptance time, and late-evening acceptance does **not** generally imply the same filing date. The standard-time counterexample below uses the ordinary before 17:30 window and needs no exceptional form. [SEC: Determine the Status of My Filing](https://www.sec.gov/submit-filings/filer-support-resources/how-do-i-guides/determine-status-my-filing)
2. The SEC describes the filing-time rule as Eastern Standard or Eastern Daylight time, whichever applies. Thus 22:00 UTC is 17:00 EST in winter and 18:00 EDT in summer. The ordinary same-day winter window extends beyond this fixed UTC cutoff. This conversion and its consequence are the audit's inference from the documented rule. [SEC: Release 33-8230, electronic filing hours](https://www.sec.gov/files/rules/final/33-8230.htm)
3. The SEC's timestamp FAQ describes complete-submission header acceptance time as EST, distinguishes the official filed-as-of date from acceptance, and says there is no timestamp identifying first availability on sec.gov. Its separate latency answer gives a usual 1-3 minute lag, with potentially greater and unguaranteed delay. This is evidence about EDGAR/header timing, not a field-level specification of JSON `acceptanceDateTime`. [SEC: Webmaster Frequently Asked Questions](https://www.sec.gov/about/webmaster-frequently-asked-questions)
4. The public API documentation says submissions/XBRL JSON is updated as filings are disseminated, with typical processing delays and nightly bulk recompilation. The page inspected does **not** specify the timezone encoding of `acceptanceDateTime`; SEC-only searches for that field plus timezone/UTC returned no additional documentation. I did not establish whether every trailing `Z` is semantically correct or whether any subset has another convention. Neither a blanket UTC assumption nor blanket Eastern relabeling is justified by this lookup. [SEC: EDGAR Application Programming Interfaces](https://www.sec.gov/search-filings/edgar-application-programming-interfaces)
5. Official filing dates can be adjusted after acceptance; the SEC says it will not adjust filing time or acceptance time. This limits any universal safety claim based solely on an official filing date plus a fixed delay. [SEC: Request a Filing Date Adjustment](https://www.sec.gov/submit-filings/filer-support-resources/how-do-i-guides/request-filing-date-adjustment)

In particular, neither moving the cutoff to 22:30 UTC nor adding a few minutes proves complete public dissemination. The ordinary rule concerns when transmission begins, and corrected filing dates are a separate exception. No such policy change is implemented or requested here.

## Concrete source-proven counterexample

This is a constructed, unexecuted example consistent with the documented ordinary filing rule, not an identified live accession.

Assume a supported quarterly revenue fact for an admitted issuer, period 2023-10-01 through 2023-12-31. Its Form 10-Q transmission starts at 17:14 EST on 2024-02-01 and EDGAR accepts it at 17:15 EST. It receives filing date 2024-02-01. Acceptance is therefore **2024-02-01 22:15 UTC**; the Company Facts payload supplies `filed="2024-02-01"` and that accession. No earlier publication of this accession's new value is assumed.

| Surface/event | Result under current code |
| --- | --- |
| Normalized fact and fundamental point | Available at 2024-02-01 22:00, fifteen minutes before acceptance. |
| Fact revision and statement point | Same 22:00 timestamp. |
| Direct quarterly standardized revenue | New value 130 at 22:00. |
| Derived `revenue_ttm` | If the other three visible quarterly revenues are 100, 110, 120, the new TTM is 460 at 22:00. |
| Daily `ps_ttm` at 22:00, with already eligible market cap 4600 | Inclusive ASOF joins admit 460, so price/sales is 10. |

For comparison, let the prior fully known trailing window be 90+100+110+120=420. Until the new accession is admissible, the prior latest TTM remains 420, so price/sales should remain 4600/420 (about 10.95238) under these assumptions. The actual formulas are catalogued at `seeds/derived_metric_definitions.csv:12` (`revenue_ttm`) and `:177` (`ps_ttm`); direct revenue routing is in `seeds/standardization_rules.csv:3`.

Nothing in period/RDQ processing changes this outcome. A later acceptance record already present in `sec_submissions` still does not participate in the eligibility predicate. Summer's 18:00 Eastern cutoff is later than the ordinary 17:30 transmission threshold, but that does not repair the winter case or establish a universal acceptance/publication upper bound.

## Smallest production-preserving remediation brief

Do not stop archive4, change raw ingestion, rewrite raw stamps, invalidate CF5 receipts, replay its ZIP, or change memory/thread/process budgets for this finding. Complete source ingestion and existing metric construction work; record that current results use the legacy modeled clock until a governed downstream clock rebuild is published.

The smallest useful correction point is **the base relation of `refresh_fundamental_fact_revisions`, before revision ordering**, currently `fundamentals.py:848-858`. Read an effective availability projection for each `(source, issuer CIK, accession)` and use that projected timestamp for `available_at` throughout sequencing and downstream statement generation. Keep the original `sec_company_facts`/`fundamental_points` values intact as raw loader provenance. A view, bounded staging projection, or small governed policy relation can implement this without changing Company Facts source receipts; choose storage during implementation, not by editing historical migrations in this audit.

Use one documented policy consistently:

- **Conservative date-only default:** admit a fact at the following calendar day's 22:00 UTC, equivalently `filed_date + 46 hours`. Daily ASOF selection then naturally waits for the next trading session when necessary. This deliberately closes the proven ordinary same-day winter leak without interpreting the JSON acceptance timestamp. Label it as a date-based eligibility policy, not exact acceptance or measured delivery. The known filing-date-adjustment and unbounded-delay limits above remain explicit; exceptional/inconsistent date records need an explicit unresolved status and later measurement, not silent exact-PIT certification.
- **Optional precise path only after verification:** for a uniquely matched accession with a verified source timestamp convention, an acceptance-based policy may accelerate eligibility relative to that conservative default. If retaining the existing daily floor, use no earlier than `max(filed_date + 22 hours, verified_acceptance_utc)`; a verified later acceptance must delay the row further. Preserve the selected policy/method and evidence. Do not accelerate from an unverified `Z`, missing match, or conflicting acceptance records. Acceptance-based eligibility remains the design's modeled acceptance contract, not proof of sec.gov/API arrival.
- **RDQ is descriptive:** neither `rdq`, `pdate`, report date, nor a different 8-K accession may reduce the admissible clock of a filing's values. If a value is separately extracted from an earlier release, it requires its own source lineage and clock; that is outside this repair.

Join by normalized **issuer CIK plus full accession**, not the accession's first ten digits (which can identify a filing agent), current ticker, or current security mapping. Require deterministic cardinality and expose missing/conflicting joins. Keep this projection bounded by the existing publication scope; no universe-sized pandas materialization, full indexed raw UPDATE, or extra concurrent runtime is needed. Do not use mutable revision counters or load time as clock evidence.

The same policy must cover direct raw-fact/point PIT readers. They may retain raw provenance columns while exposing/filtering by effective availability through the shared projection. Otherwise the narrow downstream correction fixes standardized/derived/daily outputs but cannot claim to fix **all** fact as-of paths. This is consumer closure for the clock change, not a request to revise identity resolution or add warehouse vintage dimensions.

After the writer releases the slot, rebuild revision metadata and dependent statements, periods, TTM/calendarization, standardized, derived, affected share history, daily panels, and projected/exported consumers in their existing bounded scopes. Changing clocks after standardization is too late: event order, same-time selection, IDs/hashes, dependency events and daily historical choices already depend on them. Preserve P1 states and AF1 calculations; let their existing propagation machinery consume corrected clocks. Keep raw source dates and loader receipts unchanged and record the effective policy version in inspectable lineage/catalog documentation.

## Deferred measurement and acceptance brief

Root alone should perform this after archive4 releases the runtime slot, under the existing guard and budgets. These checks were not run in this audit.

1. Inventory distinct issuer/accession keys before joining facts. Count unique/missing/conflicting submissions matches, null/malformed acceptance values, and filing-date disagreements. Split diagnostics by form, filing year, standard/daylight season and clock method. Do not convert missing match counts into assumed safe facts.
2. Treat comparisons against the currently parsed `acceptance_datetime` as **candidate timing discrepancies** until its source convention is verified. A bounded primary-source comparison of stored JSON fields with authoritative filing headers across seasons/source cohorts can establish which conversion is justified; it cannot be replaced by guessing that all `Z` values share a bug or a correct convention. Record evidence and leave unresolved cohorts on the date policy.
3. For validated timestamps, count accessions and facts with old 22:00 availability before acceptance, then measure how many selected standardized/derived/daily states cross a decision cutoff. Raw mismatch counts are not the same as changed published values. Sample concrete accessions and report both denominator and unresolved counts. No measured production occurrence is asserted until this exists.
4. Add one focused verification set when implementing: the winter 22:15 example; an ordinary early filing; a verified after-cutoff acceptance; a summer case; weekend/holiday daily selection; missing/conflicting timestamp matches; corrected-date disagreement; and an earlier RDQ that cannot accelerate a later fact. Check raw-point as-of readers, direct standardized values, derived events and daily selection on both sides of the cutoff, plus deterministic scoped rebuild behavior. Keep existing P1/AF1 arithmetic expectations unchanged apart from intended eligibility dates.
5. Reconcile raw counts/receipts unchanged; report shifted event counts, actual changed daily selections, effective-policy coverage and unresolved timing cases. Only after that evidence should publication language claim the implemented clock policy. Neither passing these checks nor storing acceptance timestamps establishes historical vendor/local-observation vintage completeness.

Disposition: **a downstream effective-clock correction is warranted for exact acceptance-based PIT claims; live prevalence is unmeasured.** The date-only fallback is a bounded conservative production option with stated limits. This report authorizes no implementation, runtime work, or interruption of the active source build.
