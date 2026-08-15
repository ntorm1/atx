from __future__ import annotations

import json
import zipfile
from pathlib import Path

from atx_db.sec_submissions import SecSubmissionsBulkDataset, SecSubmissionsBulkOptions


def _write_bulk_zip(path: Path) -> Path:
    main_one = {
        "cik": "1",
        "name": "Alpha Corp",
        "tickers": ["ALPH"],
        "filings": {
            "recent": {
                "accessionNumber": ["0000000001-24-000001", "0000000001-24-000002"],
                "filingDate": ["2024-02-01", "2024-03-01"],
                "reportDate": ["2023-12-31", ""],
                "acceptanceDateTime": [
                    "2024-02-01T16:30:00.000Z",
                    "2024-03-01T09:15:00.000Z",
                ],
                "act": ["34", "34"],
                "form": ["10-K", "4"],
                "fileNumber": ["001-00001", "001-00001"],
                "filmNumber": ["24000001", "24000002"],
                "items": ["", ""],
                "size": [1024, 256],
                "isXBRL": [1, 0],
                "isInlineXBRL": [1, 0],
                "primaryDocument": ["alpha-10k.htm", "form4.xml"],
                "primaryDocDescription": ["10-K", "FORM 4"],
            },
            "files": [
                {
                    "name": "CIK0000000001-submissions-001.json",
                    "filingCount": 1,
                    "filingFrom": "2014-01-01",
                    "filingTo": "2014-12-31",
                }
            ],
        },
    }
    # History members are columnar at the top level and omit primaryDocDescription.
    history_one = {
        "accessionNumber": ["0000000001-14-000001"],
        "filingDate": ["2014-05-01"],
        "reportDate": ["2014-03-31"],
        "acceptanceDateTime": ["2014-05-01T12:00:00.000Z"],
        "act": ["34"],
        "form": ["10-Q"],
        "fileNumber": ["001-00001"],
        "filmNumber": ["14000001"],
        "items": [""],
        "size": [512],
        "isXBRL": [1],
        "isInlineXBRL": [0],
        "primaryDocument": ["alpha-10q.htm"],
    }
    main_two = {
        "cik": "2",
        "name": "Beta Corp",
        "tickers": [],
        "filings": {
            "recent": {
                "accessionNumber": ["0000000002-24-000001"],
                "filingDate": ["2024-04-01"],
                "reportDate": [""],
                "acceptanceDateTime": ["2024-04-01T10:00:00.000Z"],
                "act": ["34"],
                "form": ["4"],
                "fileNumber": ["001-00002"],
                "filmNumber": ["24000003"],
                "items": [""],
                "size": [128],
                "isXBRL": [0],
                "isInlineXBRL": [0],
                "primaryDocument": ["form4.xml"],
                "primaryDocDescription": ["FORM 4"],
            },
            "files": [],
        },
    }
    zip_path = path / "submissions.zip"
    with zipfile.ZipFile(zip_path, "w") as archive:
        archive.writestr("CIK0000000001.json", json.dumps(main_one))
        archive.writestr("CIK0000000001-submissions-001.json", json.dumps(history_one))
        archive.writestr("CIK0000000002.json", json.dumps(main_two))
        archive.writestr("placeholder.txt", "not a submissions member")
    return zip_path


def test_bulk_load_reads_recent_and_history_with_form_filter(tmp_store, tmp_path) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO sec_company_tickers (cik,ticker,title,security_id)
        VALUES ('0000000001','ALPH','ALPHA CORP','SEC-CIK-0000000001')
        """
    )
    zip_path = _write_bulk_zip(tmp_path)

    result = SecSubmissionsBulkDataset().load(
        tmp_store,
        SecSubmissionsBulkOptions(zip_path=zip_path, run_id="bulk-test-1"),
    )

    assert result.rows_loaded == 2
    rows = tmp_store.con.execute(
        """
        SELECT cik, accession_number, form, security_id, source_url, run_id,
               primary_doc_description
        FROM sec_submissions
        ORDER BY accession_number
        """
    ).fetchall()
    assert len(rows) == 2
    by_accession = {row[1]: row for row in rows}

    recent = by_accession["0000000001-24-000001"]
    assert recent[0] == "0000000001"
    assert recent[2] == "10-K"
    assert recent[3] == "SEC-CIK-0000000001"
    assert "CIK0000000001.json" in recent[4]
    assert recent[5] == "bulk-test-1"
    assert recent[6] == "10-K"

    history = by_accession["0000000001-14-000001"]
    assert history[2] == "10-Q"
    assert history[3] == "SEC-CIK-0000000001"
    assert "CIK0000000001-submissions-001.json" in history[4]
    assert history[6] is None


def test_bulk_load_is_idempotent_on_replay(tmp_store, tmp_path) -> None:
    zip_path = _write_bulk_zip(tmp_path)
    options = SecSubmissionsBulkOptions(zip_path=zip_path, run_id="bulk-test-2")

    first = SecSubmissionsBulkDataset().load(tmp_store, options)
    second = SecSubmissionsBulkDataset().load(tmp_store, options)

    assert first.rows_loaded == 2
    assert second.rows_loaded == 2
    count = tmp_store.con.execute("SELECT count(*) FROM sec_submissions").fetchone()[0]
    assert count == 2


def test_bulk_load_supports_cik_scope_and_no_form_filter(tmp_store, tmp_path) -> None:
    zip_path = _write_bulk_zip(tmp_path)

    result = SecSubmissionsBulkDataset().load(
        tmp_store,
        SecSubmissionsBulkOptions(
            zip_path=zip_path,
            forms=None,
            ciks=("2",),
            run_id="bulk-test-3",
        ),
    )

    assert result.rows_loaded == 1
    rows = tmp_store.con.execute(
        "SELECT cik, form, security_id FROM sec_submissions"
    ).fetchall()
    assert rows == [("0000000002", "4", "SEC-CIK-0000000002")]
