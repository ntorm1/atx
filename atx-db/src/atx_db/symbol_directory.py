from __future__ import annotations

import datetime as dt
import io
import uuid
from dataclasses import dataclass
from typing import Any

import pandas as pd
import requests

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check, record_source_file, symbol_key


NASDAQ_LISTED_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt"
OTHER_LISTED_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt"
TRADING_SYSTEM_ADDS_DELETES_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/TradingSystemAddsDeletes.txt"
SOURCE_NAME = "Nasdaq Trader Symbol Directory"
LISTING_EVENTS_SOURCE_NAME = "Nasdaq Trader Trading System Adds/Deletes"


@dataclass(frozen=True)
class NasdaqSymbolDirectoryOptions:
    nasdaq_url: str = NASDAQ_LISTED_URL
    other_url: str = OTHER_LISTED_URL
    as_of_date: dt.date | None = None
    request_timeout: int = 60
    user_agent: str = "atx-db symbol directory nathan.tormaschy@gmail.com"
    run_id: str | None = None


@dataclass(frozen=True)
class NasdaqListingEventsOptions:
    source_url: str = TRADING_SYSTEM_ADDS_DELETES_URL
    as_of_date: dt.date | None = None
    request_timeout: int = 60
    user_agent: str = "atx-db nasdaq listing events nathan.tormaschy@gmail.com"
    run_id: str | None = None


def _bool_flag(value: Any) -> bool | None:
    if value is None or pd.isna(value) or value == "":
        return None
    return str(value).strip().upper() == "Y"


def _read_directory_text(text: str) -> pd.DataFrame:
    lines = [line for line in text.splitlines() if line and not line.startswith("File Creation Time")]
    if not lines:
        return pd.DataFrame()
    return pd.read_csv(io.StringIO("\n".join(lines)), sep="|", dtype=str, keep_default_na=False)


def _file_creation_time(text: str) -> dt.datetime | None:
    for line in reversed(text.splitlines()):
        if not line.startswith("File Creation Time:"):
            continue
        raw = line.split(":", 1)[1].split("|", 1)[0].strip()
        if not raw:
            return None
        for fmt in ("%m%d%Y%H:%M", "%m%d%Y%H%M"):
            try:
                return dt.datetime.strptime(raw, fmt)
            except ValueError:
                pass
    return None


def _parse_event_date(value: Any) -> dt.date | None:
    if value is None or pd.isna(value) or str(value).strip() == "":
        return None
    parsed = pd.to_datetime(str(value).strip(), format="%m/%d/%Y", errors="coerce")
    if pd.isna(parsed):
        parsed = pd.to_datetime(str(value).strip(), errors="coerce")
    if pd.isna(parsed):
        return None
    return parsed.date()


def _action_value(value: Any) -> str | None:
    if value is None or pd.isna(value):
        return None
    normalized = str(value).strip()
    return normalized or None


def _event_id_part(value: Any) -> str:
    if value is None or pd.isna(value):
        return ""
    return str(value)


def normalize_nasdaq_listed(frame: pd.DataFrame, *, as_of_date: dt.date, source_url: str, run_id: str | None) -> pd.DataFrame:
    if frame.empty:
        return pd.DataFrame()
    return pd.DataFrame(
        {
            "directory": "nasdaqlisted",
            "symbol": frame["Symbol"].map(symbol_key),
            "security_name": frame["Security Name"].str.strip(),
            "market_category": frame["Market Category"].str.strip(),
            "exchange": "NASDAQ",
            "cqs_symbol": pd.NA,
            "etf": frame["ETF"].map(_bool_flag),
            "test_issue": frame["Test Issue"].map(_bool_flag),
            "financial_status": frame["Financial Status"].str.strip(),
            "round_lot_size": pd.to_numeric(frame["Round Lot Size"], errors="coerce").astype("Int64"),
            "next_shares": frame["NextShares"].map(_bool_flag),
            "nasdaq_symbol": frame["Symbol"].map(symbol_key),
            "as_of_date": as_of_date,
            "source_url": source_url,
            "run_id": run_id,
        }
    )


def normalize_other_listed(frame: pd.DataFrame, *, as_of_date: dt.date, source_url: str, run_id: str | None) -> pd.DataFrame:
    if frame.empty:
        return pd.DataFrame()
    return pd.DataFrame(
        {
            "directory": "otherlisted",
            "symbol": frame["ACT Symbol"].map(symbol_key),
            "security_name": frame["Security Name"].str.strip(),
            "market_category": pd.NA,
            "exchange": frame["Exchange"].str.strip(),
            "cqs_symbol": frame["CQS Symbol"].map(symbol_key),
            "etf": frame["ETF"].map(_bool_flag),
            "test_issue": frame["Test Issue"].map(_bool_flag),
            "financial_status": pd.NA,
            "round_lot_size": pd.to_numeric(frame["Round Lot Size"], errors="coerce").astype("Int64"),
            "next_shares": pd.NA,
            "nasdaq_symbol": frame["NASDAQ Symbol"].map(symbol_key),
            "as_of_date": as_of_date,
            "source_url": source_url,
            "run_id": run_id,
        }
    )


def normalize_listing_events(
    frame: pd.DataFrame,
    *,
    as_of_date: dt.date,
    source_url: str,
    run_id: str | None,
    source_file_created_at: dt.datetime | None,
) -> pd.DataFrame:
    if frame.empty:
        return pd.DataFrame()
    normalized = pd.DataFrame(
        {
            "symbol": frame["Symbol"].map(symbol_key),
            "company_name": frame["Company Name"].str.strip(),
            "nasdaq_action": frame["NASDAQ Action"].map(_action_value),
            "bx_action": frame["BX Action"].map(_action_value),
            "psx_action": frame["PSX Action"].map(_action_value),
            "effective_date": frame["Effective Date"].map(_parse_event_date),
            "primary_listing_market": frame["Primary Listing Market"].str.strip().replace({"": pd.NA}),
            "as_of_date": as_of_date,
            "source_file_created_at": source_file_created_at,
            "source_url": source_url,
            "run_id": run_id,
        }
    )
    normalized = normalized[normalized["symbol"] != ""].copy()
    normalized["event_id"] = [
        str(
            uuid.uuid5(
                uuid.NAMESPACE_URL,
                "|".join(
                    _event_id_part(part)
                    for part in (
                        source_url,
                        row.symbol,
                        row.effective_date,
                        row.nasdaq_action,
                        row.bx_action,
                        row.psx_action,
                        row.primary_listing_market,
                        source_file_created_at,
                    )
                ),
            )
        )
        for row in normalized.itertuples(index=False)
    ]
    normalized.insert(0, "event_id", normalized.pop("event_id"))
    return normalized.drop_duplicates(subset=["event_id"])


def _attach_security_ids(store: DuckDBStore, frame: pd.DataFrame) -> pd.DataFrame:
    if frame.empty:
        frame["security_id"] = pd.Series(dtype="object")
        return frame
    symbols = pd.DataFrame({"symbol": sorted(set(frame["symbol"].dropna()))})
    store.con.register("nasdaq_listing_event_symbols", symbols)
    try:
        resolved = store.con.execute(
            """
            WITH candidates AS (
                SELECT ticker AS symbol, security_id, source_loaded_at
                FROM sec_company_tickers
                UNION ALL
                SELECT id_value AS symbol, security_id, source_loaded_at
                FROM security_identifier_history
                WHERE id_type = 'TICKER'
                  AND valid_to IS NULL
            )
            SELECT symbol, security_id
            FROM (
                SELECT
                    s.symbol,
                    c.security_id,
                    row_number() OVER (
                        PARTITION BY s.symbol
                        ORDER BY c.source_loaded_at DESC NULLS LAST, c.security_id
                    ) AS security_rank
                FROM nasdaq_listing_event_symbols s
                LEFT JOIN candidates c
                  ON c.symbol = s.symbol
            )
            WHERE security_rank = 1
            """
        ).df()
    finally:
        store.con.unregister("nasdaq_listing_event_symbols")
    return frame.merge(resolved, on="symbol", how="left")


class NasdaqSymbolDirectoryDataset(Dataset):
    dataset_id = "nasdaq_symbol_directory"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: NasdaqSymbolDirectoryOptions) -> DatasetLoadResult:
        as_of_date = options.as_of_date or dt.date.today()
        session = requests.Session()
        session.headers.update({"User-Agent": options.user_agent, "Accept": "text/plain,*/*"})
        frames: list[pd.DataFrame] = []
        for url, normalizer in (
            (options.nasdaq_url, normalize_nasdaq_listed),
            (options.other_url, normalize_other_listed),
        ):
            response = session.get(url, timeout=options.request_timeout)
            response.raise_for_status()
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=url,
                status="fetched",
                metadata={"as_of_date": as_of_date.isoformat()},
            )
            frames.append(normalizer(_read_directory_text(response.text), as_of_date=as_of_date, source_url=url, run_id=options.run_id))
        frame = pd.concat([frame for frame in frames if not frame.empty], ignore_index=True)
        rows = self._replace_snapshot(store, frame, as_of_date)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="nasdaq_symbol_directory",
            check_name="snapshot_rows",
            status="passed" if rows > 0 else "failed",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"as_of_date": as_of_date.isoformat()},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=f"{options.nasdaq_url};{options.other_url}",
            details={"as_of_date": as_of_date.isoformat(), "symbols": int(frame["symbol"].nunique()) if not frame.empty else 0},
        )

    def _replace_snapshot(self, store: DuckDBStore, frame: pd.DataFrame, as_of_date: dt.date) -> int:
        if frame.empty:
            return 0
        with store.transaction():
            store.con.execute("DELETE FROM nasdaq_symbol_directory WHERE as_of_date = ?", [as_of_date])
            insert_frame(store, frame, "nasdaq_symbol_directory", "nasdaq_symbol_directory_insert")
        return int(len(frame))


class NasdaqListingEventsDataset(Dataset):
    dataset_id = "nasdaq_listing_events"
    source_name = LISTING_EVENTS_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: NasdaqListingEventsOptions) -> DatasetLoadResult:
        session = requests.Session()
        session.headers.update({"User-Agent": options.user_agent, "Accept": "text/plain,*/*"})
        response = session.get(options.source_url, timeout=options.request_timeout)
        response.raise_for_status()
        source_file_created_at = _file_creation_time(response.text)
        as_of_date = options.as_of_date or (source_file_created_at.date() if source_file_created_at else dt.date.today())
        record_source_file(
            store,
            dataset_id=self.dataset_id,
            source_url=options.source_url,
            status="fetched",
            metadata={
                "as_of_date": as_of_date.isoformat(),
                "source_file_created_at": source_file_created_at.isoformat() if source_file_created_at else None,
            },
        )
        frame = normalize_listing_events(
            _read_directory_text(response.text),
            as_of_date=as_of_date,
            source_url=options.source_url,
            run_id=options.run_id,
            source_file_created_at=source_file_created_at,
        )
        frame = _attach_security_ids(store, frame)
        columns = [
            "event_id",
            "symbol",
            "security_id",
            "company_name",
            "nasdaq_action",
            "bx_action",
            "psx_action",
            "effective_date",
            "primary_listing_market",
            "as_of_date",
            "source_file_created_at",
            "source_url",
            "run_id",
        ]
        frame = frame.reindex(columns=columns)
        rows = self._upsert_events(store, frame)
        unresolved = int(frame["security_id"].isna().sum()) if not frame.empty else 0
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="nasdaq_listing_events",
            check_name="loaded_listing_event_rows",
            status="passed",
            observed_value=float(rows),
            threshold_value=0.0,
            details={
                "as_of_date": as_of_date.isoformat(),
                "source_file_created_at": source_file_created_at.isoformat() if source_file_created_at else None,
                "unresolved_security_ids": unresolved,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source_url,
            details={
                "as_of_date": as_of_date.isoformat(),
                "source_file_created_at": source_file_created_at.isoformat() if source_file_created_at else None,
                "symbols": int(frame["symbol"].nunique()) if not frame.empty else 0,
                "unresolved_security_ids": unresolved,
            },
        )

    def _upsert_events(self, store: DuckDBStore, frame: pd.DataFrame) -> int:
        if frame.empty:
            return 0
        store.con.register("nasdaq_listing_events_load", frame)
        try:
            with store.transaction():
                store.con.execute(
                    """
                    DELETE FROM nasdaq_listing_events
                    USING nasdaq_listing_events_load src
                    WHERE nasdaq_listing_events.event_id = src.event_id
                    """
                )
                insert_frame(store, frame, "nasdaq_listing_events", "nasdaq_listing_events_insert")
        finally:
            try:
                store.con.unregister("nasdaq_listing_events_load")
            except Exception:
                pass
        return int(len(frame))
