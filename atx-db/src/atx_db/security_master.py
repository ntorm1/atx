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
SEC_USER_AGENT = "atx-db security master nathan.tormaschy@gmail.com"
SECURITY_MASTER_SOURCE = "SEC company_tickers"
ENTITY_IDENTIFIER_TYPE = "ENTITY_ID"


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
                "entity_id": cik_entity_id(cik),
            }
        )
    return pd.DataFrame(rows).drop_duplicates(subset=["cik", "ticker"]).sort_values(["ticker", "cik"])


def cik_entity_id(cik: str | int) -> str:
    return f"CIK-{int(cik):010d}"


def _entity_id_from_row(row: pd.Series) -> str:
    if pd.notna(row.get("entity_id")) and str(row.get("entity_id")).strip():
        return str(row.get("entity_id")).strip()
    cik = row.get("cik")
    if pd.notna(cik) and str(cik).strip():
        return cik_entity_id(cik)
    issuer_id = row.get("issuer_id")
    if pd.notna(issuer_id) and str(issuer_id).strip():
        return str(issuer_id).strip()
    return f"ENTITY-{str(row['security_id']).strip()}"


def ensure_security_frame_entity_ids(frame: pd.DataFrame) -> pd.DataFrame:
    """Return a copy with the additive PF-S5 entity_id populated."""
    if frame is None or frame.empty:
        return frame
    out = frame.copy()
    if "entity_id" not in out.columns:
        out["entity_id"] = pd.NA
    out["entity_id"] = out.apply(_entity_id_from_row, axis=1)
    return out


def security_entity_ids_asof(
    store: DuckDBStore,
    security_ids: list[str],
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
) -> dict[str, str]:
    """Resolve security_id -> entity_id through PIT entity history.

    Bitemporal ENTITY_ID rows are authoritative. The current ``securities``
    value is used only for securities with no ENTITY_ID history at all, avoiding
    lookahead when a future merger row exists but is not yet available.
    """
    normalized = sorted({str(security_id).strip() for security_id in security_ids if str(security_id).strip()})
    if not normalized:
        return {}
    as_of_ts = as_of_ts or dt.datetime.combine(as_of_date, dt.time(23, 59, 59))
    lookup = pd.DataFrame({"security_id": normalized})
    store.con.register("security_entity_lookup", lookup)
    try:
        rows = store.con.execute(
            """
            WITH params AS (
                SELECT CAST(? AS DATE) AS as_of_date, CAST(? AS TIMESTAMP) AS as_of_ts
            ),
            history_presence AS (
                SELECT DISTINCT security_id
                FROM security_identifier_history
                WHERE id_type = ?
            ),
            candidates AS (
                SELECT
                    l.security_id,
                    h.id_value AS entity_id,
                    1 AS priority,
                    h.valid_from,
                    h.available_at,
                    h.source_loaded_at
                FROM security_entity_lookup l
                JOIN security_identifier_history h
                  ON h.security_id = l.security_id
                 AND h.id_type = ?
                CROSS JOIN params p
                WHERE h.valid_from <= p.as_of_date
                  AND coalesce(h.valid_to, DATE '9999-12-31') > p.as_of_date
                  AND (h.available_at IS NULL OR h.available_at <= p.as_of_ts)
                UNION ALL
                SELECT
                    l.security_id,
                    s.entity_id,
                    2 AS priority,
                    coalesce(s.first_seen_date, DATE '1900-01-01') AS valid_from,
                    CAST(NULL AS TIMESTAMP) AS available_at,
                    s.source_loaded_at
                FROM security_entity_lookup l
                JOIN securities s
                  ON s.security_id = l.security_id
                LEFT JOIN history_presence hp
                  ON hp.security_id = s.security_id
                WHERE hp.security_id IS NULL
                  AND s.entity_id IS NOT NULL
                  AND s.entity_id <> ''
            )
            SELECT security_id, entity_id
            FROM candidates
            QUALIFY row_number() OVER (
                PARTITION BY security_id
                ORDER BY priority, available_at DESC NULLS LAST,
                         valid_from DESC, source_loaded_at DESC NULLS LAST, entity_id
            ) = 1
            """,
            [as_of_date, as_of_ts, ENTITY_IDENTIFIER_TYPE, ENTITY_IDENTIFIER_TYPE],
        ).fetchall()
    finally:
        store.con.unregister("security_entity_lookup")
    return {security_id: entity_id for security_id, entity_id in rows if entity_id}


CIK_IDENTIFIER_TYPE = "CIK"


def security_and_entity_ids_for_ciks_asof(
    store: DuckDBStore,
    ciks: list[str],
    *,
    as_of_ts: dt.datetime,
) -> dict[str, tuple[str, str | None]]:
    """PF-S5 S5-3: resolve CIK -> (security_id, entity_id) through the spine, as of ``as_of_ts``.

    Mirrors ``security_ids_for_symbols``'s priority-ranked ``UNION ALL`` /
    ``QUALIFY row_number()`` pattern and ``security_entity_ids_asof``'s bitemporal
    interval filter, so a fact filed at time T resolves through the identifier
    state known AT T -- ``available_at <= as_of_ts`` on every candidate, never a
    current/latest snapshot. This is the read a PIT-correct fundamentals loader
    needs: two callers resolving the SAME cik at two different ``as_of_ts`` may
    legitimately get two different ``security_id``/``entity_id`` pairs (e.g.
    around a re-parenting event), and a stale-at-filing-time CIK mapping must
    never leak a later resolution backwards.

    Priority order per cik:
      1. Bitemporal CIK rows in ``security_identifier_history`` (id_type='CIK'),
         interval-filtered (``valid_from <= as_of_ts::DATE < coalesce(valid_to, 9999-12-31)``)
         and gated by ``available_at <= as_of_ts`` (NULL available_at is treated
         as always-visible, matching ``security_entity_ids_asof``'s history rows).
      2. Fallback to ``sec_company_tickers.cik`` (current snapshot; no bitemporal
         history) ONLY for ciks absent from priority 1, avoiding lookahead for
         ciks that do have real history.

    For each resolved ``security_id``, ``entity_id`` is resolved the same way
    ``security_entity_ids_asof`` does (its own priority UNION ALL over ENTITY_ID
    history vs. the current ``securities`` row), evaluated at the SAME
    ``as_of_ts`` -- so entity resolution never outruns the security resolution
    it depends on.

    CIKs with no match at any priority are simply absent from the returned dict
    (never a placeholder key) -- callers are responsible for routing absent
    ciks to the resolution ledger.
    """
    normalized = sorted({str(cik).strip() for cik in ciks if str(cik).strip()})
    if not normalized:
        return {}
    as_of_date = as_of_ts.date()
    lookup = pd.DataFrame({"cik": normalized})
    store.con.register("cik_asof_lookup", lookup)
    try:
        security_rows = store.con.execute(
            """
            WITH params AS (
                SELECT CAST(? AS DATE) AS as_of_date, CAST(? AS TIMESTAMP) AS as_of_ts
            ),
            candidates AS (
                SELECT
                    l.cik,
                    h.security_id,
                    1 AS priority,
                    h.available_at,
                    h.valid_from,
                    h.source_loaded_at
                FROM cik_asof_lookup l
                JOIN security_identifier_history h
                  ON h.id_type = ?
                 AND h.id_value = l.cik
                CROSS JOIN params p
                WHERE h.valid_from <= p.as_of_date
                  AND coalesce(h.valid_to, DATE '9999-12-31') > p.as_of_date
                  AND (h.available_at IS NULL OR h.available_at <= p.as_of_ts)
                UNION ALL
                SELECT
                    l.cik,
                    t.security_id,
                    2 AS priority,
                    CAST(NULL AS TIMESTAMP) AS available_at,
                    DATE '1900-01-01' AS valid_from,
                    t.source_loaded_at
                FROM cik_asof_lookup l
                JOIN sec_company_tickers t
                  ON t.cik = l.cik
                LEFT JOIN security_identifier_history hp
                  ON hp.id_type = ?
                 AND hp.id_value = l.cik
                WHERE hp.id_value IS NULL
                  AND t.security_id IS NOT NULL
                  AND t.security_id <> ''
            )
            SELECT cik, security_id
            FROM candidates
            QUALIFY row_number() OVER (
                PARTITION BY cik
                ORDER BY priority, available_at DESC NULLS LAST,
                         valid_from DESC, source_loaded_at DESC NULLS LAST, security_id
            ) = 1
            """,
            [as_of_date, as_of_ts, CIK_IDENTIFIER_TYPE, CIK_IDENTIFIER_TYPE],
        ).fetchall()
    finally:
        store.con.unregister("cik_asof_lookup")

    security_by_cik = {cik: security_id for cik, security_id in security_rows if security_id}
    if not security_by_cik:
        return {}

    entity_by_security = security_entity_ids_asof(
        store, list(security_by_cik.values()), as_of_date=as_of_date, as_of_ts=as_of_ts
    )
    return {
        cik: (security_id, entity_by_security.get(security_id))
        for cik, security_id in security_by_cik.items()
    }


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
    frame = ensure_security_frame_entity_ids(frame)
    today = dt.date.today()
    available_at = pd.Timestamp.utcnow().tz_localize(None)
    securities = pd.DataFrame(
        {
            "security_id": frame["security_id"],
            "entity_id": frame["entity_id"],
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
                    "id_type": ENTITY_IDENTIFIER_TYPE,
                    "id_value": frame["entity_id"],
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
    identifiers = dedupe_open_identifier_intervals(identifiers)

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
