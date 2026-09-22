"""Read-only planning evidence for the full earnings-release source queue."""

import datetime as dt
import json
from pathlib import Path

import duckdb


output = Path(__file__).with_name("earnings-source-queue-before-submissions-resume.json")
if output.exists():
    raise SystemExit("Refusing to overwrite source planning evidence")

with duckdb.connect(
    "C:/atx/atx-db/data/warehouse.duckdb",
    read_only=True,
    config={"memory_limit": "1GB", "threads": "1", "preserve_insertion_order": "false"},
) as connection:
    connection.execute("SET max_temp_directory_size='2GB'")
    rows = connection.execute(r"""
        WITH candidates AS (
            SELECT cik, accession_number, filing_date, report_date,
                   acceptance_datetime
            FROM sec_submissions
            WHERE upper(trim(form)) = '8-K'
              AND regexp_matches(coalesce(items, ''), '(^|[^0-9])2\.02([^0-9]|$)')
            QUALIFY row_number() OVER (
                PARTITION BY cik, accession_number
                ORDER BY acceptance_datetime DESC NULLS LAST,
                         filing_date DESC NULLS LAST,
                         source_loaded_at DESC NULLS LAST, source_url
            ) = 1
        )
        SELECT year(filing_date) AS filing_year, count(*) AS candidates,
               count(DISTINCT cik) AS ciks,
               count(*) FILTER (WHERE report_date IS NULL) AS missing_report_date,
               count(*) FILTER (WHERE filing_date > DATE '2026-09-20') AS after_snapshot
        FROM candidates GROUP BY year(filing_date) ORDER BY filing_year
    """).fetchall()
    submissions = connection.execute(
        "SELECT count(*), count(DISTINCT cik), min(filing_date), max(filing_date) FROM sec_submissions"
    ).fetchone()
    previous = connection.execute(
        "SELECT run_id, status, rows_loaded, finished_at FROM dataset_runs "
        "WHERE run_id = ? AND dataset_id = 'sec_submissions'",
        ["04cf947d-53bb-49b7-a276-b3c74a2a52c8"],
    ).fetchall()

result = {
    "observed_at_utc": dt.datetime.now(dt.UTC).isoformat(),
    "as_of_date": "2026-09-20",
    "read_only": True,
    "scope": "Current retained submissions before verified full archive resume; incomplete source queue",
    "submissions_rows_ciks_min_max_filing_date": submissions,
    "resume_predecessor": previous,
    "candidate_columns": ["filing_year", "candidates", "ciks", "missing_report_date", "after_snapshot"],
    "candidate_rows": rows,
    "candidate_total": sum(row[1] for row in rows),
    "network_requests_made": 0,
    "interpretation": "Loader initially attempts one filing-index request per nonterminal candidate; an unambiguous uncached EX-99 adds a document request. This is neither actual requests nor a completed denominator.",
}
with output.open("x", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2, default=str)
    handle.write("\n")
print(json.dumps(result, default=str), flush=True)
