"""PF2-S8: preliminary earnings facts from 8-K Item 2.02 press releases.

The loader is injectable by design: tests and local development pass a CSV/JSON
file or fetch/parse callables, while production wiring can later point it at a
licensed filing text source. Preliminary facts are retained in their own table
and reconciled to final reported ``est_actual`` rows when those arrive.
"""
from __future__ import annotations

import calendar
import datetime as dt
import hashlib
import html
import json
import re
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path
from typing import Any, Callable, Iterable

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
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
SEC_EARNINGS_RELEASE_USER_AGENT = "atx-db/0.1 atx-research@example.com"
DEFAULT_RECONCILIATION_TOLERANCE = 0.02
EPS_CONFLICT_TOLERANCE = 0.005
SEC_FILING_DATE_CLOCK_POLICY = "sec_filed_date_plus_46h_v1"


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
    run_id: str | None = None

    def __post_init__(self) -> None:
        if self.request_timeout <= 0:
            raise ValueError("SEC earnings-release request_timeout must be positive")
        if min(self.max_index_bytes, self.max_document_bytes, self.candidate_batch_size) < 1:
            raise ValueError("SEC earnings-release byte limits must be positive")


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


class _HtmlTableCollector(HTMLParser):
    """Small dependency-free table reader retaining cell positions."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.tables: list[list[list[str]]] = []
        self._table: list[list[str]] | None = None
        self._row: list[str] | None = None
        self._cell: list[str] | None = None

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "table" and self._table is None:
            self._table = []
        elif tag == "tr" and self._table is not None:
            self._row = []
        elif tag in {"td", "th"} and self._row is not None:
            self._cell = []
        elif tag == "br" and self._cell is not None:
            self._cell.append(" ")

    def handle_endtag(self, tag: str) -> None:
        if tag in {"td", "th"} and self._cell is not None and self._row is not None:
            self._row.append(" ".join("".join(self._cell).split()))
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


def _archive_urls(cik: str, accession_number: str) -> tuple[str, str]:
    digits = str(accession_number).replace("-", "")
    if not re.fullmatch(r"\d{18}", digits):
        raise ValueError(f"invalid SEC accession {accession_number!r}")
    numeric_cik = str(int(str(cik)))
    directory = f"https://www.sec.gov/Archives/edgar/data/{numeric_cik}/{digits}"
    return f"{directory}/index.json", directory


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


def _ex99_documents(index_payload: dict[str, Any]) -> tuple[str, ...]:
    items = index_payload.get("directory", {}).get("item", [])
    selected = {
        str(item.get("name", "")).strip()
        for item in items
        if isinstance(item, dict)
        and (
            re.fullmatch(r"ex[-_]?99(?:\.\d+)?", str(item.get("type", "")).strip(), flags=re.IGNORECASE)
            or re.match(r"(?:exhibit[-_]?|ex[-_]?)99(?:[._-]|$)", str(item.get("name", "")).strip(), flags=re.IGNORECASE)
        )
        and re.search(r"\.(?:htm|html|txt)$", str(item.get("name", "")), flags=re.IGNORECASE)
    }
    return tuple(sorted(name for name in selected if name and "/" not in name and ".." not in name))


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


def _period_header_column(table: list[list[str]], period_end: dt.date) -> int | None:
    """Require an explicit three-month header and a unique current-year column."""

    header_rows = table[: min(6, len(table))]
    header_text = " ".join(cell for row in header_rows for cell in row)
    if _duration_evidence(header_text) is None:
        return None
    month_day = f"{calendar.month_name[period_end.month]} {period_end.day}"
    normalized = _normalized_cell(header_text)
    if _normalized_cell(month_day) not in normalized:
        return None
    matches: set[int] = set()
    for row in header_rows:
        for index, cell in enumerate(row):
            if str(period_end.year) in cell:
                matches.add(index)
    return next(iter(matches)) if len(matches) == 1 else None


def _duration_evidence(header_text: str) -> str | None:
    normalized = _normalized_cell(header_text)
    if "three months" in normalized:
        return "three_months_explicit"
    if re.search(r"\b13\s+weeks?\b", normalized):
        return "thirteen_weeks_explicit"
    if re.search(r"\b14\s+weeks?\b", normalized):
        return "fourteen_weeks_explicit"
    return None


def _reported_fiscal_quarter(document: str) -> tuple[int, str] | None:
    """Require a labeled fiscal quarter instead of inferring one from month-end."""

    text = _normalized_cell(document)
    resolved: set[tuple[int, str]] = set()
    word_map = {"first": "Q1", "1st": "Q1", "q1": "Q1", "second": "Q2", "2nd": "Q2", "q2": "Q2",
                "third": "Q3", "3rd": "Q3", "q3": "Q3", "fourth": "Q4", "4th": "Q4", "q4": "Q4"}
    for match in re.finditer(
        r"\b(first|1st|q1|second|2nd|q2|third|3rd|q3|fourth|4th|q4)\s+quarter\s+(20\d{2})\b"
        r"|\bq([1-4])\s+(20\d{2})\b", text
    ):
        if match.group(1) is not None:
            resolved.add((int(match.group(2)), word_map[match.group(1)]))
        else:
            resolved.add((int(match.group(4)), f"Q{match.group(3)}"))
    return next(iter(resolved)) if len(resolved) == 1 else None


def extract_reported_gaap_diluted_eps(
    document: str,
    *,
    period_end: dt.date | None,
) -> tuple[dict[str, Any] | None, str | None]:
    """Extract one reported GAAP diluted quarter EPS or reject the document.

    The parser deliberately needs a table row and aligned current-period column;
    a nearby narrative number, adjusted EPS and continuing-operations EPS never
    qualify.  A missing/ambiguous period is a rejection rather than a synthetic
    fiscal boundary.
    """

    if period_end is None:
        return None, "period_end_missing"
    parser = _HtmlTableCollector()
    parser.feed(document)
    candidates: list[dict[str, Any]] = []
    rejected_semantics = False
    for table_number, table in enumerate(parser.tables):
        column = _period_header_column(table, period_end)
        if column is None:
            continue
        table_text = _normalized_cell(" ".join(cell for row in table for cell in row))
        if any(term in table_text for term in ("non-gaap", "non gaap", "adjusted")):
            rejected_semantics = True
            continue
        for row_number, row in enumerate(table):
            if column >= len(row):
                continue
            label = _normalized_cell(row[0]) if row else ""
            if "diluted" not in label or "per share" not in label:
                continue
            if any(term in label for term in ("basic", "continuing", "adjusted", "non-gaap", "non gaap")):
                rejected_semantics = True
                continue
            value = _parse_eps_number(row[column])
            if value is None:
                continue
            candidates.append({
                "value": value,
                "evidence_text": " | ".join(row),
                "table_number": table_number,
                "row_number": row_number,
                "column_number": column,
                "column_heading": str(period_end.year),
                "duration_evidence": _duration_evidence(" ".join(cell for row in table[:6] for cell in row)),
            })
    if len(candidates) != 1:
        return None, "ambiguous_eps_candidates" if candidates else (
            "rejected_non_gaap_or_adjusted" if rejected_semantics else "reported_gaap_diluted_eps_not_found"
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


def _cached_document_path(cache_dir: Path, candidate: EarningsReleaseCandidate, document_name: str) -> Path:
    # _archive_urls validates the accession and this selector admits flat names.
    return cache_dir / candidate.cik / candidate.accession_number.replace("-", "") / document_name


def _terminal_sec_receipt_exists(store: DuckDBStore, candidate: EarningsReleaseCandidate) -> bool:
    row = store.con.execute(
        """SELECT EXISTS (
            SELECT 1 FROM sec_earnings_release_receipts
            WHERE cik = ? AND accession_number = ? AND outcome IN ('accepted', 'rejected')
        )""",
        [candidate.cik, candidate.accession_number],
    ).fetchone()
    return bool(row and row[0])


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
    """

    session = session or sec_session(options.user_agent)
    counts = {"candidates": 0, "accepted": 0, "rejected": 0, "cache_hits": 0}
    for candidate in _iter_sec_earnings_release_candidates(store, options):
        counts["candidates"] += 1
        if _terminal_sec_receipt_exists(store, candidate):
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
            index_bytes = _fetch_sec_bytes(
                session, index_url, timeout=options.request_timeout, maximum=options.max_index_bytes
            )
            documents = _ex99_documents(json.loads(index_bytes))
        except Exception as exc:
            outcome = SecEarningsReleaseOutcome(
                candidate, "fetch_failed", f"index_fetch_or_parse:{type(exc).__name__}",
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
            if cache_path.is_file():
                document_bytes = cache_path.read_bytes()
                if len(document_bytes) > options.max_document_bytes:
                    raise ValueError("cached_document_too_large")
                counts["cache_hits"] += 1
            else:
                document_bytes = _fetch_sec_bytes(
                    session, document_url, timeout=options.request_timeout, maximum=options.max_document_bytes
                )
                cache_path.parent.mkdir(parents=True, exist_ok=True)
                cache_path.write_bytes(document_bytes)
        except Exception as exc:
            outcome = SecEarningsReleaseOutcome(
                candidate, "fetch_failed", f"document_fetch:{type(exc).__name__}", document_name,
                index_url, document_url, source_clock=clock,
            )
            _write_sec_receipt(store, outcome)
            counts["rejected"] += 1
            continue
        document_sha = hashlib.sha256(document_bytes).hexdigest()
        document_text = document_bytes.decode("utf-8", errors="replace")
        fiscal = _reported_fiscal_quarter(document_text)
        extracted, reason = extract_reported_gaap_diluted_eps(document_text, period_end=candidate.report_date)
        if fiscal is None and reason is None:
            reason = "fiscal_period_missing_or_ambiguous"
            extracted = None
        if extracted is None:
            outcome = SecEarningsReleaseOutcome(
                candidate, "rejected", reason, document_name, index_url, document_url,
                document_sha, clock,
            )
            _write_sec_receipt(store, outcome)
            counts["rejected"] += 1
            continue
        receipt_outcome = SecEarningsReleaseOutcome(
            candidate, "accepted", None, document_name, index_url, document_url, document_sha, clock
        )
        fact = {
            "source": SEC_EARNINGS_RELEASE_SOURCE,
            "security_id": cik_security_id(candidate.cik),
            "cik": candidate.cik,
            "accession_number": candidate.accession_number,
            "form": "8-K",
            "source_item": "2.02 / EX-99",
            "source_url": document_url,
            "measure_code": "EPS_DILUTED",
            "fiscal_year": fiscal[0],
            "fiscal_period": fiscal[1],
            "period_end": candidate.report_date,
            "value": extracted["value"],
            "unit": "USD_PER_SHARE",
            "basis": "GAAP",
            "is_preliminary": True,
            "extraction_confidence": 1.0,
            "evidence_text": extracted["evidence_text"],
            "filing_date": candidate.filing_date,
            "release_date": clock.available_at.date(),
            "as_of_date": candidate.report_date,
            "available_at": clock.available_at,
            "raw_payload_json": json_dumps({
                "receipt_id": _receipt_id(receipt_outcome),
                "cik": candidate.cik,
                "source_security_id": cik_security_id(candidate.cik),
                "accession_number": candidate.accession_number,
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
                "duration_evidence": extracted["duration_evidence"],
            }),
            "run_id": options.run_id,
        }
        outcome = SecEarningsReleaseOutcome(
            candidate, "accepted", None, document_name, index_url, document_url, document_sha, clock, fact
        )
        _write_sec_receipt(store, outcome)
        facts = normalize_press_release_rows(pd.DataFrame([fact]), options=PressReleaseOptions(
            source=SEC_EARNINGS_RELEASE_SOURCE, min_confidence=1.0, run_id=options.run_id
        ))
        _write_press_release_facts_frame(
            store, facts, options=PressReleaseOptions(
                source=SEC_EARNINGS_RELEASE_SOURCE, replace_source_file=False,
                min_confidence=1.0, run_id=options.run_id,
            )
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
) -> int:
    with store.transaction():
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
            return 0
        insert_frame(store, frame, "press_release_facts", "press_release_fact_insert")
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


def refresh_press_release_reconciliation(
    store: DuckDBStore,
    options: PressReleaseOptions | None = None,
) -> int:
    """Reconcile preliminary press-release facts to final ``est_actual`` rows."""

    options = options or PressReleaseOptions()
    tolerance = float(options.reconciliation_tolerance)
    with store.transaction():
        store.con.execute("DELETE FROM press_release_reconciliation WHERE source = ?", [options.source])
        store.con.execute(
            """
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
            WITH candidates AS (
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
                        ORDER BY a.available_at ASC NULLS LAST, a.accession_number
                    ) AS rn
                FROM press_release_facts pr
                LEFT JOIN est_actual a
                  ON a.security_id = pr.security_id
                 AND a.measure_code = pr.measure_code
                 AND a.fiscal_year = pr.fiscal_year
                 AND a.fiscal_period = pr.fiscal_period
                 AND a.period_end = pr.period_end
                 AND a.value IS NOT NULL
                 AND (a.available_at IS NULL OR pr.available_at IS NULL OR a.available_at >= pr.available_at)
                 AND (pr.basis IS NULL OR a.basis IS NULL OR upper(a.basis) = upper(pr.basis))
                 AND coalesce(a.source, '') <> pr.source
                WHERE pr.source = ?
                  AND pr.is_preliminary
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
