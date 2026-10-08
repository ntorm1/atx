from __future__ import annotations

import calendar
import csv
import datetime as dt
import email.utils
import gzip
import hashlib
import io
import json
import logging
import os
import time
import zlib
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Literal
from urllib.parse import urljoin

import pandas as pd
import requests

from .calendar import next_session
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .sec_http import PUBLIC_DATA_USER_AGENT
from .security_master import security_ids_for_symbols
from .warehouse import security_id_for_symbol


API_URL = "https://api.finra.org/data/group/otcMarket/name/consolidatedShortInterest"

RAW_COLUMNS = [
    "accountingYearMonthNumber",
    "symbolCode",
    "issueName",
    "issuerServicesGroupExchangeCode",
    "marketClassCode",
    "currentShortPositionQuantity",
    "previousShortPositionQuantity",
    "stockSplitFlag",
    "averageDailyVolumeQuantity",
    "daysToCoverQuantity",
    "revisionFlag",
    "changePercent",
    "changePreviousNumber",
    "settlementDate",
]

COLUMN_RENAMES = {
    "accountingYearMonthNumber": "accounting_year_month_number",
    "symbolCode": "symbol",
    "issueName": "issue_name",
    "issuerServicesGroupExchangeCode": "issuer_services_group_exchange_code",
    "marketClassCode": "market_class_code",
    "currentShortPositionQuantity": "current_short_position_quantity",
    "previousShortPositionQuantity": "previous_short_position_quantity",
    "stockSplitFlag": "stock_split_flag",
    "averageDailyVolumeQuantity": "average_daily_volume_quantity",
    "daysToCoverQuantity": "days_to_cover_quantity",
    "revisionFlag": "revision_flag",
    "changePercent": "change_percent",
    "changePreviousNumber": "change_previous_number",
    "settlementDate": "settlement_date",
}

OUTPUT_COLUMNS = [
    "settlement_date",
    "accounting_year_month_number",
    "symbol",
    "issue_name",
    "issuer_services_group_exchange_code",
    "market_class_code",
    "current_short_position_quantity",
    "previous_short_position_quantity",
    "stock_split_flag",
    "average_daily_volume_quantity",
    "days_to_cover_quantity",
    "revision_flag",
    "change_percent",
    "change_previous_number",
]


@dataclass(frozen=True)
class FinraShortInterestOptions:
    api_url: str = API_URL
    symbol: str | None = None
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    limit: int = 5000
    request_timeout: int = 120
    max_retries: int = 5
    retry_sleep: float = 1.0
    limit_dates: int | None = None
    date_order: Literal["asc", "desc"] = "desc"
    user_agent: str = PUBLIC_DATA_USER_AGENT  # non-SEC source: descriptive agent, no contact address
    run_id: str | None = None


def subtract_years(value: dt.date, years: int) -> dt.date:
    try:
        return value.replace(year=value.year - years)
    except ValueError:
        return value.replace(year=value.year - years, day=28)


def parse_date(value: str | None) -> dt.date | None:
    if value is None or value == "":
        return None
    return dt.date.fromisoformat(value)


def month_iter(start_date: dt.date, end_date: dt.date) -> list[tuple[int, int]]:
    year = start_date.year
    month = start_date.month
    result: list[tuple[int, int]] = []
    while (year, month) <= (end_date.year, end_date.month):
        result.append((year, month))
        if month == 12:
            year += 1
            month = 1
        else:
            month += 1
    return result


def period_candidate_windows(start_date: dt.date, end_date: dt.date) -> list[list[dt.date]]:
    windows: list[list[dt.date]] = []
    for year, month in month_iter(start_date, end_date):
        _, last_day = calendar.monthrange(year, month)
        for anchor_day in (15, last_day):
            anchor = dt.date(year, month, anchor_day)
            window = [anchor - dt.timedelta(days=offset) for offset in range(8)]
            window = [value for value in window if start_date <= value <= end_date]
            if window:
                windows.append(window)
    return windows


def request_session(user_agent: str) -> requests.Session:
    session = requests.Session()
    session.headers.update(
        {
            "Accept": "text/plain",
            "Content-Type": "application/json",
            "User-Agent": user_agent,
        }
    )
    return session


def post_finra(
    session: requests.Session,
    api_url: str,
    payload: dict[str, Any],
    timeout: int,
    max_retries: int,
    retry_sleep: float,
) -> requests.Response:
    last_exc: Exception | None = None
    for attempt in range(1, max_retries + 1):
        try:
            response = session.post(api_url, json=payload, timeout=timeout)
            if response.status_code in (429, 500, 502, 503, 504):
                raise requests.HTTPError(f"retryable HTTP {response.status_code}", response=response)
            response.raise_for_status()
            return response
        except Exception as exc:
            last_exc = exc
            if attempt == max_retries:
                break
            sleep_for = retry_sleep * (2 ** (attempt - 1))
            logging.warning(
                "FINRA request failed on attempt %d/%d: %s; sleeping %.1fs",
                attempt,
                max_retries,
                exc,
                sleep_for,
            )
            time.sleep(sleep_for)
    raise RuntimeError(f"FINRA request failed after {max_retries} attempts: {last_exc}") from last_exc


def date_payload(settlement_date: dt.date, limit: int, offset: int = 0) -> dict[str, Any]:
    return {
        "compareFilters": [
            {
                "compareType": "EQUAL",
                "fieldName": "settlementDate",
                "fieldValue": settlement_date.isoformat(),
            }
        ],
        "limit": limit,
        "offset": offset,
    }


def symbol_payload(symbol: str, limit: int, offset: int = 0) -> dict[str, Any]:
    return {
        "compareFilters": [
            {
                "compareType": "EQUAL",
                "fieldName": "symbolCode",
                "fieldValue": symbol.upper(),
            }
        ],
        "limit": limit,
        "offset": offset,
    }


def record_total(
    session: requests.Session,
    api_url: str,
    payload: dict[str, Any],
    timeout: int,
    max_retries: int,
    retry_sleep: float,
) -> int:
    probe = dict(payload)
    probe["limit"] = 1
    probe["offset"] = 0
    response = post_finra(session, api_url, probe, timeout, max_retries, retry_sleep)
    return int(response.headers.get("record-total", "0"))


def discover_settlement_dates(
    session: requests.Session,
    api_url: str,
    start_date: dt.date,
    end_date: dt.date,
    timeout: int,
    max_retries: int,
    retry_sleep: float,
) -> dict[dt.date, int]:
    discovered: dict[dt.date, int] = {}
    for window in period_candidate_windows(start_date, end_date):
        for candidate in window:
            if candidate in discovered:
                break
            total = record_total(
                session=session,
                api_url=api_url,
                payload=date_payload(candidate, limit=1),
                timeout=timeout,
                max_retries=max_retries,
                retry_sleep=retry_sleep,
            )
            if total > 0:
                discovered[candidate] = total
                break
    return dict(sorted(discovered.items()))


def parse_csv_frame(text: str) -> pd.DataFrame:
    if not text.strip():
        return pd.DataFrame(columns=RAW_COLUMNS)
    return pd.read_csv(
        io.StringIO(text),
        dtype=str,
        keep_default_na=False,
        na_values=[],
        quoting=csv.QUOTE_MINIMAL,
    )


def normalize_frame(frame: pd.DataFrame, fallback_settlement_date: dt.date | None = None) -> pd.DataFrame:
    if frame.empty:
        return pd.DataFrame(columns=OUTPUT_COLUMNS)

    missing = [column for column in RAW_COLUMNS if column not in frame.columns]
    if missing:
        raise ValueError(f"FINRA CSV missing expected columns: {missing}")

    frame = frame[RAW_COLUMNS].rename(columns=COLUMN_RENAMES)
    frame["symbol"] = frame["symbol"].str.upper().str.strip()
    frame["settlement_date"] = pd.to_datetime(frame["settlement_date"], errors="coerce").dt.date
    if fallback_settlement_date is not None:
        frame.loc[frame["settlement_date"].isna(), "settlement_date"] = fallback_settlement_date

    int_columns = [
        "accounting_year_month_number",
        "current_short_position_quantity",
        "previous_short_position_quantity",
        "average_daily_volume_quantity",
        "change_previous_number",
    ]
    for column in int_columns:
        frame[column] = pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce").astype("Int64")

    for column in ("days_to_cover_quantity", "change_percent"):
        frame[column] = pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce")

    text_columns = [
        "symbol",
        "issue_name",
        "issuer_services_group_exchange_code",
        "market_class_code",
        "stock_split_flag",
        "revision_flag",
    ]
    for column in text_columns:
        frame[column] = frame[column].replace("", pd.NA).astype("string")

    return frame[OUTPUT_COLUMNS]


def filter_date_range(
    frame: pd.DataFrame,
    start_date: dt.date | None,
    end_date: dt.date | None,
) -> pd.DataFrame:
    if frame.empty:
        return frame
    mask = pd.Series(True, index=frame.index)
    if start_date is not None:
        mask &= frame["settlement_date"] >= start_date
    if end_date is not None:
        mask &= frame["settlement_date"] <= end_date
    return frame.loc[mask].reset_index(drop=True)


def fetch_payload_frames(
    session: requests.Session,
    api_url: str,
    payload_factory: Any,
    total: int,
    limit: int,
    timeout: int,
    max_retries: int,
    retry_sleep: float,
    fallback_settlement_date: dt.date | None = None,
) -> pd.DataFrame:
    frames: list[pd.DataFrame] = []
    offset = 0
    while offset < total:
        response = post_finra(
            session=session,
            api_url=api_url,
            payload=payload_factory(limit, offset),
            timeout=timeout,
            max_retries=max_retries,
            retry_sleep=retry_sleep,
        )
        frame = parse_csv_frame(response.text)
        if frame.empty:
            break
        frames.append(frame)
        offset += len(frame)

    if not frames:
        raw = pd.DataFrame(columns=RAW_COLUMNS)
    else:
        raw = pd.concat(frames, ignore_index=True)
    normalized = normalize_frame(raw, fallback_settlement_date)
    if len(normalized) != total:
        raise RuntimeError(f"downloaded {len(normalized)} rows, expected {total}")
    return normalized


def concat_frames(frames: Iterable[pd.DataFrame]) -> pd.DataFrame:
    realized = [frame for frame in frames if not frame.empty]
    if not realized:
        return pd.DataFrame(columns=OUTPUT_COLUMNS)
    return pd.concat(realized, ignore_index=True)


class FinraShortInterestDataset(Dataset):
    dataset_id = "finra_short_interest"
    source_name = "FINRA consolidatedShortInterest"

    def ensure_schema(self, store: DuckDBStore) -> None:
        con = store.con
        con.execute(
            """
            CREATE TABLE IF NOT EXISTS finra_short_interest (
                security_id VARCHAR,
                settlement_date DATE NOT NULL,
                accounting_year_month_number INTEGER,
                symbol VARCHAR NOT NULL,
                issue_name VARCHAR,
                issuer_services_group_exchange_code VARCHAR,
                market_class_code VARCHAR,
                current_short_position_quantity BIGINT,
                previous_short_position_quantity BIGINT,
                stock_split_flag VARCHAR,
                average_daily_volume_quantity BIGINT,
                days_to_cover_quantity DOUBLE,
                revision_flag VARCHAR,
                change_percent DOUBLE,
                change_previous_number BIGINT,
                available_at TIMESTAMP,
                run_id VARCHAR,
                source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
                source_url VARCHAR NOT NULL
            )
            """
        )
        for statement in (
            "ALTER TABLE finra_short_interest ADD COLUMN IF NOT EXISTS security_id VARCHAR",
            "ALTER TABLE finra_short_interest ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
            "ALTER TABLE finra_short_interest ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        ):
            con.execute(statement)
        con.execute(
            """
            CREATE INDEX IF NOT EXISTS idx_finra_short_interest_symbol_date
            ON finra_short_interest(symbol, settlement_date)
            """
        )
        con.execute(
            """
            CREATE OR REPLACE VIEW v_finra_short_interest_latest AS
            SELECT *
            FROM (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY symbol
                        ORDER BY settlement_date DESC
                    ) AS recency_rank
                FROM finra_short_interest
            )
            WHERE recency_rank = 1
            """
        )

    def load(self, store: DuckDBStore, options: FinraShortInterestOptions) -> DatasetLoadResult:
        if options.limit < 1 or options.limit > 5000:
            raise ValueError("FINRA limit must be between 1 and 5000")
        if options.date_order not in {"asc", "desc"}:
            raise ValueError("FINRA date_order must be 'asc' or 'desc'")

        session = request_session(options.user_agent)
        if options.symbol:
            frames = [
                self._download_symbol(session, options),
            ]
            mode = f"symbol={options.symbol.upper()}"
        else:
            if options.start_date is None or options.end_date is None:
                raise ValueError(
                    "start_date and end_date are required for a date-range FINRA load; "
                    "the CLI passes atx_db.clock.utc_today()-derived defaults explicitly"
                )
            start_date = options.start_date
            end_date = options.end_date
            frames = self._download_dates(session, options, start_date, end_date)
            mode = f"dates={start_date}:{end_date}"

        frame = concat_frames(frames)
        frame = filter_date_range(frame, options.start_date, options.end_date)
        rows = self._replace_rows(store, frame, options)
        self._upsert_watermark(store, frame)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.api_url,
            details={
                "mode": mode,
                "min_settlement_date": None if frame.empty else str(frame["settlement_date"].min()),
                "max_settlement_date": None if frame.empty else str(frame["settlement_date"].max()),
                "date_order": options.date_order,
                "symbols": [] if frame.empty else sorted(frame["symbol"].dropna().unique().tolist()),
            },
        )

    def _download_symbol(
        self,
        session: requests.Session,
        options: FinraShortInterestOptions,
    ) -> pd.DataFrame:
        symbol = options.symbol or ""
        payload = symbol_payload(symbol, options.limit)
        total = record_total(
            session,
            options.api_url,
            payload,
            options.request_timeout,
            options.max_retries,
            options.retry_sleep,
        )
        return fetch_payload_frames(
            session=session,
            api_url=options.api_url,
            payload_factory=lambda limit, offset: symbol_payload(symbol, limit, offset),
            total=total,
            limit=options.limit,
            timeout=options.request_timeout,
            max_retries=options.max_retries,
            retry_sleep=options.retry_sleep,
        )

    def _download_dates(
        self,
        session: requests.Session,
        options: FinraShortInterestOptions,
        start_date: dt.date,
        end_date: dt.date,
    ) -> list[pd.DataFrame]:
        settlement_totals = discover_settlement_dates(
            session=session,
            api_url=options.api_url,
            start_date=start_date,
            end_date=end_date,
            timeout=options.request_timeout,
            max_retries=options.max_retries,
            retry_sleep=options.retry_sleep,
        )
        if not settlement_totals:
            raise RuntimeError(f"No FINRA settlement dates found from {start_date} through {end_date}")

        settlement_items = list(settlement_totals.items())
        if options.date_order == "desc":
            settlement_items = list(reversed(settlement_items))
        if options.limit_dates is not None:
            settlement_items = settlement_items[: options.limit_dates]

        frames: list[pd.DataFrame] = []
        for settlement_date, total in settlement_items:
            frames.append(
                fetch_payload_frames(
                    session=session,
                    api_url=options.api_url,
                    payload_factory=lambda limit, offset, value=settlement_date: date_payload(value, limit, offset),
                    total=total,
                    limit=options.limit,
                    timeout=options.request_timeout,
                    max_retries=options.max_retries,
                    retry_sleep=options.retry_sleep,
                    fallback_settlement_date=settlement_date,
                )
            )
        return frames

    def _replace_rows(self, store: DuckDBStore, frame: pd.DataFrame, options: FinraShortInterestOptions) -> int:
        if frame.empty:
            return 0

        load_frame = frame.copy()
        sec_map = security_ids_for_symbols(store, sorted(load_frame["symbol"].dropna().unique().tolist()))
        load_frame["security_id"] = load_frame["symbol"].map(lambda symbol: sec_map.get(symbol, security_id_for_symbol(symbol)))
        load_frame["available_at"] = pd.to_datetime(load_frame["settlement_date"]) + pd.Timedelta(days=10, hours=22)
        load_frame["run_id"] = options.run_id
        load_frame["source_url"] = options.api_url
        con = store.con
        con.register("finra_short_interest_load", load_frame)
        try:
            with store.transaction():
                con.execute(
                    """
                    DELETE FROM finra_short_interest AS dst
                    USING finra_short_interest_load AS src
                    WHERE dst.settlement_date = src.settlement_date
                      AND dst.symbol = src.symbol
                      AND coalesce(dst.market_class_code, '') = coalesce(src.market_class_code, '')
                    """
                )
                con.execute(
                    """
                    INSERT INTO finra_short_interest (
                        security_id,
                        settlement_date,
                        accounting_year_month_number,
                        symbol,
                        issue_name,
                        issuer_services_group_exchange_code,
                        market_class_code,
                        current_short_position_quantity,
                        previous_short_position_quantity,
                        stock_split_flag,
                        average_daily_volume_quantity,
                        days_to_cover_quantity,
                        revision_flag,
                        change_percent,
                        change_previous_number,
                        available_at,
                        run_id,
                        source_url
                    )
                    SELECT
                        security_id,
                        settlement_date,
                        accounting_year_month_number,
                        symbol,
                        issue_name,
                        issuer_services_group_exchange_code,
                        market_class_code,
                        current_short_position_quantity,
                        previous_short_position_quantity,
                        stock_split_flag,
                        average_daily_volume_quantity,
                        days_to_cover_quantity,
                        revision_flag,
                        change_percent,
                        change_previous_number,
                        available_at,
                        run_id,
                        source_url
                    FROM finra_short_interest_load
                    """
                )
                self._upsert_fallback_security_master(store)
        finally:
            con.unregister("finra_short_interest_load")
        return int(len(load_frame))

    def _upsert_fallback_security_master(self, store: DuckDBStore) -> None:
        source = f"{self.source_name} fallback security master"
        con = store.con
        con.execute(
            """
            CREATE OR REPLACE TEMP TABLE finra_fallback_security_seed AS
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                coalesce(any_value(issue_name), any_value(symbol)) AS issue_name,
                min(settlement_date) AS first_seen_date,
                max(settlement_date) AS last_seen_date,
                min(available_at) AS first_available_at
            FROM finra_short_interest
            WHERE security_id LIKE 'US-TICKER-%'
              AND security_id IS NOT NULL
              AND security_id <> ''
              AND symbol IS NOT NULL
              AND symbol <> ''
            GROUP BY security_id
            """
        )
        con.execute(
            """
            DELETE FROM securities
            USING finra_fallback_security_seed AS src
            WHERE securities.security_id = src.security_id
              AND securities.source = ?
            """,
            [source],
        )
        con.execute(
            """
            INSERT INTO securities (
                security_id,
                issuer_id,
                primary_symbol,
                name,
                asset_class,
                country,
                currency,
                active,
                first_seen_date,
                last_seen_date,
                source,
                source_loaded_at
            )
            SELECT
                src.security_id,
                'FINRA-SYMBOL-' || src.symbol,
                src.symbol,
                src.issue_name,
                'EQUITY',
                'US',
                'USD',
                TRUE,
                src.first_seen_date,
                NULL::DATE,
                ?,
                now()
            FROM finra_fallback_security_seed src
            WHERE NOT EXISTS (
                SELECT 1
                FROM securities s
                WHERE s.security_id = src.security_id
            )
            """,
            [source],
        )
        con.execute(
            """
            DELETE FROM security_identifier_history
            USING finra_fallback_security_seed AS src
            WHERE security_identifier_history.security_id = src.security_id
              AND security_identifier_history.id_type = 'TICKER'
              AND security_identifier_history.id_value = src.symbol
              AND security_identifier_history.source = ?
            """,
            [source],
        )
        con.execute(
            """
            INSERT INTO security_identifier_history (
                security_id,
                id_type,
                id_value,
                valid_from,
                valid_to,
                as_of_date,
                available_at,
                source,
                run_id,
                source_loaded_at
            )
            SELECT
                security_id,
                'TICKER',
                symbol,
                first_seen_date,
                NULL::DATE,
                first_seen_date,
                first_available_at,
                ?,
                NULL,
                now()
            FROM finra_fallback_security_seed
            """,
            [source],
        )

    def _upsert_watermark(self, store: DuckDBStore, frame: pd.DataFrame) -> None:
        if frame.empty:
            return
        max_date = str(frame["settlement_date"].max())
        store.con.execute(
            """
            DELETE FROM dataset_watermarks
            WHERE dataset_id = ? AND watermark_name = 'max_settlement_date'
            """,
            [self.dataset_id],
        )
        store.con.execute(
            """
            INSERT INTO dataset_watermarks (
                dataset_id, watermark_name, watermark_value, updated_at
            )
            VALUES (?, 'max_settlement_date', ?, now())
            """,
            [self.dataset_id, max_date],
        )


# ---------------------------------------------------------------------------
# Fetch half (tier-1 v2 X.2 Steps 1-2, ruling C-91): FINRA files + receipts, no warehouse
# ---------------------------------------------------------------------------

#: FINRA is not SEC, but it is fetched politely: one paced client per process, never above 5 requests/second.
FINRA_MAX_REQUESTS_PER_SECOND = 5.0
FINRA_DEFAULT_REQUESTS_PER_SECOND = 4.0
FINRA_RETRY_STATUS_CODES = frozenset({429, 500, 502, 503, 504})
#: CloudFront answers a file that is not (yet) published with 403 AccessDenied (sometimes 404).
FINRA_ABSENT_STATUS_CODES = frozenset({403, 404})
_FINRA_TRANSIENT_ERRORS = (
    requests.ConnectionError,
    requests.Timeout,
    requests.exceptions.ChunkedEncodingError,
    requests.exceptions.ContentDecodingError,
)
_FINRA_MAX_BACKOFF_SECONDS = 120.0

#: Short interest is published 7 business (XNYS) days after its settlement date: the FINRA reporting
#: schedule's Publication Date column (every 2025/2026 row reproduces, task-X.12-fetch-report.md).
SHORT_INTEREST_PUBLICATION_LAG_SESSIONS = 7
#: Official twice-monthly files (from 2018-11; exchange-listed issues only from June 2021 on).
SHORT_INTEREST_CDN_URL = "https://cdn.finra.org/equity/otcmarket/biweekly/shrt{day:%Y%m%d}.csv"
#: OTC-only static archive (settlements 2014-11-14 .. 2019-06-28), listed by the OTCE app's JSON index.
SHORT_INTEREST_ARCHIVE_INDEX_URL = (
    "https://otce.finra.org/otce/assets/archives/equityShortInterest/equityShortInterest.json"
)
SHORT_INTEREST_ARCHIVE_BASE_URL = "https://otce.finra.org/otce/"
#: settlementDate is the dataset's partition key: with its EQUAL filter a sort is allowed, so pages are stable.
SHORT_INTEREST_API_SORT = ("symbolCode", "marketClassCode")
FINRA_RECEIPTS_NAME = "receipts.jsonl"
FINRA_OK_OUTCOMES = frozenset({"fetched", "verified"})


def short_interest_publication_date(settlement_date: dt.date) -> dt.date:
    """Settlement date + 7 XNYS sessions: the day FINRA publishes that settlement's short interest."""

    day = settlement_date
    for _ in range(SHORT_INTEREST_PUBLICATION_LAG_SESSIONS):
        day = next_session(day)
    return day


def _retry_after_seconds(response: requests.Response) -> float:
    value = response.headers.get("Retry-After")
    if not value:
        return 0.0
    value = value.strip()
    if value.isdigit():
        return min(float(value), _FINRA_MAX_BACKOFF_SECONDS)
    try:
        when = email.utils.parsedate_to_datetime(value)
    except (TypeError, ValueError):
        return 0.0
    if when.tzinfo is None:
        when = when.replace(tzinfo=dt.UTC)
    return min(max((when - dt.datetime.now(dt.UTC)).total_seconds(), 0.0), _FINRA_MAX_BACKOFF_SECONDS)


class PoliteFinraClient:
    """One paced ``requests`` session for FINRA public data (api / cdn / otce.finra.org).

    Every attempt (retries included) is spaced at least ``1 / requests_per_second`` apart, at most 5/s;
    429/5xx and transport errors are retried with exponential backoff (``Retry-After`` honored, capped at
    120 s). The body is read inside the retry scope. The user agent is descriptive and carries no contact
    address. :meth:`stats` reports the observed rate (``max_in_any_second`` over the whole run).
    """

    def __init__(
        self,
        *,
        user_agent: str = PUBLIC_DATA_USER_AGENT,
        requests_per_second: float = FINRA_DEFAULT_REQUESTS_PER_SECOND,
        max_attempts: int = 6,
        backoff_s: float = 2.0,
        timeout: float = 60.0,
    ) -> None:
        if not 0 < requests_per_second <= FINRA_MAX_REQUESTS_PER_SECOND:
            raise ValueError(f"requests_per_second must be in (0, {FINRA_MAX_REQUESTS_PER_SECOND}]")
        if "@" in user_agent:
            raise ValueError("FINRA requests must not carry a contact address in the user agent")
        if max_attempts < 1:
            raise ValueError("max_attempts must be positive")
        self.user_agent = user_agent
        self.requests_per_second = requests_per_second
        self.interval = 1.0 / requests_per_second
        self.max_attempts = max_attempts
        self.backoff_s = backoff_s
        self.timeout = timeout
        self.session = requests.Session()
        self.session.headers.update({"User-Agent": user_agent, "Accept-Encoding": "gzip, deflate"})
        self._next_slot = 0.0
        self._window: deque[float] = deque()
        self.requests = 0
        self.retries = 0
        self.transport_errors = 0
        self.max_in_any_second = 0
        self.first_request_at: float | None = None
        self.last_request_at: float | None = None
        self.status_counts: dict[int, int] = {}

    def _pace(self) -> None:
        now = time.monotonic()
        if now < self._next_slot:
            time.sleep(self._next_slot - now)
            now = time.monotonic()
        self._next_slot = now + self.interval
        self._window.append(now)
        while now - self._window[0] >= 1.0:
            self._window.popleft()
        self.max_in_any_second = max(self.max_in_any_second, len(self._window))
        self.requests += 1
        wall = time.time()
        if self.first_request_at is None:
            self.first_request_at = wall
        self.last_request_at = wall

    def _backoff(self, attempt: int) -> float:
        return min(self.backoff_s * (2 ** (attempt - 1)), _FINRA_MAX_BACKOFF_SECONDS)

    def request(self, method: str, url: str, **kwargs: Any) -> requests.Response:
        kwargs.setdefault("timeout", self.timeout)
        for attempt in range(1, self.max_attempts + 1):
            self._pace()
            try:
                response = self.session.request(method, url, **kwargs)
                _ = response.content  # read the whole body inside the retry scope
            except _FINRA_TRANSIENT_ERRORS as exc:
                self.transport_errors += 1
                if attempt >= self.max_attempts:
                    raise
                self.retries += 1
                logging.warning("FINRA %s %s attempt %d failed: %s", method, url, attempt, type(exc).__name__)
                time.sleep(self._backoff(attempt))
                continue
            self.status_counts[response.status_code] = self.status_counts.get(response.status_code, 0) + 1
            if response.status_code in FINRA_RETRY_STATUS_CODES and attempt < self.max_attempts:
                self.retries += 1
                time.sleep(max(self._backoff(attempt), _retry_after_seconds(response)))
                continue
            return response
        raise AssertionError("unreachable")

    def get(self, url: str, **kwargs: Any) -> requests.Response:
        return self.request("GET", url, **kwargs)

    def post(self, url: str, **kwargs: Any) -> requests.Response:
        return self.request("POST", url, **kwargs)

    def stats(self) -> dict[str, Any]:
        span = (self.last_request_at or 0.0) - (self.first_request_at or 0.0)
        return {
            "user_agent": self.user_agent,
            "configured_requests_per_second": self.requests_per_second,
            "requests": self.requests,
            "retries": self.retries,
            "transport_errors": self.transport_errors,
            "status_counts": {str(key): value for key, value in sorted(self.status_counts.items())},
            "max_requests_in_any_1s_window": self.max_in_any_second,
            "span_s": round(span, 1),
            "mean_requests_per_second": round(self.requests / span, 3) if span > 0 else None,
        }

    def close(self) -> None:
        self.session.close()


def write_gzip_object(payload: bytes, destination: Path) -> str:
    """Store ``payload`` at ``destination`` gzip-compressed (level 6, mtime 0), atomically; return its raw sha256.

    Same convention as the SEC fetch store (ruling C-71): sha256 and ``bytes`` in a receipt are over the raw
    bytes, the file at rest is ``<name>.gz``.
    """

    destination.parent.mkdir(parents=True, exist_ok=True)
    part = destination.with_name(destination.name + ".part")
    with part.open("wb") as handle:
        handle.write(gzip.compress(payload, compresslevel=6, mtime=0))
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(part, destination)
    return hashlib.sha256(payload).hexdigest()


def gzip_object_verifies(path: Path, sha256: str | None, size: int | None) -> bool:
    """True when ``path`` decompresses to exactly ``size`` bytes hashing to ``sha256``."""

    try:
        raw = gzip.decompress(path.read_bytes())
    except (OSError, EOFError, zlib.error):
        return False
    return len(raw) == size and hashlib.sha256(raw).hexdigest() == sha256


class FinraReceiptStore:
    """``receipts.jsonl`` under one fetch root: one fsynced JSON line per outcome; the newest line per key wins.

    One fetch process writes a root at a time. A line torn by a kill is counted (``corrupt_lines``) and
    isolated by a leading newline on the next append.
    """

    def __init__(self, root: Path) -> None:
        self.root = root
        self.path = root / FINRA_RECEIPTS_NAME
        self.newest: dict[str, dict[str, Any]] = {}
        self.lines = 0
        self.corrupt_lines = 0

    def load(self) -> FinraReceiptStore:
        self.newest, self.lines, self.corrupt_lines = {}, 0, 0
        if self.path.is_file():
            with self.path.open("rb") as handle:
                for raw in handle:
                    if not raw.strip():
                        continue
                    self.lines += 1
                    try:
                        record = json.loads(raw)
                        self.newest[str(record["key"])] = record
                    except (ValueError, KeyError, TypeError):
                        self.corrupt_lines += 1
        return self

    def get(self, key: str) -> dict[str, Any] | None:
        return self.newest.get(key)

    def append(self, record: dict[str, Any]) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        line = json.dumps(record, sort_keys=True, default=str).encode("utf-8") + b"\n"
        with self.path.open("ab+") as handle:
            handle.seek(0, os.SEEK_END)
            if handle.tell():
                handle.seek(-1, os.SEEK_END)
                if handle.read(1) != b"\n":
                    handle.write(b"\n")
            handle.write(line)
            handle.flush()
            os.fsync(handle.fileno())
        self.newest[str(record["key"])] = record


def _utc_now_iso() -> str:
    return dt.datetime.now(dt.UTC).isoformat(timespec="milliseconds")


def _text_file_facts(payload: bytes) -> dict[str, Any]:
    first_newline = payload.find(b"\n")
    header = payload[: first_newline if first_newline >= 0 else len(payload)].decode("utf-8", "replace").strip()
    return {"lines": payload.count(b"\n") + (0 if payload.endswith(b"\n") or not payload else 1), "header": header}


def fetch_finra_file(
    client: PoliteFinraClient,
    store: FinraReceiptStore,
    *,
    key: str,
    url: str,
    relative_path: str,
    data_date: dt.date,
    run_date: dt.date,
    extra: dict[str, Any] | None = None,
    absent_final_days: int = 14,
    describe: Callable[[bytes], dict[str, Any]] = _text_file_facts,
) -> dict[str, Any]:
    """Fetch one FINRA file into ``store.root / relative_path`` (gzip at rest), or verify the retained copy.

    Returns the record with ``outcome``: ``verified`` (retained copy matches its receipt; not requested, not
    re-appended), ``fetched``, ``absent`` (403/404: not published), ``absent_known`` (an earlier absent
    receipt for a date more than ``absent_final_days`` before ``run_date``: not requested again) or ``failed``.
    A retained copy that no longer verifies is refetched.
    """

    prior = store.get(key)
    destination = store.root / relative_path
    if prior is not None:
        if prior.get("outcome") in FINRA_OK_OUTCOMES and gzip_object_verifies(
            destination, prior.get("sha256"), prior.get("bytes")
        ):
            return {**prior, "outcome": "verified"}
        if prior.get("outcome") == "absent" and (run_date - data_date).days > absent_final_days:
            return {**prior, "outcome": "absent_known"}
    base = {"key": key, "url": url, "file": relative_path, "data_date": data_date.isoformat(), **(extra or {})}
    try:
        response = client.get(url)
    except _FINRA_TRANSIENT_ERRORS as exc:
        record = {**base, "outcome": "failed", "error": f"{type(exc).__name__}: {exc}"[:300],
                  "fetched_at": _utc_now_iso()}
        store.append(record)
        return record
    status = int(response.status_code)
    if status in FINRA_ABSENT_STATUS_CODES:
        record = {**base, "outcome": "absent", "status": status, "fetched_at": _utc_now_iso()}
        store.append(record)
        return record
    if status != 200:
        record = {**base, "outcome": "failed", "status": status, "fetched_at": _utc_now_iso(),
                  "error": response.text[:200]}
        store.append(record)
        return record
    payload = response.content
    sha256 = write_gzip_object(payload, destination)
    record = {
        **base,
        "outcome": "fetched",
        "status": status,
        "bytes": len(payload),
        "sha256": sha256,
        "stored_bytes": destination.stat().st_size,
        "fetched_at": _utc_now_iso(),
        "last_modified": response.headers.get("Last-Modified"),
        "etag": response.headers.get("ETag"),
        "content_type": response.headers.get("Content-Type"),
        "user_agent": client.user_agent,
        **describe(payload),
    }
    store.append(record)
    return record


def fetch_short_interest_api_date(
    client: PoliteFinraClient,
    store: FinraReceiptStore,
    *,
    settlement_date: dt.date,
    record_total: int,
    api_url: str = API_URL,
    limit: int = 5000,
) -> dict[str, Any]:
    """All rows of one settlement date from the FINRA Query API, in stable (sorted) pages, as one CSV file.

    Stored at ``api/consolidatedShortInterest/<yyyy>/<date>.csv.gz`` (header once, then every page's rows as
    received). The row count must equal the API's ``record-total``; a retained file is only reused when its
    receipt's ``record_total`` equals today's (a revised settlement is fetched again).
    """

    key = f"api:{settlement_date.isoformat()}"
    relative_path = f"api/consolidatedShortInterest/{settlement_date.year}/{settlement_date.isoformat()}.csv.gz"
    destination = store.root / relative_path
    prior = store.get(key)
    if (
        prior is not None
        and prior.get("outcome") in FINRA_OK_OUTCOMES
        and prior.get("record_total") == record_total
        and gzip_object_verifies(destination, prior.get("sha256"), prior.get("bytes"))
    ):
        return {**prior, "outcome": "verified"}

    header_line: str | None = None
    columns: list[str] = []
    parts: list[str] = []
    keys: set[tuple[str, str]] = set()
    rows = pages = offset = 0
    started = time.perf_counter()
    while offset < record_total:
        payload = date_payload(settlement_date, limit, offset)
        payload["sortFields"] = list(SHORT_INTEREST_API_SORT)
        response = client.post(api_url, json=payload, headers={"Accept": "text/plain"})
        pages += 1
        if response.status_code != 200:
            record = {"key": key, "url": api_url, "outcome": "failed", "status": response.status_code,
                      "settlement_date": settlement_date.isoformat(), "offset": offset,
                      "error": response.text[:300], "fetched_at": _utc_now_iso()}
            store.append(record)
            return record
        text = response.text
        newline = text.find("\n")
        if newline < 0:
            break
        header, data = text[:newline].rstrip("\r"), text[newline + 1 :]
        if header_line is None:
            header_line = header
            columns = next(csv.reader([header]))
        elif header != header_line:
            raise RuntimeError(f"FINRA API header changed between pages for {settlement_date}")
        symbol_index, market_index = columns.index("symbolCode"), columns.index("marketClassCode")
        page_rows = 0
        for row in csv.reader(io.StringIO(data)):
            if row:
                page_rows += 1
                keys.add((row[symbol_index], row[market_index]))
        if page_rows == 0:
            break
        parts.append(data if data.endswith("\n") else data + "\n")
        rows += page_rows
        offset += page_rows
    if rows != record_total or header_line is None:
        record = {"key": key, "url": api_url, "outcome": "failed", "settlement_date": settlement_date.isoformat(),
                  "record_total": record_total, "rows": rows, "pages": pages,
                  "error": "row count differs from record-total", "fetched_at": _utc_now_iso()}
        store.append(record)
        return record
    body = (header_line + "\n" + "".join(parts)).encode("utf-8")
    sha256 = write_gzip_object(body, destination)
    record = {
        "key": key,
        "url": api_url,
        "request": {
            "compareFilters": [{"compareType": "EQUAL", "fieldName": "settlementDate",
                                "fieldValue": settlement_date.isoformat()}],
            "sortFields": list(SHORT_INTEREST_API_SORT),
            "limit": limit,
        },
        "file": relative_path,
        "outcome": "fetched",
        "settlement_date": settlement_date.isoformat(),
        "publication_date": short_interest_publication_date(settlement_date).isoformat(),
        "record_total": record_total,
        "rows": rows,
        "distinct_symbol_market_keys": len(keys),
        "pages": pages,
        "bytes": len(body),
        "sha256": sha256,
        "stored_bytes": destination.stat().st_size,
        "columns": columns,
        "columns_match_expected": columns == RAW_COLUMNS,
        "fetch_s": round(time.perf_counter() - started, 3),
        "fetched_at": _utc_now_iso(),
        "user_agent": client.user_agent,
    }
    store.append(record)
    return record


def _month_coverage(dates: Iterable[dt.date]) -> dict[str, Any]:
    """Months (between the first and last date) that do not hold exactly two settlement dates."""

    ordered = sorted(set(dates))
    if not ordered:
        return {"months": 0, "irregular_months": []}
    per_month: dict[tuple[int, int], int] = {}
    for day in ordered:
        per_month[(day.year, day.month)] = per_month.get((day.year, day.month), 0) + 1
    irregular = [
        {"month": f"{year:04d}-{month:02d}", "dates": per_month.get((year, month), 0)}
        for year, month in month_iter(ordered[0], ordered[-1])
        if per_month.get((year, month), 0) != 2
    ]
    return {"months": len(month_iter(ordered[0], ordered[-1])), "irregular_months": irregular}


def _tally(records: list[dict[str, Any]], date_field: str) -> dict[str, Any]:
    outcomes: dict[str, int] = {}
    for record in records:
        outcomes[str(record["outcome"])] = outcomes.get(str(record["outcome"]), 0) + 1
    ok = [record for record in records if record["outcome"] in FINRA_OK_OUTCOMES]
    ok_dates = sorted(str(record[date_field]) for record in ok)
    return {
        "attempted": len(records),
        "outcomes": outcomes,
        "files_ok": len(ok),
        "raw_bytes": sum(int(record.get("bytes") or 0) for record in ok),
        "stored_bytes": sum(int(record.get("stored_bytes") or 0) for record in ok),
        "rows_or_lines": sum(int(record.get("rows") or record.get("lines") or 0) for record in ok),
        "first_date": ok_dates[0] if ok_dates else None,
        "last_date": ok_dates[-1] if ok_dates else None,
        "failed": [
            {"key": record["key"], "status": record.get("status"), "error": record.get("error")}
            for record in records
            if record["outcome"] == "failed"
        ],
    }


def fetch_short_interest_history(
    client: PoliteFinraClient,
    root: Path,
    *,
    api_start: dt.date,
    cdn_start: dt.date,
    end: dt.date,
    include_api: bool = True,
    include_cdn: bool = True,
    include_archive: bool = True,
    absent_final_days: int = 14,
    progress: Callable[[str, int, int, dict[str, Any]], None] | None = None,
) -> dict[str, Any]:
    """Fetch the twice-monthly short-interest history into ``root`` (files + ``receipts.jsonl`` only).

    * ``api``: every settlement date the Query API holds in ``[api_start, end]`` (discovered by record-total
      probes), all rows per date (exchange-listed + OTC; the API starts at settlement 2017-12-29).
    * ``cdn``: the official ``shrt<yyyymmdd>.csv`` file for each discovered settlement date >= ``cdn_start``.
    * ``archive``: the OTC-only static archive listed by the OTCE JSON index (2014-11-14 .. 2019-06-28).

    Every record carries its settlement date and publication date (settlement + 7 XNYS sessions).
    """

    store = FinraReceiptStore(root).load()
    summary: dict[str, Any] = {"root": str(root), "receipt_lines_before": store.lines,
                               "receipt_corrupt_lines": store.corrupt_lines}
    settlement_totals = discover_settlement_dates(
        session=client,  # duck-typed: PoliteFinraClient.post paces and retries; post_finra adds no retries
        api_url=API_URL,
        start_date=api_start,
        end_date=end,
        timeout=int(client.timeout),
        max_retries=1,
        retry_sleep=0.0,
    )
    dates = sorted(settlement_totals)
    summary["discovery"] = {
        "window": [api_start.isoformat(), end.isoformat()],
        "settlement_dates": len(dates),
        "first_settlement_date": dates[0].isoformat() if dates else None,
        "last_settlement_date": dates[-1].isoformat() if dates else None,
        "record_total_sum": sum(settlement_totals.values()),
        "min_record_total": min(settlement_totals.values()) if dates else None,
        "coverage": _month_coverage(dates),
    }

    if include_api:
        records = []
        for index, settlement_date in enumerate(dates, start=1):
            record = fetch_short_interest_api_date(
                client, store, settlement_date=settlement_date, record_total=settlement_totals[settlement_date]
            )
            records.append(record)
            if progress:
                progress("si-api", index, len(dates), record)
        summary["api"] = _tally(records, "settlement_date")
        summary["api"]["columns_match_expected_all"] = all(
            bool(record.get("columns_match_expected")) for record in records if record["outcome"] == "fetched"
        )

    if include_cdn:
        cdn_dates = [day for day in dates if day >= cdn_start]
        records = []
        for index, settlement_date in enumerate(cdn_dates, start=1):
            record = fetch_finra_file(
                client,
                store,
                key=f"cdn:{settlement_date.isoformat()}",
                url=SHORT_INTEREST_CDN_URL.format(day=settlement_date),
                relative_path=f"cdn/{settlement_date.year}/shrt{settlement_date:%Y%m%d}.csv.gz",
                data_date=settlement_date,
                run_date=end,
                extra={
                    "settlement_date": settlement_date.isoformat(),
                    "publication_date": short_interest_publication_date(settlement_date).isoformat(),
                },
                absent_final_days=absent_final_days,
            )
            records.append(record)
            if progress:
                progress("si-cdn", index, len(cdn_dates), record)
        summary["cdn"] = _tally(records, "settlement_date")
        summary["cdn"]["absent_settlements"] = [
            record["settlement_date"] for record in records if record["outcome"] in ("absent", "absent_known")
        ]

    if include_archive:
        response = client.get(SHORT_INTEREST_ARCHIVE_INDEX_URL)
        response.raise_for_status()
        index_bytes = response.content
        index_path = f"archive/index/equityShortInterest-{dt.datetime.now(dt.UTC):%Y%m%dT%H%M%SZ}.json.gz"
        store.append({
            "key": "archive:index",
            "url": SHORT_INTEREST_ARCHIVE_INDEX_URL,
            "file": index_path,
            "outcome": "fetched",
            "bytes": len(index_bytes),
            "sha256": write_gzip_object(index_bytes, store.root / index_path),
            "fetched_at": _utc_now_iso(),
            "last_modified": response.headers.get("Last-Modified"),
        })
        entries = json.loads(index_bytes)
        records = []
        for index, entry in enumerate(sorted(entries, key=lambda item: str(item["path"])), start=1):
            settlement_date = dt.datetime.strptime(str(entry["date"]), "%m/%d/%Y").date()
            name = Path(str(entry["path"])).name
            record = fetch_finra_file(
                client,
                store,
                key=f"archive:{name}",
                url=urljoin(SHORT_INTEREST_ARCHIVE_BASE_URL, str(entry["path"])),
                relative_path=f"archive/{settlement_date.year}/{name}.gz",
                data_date=settlement_date,
                run_date=end,
                extra={
                    "settlement_date": settlement_date.isoformat(),
                    "publication_date": short_interest_publication_date(settlement_date).isoformat(),
                },
                absent_final_days=absent_final_days,
            )
            records.append(record)
            if progress:
                progress("si-archive", index, len(entries), record)
        summary["archive"] = _tally(records, "settlement_date")
        summary["archive"]["index_entries"] = len(entries)
        summary["archive"]["coverage"] = _month_coverage(
            dt.date.fromisoformat(record["settlement_date"])
            for record in records
            if record["outcome"] in FINRA_OK_OUTCOMES
        )
    summary["publication_clock"] = (
        f"publication_date = settlement_date + {SHORT_INTEREST_PUBLICATION_LAG_SESSIONS} XNYS sessions"
    )
    return summary
