"""C9 filing-sourced identity evidence: one end-to-end PIT/integrity fixture test.

Fixture filings go through the real path -- budgeted fetcher (fake session),
parser, evidence builder, vendor-line join -- into a ``security_identity_evidence``
table created by the 0327 DDL itself (its CHECK constraints apply).
"""

from __future__ import annotations

import datetime as dt

import duckdb
import pytest

from atx_db import historical_identity_sources as his
from atx_db.historical_identity import ACCEPTANCE_SQL_B, audit_identity_evidence
from atx_db.migrations.bodies_0327 import create_historical_identity_tables

CIK = "0000123456"
OTHER_CIK = "0000999999"
NOTICE_XML = b"""<?xml version="1.0"?>
<notificationOfRemoval>
  <schemaVersion>X0203</schemaVersion>
  <exchange><cik>0001354457</cik><entityName>NASDAQ Stock Market LLC</entityName></exchange>
  <issuer><cik>0000123456</cik><entityName>ACME WIDGETS INC</entityName><fileNumber>001-11111</fileNumber></issuer>
  <descriptionClassSecurity>Common Stock, par value $0.01 per share</descriptionClassSecurity>
  <ruleProvision>17 CFR 240.12d2-2(a)(3)</ruleProvision>
  <signatureData><signatureDate>2015-06-01</signatureDate></signatureData>
</notificationOfRemoval>"""
REGISTRATION_HTML = b"""<html><body>
<p>FORM 8-A</p><p>ACME WIDGETS INC</p><p>(Exact name of registrant as specified in its charter)</p>
<p>Securities to be registered pursuant to Section 12(b) of the Act:</p>
<table><tr><td>Title of each class to be so registered</td><td>Name of each exchange on which each class is to be
registered</td></tr><tr><td>Common Stock, par value $0.01 per share</td><td>The NASDAQ Stock Market LLC</td></tr></table>
<p>If this form relates to the registration of a class of securities pursuant to Section 12(b)...</p>
<p>Item 1. The Common Stock has been approved for listing under the symbol &ldquo;ACME&rdquo;.</p>
</body></html>"""


class _Response:
    def __init__(self, payload: bytes) -> None:
        self.status_code, self.headers, self._payload = 200, {"Content-Type": "text/html"}, payload

    def iter_content(self, chunk_size: int):
        yield self._payload

    def close(self) -> None:
        pass


class _Session:
    def __init__(self, documents: dict[str, bytes]) -> None:
        self.documents, self.calls = documents, []

    def get(self, url: str, headers: dict[str, str], timeout: float, stream: bool) -> _Response:
        self.calls.append((url, headers["User-Agent"]))
        return _Response(self.documents[url])


@pytest.fixture
def con():
    connection = duckdb.connect(":memory:", config={"memory_limit": "256MB", "threads": 1})
    create_historical_identity_tables(connection)
    yield connection
    connection.close()


def test_filing_evidence_is_point_in_time_labelled_and_schema_valid(con, tmp_path):
    notice_url = his.archive_document_url(CIK, "0001354457-15-000100", "primary_doc.xml")
    registration_url = his.archive_document_url(CIK, "0001193125-09-000200", "d8a12b.htm")
    session = _Session({notice_url: NOTICE_XML, registration_url: REGISTRATION_HTML})

    # RX5 contract: approved agent only, rate floor, cached responses never re-spend the budget.
    with pytest.raises(ValueError):
        his.SecDocumentFetcher(tmp_path / "C9-cache", user_agent="someone else@example.org")
    fetcher = his.SecDocumentFetcher(tmp_path / "C9-cache", budget=2, session=session, sleep=lambda _s: None)
    notice = fetcher.fetch(notice_url)
    registration = fetcher.fetch(registration_url)
    assert fetcher.fetch(notice_url).from_cache and len(session.calls) == 2
    assert {agent for _, agent in session.calls} == {his.USER_AGENT}
    with pytest.raises(his.FetchBudgetExhausted):
        fetcher.fetch(his.archive_document_url(CIK, "0001354457-15-000101", "primary_doc.xml"))

    # Exact acceptance stamp (16:15 EDT) -> verified, available at the FC1 floor (filed + 46h).
    notice_ref = his.FilingRef(CIK, "0001354457-15-000100", "25-NSE", dt.date(2015, 6, 1),
                               "2015-06-01T20:15:00.000Z", "xslF25X02/primary_doc.xml", notice_url,
                               notice.sha256, notice.fetched_at)
    # Legacy midnight-Eastern stamp is a date, not a publication time -> modeled.
    registration_ref = his.FilingRef(CIK, "0001193125-09-000200", "8-A12B", dt.date(2009, 8, 10),
                                     "2009-08-10T04:00:00.000Z", "d8a12b.htm", registration_url,
                                     registration.sha256, registration.fetched_at)
    rows = his.document_evidence(notice_ref, his.parse_document(notice.read(), "25-NSE", "primary_doc.xml"))
    rows += his.document_evidence(registration_ref, his.parse_document(registration.read(), "8-A12B", "d8a12b.htm"))
    facts = {(row["fact_kind"], row["native_key"], row["valid_from"]): row for row in rows}

    removal = facts[("delisting_effective", f"{CIK}|common", dt.date(2015, 6, 11))]
    assert removal["evidence_status"] == "inferred"  # rule date (filed + 10d), not stated in the notice
    assert removal["source_published_at"] == dt.datetime(2015, 6, 1, 20, 15)
    assert removal["available_at"] == dt.datetime(2015, 6, 2, 22, 0)
    assert removal["availability_status"] == "verified"
    listed = facts[("primary_listing", f"{CIK}|common", dt.date(2015, 6, 1))]
    assert listed["evidence_status"] == "verified_dated" and listed["valid_to"] == dt.date(2015, 6, 2)
    registered = facts[("primary_listing", f"{CIK}|common", dt.date(2009, 8, 10))]
    assert registered["valid_to"] is None and '"primary_mic": "XNAS"' in registered["value_json"]
    assert registered["source_published_at"] is None and registered["availability_status"] == "modeled"
    assert registered["available_at"] == dt.datetime(2009, 8, 11, 22, 0)
    symbol = facts[("symbol_mapping", "XNAS:ACME", dt.date(2009, 8, 10))]
    assert symbol["evidence_status"] == "verified_dated" and symbol["cik"] == CIK

    # Vendor lines: 777 traded ACME from the file start until the removal; 888 reused ACME in 2020.
    bars = tmp_path / "bars.parquet"
    con.execute(
        f"""COPY (
            SELECT 777::BIGINT AS securityID, d::DATE AS tradingDate, 'ACME' AS ticker_tk
            FROM range(DATE '2012-03-26', DATE '2015-05-30', INTERVAL 1 DAY) t(d)
            UNION ALL SELECT 888, d::DATE, 'ACME' FROM range(DATE '2020-01-02', DATE '2021-01-02', INTERVAL 1 DAY) t(d)
            UNION ALL SELECT 999, d::DATE, 'OTHR' FROM range(DATE '2012-03-26', DATE '2026-09-19', INTERVAL 1 DAY) t(d)
        ) TO '{bars.as_posix()}' (FORMAT parquet)"""
    )
    lines = his.stage_vendor_lines(con, bars)
    assert lines["ceased_lines"] == 2
    for statement in his.STAGING_DDL:
        con.execute(statement)
    con.execute(
        "INSERT INTO c9_sec_filings (cik, accession, form, filing_date) VALUES (?, ?, '25-NSE', DATE '2015-06-01'),"
        " (?, ?, '8-A12B', DATE '2009-08-10')",
        [CIK, "0001354457-15-000100", CIK, "0001193125-09-000200"],
    )
    his.insert_evidence(con, rows)
    loaded = dt.datetime(2026, 9, 25)
    links = his.line_link_evidence(con, observed_at=loaded)
    assert [(row["security_id"], row["cik"], row["evidence_status"]) for row in links] == [
        ("TBLTICKERHISTORY-777", CIK, "reconstructed")
    ]
    link = links[0]
    assert link["available_at"] == registered["available_at"]  # unknowable before its anchor filing
    assert (link["valid_from"], link["valid_to"]) == (dt.date(2012, 3, 26), dt.date(2015, 5, 30))
    assert '"terminal_notice_date": "2015-06-01"' in link["value_json"]
    his.insert_evidence(con, links)

    # A conflicting anchor (another issuer, same symbol, same span) is kept, never linked.
    other = dict(symbol, cik=OTHER_CIK, evidence_id="C9-conflict-anchor", source_locator="fixture#other")
    his.insert_evidence(con, [other])
    statuses = sorted(row["evidence_status"] for row in his.line_link_evidence(con, observed_at=loaded))
    assert statuses == ["conflicting", "conflicting"]

    assert [con.execute(sql).fetchone()[0] for sql in ACCEPTANCE_SQL_B] == [0, 0]
    audit = audit_identity_evidence(con)
    assert {rule: count for rule, count in audit.items() if count} == {}
