from __future__ import annotations

import calendar
import csv
import datetime as dt
import io
import logging
import time
from dataclasses import dataclass
from typing import Any, Iterable, Literal

import pandas as pd
import requests

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
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
    user_agent: str = "atx-db FINRA dataset loader"
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
            start_date = options.start_date or subtract_years(dt.date.today(), 5)
            end_date = options.end_date or dt.date.today()
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
