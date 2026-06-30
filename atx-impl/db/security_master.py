from __future__ import annotations

import datetime as dt
from dataclasses import dataclass
from typing import Any

import pandas as pd
import requests

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import cik_security_id, insert_frame, quality_check, record_source_file, symbol_key


SEC_COMPANY_TICKERS_URL = "https://www.sec.gov/files/company_tickers.json"
SEC_USER_AGENT = "atx-impl security master nathan.tormaschy@gmail.com"
SECURITY_MASTER_SOURCE = "SEC company_tickers"


@dataclass(frozen=True)
class SecurityMasterOptions:
    source_url: str = SEC_COMPANY_TICKERS_URL
    request_timeout: int = 60
    user_agent: str = SEC_USER_AGENT
    run_id: str | None = None


def sec_session(user_agent: str) -> requests.Session:
    session = requests.Session()
    session.headers.update(
        {
            "User-Agent": user_agent,
            "Accept": "application/json,text/plain,*/*",
            "Accept-Encoding": "gzip, deflate",
        }
    )
    return session


def normalize_company_tickers(payload: dict[str, Any]) -> pd.DataFrame:
    rows: list[dict[str, Any]] = []
    for value in payload.values():
        cik = str(value["cik_str"]).zfill(10)
        ticker = symbol_key(value["ticker"])
        title = str(value["title"]).strip()
        rows.append(
            {
                "cik": cik,
                "ticker": ticker,
                "title": title,
                "security_id": cik_security_id(cik),
            }
        )
    return pd.DataFrame(rows).drop_duplicates(subset=["cik", "ticker"]).sort_values(["ticker", "cik"])


def security_ids_for_symbols(store: DuckDBStore, symbols: list[str]) -> dict[str, str]:
    if not symbols:
        return {}
    normalized = sorted({symbol_key(symbol) for symbol in symbols if symbol_key(symbol)})
    frame = pd.DataFrame({"ticker": normalized})
    store.con.register("symbol_lookup", frame)
    try:
        rows = store.con.execute(
            """
            WITH candidates AS (
                SELECT
                    l.ticker,
                    t.security_id,
                    1 AS priority,
                    t.source_loaded_at,
                    t.security_id AS tie_breaker
                FROM symbol_lookup l
                JOIN sec_company_tickers t
                  ON t.ticker = l.ticker
                UNION ALL
                SELECT
                    l.ticker,
                    h.security_id,
                    2 AS priority,
                    h.source_loaded_at,
                    h.security_id AS tie_breaker
                FROM symbol_lookup l
                JOIN security_identifier_history h
                  ON h.id_type = 'TICKER'
                 AND h.id_value = l.ticker
                 AND (h.valid_to IS NULL OR h.valid_to >= current_date)
                UNION ALL
                SELECT
                    l.ticker,
                    e.security_id,
                    3 AS priority,
                    e.source_loaded_at,
                    e.security_id AS tie_breaker
                FROM symbol_lookup l
                JOIN exchange_listings e
                  ON e.ticker = l.ticker
                 AND (e.valid_to IS NULL OR e.valid_to >= current_date)
            )
            SELECT ticker, security_id
            FROM candidates
            QUALIFY row_number() OVER (
                PARTITION BY ticker
                ORDER BY priority, source_loaded_at DESC NULLS LAST, tie_breaker
            ) = 1
            """
        ).fetchall()
    finally:
        store.con.unregister("symbol_lookup")
    return {ticker: security_id for ticker, security_id in rows if security_id}


IDENTIFIER_KEY_COLUMNS = ["security_id", "id_type", "id_value", "source"]


def dedupe_open_identifier_intervals(frame: pd.DataFrame) -> pd.DataFrame:
    """Collapse repeated open-ended identifier rows to one canonical interval.

    Some seeders (notably the SEC ownership issuer seed) emit one identifier row
    per *filing*, each open-ended (``valid_to`` NULL) with that filing's date as
    ``valid_from``. Re-observing the same ``(security_id, id_type, id_value,
    source)`` does not start a new validity interval -- the identifier is
    continuously valid from its earliest sighting -- so N open-ended rows are
    redundant and produce self-overlaps. Keep exactly one row per key: the
    earliest ``valid_from`` (tie-broken by the earliest ``available_at``, the
    true first disclosure). Rows with a non-null ``valid_to`` describe real
    closed intervals (e.g. a ticker change) and are left untouched.
    """
    if frame is None or frame.empty:
        return frame
    open_mask = frame["valid_to"].isna()
    closed = frame[~open_mask]
    open_rows = frame[open_mask]
    if open_rows.empty:
        return frame
    open_rows = open_rows.sort_values(["valid_from", "available_at"], kind="stable").drop_duplicates(
        subset=IDENTIFIER_KEY_COLUMNS, keep="first"
    )
    if closed.empty:
        return open_rows.reset_index(drop=True)
    return pd.concat([open_rows, closed], ignore_index=True)


def collapse_identifier_history_open_duplicates(conn) -> int:
    """One-time repair: collapse redundant open-ended identifier intervals.

    For each ``(security_id, id_type, id_value, source)`` among rows with
    ``valid_to IS NULL``, keep the earliest ``(valid_from, available_at, rowid)``
    row and delete the rest. Closed intervals are never touched. Returns the
    number of rows removed. Safe to run repeatedly (idempotent once collapsed).
    """
    before = conn.execute("SELECT count(*) FROM security_identifier_history").fetchone()[0]
    conn.execute(
        """
        DELETE FROM security_identifier_history
        WHERE rowid IN (
            SELECT rowid FROM (
                SELECT rowid,
                    row_number() OVER (
                        PARTITION BY security_id, id_type, id_value, source
                        ORDER BY valid_from ASC, available_at ASC, rowid ASC
                    ) AS rn
                FROM security_identifier_history
                WHERE valid_to IS NULL
            )
            WHERE rn > 1
        )
        """
    )
    after = conn.execute("SELECT count(*) FROM security_identifier_history").fetchone()[0]
    return int(before - after)


def upsert_security_master_from_frame(
    store: DuckDBStore,
    frame: pd.DataFrame,
    *,
    source: str,
    run_id: str | None = None,
) -> None:
    if frame.empty:
        return
    today = dt.date.today()
    available_at = pd.Timestamp.utcnow().tz_localize(None)
    securities = pd.DataFrame(
        {
            "security_id": frame["security_id"],
            "issuer_id": "CIK-" + frame["cik"].astype(str),
            "primary_symbol": frame["ticker"],
            "name": frame["title"],
            "asset_class": "EQUITY",
            "country": "US",
            "currency": "USD",
            "active": True,
            "first_seen_date": today,
            "last_seen_date": pd.NaT,
            "source": source,
        }
    ).drop_duplicates(subset=["security_id"])

    identifiers = pd.concat(
        [
            pd.DataFrame(
                {
                    "security_id": frame["security_id"],
                    "id_type": "CIK",
                    "id_value": frame["cik"],
                    "valid_from": today,
                    "valid_to": pd.NaT,
                    "as_of_date": today,
                    "available_at": available_at,
                    "source": source,
                    "run_id": run_id,
                }
            ),
            pd.DataFrame(
                {
                    "security_id": frame["security_id"],
                    "id_type": "TICKER",
                    "id_value": frame["ticker"],
                    "valid_from": today,
                    "valid_to": pd.NaT,
                    "as_of_date": today,
                    "available_at": available_at,
                    "source": source,
                    "run_id": run_id,
                }
            ),
        ],
        ignore_index=True,
    ).drop_duplicates(subset=["security_id", "id_type", "id_value"])
    existing_identifiers = store.con.execute(
        """
        SELECT
            security_id,
            id_type,
            id_value,
            min(valid_from) AS existing_valid_from
        FROM security_identifier_history
        WHERE source = ?
        GROUP BY security_id, id_type, id_value
        """,
        [source],
    ).df()
    if not existing_identifiers.empty:
        identifiers = identifiers.merge(
            existing_identifiers,
            on=["security_id", "id_type", "id_value"],
            how="left",
        )
        identifiers["valid_from"] = identifiers["existing_valid_from"].where(
            identifiers["existing_valid_from"].notna(),
            identifiers["valid_from"],
        )
        identifiers = identifiers.drop(columns=["existing_valid_from"])

    listings = pd.DataFrame(
        {
            "security_id": frame["security_id"],
            "ticker": frame["ticker"],
            "exchange_code": pd.NA,
            "mic": pd.NA,
            "currency": "USD",
            "valid_from": today,
            "valid_to": pd.NaT,
            "as_of_date": today,
            "available_at": pd.Timestamp.utcnow().tz_localize(None),
            "source": source,
            "run_id": run_id,
        }
    ).drop_duplicates(subset=["security_id", "ticker"])
    existing_listings = store.con.execute(
        """
        SELECT
            security_id,
            ticker,
            min(valid_from) AS existing_valid_from
        FROM exchange_listings
        WHERE source = ?
        GROUP BY security_id, ticker
        """,
        [source],
    ).df()
    if not existing_listings.empty:
        listings = listings.merge(
            existing_listings,
            on=["security_id", "ticker"],
            how="left",
        )
        listings["valid_from"] = listings["existing_valid_from"].where(
            listings["existing_valid_from"].notna(),
            listings["valid_from"],
        )
        listings = listings.drop(columns=["existing_valid_from"])

    store.con.register("security_master_load", frame)
    store.con.register("securities_load", securities)
    store.con.register("identifiers_load", identifiers)
    store.con.register("listings_load", listings)
    try:
        with store.transaction():
            store.con.execute(
                """
                DELETE FROM sec_company_tickers
                USING security_master_load src
                WHERE sec_company_tickers.cik = src.cik
                  AND sec_company_tickers.ticker = src.ticker
                """
            )
            store.con.execute(
                """
                INSERT INTO sec_company_tickers (cik, ticker, title, security_id)
                SELECT cik, ticker, title, security_id
                FROM security_master_load
                """
            )
            store.con.execute(
                """
                DELETE FROM securities
                USING securities_load src
                WHERE securities.security_id = src.security_id
                """
            )
            insert_frame(store, securities, "securities", "securities_insert")
            store.con.execute(
                """
                DELETE FROM security_identifier_history
                USING identifiers_load src
                WHERE security_identifier_history.security_id = src.security_id
                  AND security_identifier_history.id_type = src.id_type
                  AND security_identifier_history.id_value = src.id_value
                  AND security_identifier_history.source = src.source
                """
            )
            insert_frame(store, identifiers, "security_identifier_history", "identifiers_insert")
            store.con.execute(
                """
                DELETE FROM exchange_listings
                USING listings_load src
                WHERE exchange_listings.security_id = src.security_id
                  AND exchange_listings.ticker = src.ticker
                  AND exchange_listings.source = src.source
                """
            )
            insert_frame(store, listings, "exchange_listings", "listings_insert")
    finally:
        for relation in ("security_master_load", "securities_load", "identifiers_load", "listings_load"):
            try:
                store.con.unregister(relation)
            except Exception:
                pass


class SecurityMasterDataset(Dataset):
    dataset_id = "sec_security_master"
    source_name = SECURITY_MASTER_SOURCE

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: SecurityMasterOptions) -> DatasetLoadResult:
        response = sec_session(options.user_agent).get(options.source_url, timeout=options.request_timeout)
        response.raise_for_status()
        frame = normalize_company_tickers(response.json())
        record_source_file(
            store,
            dataset_id=self.dataset_id,
            source_url=options.source_url,
            status="fetched",
            metadata={"rows": len(frame)},
        )
        upsert_security_master_from_frame(store, frame, source=self.source_name, run_id=options.run_id)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="sec_company_tickers",
            check_name="nonempty_company_tickers",
            status="passed" if len(frame) > 0 else "failed",
            observed_value=float(len(frame)),
            threshold_value=1.0,
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(len(frame)),
            source=options.source_url,
            details={
                "symbols": int(frame["ticker"].nunique()),
                "ciks": int(frame["cik"].nunique()),
            },
        )
