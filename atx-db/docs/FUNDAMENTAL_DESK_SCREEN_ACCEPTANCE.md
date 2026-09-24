# Fundamental desk screen from a validated FQ1 build

The desk readout answers a descriptive question: at the latest observed FQ1
decision session in a selected completed build, which historically qualified
US common names have positive diluted quarterly EPS year-over-year growth and
positive operating-margin change? It reports those two raw metrics, total
accruals, net debt / EBITDA, market cap, earnings yield, and CFO / EV yield.
It makes no return or alpha claim.

Run the [reader](../scripts/read_fundamental_desk_screen.py) after the build
writer releases the existing warehouse, under the external memory guard:

```powershell
python atx-db/scripts/read_fundamental_desk_screen.py --db-path C:/path/warehouse.duckdb --build-run-id completed_run_id --report-as-of 2026-09-20 --output-json C:/path/fresh-desk-report.json
```

All four arguments are required. The output must be a fresh JSON path. The
reader opens the database read-only with 256 MB, one thread, a 2 GB spill cap,
and external access disabled. It performs no migration or write to the
warehouse. It validates the selected FQ1 panel's manifest, calendar, rows,
proofs, and digest with the public
`validate_fundamental_signal_panel` in the same transaction as the screen.
It then requires the **exact five default frozen signal specifications**,
the default 200-day accounting freshness, and a build snapshot no later than
the requested report as-of date. Absent, incomplete, altered, or malformed
runs produce a controlled error and no output file.

The [prepared SQL](../sql/research/fundamental-desk-screen-acceptance.sql)
is an inspection contract requiring that prior validation. Running the SQL
alone does not certify a panel digest. The latest decision session comes
from the selected build's coverage rows, not from the report's calendar date.
The report records the report as-of date, selected decision date, and that
session's 22:00 UTC decision cutoff separately. A Sunday report can select
Friday's decision and does not include later weekend disclosures in that
decision. The issuer `content_as_of` API is the path for investigating those
later facts.

The FQ1 panel already resolves historical common membership, dated CIK
qualification, selected issuer owners, and dependency proofs. The screen
reads the four single-signal values and their selected input rows by the
same run ID and decision date. The **qualified raw input values** supply
accruals and net debt / EBITDA; their `low_*` scores reverse the sign as
research hypotheses. A raw number is displayed only when its selected
input reason is `valid`, lineage is `qualified`, and the value is finite.
Rejected retained raw inputs display as NULL with their reasons and state
IDs. FQ1 score eligibility is reported separately: a final observed
session may have `missing_next_session` even while its accounting inputs
are qualified for this descriptive readout. No future entry session is
inferred.

The market join uses the FQ1 trading `security_id` and the **same decision
session**. It considers rows from the pinned daily market source with
`available_at <= decision cutoff` and `as_of_date <= decision date`,
selects the latest whole row, and only then checks close, market cap,
earnings yield, and CFO / EV independently. It does not fill a missing
field from an older revision or a prior session. The displayed symbol comes
from that market row. This is same-session visibility, not independent
certification of price or market source lineage.

Every successful read has one aggregate diagnostic row, including an empty
cohort. No historical common members and no cohort rows are explicit
missing-data statuses. Aggregate counts cover the complete selected cohort;
the deterministic preview is at most 999 names, for at most 1000 SQL rows.
The JSON artifact records the SQL and validated panel hashes, selection,
row and byte caps, source limits, and whether the preview was truncated.
The 2 MB transfer limit is checked in DuckDB before JSON rows are fetched;
the final 2 MB artifact cap also fails closed. This script has no live result asserted
in this document; a passing name count must come from an actual guarded read.
