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
    assert _ex99_documents(directory_json, INDEX_DIRECTORY) == ()

    typed = COMPACT_FILING_INDEX.replace("ex99.htm", "a12312025ex9918-k.htm")
    wrong_type = """<tr><td>3</td><td>EX-99.2</td>
        <td><a href="/Archives/edgar/data/93410/000009341026000019/looks-like-ex99.htm">looks-like-ex99.htm</a></td>
        <td>text.gif</td><td>20</td></tr>"""
    wrong_accession = """<tr><td>4</td><td>EX-99.3</td>
        <td><a href="/Archives/edgar/data/93410/other/accession.htm">accession.htm</a></td>
        <td>EX-99.3</td><td>20</td></tr>"""
    typed = typed.replace("</table>", wrong_type + wrong_accession + "</table>")
    assert _ex99_documents(typed, INDEX_DIRECTORY) == ("a12312025ex9918-k.htm",)


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
    assert tmp_store.con.execute(
        "SELECT count(*) FROM press_release_facts WHERE accession_number = '0000093410-26-000019'"
    ).fetchone() == (1,)
    fiscal_year, fiscal_period, period_end, as_of_date, raw_payload = tmp_store.con.execute(
        "SELECT fiscal_year, fiscal_period, period_end, as_of_date, raw_payload_json FROM press_release_facts "
        "WHERE accession_number = '0000093410-26-000019'"
    ).fetchone()
    assert (fiscal_year, fiscal_period) == (2025, "Q4")
    assert period_end == as_of_date == dt.date(2025, 12, 31)
    assert json.loads(raw_payload)["event_report_date"] == "2026-01-30"
    assert json.loads(raw_payload)["prior_year_quarter_value"] == 1.84


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
