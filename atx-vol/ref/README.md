# ATX earnings reference

`atx-earnings-ref` builds and maintains a SQLite earnings-event database for US
equities. The initial universe is the S&P 500, reconstructed over the trailing two
years from Wikipedia's current constituent and change tables. Nasdaq's public
calendar supplies historical and forward earnings estimates; confirmed company-IR
or other open data can be appended through the CSV adapter.

The database is point-in-time (PIT) by construction. Source responses are immutable
atomic snapshots. A correction or deletion creates a later snapshot; failed requests
never replace the last good snapshot. `as_of_date` means the portfolio/reference
date, while `known_at` means the UTC timestamp at which the information was known.
Those two axes are intentionally separate.

## Install

```powershell
cd C:\atx\atx-vol\ref
python -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -e ".[dev]"
```

## Build the database

The full initial command fetches calendar dates from two years ago through one year
ahead, then materializes one row per `(as_of_date, symbol)` for the trailing two-year
S&P 500 history:

```powershell
atx-earnings --db data\earnings.sqlite backfill
```

This is intentionally polite but request-heavy because the Nasdaq endpoint is
date-scoped. It is restart-safe at the database level: completed and failed fetches
remain auditable, and reruns append newer source knowledge. For a short smoke run:

```powershell
atx-earnings --db data\smoke.sqlite backfill `
  --today 2026-07-11 --history-days 3 --horizon-days 10 --request-delay 0
```

The daily job refreshes the forward year plus a trailing correction window and
rebuilds that historical slice as a new PIT version:

```powershell
atx-earnings --db data\earnings.sqlite daily `
  --revision-lookback-days 30 --horizon-days 365
```

Use `--revision-lookback-days 730` when a daily full-history reconciliation is worth
the additional upstream requests. A more conservative operation is a 30-day daily
window plus a weekly/monthly 730-day run.

Windows Task Scheduler can execute this action daily (adjust Python and database
paths to absolute paths):

```text
Program:  C:\atx\atx-vol\ref\.venv\Scripts\atx-earnings.exe
Arguments: --db C:\atx\atx-vol\ref\data\earnings.sqlite daily
Start in: C:\atx\atx-vol\ref
```

The command returns exit code `0` for success, `2` for a partial run with one or more
failed calendar dates, and `1` for a fatal error. Inspect operational state with:

```powershell
atx-earnings --db data\earnings.sqlite status
```

## Confirmed dates and alternate open sources

Nasdaq describes its calendar as expected dates derived from historical reporting
patterns, so its rows are stored as `ESTIMATED`, even when a BMO/AMC time is present.
Import explicit company announcements as confirmed observations:

```csv
symbol,event_date,session,date_status,fiscal_quarter_ending,eps_estimate,estimator_count
AAPL,2026-07-30,AMC,CONFIRMED,2026-06-30,1.42,31
MSFT,2026-07-28,AMC,CONFIRMED,2026-06-30,3.21,29
```

```powershell
atx-earnings --db data\earnings.sqlite import-csv confirmed.csv `
  --source company-ir --observed-at 2026-07-15T13:00:00Z
atx-earnings --db data\earnings.sqlite build-reference 2026-07-15 2026-07-15
```

CSV requires `symbol,event_date`. Optional fields are `source_record_key`,
`company_name`, `session`, `date_status`, `fiscal_quarter_ending`, `eps_estimate`,
`reported_eps`, `surprise_percent`, `estimator_count`, `eps_prior_year`,
`prior_year_report_date`, and `currency`. An
event-date-only row explicitly records an empty snapshot for that date.

## Querying

`earnings_reference_latest` is the wide consumer view. It exposes four forward event
slots (normally one year of quarterly events), including date, `BMO`/`AMC` session,
estimated/confirmed/reported status, fiscal period, consensus EPS, estimate count,
reported EPS, surprise percentage, source, and source observation timestamp.

```sql
SELECT as_of_date, symbol,
       event_1_date, event_1_session, event_1_status,
       event_2_date, event_2_session, event_2_status
FROM earnings_reference_latest
WHERE as_of_date = '2026-07-11' AND symbol = 'AAPL';
```

Export that view without requiring pandas:

```powershell
atx-earnings --db data\earnings.sqlite export latest.csv --date 2026-07-11
```

Python access supports PIT raw-event and membership queries:

```python
from datetime import date
from atx_earnings import EarningsDatabase

with EarningsDatabase("data/earnings.sqlite") as db:
    events = db.events_between(
        date(2026, 7, 1), date(2027, 7, 1),
        known_at="2026-07-11T16:00:00Z",
        symbols=["AAPL"],
    )
    historical_wide_rows = db.iter_reference_as_known_at(
        "2026-07-11T16:00:00Z",
        as_of_date=date(2026, 7, 11),
        symbol="AAPL",
    )
```

## PIT rules and limitations

- A historical backfill fetched today is knowledge observed today. It is not
  backdated to pretend the information was available historically.
- Later empty snapshots represent removals, so event-date moves and source deletions
  reconstruct correctly at every `known_at` timestamp.
- Raw payloads, parse results, errors, payload hashes, and job runs are retained for
  reproducibility and source-change diagnostics.
- Wikipedia reconstructs effective S&P 500 membership dates, but the source snapshot
  itself is only known from its actual fetch timestamp.
- Nasdaq is an open web source rather than a contractual data API. Monitor partial
  jobs and comply with upstream terms and rate limits. For production trading, add a
  licensed or company-IR adapter while retaining the same snapshot interface.

## Development

```powershell
pytest
ruff check src tests
```
