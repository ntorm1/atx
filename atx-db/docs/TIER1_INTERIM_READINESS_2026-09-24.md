# Live warehouse readiness: interim measurement

The warehouse is not ready for a Tier-1 production release. A read-only
inventory started at **2026-09-24 00:46:51 UTC**, using the fixed information
snapshot **2026-09-20 22:00 UTC**. Raw facts and prices are present, but the
canonical downstream tables below have not been materialized. This measures
the current gap; it does not certify the implementation or source completion.

The [aggregate artifact](measurements/tier1-interim-2026-09-24.json) records
the source commit, measurement-script and private-report hashes, runtime
receipt, and numeric results. It deliberately excludes raw error text, stored
diagnostic JSON and source parameters. The full private report remains in
`.superpowers/sdd/tier1-parity/tier1-interim-sep24-1.json`.

| Retained table | Rows | Distinct security IDs |
| --- | ---: | ---: |
| `sec_company_facts` | 47,941,000 | 12,959 |
| `equity_daily_bars` | 31,959,271 | 34,251 |
| `exchange_listings` | 45,820 | 36,762 |
| `fundamental_standardized` | 0 | 0 |
| `derived_metric_values` | 0 | 0 |
| `market_daily_metrics` | 0 | 0 |
| `equity_price_metrics` | 0 | 0 |
| `listing_status_intervals` | 0 | 0 |
| `universe_us_listed_membership` | 0 | 0 |
| `delisting_events` | 0 | 0 |
| `delisting_terminal_returns` | 0 | 0 |
| `forward_returns_survivorship_safe` | 0 | 0 |

These are whole retained-table counts. Accounting owners, vendor price
identifiers and listed trading securities have different grains; their counts
must not be interpreted as a qualified current US common-equity universe.

Prices span 2012-03-26 through 2026-09-18. All price rows have finite, non-NULL
close and adjusted-close values and recorded availability by the cutoff.
All 31,959,271 rows lack `split_factor`; non-NULL adjusted prices alone do not
prove corporate-action conventions or total-return lineage. The listing
table has **zero rows with an exchange/MIC**, and zero venue observations
known by their interval start. Its row count is not historical listing proof.

The annual item gate has no recorded coverage or cohort measurement for any
completed year from 2015 through 2025. The unchanged requirement is at least
110 items meeting 90% coverage in every completed year, using the defined
3,000-name annual cohort. Zero items currently have the required evidence;
this is missing measurement, not an observed 0% coverage estimate.

All **12 provider SLO entries** have no recorded snapshot. The artifact retains
each schema's configured count and freshness requirements; no conditions were
promoted. The quality inventory has **379 reporting entries**: 375 have no
stored result, and four results predate a newer upstream attempt. Those four
contain three historical passes and one warning, neither of which certifies
the current source state. This command did not execute quality checks.

CF1 has one stored feature run and no recorded evaluation. The FQ1 panel and
FQ2 evaluation tables are absent, matching the pending research migrations.
There is no live fundamental desk screen, forward-return decile result,
statistical candidate or production eligibility result from this inventory.

The measurement completed in **28.411 seconds**, with DuckDB limited to 1 GB
and one thread, 2 GB spill, and a **2 GiB process-tree cap**. Its measured peak
was **1.538 GiB**. It used a single read-only transaction and external access
disabled; warehouse file size and modification time did not change. The
unchanged host memory stops remained enforced. This successful bounded read
does not establish capacity for a long source or schema job.

The next production steps remain: complete schema/numeric verification under
the sustained headroom policy, apply the pending governed migrations, finish
the full CompanyFacts and submissions archives, run the CVX source acceptance
wave, and force the full-universe activation ladder from `statement_points`.
Then execute the actual item/provider/quality measurements, validated desk
reads and research evaluations before assessing release eligibility. See the
[production runbook](PRODUCTION_RUNBOOK.md); no release or merge gate is waived.
