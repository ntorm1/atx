"""Focused, network-free contracts for SEC Item 2.02 reported EPS evidence."""

from __future__ import annotations

import datetime as dt
import json

import pytest

from atx_db import press_release
from atx_db.migrations.bodies_0320 import _sec_earnings_release_receipts
from atx_db.press_release import (
    SEC_FILING_DATE_CLOCK_POLICY,
    SecEarningsReleaseOptions,
    _daily_sec_clock,
    _ex99_documents,
    _explicit_sec_clock,
    extract_reported_gaap_diluted_eps,
    extract_reported_gaap_eps,
    refresh_sec_earnings_release_facts,
)


class _Response:
    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.headers = {"Content-Length": str(len(payload))}

    def raise_for_status(self) -> None:
        return None

    def iter_content(self, chunk_size: int):
        yield self.payload

    def close(self) -> None:
        return None


class _Session:
    def __init__(self, responses: list[bytes]) -> None:
        self.responses = list(responses)

    def get(self, _url: str, *, timeout: float, stream: bool) -> _Response:
        assert timeout > 0 and stream
        return _Response(self.responses.pop(0))


INDEX_DIRECTORY = "https://www.sec.gov/Archives/edgar/data/93410/000009341026000019"
COMPACT_FILING_INDEX = """
<table class="tableFile" summary="Document Format Files">
  <tr><th>Seq</th><th>Description</th><th>Document</th><th>Type</th><th>Size</th></tr>
  <tr><td>2</td><td>EX-99.1</td>
      <td><a href="/Archives/edgar/data/93410/000009341026000019/ex99.htm">ex99.htm</a></td>
      <td>EX-99.1</td><td>100</td></tr>
  <tr class="evenRow"><td>&nbsp;</td><td>Complete submission text file</td>
      <td><a href="/Archives/edgar/data/93410/000009341026000019/0000093410-26-000019.txt">0000093410-26-000019.txt</a></td>
      <td>&nbsp;</td><td>766161</td></tr>
</table>
"""


COMPACT_ATTACHMENT = """
<div style="text-align:center"><font style="font-weight:700">Issuer Reports Fourth Quarter 2025 Results</font></div>
<p>Compared with fourth quarter 2024, the issuer reports fourth quarter 2025 results.</p>
<table>
  <tr><th colspan="3"></th><th colspan="9">Three Months Ended December 31,</th>
      <th colspan="3"></th><th colspan="9">Year Ended December 31,</th></tr>
  <tr><th colspan="3"></th><th colspan="3">2025</th><th colspan="3"></th>
      <th colspan="3">2024</th><th colspan="3"></th><th colspan="3">2025</th>
      <th colspan="3"></th><th colspan="3">2024</th></tr>
  <tr><td colspan="24">PER SHARE OF COMMON STOCK</td></tr>
  <tr><td colspan="6">Net income attributable to the corporation</td><td colspan="18"></td></tr>
  <tr><td colspan="3">- Basic</td><td>$</td><td>1.39</td><td></td><td colspan="3"></td>
      <td>$</td><td>1.85</td><td></td><td colspan="3"></td>
      <td>$</td><td>6.65</td><td></td><td colspan="3"></td><td>$</td><td>9.76</td><td></td></tr>
  <tr><td colspan="3">- Diluted</td><td>$</td><td>1.39</td><td></td><td colspan="3"></td>
      <td>$</td><td>1.84</td><td></td><td colspan="3"></td>
      <td>$</td><td>6.63</td><td></td><td colspan="3"></td><td>$</td><td>9.72</td><td></td></tr>
  <tr><td colspan="24">Adjusted earnings per share:</td></tr>
  <tr><td colspan="3">- Diluted</td><td>$</td><td>1.52</td><td></td><td colspan="3"></td>
      <td>$</td><td>2.06</td><td></td><td colspan="3"></td>
      <td>$</td><td>7.29</td><td></td><td colspan="3"></td><td>$</td><td>10.05</td><td></td></tr>
</table>
"""


def test_comparative_first_and_currency_spacer_columns_bind_current_quarter() -> None:
    end = dt.date(2025, 12, 31)
    assert press_release._reported_fiscal_quarter(COMPACT_ATTACHMENT, end) == (2025, "Q4")
    fact, reason = extract_reported_gaap_diluted_eps(
        COMPACT_ATTACHMENT, period_end=None, filed_on=dt.date(2026, 1, 30),
        fiscal_quarter=(2025, "Q4"),
    )
    assert reason is None
    assert fact is not None
    assert fact["period_end"] == end
    assert fact["value"] == 1.39
    assert fact["prior_year_quarter_value"] == 1.84
    assert fact["column_number"] == 4  # dollar sign is column 3
    assert fact["row_lineage"] == (
        "PER SHARE OF COMMON STOCK", "Net income attributable to the corporation", "- Diluted"
    )


def test_competing_same_year_fiscal_labels_without_result_title_are_ambiguous() -> None:
    document = "<p>Third Quarter 2025. Fourth Quarter 2025.</p>"
    assert press_release._reported_fiscal_quarter(document, dt.date(2025, 12, 31)) is None
    cross_year = "<p>First Quarter Fiscal 2025. First Quarter Fiscal 2026.</p>"
    assert press_release._reported_fiscal_quarter(cross_year, dt.date(2025, 4, 27)) is None
    inverted_comparative = (
        "<p>First quarter fiscal 2025 results compared with first quarter fiscal 2026.</p>"
    )
    assert press_release._reported_fiscal_quarter(inverted_comparative, dt.date(2025, 4, 27)) is None


def test_fiscal_year_ahead_comparative_first_thirteen_week_quarter() -> None:
    document = """
    <h1>Issuer Reports First Quarter Fiscal 2026 Results</h1>
    <p>Compared with first quarter fiscal 2025, the issuer reports first quarter fiscal 2026 results.</p>
    <table><tr><th></th><th>13 Weeks Ended April 27,</th></tr>
    <tr><th></th><th>2025</th></tr>
    <tr><td colspan="2">Earnings per share:</td></tr>
    <tr><td>- Basic</td><td>1.41</td></tr>
    <tr><td>- Diluted</td><td>1.39</td></tr></table>
    """
    event_date = dt.date(2025, 5, 5)
    period_end = dt.date(2025, 4, 27)
    fiscal = press_release._reported_fiscal_quarter(document, period_end)
    assert fiscal == (2026, "Q1")
    fact, reason = extract_reported_gaap_diluted_eps(
        document, period_end=None, filed_on=event_date, fiscal_quarter=fiscal
    )
    assert reason is None
    assert fact is not None
    assert fact["period_end"] == period_end
    assert fact["value"] == 1.39
    assert fact["duration_evidence"] == "thirteen_weeks_explicit"
    assert fact["period_start"] == dt.date(2025, 1, 27)
    assert fact["duration_days"] == 91
    assert fact["fiscal_period_evidence"] == "Q1 2026"
    basic, basic_reason = extract_reported_gaap_eps(
        document, measure="basic", period_end=None, filed_on=event_date, fiscal_quarter=fiscal
    )
    assert basic_reason is None
    assert basic is not None
    assert (basic["value"], basic["measure_code"], basic["row_semantics"]) == (1.41, "EPS_BASIC", "basic")
    assert (basic["period_end"], basic["period_start"], basic["duration_days"]) == (
        period_end, dt.date(2025, 1, 27), 91
    )


def test_explicit_sec_offset_is_preserved_but_daily_clock_is_conservative() -> None:
    clock = _daily_sec_clock("2026-01-30T18:17:00-05:00", dt.date(2026, 1, 30))

    assert clock.raw_timestamp == "2026-01-30T18:17:00-05:00"
    assert clock.utc_offset == "-05:00"
    assert clock.available_at == dt.datetime(2026, 1, 31, 22, 0)
    assert clock.timezone_status == f"timestamp_offset_valid:{SEC_FILING_DATE_CLOCK_POLICY}"


def test_naive_source_timestamp_cannot_be_promoted_to_exact_utc() -> None:
    exact = _explicit_sec_clock("2026-01-30 18:17:00")
    daily = _daily_sec_clock("2026-01-30 18:17:00", dt.date(2026, 1, 30))

    assert exact.available_at is None
    assert exact.timezone_status == "timestamp_zone_unknown"
    assert daily.available_at == dt.datetime(2026, 1, 31, 22, 0)
    assert daily.timezone_status == f"timestamp_zone_unknown:{SEC_FILING_DATE_CLOCK_POLICY}"


def test_ex99_selection_rejects_multiple_documents_upstream() -> None:
    second = """<tr><td>3</td><td>EX-99.2</td>
        <td><a href="/Archives/edgar/data/93410/000009341026000019/exhibit99-2.htm">exhibit99-2.htm</a></td>
        <td>EX-99.2</td><td>101</td></tr>"""
    assert _ex99_documents(
        COMPACT_FILING_INDEX.replace("</table>", second + "</table>"), INDEX_DIRECTORY
    ) == ("ex99.htm", "exhibit99-2.htm")


def test_filing_detail_type_is_authoritative_and_href_stays_in_accession() -> None:
    # SEC directory index.json reports MIME text.gif, not the filing Type.
    directory_json = json.dumps({"directory": {"item": [
        {"name": "a12312025ex9918-k.htm", "type": "text.gif"}
    ]}})
    with pytest.raises(ValueError, match="document_format_table"):
        _ex99_documents(directory_json, INDEX_DIRECTORY)

    typed = COMPACT_FILING_INDEX.replace("ex99.htm", "a12312025ex9918-k.htm")
    wrong_type = """<tr><td>3</td><td>EX-99.2</td>
        <td><a href="/Archives/edgar/data/93410/000009341026000019/looks-like-ex99.htm">looks-like-ex99.htm</a></td>
        <td>text.gif</td><td>20</td></tr>"""
    wrong_accession = """<tr><td>4</td><td>EX-99.3</td>
        <td><a href="/Archives/edgar/data/93410/other/accession.htm">accession.htm</a></td>
        <td>EX-99.3</td><td>20</td></tr>"""
    typed = typed.replace("</table>", wrong_type + wrong_accession + "</table>")
    assert _ex99_documents(typed, INDEX_DIRECTORY) == ("a12312025ex9918-k.htm",)


@pytest.mark.parametrize("index_html", [
    COMPACT_FILING_INDEX.replace("<th>Type</th>", "<th>MIME</th>"),
    COMPACT_FILING_INDEX.replace("</table>", ""),
    COMPACT_FILING_INDEX.replace("<td>EX-99.1</td><td>100</td>", "<td>EX-99.1</td>"),
])
def test_incomplete_or_invalid_document_table_is_retryable(index_html: str) -> None:
    with pytest.raises(ValueError):
        _ex99_documents(index_html, INDEX_DIRECTORY)


def test_extracts_one_gaap_diluted_eps_with_aligned_three_month_column() -> None:
    document = """
    <table><tr><th></th><th colspan="2">Three Months Ended December 31,</th></tr>
    <tr><th></th><th>2025</th><th>2024</th></tr>
    <tr><td>Earnings per share - diluted</td><td>1.39</td><td>1.84</td></tr></table>
    """
    fact, reason = extract_reported_gaap_diluted_eps(document, period_end=dt.date(2025, 12, 31))

    assert reason is None
    assert fact is not None
    assert fact["value"] == 1.39
    assert fact["prior_year_quarter_value"] == 1.84
    assert fact["column_number"] == 1


def test_adjusted_or_continuing_operations_eps_is_not_reported_gaap_diluted_eps() -> None:
    document = """
    <table><tr><th></th><th>Three Months Ended December 31,</th></tr>
    <tr><th></th><th>2025</th></tr>
    <tr><td>Adjusted diluted earnings per share from continuing operations</td><td>4.20</td></tr></table>
    """
    fact, reason = extract_reported_gaap_diluted_eps(document, period_end=dt.date(2025, 12, 31))

    assert fact is None
    assert reason == "rejected_non_gaap_or_adjusted"


def test_span_grid_selects_current_quarter_diluted_leaf_not_annual_or_adjusted() -> None:
    """The CVX EX-99 shape has duplicate 2025 leaves below quarter/year groups."""

    document = """
    <p>Fourth Quarter 2025 results compared with Fourth Quarter 2024.</p>
    <table>
      <tr><th rowspan="2"></th><th colspan="2">Three Months Ended December 31,</th>
          <th colspan="2">Year Ended December 31,</th></tr>
      <tr><th>2025</th><th>2024</th><th>2025</th><th>2024</th></tr>
      <tr><td colspan="5">Net income attributable to the corporation</td></tr>
      <tr><td colspan="5">per share:</td></tr>
      <tr><td>- Basic</td><td>1.40</td><td>1.85</td><td>6.64</td><td>7.20</td></tr>
      <tr><td>- Diluted</td><td>1.39</td><td>1.84</td><td>6.63</td><td>7.18</td></tr>
      <tr><td colspan="5">Adjusted earnings per share:</td></tr>
      <tr><td>- Diluted</td><td>1.52</td><td>1.88</td><td>9.99</td><td>9.88</td></tr>
    </table>
    """
    fact, reason = extract_reported_gaap_diluted_eps(document, period_end=dt.date(2025, 12, 31))

    assert reason is None
    assert fact is not None
    assert fact["value"] == 1.39
    assert fact["prior_year_quarter_value"] == 1.84
    assert fact["column_number"] == 1
    assert "Three Months Ended" in fact["column_heading"]
    assert "Year Ended" not in fact["column_heading"]
    assert fact["row_lineage"] == (
        "Net income attributable to the corporation", "per share:", "- Diluted"
    )

    # The 8-K event date is January 30; the EX-99 table explicitly ends the
    # earnings quarter on December 31. The selected quarter comes from the
    # table's duration group and year leaf, not from the event date.
    from_event, event_reason = extract_reported_gaap_diluted_eps(
        document, period_end=None, filed_on=dt.date(2026, 1, 30),
        fiscal_quarter=(2025, "Q4"),
    )
    assert event_reason is None
    assert from_event is not None
    assert from_event["period_end"] == dt.date(2025, 12, 31)
    assert from_event["value"] == 1.39
    assert from_event["prior_year_quarter_value"] == 1.84


def test_qualified_thirteen_week_period_retains_exact_boundary_and_fiscal_evidence() -> None:
    document = """
    <table><tr><th></th><th>13 Weeks Ended July 3,</th></tr>
    <tr><th></th><th>2026</th></tr>
    <tr><td colspan="2">Earnings per share:</td></tr>
    <tr><td>- Diluted</td><td>1.25</td></tr></table>
    """
    fact, reason = extract_reported_gaap_diluted_eps(
        document, period_end=dt.date(2026, 7, 3), fiscal_quarter=(2026, "Q2")
    )

    assert reason is None
    assert fact is not None
    assert fact["duration_evidence"] == "thirteen_weeks_explicit"
    assert fact["period_start"] == dt.date(2026, 4, 4)
    assert fact["duration_days"] == 91
    assert fact["fiscal_period_evidence"] == "Q2 2026"


def test_week_period_without_fiscal_evidence_is_explicitly_rejected() -> None:
    document = """
    <table><tr><th></th><th>14 Weeks Ended July 3,</th></tr>
    <tr><th></th><th>2026</th></tr>
    <tr><td colspan="2">Earnings per share:</td></tr>
    <tr><td>- Diluted</td><td>1.25</td></tr></table>
    """
    fact, reason = extract_reported_gaap_diluted_eps(document, period_end=dt.date(2026, 7, 3))

    assert fact is None
    assert reason == "week_fiscal_quarter_missing_or_ambiguous"


def test_53_week_issuer_fourteen_week_quarter_retains_exact_boundary() -> None:
    document = """
    <p>Fourth Quarter 2026</p>
    <table><tr><th></th><th>14 Weeks Ended January 2,</th></tr>
    <tr><th></th><th>2027</th></tr>
    <tr><td colspan="2">Earnings per share:</td></tr>
    <tr><td>- Basic</td><td>1.41</td></tr>
    <tr><td>- Diluted</td><td>1.39</td></tr></table>
    """
    assert press_release._reported_fiscal_quarter(document, dt.date(2027, 1, 2)) == (2026, "Q4")
    fact, reason = extract_reported_gaap_diluted_eps(
        document, period_end=dt.date(2027, 1, 2), fiscal_quarter=(2026, "Q4")
    )

    assert reason is None
    assert fact is not None
    assert fact["value"] == 1.39
    assert fact["duration_evidence"] == "fourteen_weeks_explicit"
    assert fact["period_start"] == dt.date(2026, 9, 27)
    assert fact["duration_days"] == 98
    assert fact["fiscal_period_evidence"] == "Q4 2026"


@pytest.mark.parametrize("heading, end", [
    ("13 Weeks Ended January 1,", dt.date(2027, 1, 2)),
    ("14 Weeks Ended January 2,", dt.date(2027, 1, 1)),
])
def test_week_quarter_rejects_mismatched_document_end(heading, end) -> None:
    document = f"""
    <table><tr><th></th><th>{heading}</th></tr>
    <tr><th></th><th>2027</th></tr>
    <tr><td colspan="2">Earnings per share:</td></tr>
    <tr><td>- Diluted</td><td>1.39</td></tr></table>
    """
    fact, reason = extract_reported_gaap_diluted_eps(
        document, period_end=end, fiscal_quarter=(2026, "Q4")
    )

    assert fact is None
    assert reason == "reported_gaap_diluted_eps_not_found"


def test_accepted_receipt_rolls_back_with_fact_failure_then_resumes(tmp_store, tmp_path, monkeypatch) -> None:
    """A terminal accepted receipt cannot suppress recovery of its missing fact."""

    _sec_earnings_release_receipts(tmp_store.con)
    tmp_store.con.execute(
        """INSERT INTO sec_submissions (
               security_id, cik, accession_number, filing_date, report_date,
               acceptance_datetime_raw, form, items, source_url
           ) VALUES ('SEC-CIK-0000093410', '0000093410', '0000093410-26-000019',
                     DATE '2026-01-30', DATE '2026-01-30', '2026-01-30T18:17:00-05:00',
                     '8-K', '2.02', 'bulk-fixture')"""
    )
    index = COMPACT_FILING_INDEX.encode()
    document = COMPACT_ATTACHMENT.encode()
    options = SecEarningsReleaseOptions(cache_dir=tmp_path / "cache", run_id="atomic-retry")
    original = press_release._write_press_release_facts_frame

    def fail_fact_write(*args, **kwargs):
        raise RuntimeError("injected fact write fault")

    monkeypatch.setattr(press_release, "_write_press_release_facts_frame", fail_fact_write)
    with pytest.raises(RuntimeError, match="injected fact write fault"):
        refresh_sec_earnings_release_facts(tmp_store, options, session=_Session([index, document]))
    assert tmp_store.con.execute(
        "SELECT count(*) FROM sec_earnings_release_receipts WHERE outcome = 'accepted'"
    ).fetchone() == (0,)

    monkeypatch.setattr(press_release, "_write_press_release_facts_frame", original)
    result = refresh_sec_earnings_release_facts(tmp_store, options, session=_Session([index]))
    assert result["accepted"] == 1
    assert tmp_store.con.execute(
        "SELECT count(*) FROM sec_earnings_release_receipts WHERE outcome = 'accepted'"
    ).fetchone() == (1,)
    facts = tmp_store.con.execute(
        "SELECT measure_code, value, fiscal_year, fiscal_period, period_end, as_of_date, available_at, "
        "raw_payload_json FROM press_release_facts "
        "WHERE accession_number = '0000093410-26-000019' ORDER BY measure_code"
    ).fetchall()
    assert [(row[0], row[1]) for row in facts] == [("EPS_BASIC", 1.39), ("EPS_DILUTED", 1.39)]
    basic_payload, diluted_payload = (json.loads(row[7]) for row in facts)
    for _, _, fiscal_year, fiscal_period, period_end, as_of_date, _, _ in facts:
        assert (fiscal_year, fiscal_period) == (2025, "Q4")
        assert period_end == as_of_date == dt.date(2025, 12, 31)
    assert facts[0][6] == facts[1][6]
    # Both measures are backed by the one immutable document receipt.
    assert basic_payload["receipt_id"] == diluted_payload["receipt_id"]
    assert basic_payload["document_sha256"] == diluted_payload["document_sha256"]
    assert diluted_payload["event_report_date"] == "2026-01-30"
    assert diluted_payload["prior_year_quarter_value"] == 1.84
    assert basic_payload["prior_year_quarter_value"] == 1.85
    assert basic_payload["row_lineage"] == [
        "PER SHARE OF COMMON STOCK", "Net income attributable to the corporation", "- Basic"
    ]
    assert basic_payload["measure_outcomes"] == {"EPS_DILUTED": "accepted", "EPS_BASIC": "accepted"}


def test_fetch_failed_retries_and_corrupt_cache_is_refetched(tmp_store, tmp_path) -> None:
    _sec_earnings_release_receipts(tmp_store.con)
    tmp_store.con.execute(
        """INSERT INTO sec_submissions (
               security_id, cik, accession_number, filing_date, report_date,
               acceptance_datetime_raw, form, items, source_url
           ) VALUES ('SEC-CIK-0000093410', '0000093410', '0000093410-26-000019',
                     DATE '2026-01-30', DATE '2025-12-31', '2026-01-30T18:17:00-05:00',
                     '8-K', '2.02', 'bulk-fixture')"""
    )
    index = COMPACT_FILING_INDEX.encode()
    document = b"""
        <p>Fourth Quarter 2025</p><table>
        <tr><th></th><th>Three Months Ended December 31,</th></tr><tr><th></th><th>2025</th></tr>
        <tr><td colspan='2'>Earnings per share:</td></tr>
        <tr><td>- Diluted</td><td>1.39</td></tr></table>
    """
    options = SecEarningsReleaseOptions(cache_dir=tmp_path / "cache", run_id="cache-retry")
    first = refresh_sec_earnings_release_facts(tmp_store, options, session=_Session([index]))
    assert first["accepted"] == 0
    assert tmp_store.con.execute(
        "SELECT outcome, rejection_reason FROM sec_earnings_release_receipts"
    ).fetchall() == [("fetch_failed", "document_fetch:IndexError")]

    cache = options.cache_dir / "0000093410" / "000009341026000019" / "ex99.htm"
    cache.parent.mkdir(parents=True)
    cache.write_bytes(document[:20])  # interrupted cache write with no SHA sidecar
    second_session = _Session([index, document])
    second = refresh_sec_earnings_release_facts(tmp_store, options, session=second_session)

    assert second["accepted"] == 1
    assert second["cache_hits"] == 0
    assert second_session.responses == []
    assert cache.read_bytes() == document
    assert tmp_store.con.execute(
        "SELECT outcome FROM sec_earnings_release_receipts ORDER BY outcome"
    ).fetchall() == [("accepted",), ("fetch_failed",)]
    facts = tmp_store.con.execute(
        "SELECT measure_code, json_extract_string(raw_payload_json, '$.measure_outcomes') "
        "FROM press_release_facts"
    ).fetchall()
    assert [row[0] for row in facts] == ["EPS_DILUTED"]
    assert json.loads(facts[0][1]) == {
        "EPS_DILUTED": "accepted", "EPS_BASIC": "reported_gaap_basic_eps_not_found",
    }


def test_malformed_200_index_is_retryable_then_valid_index_accepts(tmp_store, tmp_path) -> None:
    _sec_earnings_release_receipts(tmp_store.con)
    tmp_store.con.execute(
        """INSERT INTO sec_submissions (
               security_id, cik, accession_number, filing_date, report_date,
               acceptance_datetime_raw, form, items, source_url
           ) VALUES ('SEC-CIK-0000093410', '0000093410', '0000093410-26-000019',
                     DATE '2026-01-30', DATE '2025-12-31', '2026-01-30T18:17:00-05:00',
                     '8-K', '2.02', 'bulk-fixture')"""
    )
    options = SecEarningsReleaseOptions(cache_dir=tmp_path / "cache", run_id="index-retry")
    first = refresh_sec_earnings_release_facts(
        tmp_store, options, session=_Session([b"<html><body>temporary archive error</body></html>"])
    )
    assert first["accepted"] == 0
    assert tmp_store.con.execute(
        "SELECT outcome, rejection_reason FROM sec_earnings_release_receipts"
    ).fetchall() == [("fetch_failed", "index_fetch_or_parse:ValueError")]

    second_session = _Session([COMPACT_FILING_INDEX.encode(), COMPACT_ATTACHMENT.encode()])
    second = refresh_sec_earnings_release_facts(tmp_store, options, session=second_session)
    assert second["accepted"] == 1
    assert second_session.responses == []
    assert tmp_store.con.execute(
        "SELECT outcome FROM sec_earnings_release_receipts ORDER BY outcome"
    ).fetchall() == [("accepted",), ("fetch_failed",)]
    assert tmp_store.con.execute(
        "SELECT count(*) FROM press_release_facts WHERE accession_number = '0000093410-26-000019'"
    ).fetchone() == (2,)


def test_valid_index_without_ex99_is_terminal(tmp_store, tmp_path) -> None:
    _sec_earnings_release_receipts(tmp_store.con)
    tmp_store.con.execute(
        """INSERT INTO sec_submissions (
               security_id, cik, accession_number, filing_date, report_date,
               acceptance_datetime_raw, form, items, source_url
           ) VALUES ('SEC-CIK-0000093410', '0000093410', '0000093410-26-000019',
                     DATE '2026-01-30', DATE '2025-12-31', '2026-01-30T18:17:00-05:00',
                     '8-K', '2.02', 'bulk-fixture')"""
    )
    no_ex99 = COMPACT_FILING_INDEX.replace("EX-99.1", "8-K").replace("ex99.htm", "form8k.htm")
    options = SecEarningsReleaseOptions(cache_dir=tmp_path / "cache", run_id="no-ex99")
    first = refresh_sec_earnings_release_facts(tmp_store, options, session=_Session([no_ex99.encode()]))
    assert first["accepted"] == 0
    assert tmp_store.con.execute(
        "SELECT outcome, rejection_reason FROM sec_earnings_release_receipts"
    ).fetchall() == [("rejected", "ex99_document_not_found")]

    unused_session = _Session([COMPACT_FILING_INDEX.encode(), COMPACT_ATTACHMENT.encode()])
    second = refresh_sec_earnings_release_facts(tmp_store, options, session=unused_session)
    assert second["skipped_terminal"] == 1
    assert len(unused_session.responses) == 2


SPAN_GRID_ATTACHMENT = """
<p>Fourth Quarter 2025 results compared with Fourth Quarter 2024.</p>
<table>
  <tr><th rowspan="2"></th><th colspan="2">Three Months Ended December 31,</th>
      <th colspan="2">Year Ended December 31,</th></tr>
  <tr><th>2025</th><th>2024</th><th>2025</th><th>2024</th></tr>
  <tr><td colspan="5">Net income attributable to the corporation</td></tr>
  <tr><td colspan="5">per share:</td></tr>
  <tr><td>- Basic</td><td>1.40</td><td>1.85</td><td>6.64</td><td>7.20</td></tr>
  <tr><td>- Diluted</td><td>1.39</td><td>1.84</td><td>6.63</td><td>7.18</td></tr>
  <tr><td colspan="5">Adjusted earnings per share:</td></tr>
  <tr><td>- Basic</td><td>1.53</td><td>1.89</td><td>9.98</td><td>9.87</td></tr>
  <tr><td>- Diluted</td><td>1.52</td><td>1.88</td><td>9.99</td><td>9.88</td></tr>
</table>
"""


def test_explicit_gaap_basic_row_is_extracted_not_adjusted_or_annual() -> None:
    fact, reason = extract_reported_gaap_eps(
        SPAN_GRID_ATTACHMENT, measure="basic", period_end=dt.date(2025, 12, 31)
    )

    assert reason is None
    assert fact is not None
    assert (fact["value"], fact["prior_year_quarter_value"]) == (1.40, 1.85)
    assert (fact["measure_code"], fact["row_semantics"]) == ("EPS_BASIC", "basic")
    assert fact["column_number"] == 1
    assert "Year Ended" not in fact["column_heading"]
    assert fact["row_lineage"] == (
        "Net income attributable to the corporation", "per share:", "- Basic"
    )
    diluted, _ = extract_reported_gaap_eps(
        SPAN_GRID_ATTACHMENT, measure="diluted", period_end=dt.date(2025, 12, 31)
    )
    assert diluted is not None and diluted["value"] == 1.39


def _single_row_table(label: str, value: str = "(0.12)") -> str:
    return f"""
    <table><tr><th></th><th>Three Months Ended December 31,</th></tr>
    <tr><th></th><th>2025</th></tr>
    <tr><td>{label}</td><td>{value}</td></tr></table>
    """


def test_combined_basic_and_diluted_line_is_basic_eps_only() -> None:
    document = _single_row_table("Net loss per share - basic and diluted")
    basic, reason = extract_reported_gaap_eps(document, measure="basic", period_end=dt.date(2025, 12, 31))

    assert reason is None
    assert basic is not None
    assert (basic["value"], basic["row_semantics"]) == (-0.12, "basic_and_diluted")
    # The Company Facts convention maps the combined line to basic EPS only.
    diluted, diluted_reason = extract_reported_gaap_eps(
        document, measure="diluted", period_end=dt.date(2025, 12, 31)
    )
    assert diluted is None
    assert diluted_reason == "diluted_row_mentions_basic"


@pytest.mark.parametrize(("measure", "label"), [
    ("basic", "Adjusted basic earnings per share"),
    ("basic", "Basic earnings per share from continuing operations"),
    ("basic", "Non-GAAP basic earnings per share"),
    ("basic", "Non GAAP net income per share - basic"),
    ("basic", "Basic earnings per share from discontinued operations"),
    ("basic", "Core earnings per share - basic"),
    ("diluted", "Diluted earnings per share - discontinued operations"),
    ("diluted", "Core earnings per diluted share"),
    ("diluted", "Pro forma diluted earnings per share"),
])
def test_non_gaap_or_component_eps_is_rejected(measure: str, label: str) -> None:
    fact, reason = extract_reported_gaap_eps(
        _single_row_table(label), measure=measure, period_end=dt.date(2025, 12, 31)
    )

    assert fact is None
    assert reason == "rejected_non_gaap_or_adjusted"


def _rows_table(rows: list[tuple[str, str]]) -> str:
    body = "".join(f"<tr><td>{label}</td><td>{value}</td></tr>" for label, value in rows)
    return f"""
    <table><tr><th></th><th>Three Months Ended December 31,</th></tr>
    <tr><th></th><th>2025</th></tr>{body}</table>
    """


def _both(document: str) -> dict[str, tuple]:
    end = dt.date(2025, 12, 31)
    result = {}
    for measure in ("basic", "diluted"):
        fact, reason = extract_reported_gaap_eps(document, measure=measure, period_end=end)
        result[measure] = (None if fact is None else fact["value"], reason)
    return result


def test_combined_share_count_row_is_never_basic_eps() -> None:
    """Review probe B: a 'basic and diluted' share-count row next to the EPS row."""

    share_row = ("Weighted average shares used to compute net loss per share, basic and diluted", "45,123,456")
    eps_row = ("Basic and diluted net loss per common share", "(0.25)")
    assert _both(_rows_table([eps_row, share_row])) == {
        "basic": (-0.25, None), "diluted": (None, "diluted_row_mentions_basic"),
    }
    # Without a recognizable EPS row the share count is still never a value.
    assert _both(_rows_table([share_row]))["basic"] == (None, "reported_gaap_basic_eps_not_found")
    # Even an EPS-shaped (decimal, in-millions) count is excluded by its wording.
    assert _both(_rows_table([(share_row[0], "45.1")]))["basic"] == (None, "reported_gaap_basic_eps_not_found")


def test_per_basic_and_per_diluted_share_rows_beat_share_count_rows() -> None:
    """Review probe C: 'per basic/diluted share' EPS rows plus 'Shares used ...' rows."""

    share_rows = [
        ("Shares used in computing basic net income per share", "125,432"),
        ("Shares used in computing diluted net income per share", "127,001"),
    ]
    eps_rows = [("Net income per basic share", "$ 1.21"), ("Net income per diluted share", "$ 1.18")]
    assert _both(_rows_table([*eps_rows, *share_rows])) == {"basic": (1.21, None), "diluted": (1.18, None)}
    assert _both(_rows_table(share_rows)) == {
        "basic": (None, "reported_gaap_basic_eps_not_found"),
        "diluted": (None, "reported_gaap_diluted_eps_not_found"),
    }


def test_share_count_section_under_per_share_heading_is_excluded() -> None:
    document = """
    <table><tr><th></th><th>Three Months Ended December 31,</th></tr>
    <tr><th></th><th>2025</th></tr>
    <tr><td colspan="2">PER SHARE DATA</td></tr>
    <tr><td colspan="2">Weighted average common shares (millions):</td></tr>
    <tr><td>- Basic</td><td>875.4</td></tr>
    <tr><td>- Diluted</td><td>880.1</td></tr>
    <tr><td colspan="2">Net income per common share:</td></tr>
    <tr><td>- Basic</td><td>1.12</td></tr>
    <tr><td>- Diluted</td><td>1.11</td></tr></table>
    """
    assert _both(document) == {"basic": (1.12, None), "diluted": (1.11, None)}


@pytest.mark.parametrize(("cell", "value"), [
    ("1.39", 1.39), ("(0.25)", -0.25), ("$ .25", 0.25), ("-0.10", -0.10), ("999.99", 999.99),
    ("45,123,456", None), ("125432", None), ("2", None), ("1250.00", None), ("(1,234.5)", None),
])
def test_eps_value_is_decimal_and_below_one_thousand(cell: str, value: float | None) -> None:
    assert press_release._parse_eps_value(cell) == value


def test_same_gaap_value_in_highlights_and_statement_is_one_fact() -> None:
    table = _rows_table([("Diluted earnings per share", "1.39")])
    fact, reason = extract_reported_gaap_eps(table + table, measure="diluted", period_end=dt.date(2025, 12, 31))
    assert (fact["value"], reason) == (1.39, None)
    conflicting = table + _rows_table([("Diluted earnings per share", "1.40")])
    assert extract_reported_gaap_eps(
        conflicting, measure="diluted", period_end=dt.date(2025, 12, 31)
    ) == (None, "ambiguous_eps_candidates")


def test_diluted_row_under_basic_heading_is_never_basic_eps() -> None:
    document = """
    <table><tr><th></th><th>Three Months Ended December 31,</th></tr>
    <tr><th></th><th>2025</th></tr>
    <tr><td colspan="2">Earnings per share, basic and diluted:</td></tr>
    <tr><td>- Diluted</td><td>1.39</td></tr></table>
    """
    fact, reason = extract_reported_gaap_eps(document, measure="basic", period_end=dt.date(2025, 12, 31))

    assert fact is None
    assert reason == "reported_gaap_basic_eps_not_found"


def test_rejected_receipt_names_each_measure_reason(tmp_store, tmp_path) -> None:
    tmp_store.con.execute(
        """INSERT INTO sec_submissions (
               security_id, cik, accession_number, filing_date, report_date,
               acceptance_datetime_raw, form, items, source_url
           ) VALUES ('SEC-CIK-0000093410', '0000093410', '0000093410-26-000019',
                     DATE '2026-01-30', DATE '2025-12-31', '2026-01-30T18:17:00-05:00',
                     '8-K', '2.02', 'bulk-fixture')"""
    )
    document = b"""
    <p>Fourth Quarter 2025</p><table>
    <tr><th></th><th>Three Months Ended December 31,</th></tr><tr><th></th><th>2025</th></tr>
    <tr><td>Adjusted diluted earnings per share</td><td>1.52</td></tr></table>
    """
    options = SecEarningsReleaseOptions(cache_dir=tmp_path / "cache", run_id="per-measure-reject")
    result = refresh_sec_earnings_release_facts(
        tmp_store, options, session=_Session([COMPACT_FILING_INDEX.encode(), document])
    )

    assert (result["accepted"], result["rejected"]) == (0, 1)
    assert tmp_store.con.execute(
        "SELECT outcome, rejection_reason FROM sec_earnings_release_receipts"
    ).fetchall() == [(
        "rejected",
        "EPS_DILUTED:rejected_non_gaap_or_adjusted;EPS_BASIC:reported_gaap_basic_eps_not_found",
    )]
    assert tmp_store.con.execute("SELECT count(*) FROM press_release_facts").fetchone() == (0,)


@pytest.mark.parametrize(("basic", "diluted", "reason"), [
    # Dilution cannot raise EPS: flagged and withheld, never swapped.
    ("1.20", "1.30", "diluted_exceeds_basic:basic=1.2;diluted=1.3"),
    # A loss cannot become income through dilution.
    ("(0.10)", "0.05", "diluted_exceeds_basic:basic=-0.1;diluted=0.05"),
    # Rounding tolerance and anti-dilutive losses are consistent.
    ("1.20", "1.204", None),
    ("(0.40)", "(0.38)", None),
])
def test_diluted_above_basic_income_is_flagged_and_withheld(tmp_store, tmp_path, basic, diluted, reason) -> None:
    tmp_store.con.execute(
        """INSERT INTO sec_submissions (
               security_id, cik, accession_number, filing_date, report_date,
               acceptance_datetime_raw, form, items, source_url
           ) VALUES ('SEC-CIK-0000093410', '0000093410', '0000093410-26-000019',
                     DATE '2026-01-30', DATE '2025-12-31', '2026-01-30T18:17:00-05:00',
                     '8-K', '2.02', 'bulk-fixture')"""
    )
    document = f"""
    <p>Fourth Quarter 2025</p><table>
    <tr><th></th><th>Three Months Ended December 31,</th></tr><tr><th></th><th>2025</th></tr>
    <tr><td colspan="2">Earnings per share:</td></tr>
    <tr><td>- Basic</td><td>{basic}</td></tr>
    <tr><td>- Diluted</td><td>{diluted}</td></tr></table>
    """.encode()
    options = SecEarningsReleaseOptions(cache_dir=tmp_path / "cache", run_id="dilution-check")
    result = refresh_sec_earnings_release_facts(
        tmp_store, options, session=_Session([COMPACT_FILING_INDEX.encode(), document])
    )

    receipts = tmp_store.con.execute(
        "SELECT outcome, rejection_reason FROM sec_earnings_release_receipts"
    ).fetchall()
    facts = tmp_store.con.execute("SELECT count(*) FROM press_release_facts").fetchone()[0]
    if reason is None:
        assert (result["accepted"], receipts, facts) == (1, [("accepted", None)], 2)
    else:
        assert (result["rejected"], receipts, facts) == (1, [("rejected", reason)], 0)


def _workiva_loss_table(current: tuple[str, str, str], prior: tuple[str, str, str]) -> str:
    """Workiva-style cells: currency, number and a closing parenthesis split apart."""

    def cells(values: tuple[str, str, str]) -> str:
        return "".join(f"<td>{value}</td>" for value in values)

    return f"""
    <p>Fourth Quarter 2025</p>
    <table>
      <tr><th></th><th colspan="7">Three Months Ended December 31,</th></tr>
      <tr><th></th><th colspan="3">2025</th><th></th><th colspan="3">2024</th></tr>
      <tr><td colspan="8">Net loss per share:</td></tr>
      <tr><td>- Basic</td>{cells(current)}<td></td>{cells(prior)}</tr>
      <tr><td>- Diluted</td>{cells(current)}<td></td>{cells(prior)}</tr>
    </table>
    """


@pytest.mark.parametrize(("current", "value"), [
    (("$", "(0.25", ")"), -0.25),
    (("$ (", "0.25", ")"), -0.25),
    (("$ (0.25", ")", ""), -0.25),
    (("$", "(0.25)", ""), -0.25),
    (("$", "0.25", ""), 0.25),
])
def test_workiva_split_parenthesis_loss_quarter_is_negative_for_both_measures(current, value) -> None:
    """Review probe G / P1: a split ')' must never turn a loss into a profit."""

    document = _workiva_loss_table(current, ("$", "(0.10", ")"))
    for measure in ("basic", "diluted"):
        fact, reason = extract_reported_gaap_eps(document, measure=measure, period_end=dt.date(2025, 12, 31))
        assert reason is None
        assert fact is not None
        assert (fact["value"], fact["prior_year_quarter_value"]) == (value, -0.10)


@pytest.mark.parametrize("current", [
    ("$", "(0.25", ""),       # opening parenthesis never closed
    ("$", "0.25)", ""),       # closing parenthesis never opened
    ("$ (", "0.25", ""),      # opened in the currency cell, never closed
])
def test_unbalanced_parenthesis_is_rejected_not_read_as_profit(current) -> None:
    document = _workiva_loss_table(current, ("$", "(0.10", ")"))
    for measure in ("basic", "diluted"):
        fact, reason = extract_reported_gaap_eps(document, measure=measure, period_end=dt.date(2025, 12, 31))
        assert fact is None
        assert reason == f"reported_gaap_{measure}_eps_not_found"


def test_workiva_loss_quarter_is_stored_negative_under_one_receipt(tmp_store, tmp_path) -> None:
    tmp_store.con.execute(
        """INSERT INTO sec_submissions (
               security_id, cik, accession_number, filing_date, report_date,
               acceptance_datetime_raw, form, items, source_url
           ) VALUES ('SEC-CIK-0000093410', '0000093410', '0000093410-26-000019',
                     DATE '2026-01-30', DATE '2025-12-31', '2026-01-30T18:17:00-05:00',
                     '8-K', '2.02', 'bulk-fixture')"""
    )
    document = _workiva_loss_table(("$", "(0.25", ")"), ("$", "(0.10", ")")).encode()
    options = SecEarningsReleaseOptions(cache_dir=tmp_path / "cache", run_id="workiva-loss")
    result = refresh_sec_earnings_release_facts(
        tmp_store, options, session=_Session([COMPACT_FILING_INDEX.encode(), document])
    )

    assert result["accepted"] == 1
    assert tmp_store.con.execute(
        "SELECT measure_code, value, CAST(json_extract(raw_payload_json, '$.prior_year_quarter_value') AS DOUBLE) "
        "FROM press_release_facts ORDER BY measure_code"
    ).fetchall() == [("EPS_BASIC", -0.25, -0.10), ("EPS_DILUTED", -0.25, -0.10)]


def _headed_eps_table(*headings: str, basic: str = "1.21", diluted: str = "1.18") -> str:
    heading_rows = "".join(f'<tr><td colspan="2">{heading}</td></tr>' for heading in headings)
    return f"""
    <table><tr><th></th><th>Three Months Ended December 31,</th></tr>
    <tr><th></th><th>2025</th></tr>{heading_rows}
    <tr><td>- Basic</td><td>{basic}</td></tr>
    <tr><td>- Diluted</td><td>{diluted}</td></tr></table>
    """


@pytest.mark.parametrize("headings", [
    # Review probe J: a GAAP section heading above the per-share heading.
    ("Operating results", "Earnings per share:"),
    ("Cash flow and operating data", "Net income per common share:"),
    # Review probes H/I: count words after the per-share phrase describe EPS.
    ("Net income per share (based on weighted average shares outstanding):",),
    ("Net income per share attributable to Class A common shares:",),
])
def test_gaap_eps_under_section_or_basis_wording_is_accepted(headings) -> None:
    assert _both(_headed_eps_table(*headings)) == {"basic": (1.21, None), "diluted": (1.18, None)}


@pytest.mark.parametrize("headings", [
    ("Operating earnings per share:",),
    ("Funds from operations per share:",),
    ("FFO per share:",),
    ("AFFO per share:",),
    ("Non-GAAP results", "Earnings per share:"),
    ("Core results", "Earnings per share:"),
    ("Earnings per share:", "Discontinued operations:"),
    # A7 re-review 2 V1: a discontinued-operations SECTION heading above its own per-share label.
    ("Discontinued operations:", "Earnings per share:"),
])
def test_non_gaap_or_component_headings_still_reject_both_measures(headings) -> None:
    assert _both(_headed_eps_table(*headings)) == {
        "basic": (None, "rejected_non_gaap_or_adjusted"), "diluted": (None, "rejected_non_gaap_or_adjusted"),
    }


def test_discontinued_operations_block_is_never_total_gaap_eps() -> None:
    """A7 re-review 2 V1: continuing and discontinued EPS blocks, each under its own
    "Earnings per share:" label. Neither component is total EPS; admitting the
    discontinued 0.10 would publish a wrong Q4 value that no 10-Q conflict corrects."""

    document = """
    <table><tr><th></th><th>Three Months Ended December 31,</th></tr>
    <tr><th></th><th>2025</th></tr>
    <tr><td colspan="2">Continuing operations:</td></tr>
    <tr><td colspan="2">Earnings per share:</td></tr>
    <tr><td>- Basic</td><td>1.00</td></tr>
    <tr><td>- Diluted</td><td>0.99</td></tr>
    <tr><td colspan="2">Discontinued operations:</td></tr>
    <tr><td colspan="2">Earnings per share:</td></tr>
    <tr><td>- Basic</td><td>0.10</td></tr>
    <tr><td>- Diluted</td><td>0.10</td></tr></table>
    """
    assert _both(document) == {
        "basic": (None, "rejected_non_gaap_or_adjusted"), "diluted": (None, "rejected_non_gaap_or_adjusted"),
    }


def test_ffo_row_is_never_gaap_diluted_eps() -> None:
    """Review probe L (REIT): funds from operations per share is not GAAP EPS."""

    for label in ("FFO per share - diluted", "Funds from operations per share - diluted"):
        assert _both(_rows_table([(label, "0.85")]))["diluted"] == (None, "rejected_non_gaap_or_adjusted")
