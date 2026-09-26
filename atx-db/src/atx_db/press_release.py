"""PF2-S8: preliminary earnings facts from 8-K Item 2.02 press releases.

The loader is injectable by design: tests and local development pass a CSV/JSON
file or fetch/parse callables, while production wiring can later point it at a
licensed filing text source. Preliminary facts are retained in their own table
and reconciled to final reported ``est_actual`` rows when those arrive.

SEC Item 2.02 EX-99 evidence is split into fetch and load (node 1.7):
:func:`fetch_sec_earnings_release_documents` is the fetch worker (no DuckDB; it
writes a ``sec_http.FetchLedgerStore``: content-addressed files plus
``fetch-ledger.jsonl``, resumable), and :func:`refresh_sec_earnings_release_facts`
with ``SecEarningsReleaseOptions.fetch_dir`` is the loader that reads only that
store and never opens the network.
"""
from __future__ import annotations

import calendar
import datetime as dt
import hashlib
import html
import json
import os
import re
import tempfile
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path
from typing import Any, Callable, Iterable, Iterator
from urllib.parse import urlsplit

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .reported_eps_core import BASIC_EPS, DILUTED_EPS
from .sec_http import (
    APPROVED_SEC_USER_AGENT,
    FETCH_OBJECT_SHA_MISMATCH,
    RESPONSE_TOO_LARGE,
    FetchLedgerStore,
    SecRateLimiter,
)
from .sec_submissions import EarningsReleaseCandidate, select_earnings_release_candidates
from .security_master import sec_session
from .estimates import (
    GUIDANCE_MEASURE_PATTERNS,
    GUIDANCE_TEXT_COLUMNS,
    GUIDANCE_VALUE_RE,
    _bool_series,
    _canonical_measure,
    _clean_guidance_text,
    _date_series,
    _guidance_basis,
    _guidance_date_value,
    _guidance_period_from_text,
    _guidance_record_value,
    _guidance_scale,
    _guidance_source_item,
    _guidance_ts_value,
    _guidance_values_after,
    _integer_series,
    _numeric_series,
    _period_end_from_fiscal_fields,
    _raw_payloads,
    _read_guidance_source_file,
    _string_series,
    _timestamp_series,
)
from .warehouse import (
    file_sha256,
    cik_security_id,
    insert_frame,
    json_dumps,
    now_utc_naive,
    quality_check,
    record_source_file,
    security_id_for_symbol,
    snake_case,
    symbol_key,
)


SOURCE_NAME = "press_release_injectable"
SEC_EARNINGS_RELEASE_SOURCE = "SEC 8-K Item 2.02 reported earnings release"
SEC_EARNINGS_RELEASE_USER_AGENT = APPROVED_SEC_USER_AGENT
DEFAULT_RECONCILIATION_TOLERANCE = 0.02
EPS_CONFLICT_TOLERANCE = 0.005
SEC_FILING_DATE_CLOCK_POLICY = "sec_filed_date_plus_46h_v1"
# Governed EX-99 extraction emits GAAP basic and diluted quarterly EPS from one
# document under one receipt; each measure keeps the same duration proof.
REPORTED_EPS_EXTRACTOR_VERSION = "reported_gaap_eps_basic_diluted_v2"
REPORTED_EPS_MEASURE_CODES = {"diluted": DILUTED_EPS.measure_code, "basic": BASIC_EPS.measure_code}
# Rows that are not total GAAP EPS. Non-GAAP context words mark a non-GAAP
# figure wherever they appear in the row's lineage, including a section
# heading (pre-A7 contract): adjusted, non-GAAP, core, pro forma, excluding,
# REIT funds from operations, and the continuing- and discontinued-operations
# components. "Discontinued" never titles a GAAP-total section, and a
# "Discontinued operations:" section heading sits above its own "Earnings per
# share:" label, so it must be seen beyond the row scope (else 0.10 of
# discontinued EPS is published as total GAAP EPS once the continuing block is
# rejected).
_NON_GAAP_CONTEXT_RE = re.compile(
    r"continuing|adjusted|non-gaap|non gaap"
    r"|\b(?:discontinued|core|pro[- ]?forma|excluding|funds\s+from\s+operations|ffo|affo)\b"
)
# Words that also title ordinary GAAP sections ("Operating results") only
# disqualify the row itself and headings up to its nearest per-share label.
_NON_TOTAL_ROW_RE = re.compile(r"\b(?:operating|cash)\b")
# "per share", "per common share", "per diluted share", "per basic and diluted share" ...
_PER_SHARE_RE = re.compile(r"\bper\s+(?:(?:basic|diluted|common|ordinary|and)\s+)*share\b")
# Share-count rows ("Weighted average shares used in computing ... per share,
# basic and diluted", "Shares used in computing diluted net income per share").
# Only count words before a label's per-share phrase count: "Net income per
# share (based on weighted average shares outstanding)" is an EPS label.
_SHARE_COUNT_RE = re.compile(r"\b(?:shares|weighted|denominator|share\s+count)\b")
# A reported EPS amount is a decimal with 1-4 fraction digits and magnitude
# below $1,000. Integers (including a bare "$1") and thousands-separated
# numbers (share counts, dollar amounts) never qualify. Per-share amounts of
# $1,000 or more (e.g. some Class A shares) are rejected by design rather than
# risk a share count. Parentheses must balance, within the cell or across the
# adjacent cells (``_cell_eps_value``).
_EPS_VALUE_RE = re.compile(r"\d{0,3}\.\d{1,4}")
_EPS_VALUE_BOUND = 1000.0
# Under GAAP, dilution never raises EPS: diluted EPS above basic EPS (beyond
# rounding) means the document or its extraction is inconsistent.
_DILUTION_TOLERANCE = 0.005
# Release-duration windows (inclusive days) used to match a preliminary fact
# to a final actual by period geometry rather than by fiscal labels.
_RELEASE_DURATION_WINDOWS = {
    "three_months_explicit": (89, 93),
    "thirteen_weeks_explicit": (91, 91),
    "fourteen_weeks_explicit": (98, 98),
}
_QUARTER_DAYS = (70, 115)
_ANNUAL_DAYS = (330, 380)


PRESS_RELEASE_FACT_COLUMNS = [
    "press_release_fact_id",
    "source",
    "security_id",
    "symbol",
    "cik",
    "accession_number",
    "form",
    "source_item",
    "source_url",
    "measure_code",
    "fiscal_year",
    "fiscal_period",
    "period_end",
    "value",
    "unit",
    "basis",
    "is_preliminary",
    "extraction_confidence",
    "evidence_text",
    "source_file",
    "source_file_sha256",
    "filing_date",
    "release_date",
    "as_of_date",
    "available_at",
    "is_latest_revision",
    "input_codes_json",
    "raw_payload_json",
    "run_id",
    "source_loaded_at",
]


PRESS_RELEASE_RECONCILIATION_COLUMNS = [
    "press_release_reconciliation_id",
    "source",
    "press_release_fact_id",
    "security_id",
    "symbol",
    "cik",
    "accession_number",
    "measure_code",
    "fiscal_year",
    "fiscal_period",
    "period_end",
    "basis",
    "preliminary_value",
    "preliminary_available_at",
    "final_actual_value",
    "final_actual_available_at",
    "final_actual_accession_number",
    "value_difference",
    "relative_difference",
    "reconciliation_tolerance",
    "reconciliation_status",
    "pdate",
    "rdq",
    "as_of_date",
    "available_at",
    "is_latest_revision",
    "run_id",
    "source_loaded_at",
]


PRESS_RELEASE_COLUMN_ALIASES = {
    "acceptancedatetime": "acceptance_datetime",
    "accession": "accession_number",
    "accessionnumber": "accession_number",
    "confidence": "extraction_confidence",
    "content": "document_text",
    "document": "document_text",
    "documenttext": "document_text",
    "exhibittext": "document_text",
    "extractionconfidence": "extraction_confidence",
    "filingdate": "filing_date",
    "fiscalperiod": "fiscal_period",
    "fiscalyear": "fiscal_year",
    "item": "source_item",
    "measure": "measure_code",
    "measurecode": "measure_code",
    "periodend": "period_end",
    "rawtext": "document_text",
    "releasedate": "release_date",
    "releasedatetime": "release_datetime",
    "reportdate": "report_date",
    "securityid": "security_id",
    "sourceitem": "source_item",
    "sourceurl": "source_url",
    "ticker": "symbol",
}

PRESS_RELEASE_TEXT_COLUMNS = (*GUIDANCE_TEXT_COLUMNS, "press_release_text")

PRESS_RELEASE_CUE_RE = re.compile(
    r"\b(reports?|reported|announces?|announced|results?|earnings|"
    r"revenue|revenues|sales|eps|earnings\s+per\s+share|net\s+income|"
    r"operating\s+income)\b",
    flags=re.IGNORECASE,
)


@dataclass(frozen=True)
class PressReleaseOptions:
    """Press-release loader for injectable rows or local SEC 8-K text files."""

    source_file: Path | None = None
    source: str = SOURCE_NAME
    fetch: Callable[[], Iterable[Any]] | None = None
    parse: Callable[[Any], Iterable[dict]] | None = None
    replace_source_file: bool = True
    min_confidence: float = 0.70
    reconciliation_tolerance: float = DEFAULT_RECONCILIATION_TOLERANCE
    run_id: str | None = None


@dataclass(frozen=True)
class SecEarningsReleaseOptions:
    """Bounded public-SEC source controls for reported-quarter EPS evidence."""

    cache_dir: Path
    history_start: dt.date | None = None
    history_end: dt.date | None = None
    ciks: tuple[str, ...] | None = None
    request_timeout: float = 30.0
    max_index_bytes: int = 2_000_000
    max_document_bytes: int = 8_000_000
    candidate_batch_size: int = 250
    user_agent: str = SEC_EARNINGS_RELEASE_USER_AGENT
    # Set: loader-only mode. Index/EX-99 bytes come from this sec_http fetch store
    # (fetch_sec_earnings_release_documents); nothing is requested from SEC.
    fetch_dir: Path | None = None
    run_id: str | None = None

    def __post_init__(self) -> None:
        if self.request_timeout <= 0:
            raise ValueError("SEC earnings-release request_timeout must be positive")
        if min(self.max_index_bytes, self.max_document_bytes, self.candidate_batch_size) < 1:
            raise ValueError("SEC earnings-release byte limits must be positive")
        if self.user_agent != SEC_EARNINGS_RELEASE_USER_AGENT:
            raise ValueError("SEC earnings-release source requires the project-only SEC user agent")


@dataclass(frozen=True)
class SecSourceClock:
    raw_timestamp: str | None
    utc_offset: str | None
    available_at: dt.datetime | None
    timezone_status: str


@dataclass(frozen=True)
class SecEarningsReleaseOutcome:
    candidate: EarningsReleaseCandidate
    status: str
    reason: str | None
    document_name: str | None = None
    index_url: str | None = None
    document_url: str | None = None
    document_sha256: str | None = None
    source_clock: SecSourceClock | None = None
    fact: dict[str, Any] | None = None


@dataclass(frozen=True)
class _HtmlCell:
    """One physical HTML cell before its row/column spans are expanded."""

    text: str
    rowspan: int = 1
    colspan: int = 1


class _HtmlTableCollector(HTMLParser):
    """Dependency-free table reader retaining span information for a grid."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.tables: list[list[list[_HtmlCell]]] = []
        self._table: list[list[_HtmlCell]] | None = None
        self._row: list[_HtmlCell] | None = None
        self._cell: list[str] | None = None
        self._cell_rowspan = 1
        self._cell_colspan = 1

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "table" and self._table is None:
            self._table = []
        elif tag == "tr" and self._table is not None:
            self._row = []
        elif tag in {"td", "th"} and self._row is not None:
            self._cell = []
            attr_map = {name.lower(): value for name, value in attrs}
            self._cell_rowspan = _positive_span(attr_map.get("rowspan"))
            self._cell_colspan = _positive_span(attr_map.get("colspan"))
        elif tag == "br" and self._cell is not None:
            self._cell.append(" ")

    def handle_endtag(self, tag: str) -> None:
        if tag in {"td", "th"} and self._cell is not None and self._row is not None:
            self._row.append(_HtmlCell(
                " ".join("".join(self._cell).split()), self._cell_rowspan, self._cell_colspan
            ))
            self._cell = None
        elif tag == "tr" and self._row is not None and self._table is not None:
            if self._row:
                self._table.append(self._row)
            self._row = None
        elif tag == "table" and self._table is not None:
            self.tables.append(self._table)
            self._table = None

    def handle_data(self, data: str) -> None:
        if self._cell is not None:
            self._cell.append(data)


def _positive_span(value: str | None) -> int:
    """HTML spans are untrusted input: invalid/zero spans mean one cell."""

    try:
        return max(1, int(value or "1"))
    except ValueError:
        return 1


def _span_expanded_grid(table: list[list[_HtmlCell]]) -> list[list[str]]:
    """Expand rowspan/colspan into a rectangular logical grid.

    The SEC's tagged exhibits commonly use a duration group above duplicate
    year leaves.  Physical-cell indexes cannot associate those leaves with the
    quarterly or annual group, so extraction only works from this grid.
    """

    active: dict[int, tuple[int, str]] = {}
    rows: list[list[str]] = []
    for physical_row in table:
        expanded: list[str] = []
        column = 0
        for cell in physical_row:
            while column in active:
                remaining, text = active[column]
                expanded.append(text)
                if remaining == 1:
                    del active[column]
                else:
                    active[column] = (remaining - 1, text)
                column += 1
            for _ in range(cell.colspan):
                expanded.append(cell.text)
                if cell.rowspan > 1:
                    active[column] = (cell.rowspan - 1, cell.text)
                column += 1
        while column in active:
            remaining, text = active[column]
            expanded.append(text)
            if remaining == 1:
                del active[column]
            else:
                active[column] = (remaining - 1, text)
            column += 1
        rows.append(expanded)
    width = max((len(row) for row in rows), default=0)
    return [row + [""] * (width - len(row)) for row in rows]


def _archive_urls(cik: str, accession_number: str) -> tuple[str, str]:
    digits = str(accession_number).replace("-", "")
    if not re.fullmatch(r"\d{18}", digits):
        raise ValueError(f"invalid SEC accession {accession_number!r}")
    numeric_cik = str(int(str(cik)))
    directory = f"https://www.sec.gov/Archives/edgar/data/{numeric_cik}/{digits}"
    return f"{directory}/{accession_number}-index.html", directory


def _explicit_sec_clock(raw_timestamp: object) -> SecSourceClock:
    """Retain the SEC timestamp evidence; a naive value is never PIT-eligible."""

    raw = _clean_string(raw_timestamp)
    if raw is None:
        return SecSourceClock(None, None, None, "timestamp_missing")
    # EDGAR's accepted ISO timestamps normally carry Z. Do not convert naive
    # legacy payloads with utc=True: that would invent the original zone.
    has_offset = bool(re.search(r"(?:Z|[+-]\d{2}:?\d{2})$", raw, flags=re.IGNORECASE))
    if not has_offset:
        return SecSourceClock(raw, None, None, "timestamp_zone_unknown")
    parsed = pd.to_datetime(raw, errors="coerce", utc=True)
    if pd.isna(parsed):
        return SecSourceClock(raw, None, None, "timestamp_invalid")
    offset = "Z" if raw.upper().endswith("Z") else raw[-6:]
    return SecSourceClock(raw, offset, parsed.tz_convert(None).to_pydatetime(), "timestamp_offset_valid")


def _daily_sec_clock(raw_timestamp: object, filing_date: dt.date | None) -> SecSourceClock:
    """Use FC1's conservative daily SEC clock without claiming intraday delivery.

    EDGAR acceptance and dissemination are distinct.  The original acceptance
    string is still retained for lineage, while the daily pipeline uses the
    reviewed filed-date-plus-46-hour eligibility floor.  This permits a dated
    filing with a legacy/naive source timestamp to remain useful daily evidence
    without relabeling that timestamp as exact UTC publication.
    """

    exact = _explicit_sec_clock(raw_timestamp)
    if filing_date is None:
        return exact
    conservative = dt.datetime.combine(filing_date, dt.time()) + dt.timedelta(hours=46)
    return SecSourceClock(
        exact.raw_timestamp,
        exact.utc_offset,
        max(filter(None, (exact.available_at, conservative))),
        f"{exact.timezone_status}:{SEC_FILING_DATE_CLOCK_POLICY}",
    )


def _bounded_response_bytes(response: Any, *, maximum: int) -> bytes:
    """Read a response stream without admitting an unbounded SEC document."""

    declared = response.headers.get("Content-Length") if getattr(response, "headers", None) else None
    if declared and declared.isdigit() and int(declared) > maximum:
        raise ValueError("response_too_large")
    chunks: list[bytes] = []
    observed = 0
    for chunk in response.iter_content(chunk_size=64 * 1024):
        if not chunk:
            continue
        observed += len(chunk)
        if observed > maximum:
            raise ValueError("response_too_large")
        chunks.append(chunk)
    return b"".join(chunks)


class _SecFilingDocumentTable(HTMLParser):
    """Read typed rows only from the filing index's document-format table."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.rows: list[list[tuple[str, tuple[str, ...]]]] = []
        self.table_found = False
        self.table_complete = False
        self._header_seen = False
        self._table_depth = 0
        self._row: list[tuple[str, tuple[str, ...]]] | None = None
        self._row_tags: list[str] = []
        self._cell_text: list[str] | None = None
        self._cell_tag: str | None = None
        self._cell_hrefs: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        if tag == "table":
            if self._table_depth:
                self._table_depth += 1
            elif _normalized_cell(attributes.get("summary") or "") == "document format files":
                if self.table_found:
                    raise ValueError("multiple_document_format_tables")
                self.table_found = True
                self._table_depth = 1
        elif self._table_depth == 1 and tag == "tr":
            if self._row is not None:
                raise ValueError("incomplete_document_format_row")
            self._row = []
            self._row_tags = []
        elif self._table_depth == 1 and tag in ("td", "th") and self._row is not None:
            if self._cell_text is not None:
                raise ValueError("incomplete_document_format_cell")
            self._cell_text = []
            self._cell_tag = tag
            self._cell_hrefs = []
        elif (self._table_depth == 1 and tag == "a" and self._cell_text is not None
              and attributes.get("href") is not None):
            self._cell_hrefs.append(attributes["href"] or "")

    def handle_data(self, data: str) -> None:
        if self._cell_text is not None:
            self._cell_text.append(data)

    def handle_endtag(self, tag: str) -> None:
        if tag in ("td", "th") and self._cell_text is not None and self._row is not None:
            if tag != self._cell_tag:
                raise ValueError("invalid_document_format_cell")
            self._row.append((" ".join("".join(self._cell_text).split()), tuple(self._cell_hrefs)))
            self._row_tags.append(tag)
            self._cell_text = None
            self._cell_tag = None
            self._cell_hrefs = []
        elif tag == "tr" and self._table_depth == 1 and self._row is not None:
            if self._cell_text is not None:
                raise ValueError("incomplete_document_format_cell")
            if not self._header_seen:
                if (len(self._row) != 5 or self._row_tags != ["th"] * 5
                        or _normalized_cell(self._row[2][0]) != "document"
                        or _normalized_cell(self._row[3][0]) != "type"):
                    raise ValueError("invalid_document_format_header")
                self._header_seen = True
            else:
                if (len(self._row) != 5 or self._row_tags != ["td"] * 5
                        or len(self._row[2][1]) != 1):
                    raise ValueError("invalid_document_format_row")
                self.rows.append(self._row)
            self._row = None
        elif tag == "table" and self._table_depth:
            if self._table_depth == 1:
                if self._row is not None or not self._header_seen or not self.rows:
                    raise ValueError("incomplete_document_format_table")
                self.table_complete = True
            self._table_depth -= 1


def _ex99_documents(index_html: str, directory_url: str) -> tuple[str, ...]:
    """Trust SEC filing-detail Type cells, never directory MIME or filenames."""

    parser = _SecFilingDocumentTable()
    parser.feed(index_html)
    parser.close()
    if not parser.table_complete:
        raise ValueError("document_format_table_not_found_or_incomplete")
    expected_path = urlsplit(directory_url).path.rstrip("/") + "/"
    selected: set[str] = set()
    for row in parser.rows:
        if len(row) < 4 or not re.fullmatch(r"EX[-_]?99(?:\.\d+)?", row[3][0], flags=re.IGNORECASE):
            continue
        hrefs = row[2][1]
        if len(hrefs) != 1:
            continue
        link = urlsplit(hrefs[0])
        if link.scheme or link.netloc or link.query or link.fragment or not link.path.startswith(expected_path):
            continue
        name = link.path[len(expected_path):]
        if ".." in name or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*\.(?:htm|html|txt)", name, re.IGNORECASE):
            continue
        selected.add(name)
    return tuple(sorted(selected))


def _normalized_cell(value: str) -> str:
    return re.sub(r"\s+", " ", html.unescape(value).replace("\xa0", " ")).strip().lower()


def _parse_eps_number(value: str) -> float | None:
    token = value.replace("$", "").replace(",", "").strip()
    negative = token.startswith("(") and token.endswith(")")
    token = token.strip("() ")
    if not re.fullmatch(r"\d+(?:\.\d+)?", token):
        return None
    result = float(token)
    return -result if negative else result


def _parse_eps_value(value: str) -> float | None:
    """Parse only an EPS-shaped amount; see ``_EPS_VALUE_RE``/``_EPS_VALUE_BOUND``.

    ``_parse_eps_number`` still detects numeric table rows; this stricter
    parser decides whether a cell can be a reported per-share amount.
    """

    token = value.replace("$", "").replace(chr(0x2212), "-").strip()  # U+2212 minus sign
    if "," in token:
        return None
    negative = token.startswith("(") and token.endswith(")")
    if token.count("(") != int(negative) or token.count(")") != int(negative):
        return None  # An unbalanced parenthesis never flips to a positive amount.
    token = token.strip("() ")
    if token.startswith("-") and not negative:
        negative, token = True, token[1:].strip()
    if not _EPS_VALUE_RE.fullmatch(token):
        return None
    result = float(token)
    if abs(result) >= _EPS_VALUE_BOUND:
        return None
    return -result if negative else result


def _adjacent_cell(row: list[str], column: int, step: int) -> str:
    """Nearest non-empty neighbour, skipping span copies of the cell itself."""

    own = row[column].strip()
    index = column + step
    while 0 <= index < len(row):
        text = row[index].strip()
        if text and text != own:
            return text
        index += step
    return ""


def _cell_eps_value(row: list[str], column: int) -> float | None:
    """EPS amount of one grid cell, completing parentheses split across cells.

    EDGAR (e.g. Workiva) HTML routinely prints a loss as ``(0.25`` with the
    closing ``)`` in the next cell, or ``$ (`` before the number.  The sign is
    taken only when both halves are present; any other unbalanced parenthesis
    rejects the cell instead of reading a loss as a profit.
    """

    text = row[column].strip()
    opens, closes = text.count("("), text.count(")")
    before, after = _adjacent_cell(row, column, -1), _adjacent_cell(row, column, 1)
    if opens == closes == 0:
        opened, closed = before.endswith("("), after.startswith(")")
        if opened and closed:
            return _parse_eps_value(f"({text})")
        return None if opened or closed else _parse_eps_value(text)
    if opens == 1 and closes == 0:
        return _parse_eps_value(f"{text})") if after.startswith(")") else None
    if opens == 0 and closes == 1:
        return _parse_eps_value(f"({text}") if before.endswith("(") else None
    return _parse_eps_value(text)


def _duration_evidence(header_text: str) -> str | None:
    normalized = _normalized_cell(header_text)
    if "three months" in normalized:
        return "three_months_explicit"
    if re.search(r"\b13\s+weeks?\b", normalized):
        return "thirteen_weeks_explicit"
    if re.search(r"\b14\s+weeks?\b", normalized):
        return "fourteen_weeks_explicit"
    return None


def _date_matches_text(value: str, period_end: dt.date) -> bool:
    """Accept only a document's explicit end-date leaf, never calendar mapping."""

    text = _normalized_cell(value)
    month = _normalized_cell(calendar.month_name[period_end.month])
    day = str(period_end.day)
    year = str(period_end.year)
    return bool(
        re.search(rf"\b{re.escape(month)}\s+{day}(?:st|nd|rd|th)?[,]?\s+{year}\b", text)
        # Multi-level headers commonly put the date group and year leaf in
        # different cells.  The caller separately requires the exact year.
        or re.search(rf"\b{re.escape(month)}\s+{day}(?:st|nd|rd|th)?\b", text)
        or re.search(rf"\b{period_end.month}[/-]{period_end.day}[/-]{year}\b", text)
    )


def _table_header_rows(grid: list[list[str]]) -> list[list[str]]:
    """Stop all leaf traces together before the first numeric data row."""

    for row_number, row in enumerate(grid[:10]):
        if any(_parse_eps_number(cell) is not None and not re.fullmatch(r"20\d{2}", cell.strip())
               for cell in row):
            return grid[:row_number]
    return grid[:10]


def _document_quarter_end(tables: list[list[list[str]]], filed_on: dt.date) -> dt.date | None:
    """Find the latest explicitly headed quarterly end visible by filing day.

    An 8-K's ``reportDate`` is the event date, not the earnings period end.
    Combine each quarterly duration group's month/day with its own year leaf;
    never infer a quarter end from the event date or a calendar-quarter map.
    """

    ends: set[dt.date] = set()
    month_pattern = "|".join(calendar.month_name[1:])
    for grid in tables:
        headers = _table_header_rows(grid)
        for column in range(max((len(row) for row in grid), default=0)):
            cells: list[str] = []
            for row in headers:
                cell = row[column].strip() if column < len(row) else ""
                if cell:
                    cells.append(cell)
            heading = " | ".join(dict.fromkeys(cells))
            normalized = _normalized_cell(heading)
            if _duration_evidence(heading) is None or any(
                term in normalized for term in ("year ended", "years ended", "twelve months")
            ):
                continue
            match = re.search(rf"\b({month_pattern})\s+(\d{{1,2}})(?:st|nd|rd|th)?\b", heading, re.IGNORECASE)
            years = set(re.findall(r"\b20\d{2}\b", heading))
            if match is None or len(years) != 1:
                continue
            month = next(i for i in range(1, 13) if calendar.month_name[i].lower() == match.group(1).lower())
            try:
                end = dt.date(int(next(iter(years))), month, int(match.group(2)))
            except ValueError:
                continue
            if end <= filed_on:
                ends.add(end)
    return max(ends) if ends else None


def _header_columns(grid: list[list[str]], period_end: dt.date) -> tuple[dict[int, dict[str, Any]], str | None]:
    """Resolve explicit quarterly leaves in a span-expanded header hierarchy.

    A leaf must contain a duration group and current fiscal-year leaf.  The
    same year under an annual group is deliberately not an alternative match.
    """

    candidates: dict[int, dict[str, Any]] = {}
    headers = _table_header_rows(grid)
    for column in range(max((len(row) for row in grid), default=0)):
        trace_cells: list[str] = []
        for row in headers:
            cell = row[column].strip() if column < len(row) else ""
            if cell:
                trace_cells.append(cell)
        trace = tuple(trace_cells)
        heading = " | ".join(dict.fromkeys(trace))
        duration = _duration_evidence(heading)
        if duration is None or str(period_end.year) not in heading or not _date_matches_text(heading, period_end):
            continue
        # A duration group must be quarterly. "Year Ended" may share the same
        # period-end date and current-year leaf in the same exhibit.
        normalized = _normalized_cell(heading)
        if "year ended" in normalized or "years ended" in normalized or "twelve months" in normalized:
            continue
        candidates[column] = {
            "column_heading": heading,
            "duration_evidence": duration,
            "header_trace": trace,
        }
    if not candidates:
        return {}, "qualified_quarter_column_not_found"
    # A logical year leaf can span currency, value and spacer cells. Those
    # adjacent physical columns are one quarterly leaf, not three alternatives.
    ordered = sorted(candidates)
    if ordered == list(range(ordered[0], ordered[-1] + 1)) and len({
        evidence["column_heading"] for evidence in candidates.values()
    }) == 1:
        return candidates, None
    return {}, "ambiguous_qualified_quarter_column"


def _is_value_decoration(cell: str) -> bool:
    """Empty, currency-symbol or parenthesis-only cells are never labels."""

    return re.fullmatch(r"[\s$€£()]*", cell) is not None


def _label_lineage(grid: list[list[str]], row_number: int, column: int) -> tuple[str, ...]:
    """Recover inherited labels for indented/exhibit rows such as ``- Diluted``.

    An indented numeric Basic row is a sibling of an indented Diluted row, not
    the end of their shared ``per share`` section.  Walk back through those
    siblings to the nearest section heading, but stop at a separate numeric
    row or an older per-share section (for example, adjusted EPS).
    """

    row = grid[row_number]
    own = " ".join(dict.fromkeys(
        cell for cell in row[:column] if not _is_value_decoration(cell)
        and _parse_eps_number(cell) is None
    )).strip()
    if not own:
        return ()
    if not re.match(r"^[-\u2013\u2014\u2022]\s*", own):
        return (own,)

    parents: list[str] = []
    found_per_share = False
    for prior in range(row_number - 1, max(-1, row_number - 9), -1):
        row = grid[prior]
        leading = " ".join(dict.fromkeys(
            cell for cell in row[:column] if not _is_value_decoration(cell)
            and _parse_eps_number(cell) is None
        )).strip()
        if not leading:
            continue
        # A sibling's number may sit right of this column when the column is
        # its currency or parenthesis cell; the whole value area decides.
        if any(_parse_eps_number(cell) is not None for cell in row[column:]):
            if re.match(r"^[-\u2013\u2014\u2022]\s*", leading):
                continue
            break
        is_per_share = _PER_SHARE_RE.search(_normalized_cell(leading)) is not None
        if is_per_share and found_per_share:
            break
        parents.append(leading)
        found_per_share |= is_per_share
        if len(parents) == 2:
            break
    return (*reversed(parents), own)


def _qualified_week_period(
    *, duration: str, heading: str, period_end: dt.date, fiscal_quarter: tuple[int, str] | None
) -> tuple[dict[str, Any] | None, str | None]:
    """Create an exact week-period boundary only from explicit evidence."""

    if duration == "three_months_explicit":
        return {
            "period_start": None,
            "period_start_basis": "month_start_deferred_to_bridge",
            "duration_days": None,
        }, None
    week_count = 13 if duration == "thirteen_weeks_explicit" else 14
    if fiscal_quarter is None:
        return None, "week_fiscal_quarter_missing_or_ambiguous"
    if not _date_matches_text(heading, period_end):
        return None, "week_period_end_missing_or_ambiguous"
    # Week count plus a document-labelled end is an exact duration boundary,
    # not a calendar-quarter approximation.  Retain the derivation evidence.
    return {
        "period_start": period_end - dt.timedelta(days=week_count * 7 - 1),
        "period_start_basis": "derived_from_explicit_week_count_and_end",
        "duration_days": week_count * 7,
        "week_count": week_count,
        "fiscal_period_evidence": f"{fiscal_quarter[1]} {fiscal_quarter[0]}",
    }, None


def _fiscal_labels(text: str) -> set[tuple[int, str]]:
    """Read stated fiscal labels without mapping them to calendar dates."""

    word_map = {"first": "Q1", "1st": "Q1", "q1": "Q1", "second": "Q2", "2nd": "Q2", "q2": "Q2",
                "third": "Q3", "3rd": "Q3", "q3": "Q3", "fourth": "Q4", "4th": "Q4", "q4": "Q4"}
    labels: set[tuple[int, str]] = set()
    for match in re.finditer(
        r"\b(first|1st|q1|second|2nd|q2|third|3rd|q3|fourth|4th|q4)\s+quarter\s+"
        r"(?:fiscal\s+|fy\s*)?(20\d{2})\b"
        r"|\b(?:fiscal\s+|fy\s*)?(20\d{2})\s+"
        r"(first|1st|q1|second|2nd|q2|third|3rd|q3|fourth|4th|q4)\s+quarter\b"
        r"|\bq([1-4])\s+(?:fiscal\s+|fy\s*)?(20\d{2})\b"
        r"|\b(?:fiscal\s+|fy\s*)(20\d{2})\s+q([1-4])\b", text
    ):
        if match.group(1) is not None:
            fiscal = (int(match.group(2)), word_map[match.group(1)])
        elif match.group(3) is not None:
            fiscal = (int(match.group(3)), word_map[match.group(4)])
        elif match.group(5) is not None:
            fiscal = (int(match.group(6)), f"Q{match.group(5)}")
        else:
            fiscal = (int(match.group(7)), f"Q{match.group(8)}")
        labels.add(fiscal)
    return labels


def _reported_fiscal_quarter(document: str, period_end: dt.date | None) -> tuple[int, str] | None:
    """Use a unique structural results title, or a sole document label.

    The title structure survives HTML parsing. Prose that happens to say
    ``quarter ... results`` cannot make a comparative the current quarter.
    """

    if period_end is None:
        return None
    visible = _normalized_cell(re.sub(r"<[^>]*>", " ", document))
    labels = _fiscal_labels(visible)
    title_labels: set[tuple[int, str]] = set()

    def consider_title(markup: str, *, centered_bold: bool) -> None:
        title = _normalized_cell(re.sub(r"<[^>]*>", " ", markup))
        if len(title) > 180 or re.search(r"\b(?:compared|versus|vs\.?|prior year)\b", title):
            return
        if centered_bold and not re.search(r"\b(?:results|earnings)\s*$", title):
            return
        found = _fiscal_labels(title)
        if len(found) == 1:
            title_labels.update(found)

    for match in re.finditer(
        r"<(title|h[1-6])\b[^>]*>(.*?)</\1\s*>", document, flags=re.IGNORECASE | re.DOTALL
    ):
        consider_title(match.group(2), centered_bold=False)
    for match in re.finditer(
        r"<div\b([^>]*)>(.*?)</div\s*>", document, flags=re.IGNORECASE | re.DOTALL
    ):
        if not re.search(r"text-align\s*:\s*center\b", match.group(1), flags=re.IGNORECASE):
            continue
        if not re.search(
            r"<(?:b|strong)\b|<(?:font|span)\b[^>]*font-weight\s*:\s*(?:700|bold)\b",
            match.group(2), flags=re.IGNORECASE,
        ):
            continue
        consider_title(match.group(2), centered_bold=True)
    if len(title_labels) == 1:
        return next(iter(title_labels))
    if not title_labels and len(labels) == 1:
        return next(iter(labels))
    return None


def _eps_row_semantics(lineage: tuple[str, ...]) -> str | None:
    """Return the nearest explicit basic/diluted statement, own label first.

    A ``- Diluted`` row under a heading that mentions basic EPS is a diluted
    row; a combined ``basic and diluted`` label states both measures at once.
    """

    for part in reversed(lineage):
        text = _normalized_cell(part)
        basic, diluted = "basic" in text, "diluted" in text
        if basic and diluted:
            return "basic_and_diluted"
        if basic:
            return "basic"
        if diluted:
            return "diluted"
    return None


def _row_scope(lineage: tuple[str, ...]) -> tuple[str, ...]:
    """The row's own label and its headings up to the nearest per-share label.

    Only these describe the row; a section heading above the per-share label
    (e.g. an "Operating results" or share-count section) does not.
    """

    scope: list[str] = []
    for part in reversed(lineage):
        text = _normalized_cell(part)
        scope.append(text)
        if _PER_SHARE_RE.search(text):
            break
    return tuple(scope)


def _share_count_context(lineage: tuple[str, ...]) -> bool:
    """True when the row, or a heading in its row scope, counts shares.

    A count word only counts before a label's per-share phrase: "Shares used in
    computing net income per share" counts shares, while "Net income per share
    (based on weighted average shares)" is an EPS label.
    """

    for text in _row_scope(lineage):
        per_share = _PER_SHARE_RE.search(text)
        if _SHARE_COUNT_RE.search(text[:per_share.start()] if per_share else text):
            return True
    return False


def _dilution_inconsistency(extractions: dict[str, dict[str, Any]]) -> str | None:
    """Flag diluted EPS above basic EPS for income (never swap the values)."""

    basic = extractions.get(BASIC_EPS.measure_code)
    diluted = extractions.get(DILUTED_EPS.measure_code)
    if basic is None or diluted is None:
        return None
    if diluted["value"] > 0 and diluted["value"] - basic["value"] > _DILUTION_TOLERANCE:
        return f"diluted_exceeds_basic:basic={basic['value']:g};diluted={diluted['value']:g}"
    return None


def extract_reported_gaap_diluted_eps(
    document: str,
    *,
    period_end: dt.date | None,
    fiscal_quarter: tuple[int, str] | None = None,
    filed_on: dt.date | None = None,
) -> tuple[dict[str, Any] | None, str | None]:
    """Extract one reported GAAP diluted quarter EPS or reject the document."""

    return extract_reported_gaap_eps(
        document, measure="diluted", period_end=period_end,
        fiscal_quarter=fiscal_quarter, filed_on=filed_on,
    )


def extract_reported_gaap_eps(
    document: str,
    *,
    measure: str,
    period_end: dt.date | None,
    fiscal_quarter: tuple[int, str] | None = None,
    filed_on: dt.date | None = None,
) -> tuple[dict[str, Any] | None, str | None]:
    """Extract one reported GAAP basic or diluted quarter EPS or reject.

    The parser deliberately needs a table row and aligned current-period column;
    a nearby narrative number, adjusted/core/non-GAAP/FFO EPS and continuing- or
    discontinued-operations components never qualify.  Share-count rows never
    qualify, and the value must be EPS-shaped (``_cell_eps_value``: a decimal
    below $1,000 with balanced parentheses, possibly split across cells).  A
    missing/ambiguous period is a rejection rather than a synthetic fiscal
    boundary.  Diluted rows must not mention basic EPS.  Basic rows must state
    basic EPS explicitly; a combined ``basic and diluted`` line is basic EPS
    (the Company Facts ``EarningsPerShareBasicAndDiluted`` convention), while a
    diluted row under a basic-mentioning heading never is.  Identical GAAP
    values repeated in several tables (highlights and statements) are one fact.
    """

    if measure not in REPORTED_EPS_MEASURE_CODES:
        raise ValueError(f"unsupported reported EPS measure {measure!r}")
    parser = _HtmlTableCollector()
    parser.feed(document)
    tables = [_span_expanded_grid(table) for table in parser.tables]
    if period_end is None and filed_on is not None:
        period_end = _document_quarter_end(tables, filed_on)
    if period_end is None:
        return None, "period_end_missing"
    candidates: list[dict[str, Any]] = []
    rejected_semantics = False
    diluted_mentions_basic = False
    duration_rejection: str | None = None
    for table_number, table in enumerate(tables):
        columns, _column_reason = _header_columns(table, period_end)
        if not columns:
            continue
        _, evidence = next(iter(columns.items()))
        period_evidence, week_reason = _qualified_week_period(
            duration=evidence["duration_evidence"], heading=evidence["column_heading"],
            period_end=period_end, fiscal_quarter=fiscal_quarter,
        )
        if week_reason is not None:
            duration_rejection = week_reason
            continue
        try:
            prior_end = period_end.replace(year=period_end.year - 1)
        except ValueError:
            prior_end = None  # A leap-day comparison needs its own explicit end.
        prior_columns, _ = _header_columns(table, prior_end) if prior_end else ({}, None)
        for row_number, row in enumerate(table):
            prior_values = [value for prior_column in prior_columns if prior_column < len(row)
                            if (value := _cell_eps_value(row, prior_column)) is not None]
            prior_value = prior_values[0] if len(prior_values) == 1 else None
            for column in columns:
                if column >= len(row):
                    continue
                lineage = _label_lineage(table, row_number, column)
                label = _normalized_cell(" | ".join(lineage))
                if measure not in label or not _PER_SHARE_RE.search(label) or _share_count_context(lineage):
                    continue
                if _NON_GAAP_CONTEXT_RE.search(label) or any(
                    _NON_TOTAL_ROW_RE.search(text) for text in _row_scope(lineage)
                ):
                    rejected_semantics = True
                    continue
                if measure == "diluted":
                    row_semantics = "diluted"
                    if "basic" in label:
                        diluted_mentions_basic = True
                        continue
                else:
                    row_semantics = _eps_row_semantics(lineage)
                    if row_semantics not in ("basic", "basic_and_diluted"):
                        continue
                value = _cell_eps_value(row, column)
                if value is None:
                    continue
                candidates.append({
                    "value": value,
                    "measure_code": REPORTED_EPS_MEASURE_CODES[measure],
                    "row_semantics": row_semantics,
                    "period_end": period_end,
                    "prior_year_quarter_value": prior_value,
                    "evidence_text": " | ".join((*lineage, row[column])),
                    "table_number": table_number,
                    "row_number": row_number,
                    "column_number": column,
                    "column_heading": evidence["column_heading"],
                    "header_trace": evidence["header_trace"],
                    "row_lineage": lineage,
                    "duration_evidence": evidence["duration_evidence"],
                    **(period_evidence or {}),
                })
    if len({(c["value"], c["duration_evidence"], c.get("period_start")) for c in candidates}) == 1:
        # The same GAAP amount for the same span in several tables is one fact.
        candidates = candidates[:1]
    if len(candidates) != 1:
        return None, "ambiguous_eps_candidates" if candidates else (
            duration_rejection or (
                "rejected_non_gaap_or_adjusted" if rejected_semantics
                else "diluted_row_mentions_basic" if diluted_mentions_basic
                else f"reported_gaap_{measure}_eps_not_found"
            )
        )
    return candidates[0], None


def _receipt_id(outcome: SecEarningsReleaseOutcome) -> str:
    return _stable_id(
        "sec_earnings_release_receipt", outcome.candidate.cik, outcome.candidate.accession_number,
        outcome.document_name, outcome.document_sha256, outcome.status, outcome.reason,
    )


def _write_sec_receipt(store: DuckDBStore, outcome: SecEarningsReleaseOutcome) -> None:
    """Persist an immutable attempt, including rejects and cache outcomes."""

    clock = outcome.source_clock or SecSourceClock(None, None, None, "timestamp_missing")
    candidate = outcome.candidate
    store.con.execute(
        """
        INSERT OR IGNORE INTO sec_earnings_release_receipts (
            receipt_id, cik, source_security_id, accession_number, document_name,
            filing_date, report_date, acceptance_datetime, raw_acceptance_timestamp,
            acceptance_utc_offset, timestamp_zone_status, available_at, index_url,
            document_url, document_sha256, outcome, rejection_reason, source_url,
            retrieval_at, run_id
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, now(), ?)
        """,
        [
            _receipt_id(outcome),
            candidate.cik,
            cik_security_id(candidate.cik),
            candidate.accession_number,
            outcome.document_name,
            candidate.filing_date,
            candidate.report_date,
            candidate.acceptance_datetime,
            clock.raw_timestamp,
            clock.utc_offset,
            clock.timezone_status,
            clock.available_at,
            outcome.index_url,
            outcome.document_url,
            outcome.document_sha256,
            outcome.status,
            outcome.reason,
            candidate.source_url,
            candidate.run_id,
        ],
    )


def _fetch_sec_bytes(session: Any, url: str, *, timeout: float, maximum: int) -> bytes:
    response = session.get(url, timeout=timeout, stream=True)
    try:
        response.raise_for_status()
        return _bounded_response_bytes(response, maximum=maximum)
    finally:
        response.close()


class _PrefetchMissing(Exception):
    """The fetch worker holds no final outcome for this URL yet: the loader skips it, never fetches it."""


class _LedgeredFetchFailure(Exception):
    """A final non-200 outcome in the fetch ledger; ``kind`` matches the network path's reason."""

    def __init__(self, kind: str) -> None:
        super().__init__(kind)
        self.kind = kind


def _prefetched_bytes(store: FetchLedgerStore, url: str, *, maximum: int) -> bytes:
    record = store.lookup(url)
    if record is None:
        raise _PrefetchMissing(url)
    if record.ok:
        return store.read(record, maximum=maximum)  # SHA-verified; ValueError above the loader's cap
    if record.error == RESPONSE_TOO_LARGE:
        raise ValueError(RESPONSE_TOO_LARGE)
    raise _LedgeredFetchFailure("HTTPError")  # the network path's raise_for_status on a final 4xx


def _fetch_error_name(exc: Exception) -> str:
    return exc.kind if isinstance(exc, _LedgeredFetchFailure) else type(exc).__name__


def earnings_release_candidates_from_submissions_archive(
    zip_path: Path,
    *,
    ciks: tuple[str, ...] | None = None,
    history_start: dt.date | None = None,
    history_end: dt.date | None = None,
    run_id: str | None = None,
) -> Iterator[EarningsReleaseCandidate]:
    """Item 2.02 candidates read straight from the retained bulk submissions ZIP (no warehouse).

    Same selection as :func:`select_earnings_release_candidates`: form ``8-K``, ``items``
    containing ``2.02``, ``report_date`` inside the bounds (a missing report date fails any
    bound), one row per accession (latest acceptance, then filing date), yielded in
    ``(cik, accession)`` order. History members are required: a referenced member missing
    from the archive raises instead of silently narrowing the candidate set, and so does an
    explicitly requested CIK that the archive does not hold.
    """

    from ._submissions_archive import SubmissionsArchive
    from .sec_submissions import _normalized_cik, _parse_acceptance, _parse_date

    item_202 = re.compile(r"(^|[^0-9])2\.02([^0-9]|$)")
    with SubmissionsArchive(Path(zip_path)) as archive:
        members: Iterable[str]
        if ciks:
            members = [f"CIK{cik}.json" for cik in sorted({_normalized_cik(cik) for cik in ciks})]
            missing = [member[3:13] for member in members if member not in archive]
            if missing:
                raise ValueError(f"submissions archive {zip_path} holds no member for requested CIK(s): "
                                 f"{', '.join(missing)}")
        else:
            members = archive.main_members()
        for member in members:
            if member not in archive:
                continue
            cik = member[3:13]
            payload = json.loads(archive.read(member))
            filings = payload.get("filings", {}) or {}
            blocks = [(filings.get("recent") or {}, f"{zip_path}!{member}")]
            for item in filings.get("files", []) or []:
                name = item.get("name")
                if not name:
                    continue
                if name not in archive:
                    raise ValueError(f"submissions archive lacks history member {name} referenced by {member}")
                blocks.append((json.loads(archive.read(name)), f"{zip_path}!{name}"))
            best: dict[str, tuple[tuple[Any, ...], EarningsReleaseCandidate]] = {}
            for block, source_url in blocks:
                forms = block.get("form") or []
                for index, form in enumerate(forms):
                    if str(form or "").strip().upper() != "8-K":
                        continue
                    row = {
                        key: _submissions_column(block, key, index)
                        for key in ("items", "reportDate", "acceptanceDateTime", "filingDate",
                                    "accessionNumber", "primaryDocument")
                    }
                    if not item_202.search(str(row["items"] or "").strip()):
                        continue
                    report_date = _parse_date(row["reportDate"])
                    if history_start is not None and (report_date is None or report_date < history_start):
                        continue
                    if history_end is not None and (report_date is None or report_date > history_end):
                        continue
                    raw_acceptance = row["acceptanceDateTime"]
                    acceptance = _parse_acceptance(raw_acceptance)
                    filing_date = _parse_date(row["filingDate"])
                    candidate = EarningsReleaseCandidate(
                        cik=cik,
                        accession_number=str(row["accessionNumber"] or "").strip(),
                        filing_date=filing_date,
                        report_date=report_date,
                        acceptance_datetime=None if acceptance is None else acceptance.to_pydatetime(),
                        acceptance_datetime_raw=None if raw_acceptance is None else str(raw_acceptance).strip(),
                        primary_document=_clean_string(row["primaryDocument"]),
                        source_url=source_url,
                        run_id=run_id,
                    )
                    rank = (acceptance is not None, acceptance or pd.Timestamp.min,
                            filing_date is not None, filing_date or dt.date.min)
                    held = best.get(candidate.accession_number)
                    if held is None or rank > held[0]:
                        best[candidate.accession_number] = (rank, candidate)
            for accession in sorted(best):
                yield best[accession][1]


def _submissions_column(block: dict[str, Any], key: str, index: int) -> Any:
    values = block.get(key) or []
    return values[index] if index < len(values) else None


def fetch_sec_earnings_release_documents(
    candidates: Iterable[EarningsReleaseCandidate],
    fetch_dir: Path,
    *,
    limiter: SecRateLimiter | None = None,
    request_timeout: float = 30.0,
    max_index_bytes: int = 2_000_000,
    max_document_bytes: int = 8_000_000,
    progress: Callable[[dict[str, int]], None] | None = None,
    progress_every: int = 0,
) -> dict[str, int]:
    """Fetch half of the Item 2.02 split: filing index plus its single EX-99, into a fetch store.

    Holds no DuckDB connection. It makes exactly the requests the loader would make (a
    candidate without a usable SEC clock, or without exactly one EX-99, fetches nothing
    more), so the cache-mode loader finds every byte it needs. Ledgered URLs are never
    refetched unless their stored object is missing or corrupt; transient failures
    (0/403/429/5xx) stay retryable on the next run. A host-wide SEC block
    (``sec_http.SecBlockedError``) propagates and stops the worker.
    """

    store = FetchLedgerStore(fetch_dir).load()
    counts = {
        "candidates": 0, "no_clock": 0, "index_fetched": 0, "index_ledgered": 0, "index_failed": 0,
        "index_unparsed": 0, "index_object_repaired": 0, "ex99_not_single": 0, "document_fetched": 0,
        "document_ledgered": 0, "document_failed": 0,
        "ledger_duplicate_ok_urls_at_start": store.duplicate_ok_urls,
        "ledger_corrupt_lines_at_start": store.corrupt_lines,
    }
    for candidate in candidates:
        counts["candidates"] += 1
        clock = _daily_sec_clock(candidate.acceptance_datetime_raw, candidate.filing_date)
        if clock.available_at is None:
            counts["no_clock"] += 1
        else:
            _fetch_candidate_documents(
                store, candidate, counts, limiter=limiter, timeout=request_timeout,
                max_index_bytes=max_index_bytes, max_document_bytes=max_document_bytes,
            )
        if progress is not None and progress_every and counts["candidates"] % progress_every == 0:
            progress(dict(counts))
    return counts


def _fetch_candidate_documents(
    store: FetchLedgerStore,
    candidate: EarningsReleaseCandidate,
    counts: dict[str, int],
    *,
    limiter: SecRateLimiter | None,
    timeout: float,
    max_index_bytes: int,
    max_document_bytes: int,
) -> None:
    index_url, directory_url = _archive_urls(candidate.cik, candidate.accession_number)
    record, fetched = store.ensure(index_url, limiter=limiter, timeout=timeout, maximum=max_index_bytes)
    counts["index_fetched" if fetched else "index_ledgered"] += 1
    if not record.ok:
        counts["index_failed"] += 1
        return
    try:
        index_bytes = store.read(record)
    except ValueError as exc:
        if str(exc) != FETCH_OBJECT_SHA_MISMATCH:
            raise
        # read() quarantined the corrupt object, so ensure() refetches it; a second mismatch on freshly
        # written bytes is storage corruption and propagates instead of posing as a parse failure.
        counts["index_object_repaired"] += 1
        record, _ = store.ensure(index_url, limiter=limiter, timeout=timeout, maximum=max_index_bytes)
        if not record.ok:
            counts["index_failed"] += 1
            return
        index_bytes = store.read(record)
    try:
        documents = _ex99_documents(index_bytes.decode("utf-8", errors="replace"), directory_url)
    except Exception:
        counts["index_unparsed"] += 1  # the loader records the same parse failure from the same bytes
        return
    if len(documents) != 1:
        counts["ex99_not_single"] += 1
        return
    record, fetched = store.ensure(
        f"{directory_url}/{documents[0]}", limiter=limiter, timeout=timeout, maximum=max_document_bytes,
    )
    counts["document_fetched" if fetched else "document_ledgered"] += 1
    if not record.ok:
        counts["document_failed"] += 1


def _cached_document_path(cache_dir: Path, candidate: EarningsReleaseCandidate, document_name: str) -> Path:
    # _archive_urls validates the accession and this selector admits flat names.
    return cache_dir / candidate.cik / candidate.accession_number.replace("-", "") / document_name


def _cache_metadata_path(cache_path: Path) -> Path:
    return cache_path.with_name(f"{cache_path.name}.sha256.json")


def _atomic_cache_file(path: Path, payload: bytes) -> None:
    """Durably install one cache artifact without exposing a partial file."""

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=path.parent, prefix=f".{path.name}.", suffix=".tmp", delete=False
        ) as handle:
            temporary = Path(handle.name)
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        assert temporary is not None
        os.replace(temporary, path)
        # Directory fsync is unsupported on some Windows filesystems; the
        # document and metadata are still individually flushed and verified.
        try:
            directory_fd = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        except OSError:
            pass
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def _discard_invalid_cache(cache_path: Path) -> None:
    """Remove only the known accession-local cache object and its metadata."""

    for path in (cache_path, _cache_metadata_path(cache_path)):
        if path.is_file():
            path.unlink()


def _receipt_document_hashes(
    store: DuckDBStore, candidate: EarningsReleaseCandidate, document_name: str
) -> set[str]:
    rows = store.con.execute(
        """SELECT DISTINCT document_sha256
           FROM sec_earnings_release_receipts
           WHERE cik = ? AND accession_number = ? AND document_name = ?
             AND document_sha256 IS NOT NULL""",
        [candidate.cik, candidate.accession_number, document_name],
    ).fetchall()
    return {str(row[0]) for row in rows}


def _read_verified_cache(
    store: DuckDBStore,
    candidate: EarningsReleaseCandidate,
    document_name: str,
    cache_path: Path,
    *,
    maximum: int,
) -> bytes | None:
    """Return a cache hit only when byte count and immutable SHA agree."""

    metadata_path = _cache_metadata_path(cache_path)
    if not cache_path.is_file() and not metadata_path.is_file():
        return None
    try:
        payload = cache_path.read_bytes()
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        actual_sha = hashlib.sha256(payload).hexdigest()
        if (len(payload) > maximum or not isinstance(metadata, dict)
                or metadata.get("sha256") != actual_sha or metadata.get("byte_count") != len(payload)):
            raise ValueError("cache_sha_or_size_mismatch")
        known_hashes = _receipt_document_hashes(store, candidate, document_name)
        if known_hashes and actual_sha not in known_hashes:
            raise ValueError("cache_receipt_sha_mismatch")
        return payload
    except (OSError, ValueError, TypeError, json.JSONDecodeError):
        _discard_invalid_cache(cache_path)
        return None


def _write_verified_cache(cache_path: Path, payload: bytes) -> str:
    """Write content and its SHA metadata atomically; next run verifies both."""

    digest = hashlib.sha256(payload).hexdigest()
    _atomic_cache_file(cache_path, payload)
    _atomic_cache_file(
        _cache_metadata_path(cache_path),
        json.dumps({"sha256": digest, "byte_count": len(payload)}, sort_keys=True).encode("utf-8"),
    )
    return digest


def _terminal_sec_receipt_exists(store: DuckDBStore, candidate: EarningsReleaseCandidate) -> bool:
    row = store.con.execute(
        """SELECT EXISTS (
            SELECT 1 FROM sec_earnings_release_receipts
            WHERE cik = ? AND accession_number = ? AND outcome = 'rejected'
            UNION ALL
            SELECT 1
            FROM sec_earnings_release_receipts receipt
            WHERE receipt.cik = ? AND receipt.accession_number = ?
              AND receipt.outcome = 'accepted'
              AND EXISTS (
                  SELECT 1 FROM press_release_facts fact
                  WHERE fact.source = ? AND fact.cik = receipt.cik
                    AND fact.accession_number = receipt.accession_number
                    AND json_extract_string(fact.raw_payload_json, '$.receipt_id') = receipt.receipt_id
                    AND json_extract_string(fact.raw_payload_json, '$.document_sha256') = receipt.document_sha256
              )
        )""",
        [candidate.cik, candidate.accession_number, candidate.cik, candidate.accession_number,
         SEC_EARNINGS_RELEASE_SOURCE],
    ).fetchone()
    return bool(row and row[0])


def _combined_rejection_reason(measure_outcomes: dict[str, str]) -> str:
    """One receipt reason; per-measure detail only when the measures differ."""

    reasons = set(measure_outcomes.values())
    if len(reasons) == 1:
        return next(iter(reasons))
    return ";".join(f"{code}:{reason}" for code, reason in measure_outcomes.items())


def _sec_reported_eps_fact(
    candidate: EarningsReleaseCandidate,
    extracted: dict[str, Any],
    *,
    measure_code: str,
    fiscal: tuple[int, str],
    clock: SecSourceClock,
    receipt_id: str,
    document_name: str,
    document_sha: str,
    index_url: str,
    document_url: str,
    measure_outcomes: dict[str, str],
    run_id: str | None,
) -> dict[str, Any]:
    assert clock.available_at is not None
    return {
        "source": SEC_EARNINGS_RELEASE_SOURCE,
        "security_id": cik_security_id(candidate.cik),
        "cik": candidate.cik,
        "accession_number": candidate.accession_number,
        "form": "8-K",
        "source_item": "2.02 / EX-99",
        "source_url": document_url,
        "measure_code": measure_code,
        "fiscal_year": fiscal[0],
        "fiscal_period": fiscal[1],
        "period_end": extracted["period_end"],
        "value": extracted["value"],
        "unit": "USD_PER_SHARE",
        "basis": "GAAP",
        "is_preliminary": True,
        "extraction_confidence": 1.0,
        "evidence_text": extracted["evidence_text"],
        "filing_date": candidate.filing_date,
        "release_date": clock.available_at.date(),
        "as_of_date": extracted["period_end"],
        "available_at": clock.available_at,
        "raw_payload_json": json_dumps({
            "receipt_id": receipt_id,
            "extractor_version": REPORTED_EPS_EXTRACTOR_VERSION,
            "measure_code": measure_code,
            "row_semantics": extracted["row_semantics"],
            "measure_outcomes": measure_outcomes,
            "cik": candidate.cik,
            "source_security_id": cik_security_id(candidate.cik),
            "accession_number": candidate.accession_number,
            "event_report_date": candidate.report_date.isoformat() if candidate.report_date else None,
            "document_name": document_name,
            "document_sha256": document_sha,
            "index_url": index_url,
            "document_url": document_url,
            "raw_acceptance_timestamp": clock.raw_timestamp,
            "acceptance_utc_offset": clock.utc_offset,
            "timestamp_zone_status": clock.timezone_status,
            "table_number": extracted["table_number"],
            "row_number": extracted["row_number"],
            "column_number": extracted["column_number"],
            "column_heading": extracted["column_heading"],
            "header_trace": extracted["header_trace"],
            "row_lineage": extracted["row_lineage"],
            "duration_evidence": extracted["duration_evidence"],
            "period_start": extracted["period_start"].isoformat() if extracted["period_start"] else None,
            "period_start_basis": extracted["period_start_basis"],
            "duration_days": extracted["duration_days"],
            "week_count": extracted.get("week_count"),
            "fiscal_period_evidence": extracted.get("fiscal_period_evidence"),
            "prior_year_quarter_value": extracted["prior_year_quarter_value"],
        }),
        "run_id": run_id,
    }


def _iter_sec_earnings_release_candidates(
    store: DuckDBStore, options: SecEarningsReleaseOptions
) -> Iterable[EarningsReleaseCandidate]:
    """Keyset-stream metadata batches; no all-universe candidate list is built."""

    after: tuple[str, str] | None = None
    while True:
        batch = select_earnings_release_candidates(
            store,
            history_start=options.history_start,
            history_end=options.history_end,
            ciks=options.ciks,
            after=after,
            limit=options.candidate_batch_size,
        )
        if not batch:
            return
        yield from batch
        after = (batch[-1].cik, batch[-1].accession_number)


def refresh_sec_earnings_release_facts(
    store: DuckDBStore,
    options: SecEarningsReleaseOptions,
    *,
    session: Any | None = None,
) -> dict[str, int]:
    """Discover, fetch, cache and retain qualified reported-quarter EPS evidence.

    Results are resumable through immutable receipts.  A receipt never changes
    its CIK/accession/document identity; replays only skip terminal document
    fingerprints and retain each later accession as its own source vintage.
    This routine requires the additive receipt migration before execution.

    With ``options.fetch_dir`` the routine is loader-only: bytes come from the
    fetch worker's SHA-verified store, no session is opened, and a candidate the
    worker has not fetched yet is skipped (``not_prefetched``) without a receipt.
    """

    prefetched = FetchLedgerStore(options.fetch_dir).load() if options.fetch_dir is not None else None
    if prefetched is not None and session is not None:
        raise ValueError("fetch_dir mode is loader-only and takes no network session")
    if prefetched is None:
        session = session or sec_session(options.user_agent)
    counts = {"candidates": 0, "accepted": 0, "rejected": 0, "skipped_terminal": 0, "cache_hits": 0}
    if prefetched is not None:
        counts.update(not_prefetched=0, prefetched_documents=0)

    def fetch_bytes(url: str, maximum: int) -> bytes:
        if prefetched is not None:
            return _prefetched_bytes(prefetched, url, maximum=maximum)
        return _fetch_sec_bytes(session, url, timeout=options.request_timeout, maximum=maximum)

    for candidate in _iter_sec_earnings_release_candidates(store, options):
        counts["candidates"] += 1
        if _terminal_sec_receipt_exists(store, candidate):
            counts["skipped_terminal"] += 1
            continue
        # A normalized legacy column cannot prove the original zone. The bulk
        # loader now persists the exact SEC string. Legacy rows retain that
        # diagnosis, while a known filing date may supply only FC1's explicitly
        # conservative daily eligibility floor.
        clock = _daily_sec_clock(candidate.acceptance_datetime_raw, candidate.filing_date)
        index_url, directory_url = _archive_urls(candidate.cik, candidate.accession_number)
        if clock.available_at is None:
            outcome = SecEarningsReleaseOutcome(
                candidate, "rejected", clock.timezone_status, index_url=index_url, source_clock=clock
            )
            _write_sec_receipt(store, outcome)
            counts["rejected"] += 1
            continue
        try:
            index_bytes = fetch_bytes(index_url, options.max_index_bytes)
            documents = _ex99_documents(index_bytes.decode("utf-8", errors="replace"), directory_url)
        except _PrefetchMissing:
            counts["not_prefetched"] += 1
            continue
        except Exception as exc:
            outcome = SecEarningsReleaseOutcome(
                candidate, "fetch_failed", f"index_fetch_or_parse:{_fetch_error_name(exc)}",
                index_url=index_url, source_clock=clock,
            )
            _write_sec_receipt(store, outcome)
            counts["rejected"] += 1
            continue
        if len(documents) != 1:
            outcome = SecEarningsReleaseOutcome(
                candidate, "rejected", "ambiguous_ex99_document" if documents else "ex99_document_not_found",
                index_url=index_url, source_clock=clock,
            )
            _write_sec_receipt(store, outcome)
            counts["rejected"] += 1
            continue
        document_name = documents[0]
        document_url = f"{directory_url}/{document_name}"
        cache_path = _cached_document_path(options.cache_dir, candidate, document_name)
        try:
            if prefetched is not None:
                # The fetch store is itself content-addressed and SHA-verified: no second copy.
                document_bytes = fetch_bytes(document_url, options.max_document_bytes)
                counts["prefetched_documents"] += 1
            else:
                document_bytes = _read_verified_cache(
                    store, candidate, document_name, cache_path, maximum=options.max_document_bytes
                )
                if document_bytes is not None:
                    counts["cache_hits"] += 1
                else:
                    document_bytes = fetch_bytes(document_url, options.max_document_bytes)
                    _write_verified_cache(cache_path, document_bytes)
        except _PrefetchMissing:
            counts["not_prefetched"] += 1
            continue
        except Exception as exc:
            outcome = SecEarningsReleaseOutcome(
                candidate, "fetch_failed", f"document_fetch:{_fetch_error_name(exc)}", document_name,
                index_url, document_url, source_clock=clock,
            )
            _write_sec_receipt(store, outcome)
            counts["rejected"] += 1
            continue
        document_sha = hashlib.sha256(document_bytes).hexdigest()
        known_hashes = _receipt_document_hashes(store, candidate, document_name)
        if known_hashes and document_sha not in known_hashes:
            outcome = SecEarningsReleaseOutcome(
                candidate, "fetch_failed", "document_sha_mismatches_prior_receipt", document_name,
                index_url, document_url, document_sha, clock,
            )
            _write_sec_receipt(store, outcome)
            counts["rejected"] += 1
            continue
        document_text = document_bytes.decode("utf-8", errors="replace")
        document_parser = _HtmlTableCollector()
        document_parser.feed(document_text)
        headed_end = _document_quarter_end(
            [_span_expanded_grid(table) for table in document_parser.tables],
            candidate.filing_date or candidate.report_date,
        ) if candidate.filing_date or candidate.report_date else None
        fiscal = _reported_fiscal_quarter(document_text, headed_end)
        extractions: dict[str, dict[str, Any]] = {}
        measure_outcomes: dict[str, str] = {}
        for measure, measure_code in REPORTED_EPS_MEASURE_CODES.items():
            extracted, reason = extract_reported_gaap_eps(
                document_text, measure=measure, period_end=headed_end, fiscal_quarter=fiscal,
            )
            if fiscal is None and reason is None:
                reason = "fiscal_period_missing_or_ambiguous"
                extracted = None
            if extracted is None:
                measure_outcomes[measure_code] = reason or "reported_gaap_eps_not_found"
            else:
                extractions[measure_code] = extracted
                measure_outcomes[measure_code] = "accepted"
        dilution = _dilution_inconsistency(extractions)
        if dilution is not None:
            # Which measure is wrong is unknowable: withhold both, keep the flag.
            measure_outcomes = {code: dilution for code in measure_outcomes}
            extractions = {}
        if not extractions:
            outcome = SecEarningsReleaseOutcome(
                candidate, "rejected", _combined_rejection_reason(measure_outcomes), document_name,
                index_url, document_url, document_sha, clock,
            )
            _write_sec_receipt(store, outcome)
            counts["rejected"] += 1
            continue
        assert fiscal is not None
        receipt_outcome = SecEarningsReleaseOutcome(
            candidate, "accepted", None, document_name, index_url, document_url, document_sha, clock
        )
        # One immutable document receipt backs every accepted measure; each
        # measure is its own fact with the same clock and period evidence.
        fact_rows = [
            _sec_reported_eps_fact(
                candidate, extracted, measure_code=measure_code, fiscal=fiscal, clock=clock,
                receipt_id=_receipt_id(receipt_outcome), document_name=document_name,
                document_sha=document_sha, index_url=index_url, document_url=document_url,
                measure_outcomes=measure_outcomes, run_id=options.run_id,
            )
            for measure_code, extracted in extractions.items()
        ]
        outcome = SecEarningsReleaseOutcome(
            candidate, "accepted", None, document_name, index_url, document_url, document_sha, clock,
            fact_rows[0],
        )
        facts = normalize_press_release_rows(pd.DataFrame(fact_rows), options=PressReleaseOptions(
            source=SEC_EARNINGS_RELEASE_SOURCE, min_confidence=1.0, run_id=options.run_id
        ))
        # Accepted evidence is terminal only when its immutable source fact is
        # durable.  The same transaction rolls both back on a fact write fault.
        with store.transaction():
            _write_sec_receipt(store, outcome)
            _write_press_release_facts_frame(
                store, facts, options=PressReleaseOptions(
                    source=SEC_EARNINGS_RELEASE_SOURCE, replace_source_file=False,
                    min_confidence=1.0, run_id=options.run_id,
                ), transaction=False,
            )
        counts["accepted"] += 1
    return counts


def _stable_id(*parts: object) -> str:
    payload = "|".join("" if _is_missing(part) else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _is_missing(value: object) -> bool:
    if value is None or value is pd.NA:
        return True
    try:
        return bool(pd.isna(value))
    except (TypeError, ValueError):
        return False


def _clean_string(value: object) -> str | None:
    if _is_missing(value):
        return None
    text = str(value).strip()
    return text or None


def _normalize_press_release_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = PRESS_RELEASE_COLUMN_ALIASES.get(
            normalized,
            PRESS_RELEASE_COLUMN_ALIASES.get(compact, normalized),
        )
    return frame.rename(columns=renamed)


def _press_release_values_before(
    sentence: str,
    end: int,
    *,
    measure_code: str,
) -> tuple[float | None, float | None, float | None, int, str, str | None]:
    segment = sentence[max(0, end - 180) : end]
    values: list[tuple[float, str | None, bool]] = []
    for match in GUIDANCE_VALUE_RE.finditer(segment):
        number = float(match.group("number").replace(",", ""))
        unit = match.group("unit")
        has_currency = bool(match.group("currency"))
        if not has_currency and unit is None and 1900 <= number <= 2100 and number.is_integer():
            continue
        values.append((number, unit, has_currency))
    if not values:
        return None, None, None, 1, "PER_SHARE" if measure_code.startswith("EPS") else "VALUE", None
    number, unit, has_currency = values[-1]
    if measure_code.startswith("EPS"):
        return None, None, number, 1, "USD_PER_SHARE" if has_currency else "PER_SHARE", "USD" if has_currency else None
    scale = _guidance_scale(unit)
    return None, None, number, scale, "USD" if has_currency else "VALUE", "USD" if has_currency else None


def _press_release_value_near(
    sentence: str,
    match: re.Match[str],
    *,
    measure_code: str,
) -> tuple[float | None, int, str, str | None]:
    low, high, mid, units_scale, value_unit, currency = _guidance_values_after(
        sentence,
        match.end(),
        measure_code=measure_code,
    )
    if low is None and high is None and mid is None:
        low, high, mid, units_scale, value_unit, currency = _press_release_values_before(
            sentence,
            match.start(),
            measure_code=measure_code,
        )
    value = mid
    if value is None and low is not None and high is not None:
        value = (low + high) / 2.0
    if value is None and low is not None:
        value = low
    if value is None and high is not None:
        value = high
    if value is None:
        return None, units_scale, value_unit, currency
    return float(value) * float(units_scale), units_scale, value_unit, currency


def _press_release_confidence(
    *,
    has_period: bool,
    source_item: str,
    evidence_text: str,
) -> float:
    score = 0.78
    if "2.02" in source_item:
        score += 0.12
    if "EX-99" in source_item.upper() or "EX99" in source_item.upper():
        score += 0.03
    if has_period:
        score += 0.04
    if PRESS_RELEASE_CUE_RE.search(evidence_text):
        score += 0.02
    return min(score, 0.97)


def _release_date_from_record(record: dict[str, Any], available_at: dt.datetime | None) -> dt.date | None:
    explicit = _guidance_date_value(_guidance_record_value(record, "release_date"))
    if explicit is not None:
        return explicit
    release_ts = _guidance_ts_value(_guidance_record_value(record, "release_datetime"))
    if release_ts is not None:
        return release_ts.date()
    if available_at is not None:
        return available_at.date()
    return (
        _guidance_date_value(_guidance_record_value(record, "filing_date"))
        or _guidance_date_value(_guidance_record_value(record, "report_date"))
        or _guidance_date_value(_guidance_record_value(record, "acceptance_datetime"))
    )


def _available_at_from_record(record: dict[str, Any], release_date: dt.date | None) -> dt.datetime | None:
    value = (
        _guidance_ts_value(_guidance_record_value(record, "available_at"))
        or _guidance_ts_value(_guidance_record_value(record, "release_datetime"))
        or _guidance_ts_value(_guidance_record_value(record, "acceptance_datetime"))
    )
    if value is not None:
        return value
    if release_date is None:
        return None
    return dt.datetime.combine(release_date, dt.time(23, 59, 59))


def _security_id_from_record(record: dict[str, Any]) -> str | None:
    security_id = _guidance_record_value(record, "security_id")
    if security_id is not None:
        return str(security_id).strip()
    symbol = _guidance_record_value(record, "symbol")
    if symbol is not None:
        return security_id_for_symbol(symbol_key(symbol))
    return None


def _document_text_from_record(record: dict[str, Any]) -> str | None:
    for column in PRESS_RELEASE_TEXT_COLUMNS:
        text = _clean_guidance_text(record.get(column))
        if text:
            return text
    return None


def _extract_press_release_rows_from_record(
    record: dict[str, Any],
    *,
    source: str,
    run_id: str | None,
) -> list[dict[str, Any]]:
    text = _document_text_from_record(record)
    if not text:
        return []
    source_item = _guidance_source_item(record, text)
    if "2.02" not in source_item:
        return []

    available_at_seed = (
        _guidance_ts_value(_guidance_record_value(record, "release_datetime"))
        or _guidance_ts_value(_guidance_record_value(record, "available_at"))
        or _guidance_ts_value(_guidance_record_value(record, "acceptance_datetime"))
    )
    release_date = _release_date_from_record(record, available_at_seed)
    available_at = _available_at_from_record(record, release_date)
    filing_date = (
        _guidance_date_value(_guidance_record_value(record, "filing_date"))
        or _guidance_date_value(_guidance_record_value(record, "acceptance_datetime"))
    )
    security_id = _security_id_from_record(record)
    symbol = _guidance_record_value(record, "symbol")
    symbol = symbol_key(symbol) if symbol is not None else None
    cik = _guidance_record_value(record, "cik")
    sentences = re.split(r"(?<=[.!?])\s+", text)

    rows: list[dict[str, Any]] = []
    for sentence in sentences:
        if not PRESS_RELEASE_CUE_RE.search(sentence):
            continue
        fiscal_year, fiscal_period, period_end = _guidance_period_from_text(sentence, record)
        for measure_code, measure_re in GUIDANCE_MEASURE_PATTERNS:
            for match in measure_re.finditer(sentence):
                value, _units_scale, value_unit, _currency = _press_release_value_near(
                    sentence,
                    match,
                    measure_code=measure_code,
                )
                if value is None:
                    continue
                basis = (
                    _clean_string(_guidance_record_value(record, "basis"))
                    or _guidance_basis(sentence)
                    or "GAAP"
                )
                evidence = sentence.strip()
                rows.append(
                    {
                        "source": source,
                        "security_id": security_id,
                        "symbol": symbol,
                        "cik": str(cik).strip() if cik is not None else None,
                        "accession_number": _guidance_record_value(record, "accession_number"),
                        "form": _guidance_record_value(record, "form") or "8-K",
                        "source_item": source_item,
                        "source_url": _guidance_record_value(record, "source_url"),
                        "measure_code": measure_code,
                        "fiscal_year": fiscal_year,
                        "fiscal_period": fiscal_period,
                        "period_end": period_end,
                        "value": value,
                        "unit": value_unit,
                        "basis": basis,
                        "is_preliminary": True,
                        "extraction_confidence": _press_release_confidence(
                            has_period=period_end is not None,
                            source_item=source_item,
                            evidence_text=evidence,
                        ),
                        "evidence_text": evidence,
                        "filing_date": filing_date,
                        "release_date": release_date,
                        "as_of_date": period_end,
                        "available_at": available_at,
                        "is_latest_revision": True,
                        "raw_payload_json": json_dumps(record),
                        "run_id": run_id,
                    }
                )
    return rows


def _empty_press_release_facts_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=PRESS_RELEASE_FACT_COLUMNS)


def _series_date_from_timestamp(series: pd.Series) -> pd.Series:
    return series.map(lambda value: pd.NA if pd.isna(value) else pd.Timestamp(value).date())


def _fallback_source_item(row: pd.Series) -> str:
    explicit = row.get("source_item")
    if not _is_missing(explicit) and str(explicit).strip():
        return str(explicit).strip()
    text = _clean_guidance_text(row.get("evidence_text")) or _document_text_from_record(row.to_dict()) or ""
    return _guidance_source_item(row.to_dict(), text)


def normalize_press_release_rows(
    frame: pd.DataFrame,
    *,
    options: PressReleaseOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    """Normalize injected rows and/or extract facts from 8-K Item 2.02 text."""

    if frame.empty:
        return _empty_press_release_facts_frame()

    raw = _normalize_press_release_columns(frame.copy())
    extracted_rows: list[dict[str, Any]] = []
    has_text = pd.Series(False, index=raw.index)
    for column in PRESS_RELEASE_TEXT_COLUMNS:
        if column in raw.columns:
            has_text = has_text | _string_series(raw, column).notna()
    for record in raw[has_text].to_dict("records"):
        extracted_rows.extend(
            _extract_press_release_rows_from_record(
                record,
                source=options.source,
                run_id=options.run_id,
            )
        )

    direct_measure = raw.get("measure_code", pd.Series([pd.NA] * len(raw), index=raw.index))
    direct_value = raw.get("value", pd.Series([pd.NA] * len(raw), index=raw.index))
    direct_candidates = raw[
        direct_measure.replace("", pd.NA).notna()
        & direct_value.replace("", pd.NA).notna()
    ].copy()
    raw = (
        pd.concat([direct_candidates, pd.DataFrame(extracted_rows)], ignore_index=True)
        if extracted_rows
        else direct_candidates
    )
    if raw.empty:
        return _empty_press_release_facts_frame()

    now = now_utc_naive()
    symbol = _string_series(raw, "symbol").map(
        lambda value: symbol_key(value) if not pd.isna(value) and str(value).strip() else pd.NA
    ).astype("string")
    security_raw = _string_series(raw, "security_id")
    security_id = pd.Series(
        [
            str(existing).strip()
            if not pd.isna(existing) and str(existing).strip()
            else (security_id_for_symbol(sym) if not pd.isna(sym) and str(sym).strip() else pd.NA)
            for existing, sym in zip(security_raw, symbol)
        ],
        index=raw.index,
        dtype="string",
    )
    measure_code = pd.Series(
        [
            _canonical_measure(measure, None)
            for measure in _string_series(raw, "measure_code")
        ],
        index=raw.index,
        dtype="string",
    )
    fiscal_year = _integer_series(raw, "fiscal_year")
    fiscal_period = _string_series(raw, "fiscal_period").str.upper()
    period_end = _date_series(raw, "period_end")
    derived_period_end = pd.Series(
        [
            _period_end_from_fiscal_fields(fy, fp)
            for fy, fp in zip(fiscal_year, fiscal_period)
        ],
        index=raw.index,
        dtype="object",
    )
    period_end = period_end.where(pd.notna(period_end), derived_period_end)
    fiscal_year = fiscal_year.where(
        fiscal_year.notna(),
        period_end.map(lambda value: pd.NA if pd.isna(value) else value.year).astype("Int64"),
    )
    fiscal_period = fiscal_period.where(
        fiscal_period.notna(),
        period_end.map(lambda value: pd.NA if pd.isna(value) else f"Q{((value.month - 1) // 3) + 1}").astype("string"),
    )

    release_ts = _timestamp_series(raw, "release_datetime")
    acceptance_at = _timestamp_series(raw, "acceptance_datetime")
    available_at = _timestamp_series(raw, "available_at")
    available_at = available_at.where(available_at.notna(), release_ts)
    available_at = available_at.where(available_at.notna(), acceptance_at)
    release_date = _date_series(raw, "release_date")
    release_date = release_date.where(pd.notna(release_date), _series_date_from_timestamp(release_ts))
    release_date = release_date.where(pd.notna(release_date), _series_date_from_timestamp(available_at))
    release_date = release_date.where(pd.notna(release_date), _date_series(raw, "filing_date"))
    release_date = release_date.where(pd.notna(release_date), _date_series(raw, "report_date"))
    fallback_available_at = release_date.map(
        lambda value: (
            dt.datetime.combine(value, dt.time(23, 59, 59))
            if not pd.isna(value)
            else now
        )
    )
    available_at = available_at.where(available_at.notna(), fallback_available_at)
    filing_date = _date_series(raw, "filing_date")
    filing_date = filing_date.where(pd.notna(filing_date), _date_series(raw, "acceptance_datetime"))

    raw_value = _numeric_series(raw, "value")
    scale = _integer_series(raw, "units_scale").fillna(1)
    value = raw_value * scale
    source = _string_series(raw, "source").where(_string_series(raw, "source").notna(), options.source)
    source_item = raw.apply(_fallback_source_item, axis=1)
    evidence_text = _string_series(raw, "evidence_text")
    evidence_text = evidence_text.where(evidence_text.notna(), _string_series(raw, "document_text").str.slice(0, 500))
    extraction_confidence = _numeric_series(raw, "extraction_confidence")
    default_confidence = pd.Series(
        [
            _press_release_confidence(
                has_period=not pd.isna(pe),
                source_item=str(item),
                evidence_text="" if pd.isna(evidence) else str(evidence),
            )
            for pe, item, evidence in zip(period_end, source_item, evidence_text)
        ],
        index=raw.index,
        dtype="float64",
    )
    extraction_confidence = extraction_confidence.where(extraction_confidence.notna(), default_confidence)

    normalized = pd.DataFrame(index=raw.index)
    normalized["source"] = source
    normalized["security_id"] = security_id
    normalized["symbol"] = symbol
    normalized["cik"] = _string_series(raw, "cik")
    normalized["accession_number"] = _string_series(raw, "accession_number")
    normalized["form"] = _string_series(raw, "form").str.upper().where(_string_series(raw, "form").notna(), "8-K")
    normalized["source_item"] = source_item
    normalized["source_url"] = _string_series(raw, "source_url")
    normalized["measure_code"] = measure_code
    normalized["fiscal_year"] = fiscal_year
    normalized["fiscal_period"] = fiscal_period
    normalized["period_end"] = period_end
    normalized["value"] = value
    normalized["unit"] = _string_series(raw, "unit").str.upper()
    normalized["unit"] = normalized["unit"].where(normalized["unit"].notna(), measure_code.map(lambda m: "USD_PER_SHARE" if str(m).startswith("EPS") else "USD"))
    normalized["basis"] = _string_series(raw, "basis").str.upper()
    normalized["basis"] = normalized["basis"].where(normalized["basis"].notna(), "GAAP")
    normalized["is_preliminary"] = _bool_series(raw, "is_preliminary").where(_bool_series(raw, "is_preliminary").notna(), True)
    normalized["extraction_confidence"] = extraction_confidence
    normalized["evidence_text"] = evidence_text
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["filing_date"] = filing_date
    normalized["release_date"] = release_date
    normalized["as_of_date"] = _date_series(raw, "as_of_date").where(_date_series(raw, "as_of_date").notna(), period_end)
    normalized["available_at"] = available_at
    normalized["is_latest_revision"] = _bool_series(raw, "is_latest_revision").where(_bool_series(raw, "is_latest_revision").notna(), True)
    normalized["raw_payload_json"] = raw.get("raw_payload_json", _raw_payloads(raw))
    normalized["run_id"] = _string_series(raw, "run_id").where(_string_series(raw, "run_id").notna(), options.run_id)
    normalized["source_loaded_at"] = now

    confidence_ok = normalized["extraction_confidence"].notna() & (
        normalized["extraction_confidence"] >= float(options.min_confidence)
    )
    normalized = normalized[
        normalized["security_id"].notna()
        & normalized["measure_code"].notna()
        & normalized["fiscal_year"].notna()
        & normalized["fiscal_period"].notna()
        & normalized["period_end"].notna()
        & normalized["value"].notna()
        & normalized["available_at"].notna()
        & normalized["basis"].notna()
        & normalized["source_item"].astype("string").str.contains("2.02", na=False)
        & confidence_ok
    ].copy()
    if normalized.empty:
        return _empty_press_release_facts_frame()

    normalized["input_codes_json"] = [
        json_dumps(
            {
                "source_item": row["source_item"],
                "accession_number": row["accession_number"],
                "source_file_sha256": row["source_file_sha256"],
                "measure_code": row["measure_code"],
            }
        )
        for _, row in normalized.iterrows()
    ]
    normalized["press_release_fact_id"] = [
        _stable_id(
            "PRESS-RELEASE-FACT",
            row["source"],
            row["security_id"],
            row["measure_code"],
            row["fiscal_year"],
            row["fiscal_period"],
            row["period_end"],
            row["accession_number"],
            row["basis"],
            row["value"],
            row["source_file_sha256"],
            str(row["evidence_text"])[:160] if not pd.isna(row["evidence_text"]) else "",
        )
        for _, row in normalized.iterrows()
    ]
    return normalized[PRESS_RELEASE_FACT_COLUMNS].drop_duplicates(subset=["press_release_fact_id"])


def _write_press_release_facts_frame(
    store: DuckDBStore,
    frame: pd.DataFrame,
    *,
    options: PressReleaseOptions,
    source_file_sha256: str | None = None,
    transaction: bool = True,
) -> int:
    def write() -> None:
        if options.replace_source_file:
            if source_file_sha256:
                store.con.execute(
                    """
                    DELETE FROM press_release_facts
                    WHERE source = ?
                      AND source_file_sha256 = ?
                    """,
                    [options.source, source_file_sha256],
                )
            else:
                store.con.execute("DELETE FROM press_release_facts WHERE source = ?", [options.source])
        if frame.empty:
            return
        insert_frame(store, frame, "press_release_facts", "press_release_fact_insert")
    if transaction:
        with store.transaction():
            write()
    else:
        write()
    return int(len(frame))


def refresh_press_release_facts(
    store: DuckDBStore,
    options: PressReleaseOptions | None = None,
) -> dict[str, Any]:
    """Load preliminary press-release facts from a source file or callables."""

    options = options or PressReleaseOptions()
    if options.source_file is None and (options.fetch is None or options.parse is None):
        return {"fact_rows": 0, "reason": "source_file or fetch/parse not supplied"}

    if options.source_file is not None:
        source_file = Path(options.source_file)
        frame = _read_guidance_source_file(source_file)
        source_hash = file_sha256(source_file)
        facts = normalize_press_release_rows(
            frame,
            options=options,
            source_file_sha256=source_hash,
            source_file=source_file,
        )
        record_source_file(
            store,
            dataset_id="press_release_facts",
            source_url=str(source_file),
            cache_path=source_file,
            sha256=source_hash,
            status="loaded",
            metadata={"source": options.source, "rows": int(len(frame)), "parsed_rows": int(len(facts))},
        )
        rows_loaded = _write_press_release_facts_frame(
            store,
            facts,
            options=options,
            source_file_sha256=source_hash,
        )
        return {
            "fact_rows": rows_loaded,
            "source_file": str(source_file),
            "source_file_sha256": source_hash,
            "parsed_rows": int(len(facts)),
        }

    parsed_rows: list[dict] = []
    assert options.fetch is not None and options.parse is not None
    for raw in options.fetch():
        parsed_rows.extend(options.parse(raw))
    facts = normalize_press_release_rows(pd.DataFrame(parsed_rows), options=options)
    rows_loaded = _write_press_release_facts_frame(store, facts, options=options)
    return {"fact_rows": rows_loaded, "parsed_rows": int(len(facts))}


def _release_duration_case(bound: int) -> str:
    """SQL CASE arms giving a release's own duration window bound in days."""

    arms = [
        f"WHEN r.duration_evidence = '{evidence}' THEN {int(window[bound])}"
        for evidence, window in _RELEASE_DURATION_WINDOWS.items()
    ]
    arms.append(
        "WHEN r.duration_evidence IS NULL AND r.fiscal_period IN ('Q1', 'Q2', 'Q3', 'Q4') "
        f"THEN {int(_QUARTER_DAYS[bound])}"
    )
    arms.append(f"WHEN r.duration_evidence IS NULL AND r.fiscal_period = 'FY' THEN {int(_ANNUAL_DAYS[bound])}")
    return " ".join(arms)


def refresh_press_release_reconciliation(
    store: DuckDBStore,
    options: PressReleaseOptions | None = None,
) -> int:
    """Reconcile preliminary press-release facts to final ``est_actual`` rows.

    A final actual matches by owner, measure and period geometry (period end
    plus a duration compatible with the release's own), never by fiscal
    labels: release and filing labels follow different conventions (52/53-week
    and Jan--May year ends, fiscal-year-end changes), and a 10-Q's year-to-date
    value shares its quarter's end and filing labels.
    """

    options = options or PressReleaseOptions()
    tolerance = float(options.reconciliation_tolerance)
    with store.transaction():
        store.con.execute("DELETE FROM press_release_reconciliation WHERE source = ?", [options.source])
        store.con.execute(
            f"""
            INSERT INTO press_release_reconciliation (
                press_release_reconciliation_id,
                source,
                press_release_fact_id,
                security_id,
                symbol,
                cik,
                accession_number,
                measure_code,
                fiscal_year,
                fiscal_period,
                period_end,
                basis,
                preliminary_value,
                preliminary_available_at,
                final_actual_value,
                final_actual_available_at,
                final_actual_accession_number,
                value_difference,
                relative_difference,
                reconciliation_tolerance,
                reconciliation_status,
                pdate,
                rdq,
                as_of_date,
                available_at,
                is_latest_revision,
                run_id,
                source_loaded_at
            )
            WITH releases AS (
                SELECT
                    pr.*,
                    CASE WHEN json_valid(pr.raw_payload_json)
                         THEN json_extract_string(pr.raw_payload_json, '$.duration_evidence')
                    END AS duration_evidence
                FROM press_release_facts pr
                WHERE pr.source = ?
                  AND pr.is_preliminary
            ),
            release_geometry AS (
                -- The preliminary fact's own duration: the explicit table
                -- evidence recorded by the governed extractor, else the
                -- release's own period kind. Filing labels never join.
                SELECT
                    r.*,
                    CASE {_release_duration_case(0)} END AS min_days,
                    CASE {_release_duration_case(1)} END AS max_days
                FROM releases r
            ),
            release_keys AS (
                SELECT DISTINCT security_id, measure_code, period_end
                FROM release_geometry
            ),
            measure_concepts AS (
                -- The same measure -> us-gaap concept map that builds est_actual.
                SELECT measure_code, unnest(from_json(us_gaap_concepts, '["VARCHAR"]')) AS concept
                FROM est_measure
                WHERE us_gaap_concepts IS NOT NULL
                  AND json_valid(us_gaap_concepts)
            ),
            final_geometry AS (
                -- est_actual keeps Company Facts filing-context fiscal labels
                -- and no period start, and one filing can carry both a
                -- quarter and a year-to-date value at the same end. Its own
                -- geometry is the duration of the exact Company Facts fact it
                -- copied (owner, accession, end, measure concept, unit, value).
                SELECT DISTINCT
                    a.security_id,
                    a.measure_code,
                    a.period_end,
                    a.value,
                    a.basis,
                    a.source,
                    a.available_at,
                    a.accession_number,
                    date_diff('day', f.period_start, f.period_end) + 1 AS duration_days
                FROM est_actual a
                JOIN release_keys k
                  ON k.security_id = a.security_id
                 AND k.measure_code = a.measure_code
                 AND k.period_end = a.period_end
                JOIN measure_concepts mc
                  ON mc.measure_code = a.measure_code
                JOIN sec_company_facts f
                  ON f.security_id = a.security_id
                 AND f.accession_number = a.accession_number
                 AND f.period_end = a.period_end
                 AND f.concept = mc.concept
                 AND f.value = a.value
                 AND f.unit IS NOT DISTINCT FROM a.unit
                WHERE a.value IS NOT NULL
                  AND f.period_start IS NOT NULL
            ),
            candidates AS (
                SELECT
                    pr.press_release_fact_id,
                    pr.source,
                    pr.security_id,
                    pr.symbol,
                    pr.cik,
                    pr.accession_number,
                    pr.measure_code,
                    pr.fiscal_year,
                    pr.fiscal_period,
                    pr.period_end,
                    pr.basis,
                    pr.value AS preliminary_value,
                    pr.available_at AS preliminary_available_at,
                    pr.release_date,
                    a.value AS final_actual_value,
                    a.available_at AS final_actual_available_at,
                    a.accession_number AS final_actual_accession_number,
                    row_number() OVER (
                        PARTITION BY pr.press_release_fact_id
                        ORDER BY a.available_at ASC NULLS LAST, a.accession_number, a.duration_days
                    ) AS rn
                FROM release_geometry pr
                LEFT JOIN final_geometry a
                  ON a.security_id = pr.security_id
                 AND a.measure_code = pr.measure_code
                 AND a.period_end = pr.period_end
                 AND a.duration_days BETWEEN pr.min_days AND pr.max_days
                 AND (a.available_at IS NULL OR pr.available_at IS NULL OR a.available_at >= pr.available_at)
                 AND (pr.basis IS NULL OR a.basis IS NULL OR upper(a.basis) = upper(pr.basis))
                 AND coalesce(a.source, '') <> pr.source
            ),
            selected AS (
                SELECT *
                FROM candidates
                WHERE rn = 1
            )
            SELECT
                sha256(concat_ws('|', 'PRESS-RELEASE-RECONCILIATION', source, press_release_fact_id)) AS press_release_reconciliation_id,
                source,
                press_release_fact_id,
                security_id,
                symbol,
                cik,
                accession_number,
                measure_code,
                fiscal_year,
                fiscal_period,
                period_end,
                basis,
                preliminary_value,
                preliminary_available_at,
                final_actual_value,
                final_actual_available_at,
                final_actual_accession_number,
                CASE
                    WHEN final_actual_value IS NULL THEN NULL
                    ELSE preliminary_value - final_actual_value
                END AS value_difference,
                CASE
                    WHEN final_actual_value IS NULL OR final_actual_value = 0 THEN NULL
                    ELSE abs(preliminary_value - final_actual_value) / abs(final_actual_value)
                END AS relative_difference,
                ? AS reconciliation_tolerance,
                CASE
                    WHEN final_actual_value IS NULL THEN 'pending_final'
                    WHEN abs(preliminary_value - final_actual_value) <= ?
                      OR (
                          final_actual_value <> 0
                          AND abs(preliminary_value - final_actual_value) / abs(final_actual_value) <= ?
                      )
                    THEN 'matched_final'
                    ELSE 'value_differs'
                END AS reconciliation_status,
                coalesce(release_date, CAST(preliminary_available_at AS DATE)) AS pdate,
                coalesce(release_date, CAST(preliminary_available_at AS DATE)) AS rdq,
                period_end AS as_of_date,
                coalesce(final_actual_available_at, preliminary_available_at) AS available_at,
                true AS is_latest_revision,
                ? AS run_id,
                now() AS source_loaded_at
            FROM selected
            """,
            [options.source, tolerance, tolerance, tolerance, options.run_id],
        )
        store.con.execute(
            """
            UPDATE fundamental_periods AS fp
            SET
                pdate = CASE
                    WHEN fp.pdate IS NULL OR flash.flash_date < fp.pdate THEN flash.flash_date
                    ELSE fp.pdate
                END,
                rdq = CASE
                    WHEN fp.rdq IS NULL OR flash.flash_date < fp.rdq THEN flash.flash_date
                    ELSE fp.rdq
                END,
                updated_at = now()
            FROM (
                SELECT security_id, period_end, min(pdate) AS flash_date
                FROM press_release_reconciliation
                WHERE source = ?
                  AND pdate IS NOT NULL
                GROUP BY security_id, period_end
            ) flash
            WHERE fp.security_id = flash.security_id
              AND fp.period_end = flash.period_end
            """,
            [options.source],
        )
    return int(
        store.con.execute(
            "SELECT count(*) FROM press_release_reconciliation WHERE source = ?",
            [options.source],
        ).fetchone()[0]
    )


def press_release_coverage(
    store: DuckDBStore,
    *,
    source: str = SOURCE_NAME,
) -> dict[str, int]:
    row = store.con.execute(
        """
        SELECT
            (SELECT count(*) FROM press_release_facts WHERE source = ?) AS fact_count,
            (SELECT count(*) FROM press_release_facts WHERE source = ? AND is_preliminary) AS preliminary_count,
            (SELECT count(*) FROM press_release_reconciliation WHERE source = ? AND reconciliation_status = 'matched_final') AS matched_final_count,
            (SELECT count(*) FROM press_release_reconciliation WHERE source = ? AND reconciliation_status = 'pending_final') AS pending_final_count,
            (SELECT count(*) FROM press_release_reconciliation WHERE source = ? AND reconciliation_status = 'value_differs') AS value_differs_count,
            (
                SELECT count(*)
                FROM press_release_facts
                WHERE source = ?
                  AND release_date IS NOT NULL
                  AND CAST(available_at AS DATE) < release_date
            ) AS no_lookahead_violations,
            (
                SELECT count(*)
                FROM press_release_facts
                WHERE source = ?
                  AND measure_code LIKE 'EPS%'
                  AND (basis IS NULL OR basis = '')
            ) AS eps_missing_basis_count
        """,
        [source, source, source, source, source, source, source],
    ).fetchone()
    names = [
        "fact_count",
        "preliminary_count",
        "matched_final_count",
        "pending_final_count",
        "value_differs_count",
        "no_lookahead_violations",
        "eps_missing_basis_count",
    ]
    return {name: int(value or 0) for name, value in zip(names, row)}


def run_press_release_refresh(
    store: DuckDBStore,
    options: PressReleaseOptions | None = None,
) -> dict[str, Any]:
    """Refresh facts, reconcile to final actuals, and record S8 quality summaries."""

    options = options or PressReleaseOptions()
    fact_details = refresh_press_release_facts(store, options)
    reconciliation_rows = refresh_press_release_reconciliation(store, options)
    coverage = press_release_coverage(store, source=options.source)
    status = "passed" if coverage["no_lookahead_violations"] == 0 else "failed"
    quality_check(
        store,
        dataset_id="press_release_facts",
        table_name="press_release_facts",
        check_name="press_release_no_lookahead",
        status=status,
        observed_value=float(coverage["no_lookahead_violations"]),
        threshold_value=0.0,
        details={**coverage, "source": options.source},
    )
    return {
        **fact_details,
        "reconciliation_rows": reconciliation_rows,
        **coverage,
    }


def press_release_facts_asof(
    store: DuckDBStore,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    measure_codes: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return preliminary press-release facts visible as of a PIT timestamp."""

    as_of_ts = as_of_ts or dt.datetime.combine(as_of_date, dt.time(23, 59, 59))
    registered: list[str] = []
    try:
        joins: list[str] = []
        if security_ids:
            store.con.register("asof_press_release_sid_filter", pd.DataFrame({"security_id": list(security_ids)}))
            registered.append("asof_press_release_sid_filter")
            joins.append("JOIN asof_press_release_sid_filter sf ON sf.security_id = pr.security_id")
        if measure_codes:
            store.con.register("asof_press_release_mc_filter", pd.DataFrame({"measure_code": list(measure_codes)}))
            registered.append("asof_press_release_mc_filter")
            joins.append("JOIN asof_press_release_mc_filter mf ON mf.measure_code = pr.measure_code")
        sql = f"""
        SELECT pr.*
        FROM press_release_facts pr
        {' '.join(joins)}
        WHERE pr.as_of_date <= CAST(? AS DATE)
          AND pr.available_at <= CAST(? AS TIMESTAMP)
          AND pr.is_latest_revision
        ORDER BY pr.security_id, pr.measure_code, pr.period_end, pr.available_at
        """
        return store.con.execute(sql, [as_of_date, as_of_ts]).df()
    finally:
        for relation in registered:
            store.con.unregister(relation)


class PressReleaseDataset(Dataset):
    dataset_id = "press_release_facts"
    source_name = SOURCE_NAME
    depends_on = ("est_actual",)

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: PressReleaseOptions) -> DatasetLoadResult:
        if options.min_confidence < 0 or options.min_confidence > 1:
            raise ValueError("min_confidence must be in [0, 1]")
        details = run_press_release_refresh(store, options)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(details.get("fact_rows", 0)),
            source=options.source,
            details=details,
            run_id=options.run_id,
        )


class SecEarningsReleaseDataset(Dataset):
    """Governed public-SEC reported-quarter-EPS evidence source."""

    dataset_id = "sec_earnings_release_facts"
    source_name = SEC_EARNINGS_RELEASE_SOURCE
    depends_on = ("sec_submissions",)

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: SecEarningsReleaseOptions) -> DatasetLoadResult:
        details = refresh_sec_earnings_release_facts(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="sec_earnings_release_receipts",
            check_name="source_outcomes_recorded",
            status="passed" if details["candidates"] == details["accepted"] + details["rejected"] + details["skipped_terminal"] else "warning",
            observed_value=float(details["accepted"] + details["rejected"] + details["skipped_terminal"]),
            threshold_value=float(details["candidates"]),
            details=details,
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(details["accepted"]),
            source=self.source_name,
            details=details,
            run_id=options.run_id,
        )
