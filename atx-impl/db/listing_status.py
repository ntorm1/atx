from __future__ import annotations

import datetime as dt
import uuid
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check, symbol_key


SOURCE_NAME = "ATX listing status interval builder"
DEFAULT_SOURCE = "atx_listing_status_intervals_v1"

VENUE_NAMES = {
    "A": "NYSE American",
    "G": "NASDAQ Global Market",
    "M": "NYSE Chicago",
    "N": "New York Stock Exchange",
    "P": "NYSE Arca",
    "Q": "NASDAQ Global Select Market",
    "S": "NASDAQ Capital Market",
    "V": "Investors Exchange",
    "Z": "Cboe BZX",
    "NASDAQ": "NASDAQ",
}

OUTPUT_COLUMNS = [
    "listing_status_id",
    "security_id",
    "symbol",
    "listing_venue_code",
    "listing_venue_name",
    "listing_exchange_code",
    "status",
    "valid_from",
    "valid_to",
    "as_of_date",
    "available_at",
    "last_evidence_as_of_date",
    "last_evidence_at",
    "source",
    "evidence_source",
    "evidence_source_table",
    "source_event_id",
    "source_snapshot_directory",
    "source_url",
    "method",
    "details_json",
    "run_id",
]


@dataclass(frozen=True)
class ListingStatusIntervalOptions:
    source: str = DEFAULT_SOURCE
    run_id: str | None = None


def _blank_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=OUTPUT_COLUMNS)


def _value(value: Any) -> str:
    if value is None or pd.isna(value):
        return ""
    if isinstance(value, (dt.date, dt.datetime, pd.Timestamp)):
        return value.isoformat()
    return str(value)


def _stable_id(*parts: Any) -> str:
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "|".join(_value(part) for part in parts)))


def _venue_name(code: Any) -> str | None:
    if code is None or pd.isna(code):
        return None
    normalized = str(code).strip().upper()
    return VENUE_NAMES.get(normalized, normalized or None)


def _resolve_security_ids(store: DuckDBStore, symbols: list[str]) -> pd.DataFrame:
    normalized = sorted({symbol_key(symbol) for symbol in symbols if symbol_key(symbol)})
    if not normalized:
        return pd.DataFrame(columns=["symbol", "security_id"])
    store.con.register("listing_status_symbol_lookup", pd.DataFrame({"symbol": normalized}))
    try:
        return store.con.execute(
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
                    l.symbol,
                    c.security_id,
                    row_number() OVER (
                        PARTITION BY l.symbol
                        ORDER BY c.source_loaded_at DESC NULLS LAST, c.security_id
                    ) AS resolution_rank
                FROM listing_status_symbol_lookup l
                LEFT JOIN candidates c
                  ON c.symbol = l.symbol
            )
            WHERE resolution_rank = 1
            """
        ).df()
    finally:
        store.con.unregister("listing_status_symbol_lookup")


def _date_or_nat(value: Any) -> dt.date | pd.NaT:
    if value is None or pd.isna(value):
        return pd.NaT
    if isinstance(value, pd.Timestamp):
        return value.date()
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return pd.Timestamp(value).date()


def _details(payload: dict[str, Any]) -> str:
    return json_dumps({key: value for key, value in payload.items() if value is not None and not pd.isna(value)})


def _snapshot_intervals(store: DuckDBStore, *, source: str, run_id: str | None) -> pd.DataFrame:
    snapshots = store.con.execute(
        """
        SELECT
            directory,
            symbol,
            security_name,
            market_category,
            exchange,
            cqs_symbol,
            etf,
            test_issue,
            financial_status,
            round_lot_size,
            next_shares,
            nasdaq_symbol,
            as_of_date,
            source_url,
            source_loaded_at
        FROM nasdaq_symbol_directory
        WHERE symbol IS NOT NULL
          AND symbol <> ''
        """
    ).df()
    if snapshots.empty:
        return _blank_frame()

    snapshots["symbol"] = snapshots["symbol"].map(symbol_key)
    snapshots["listing_venue_code"] = snapshots.apply(
        lambda row: row["market_category"] if row["directory"] == "nasdaqlisted" else row["exchange"],
        axis=1,
    )
    snapshots["listing_venue_code"] = snapshots["listing_venue_code"].fillna("").astype(str).str.strip().str.upper()
    snapshots["listing_exchange_code"] = snapshots.apply(
        lambda row: "NASDAQ" if row["directory"] == "nasdaqlisted" else row["exchange"],
        axis=1,
    )
    snapshots["listing_exchange_code"] = snapshots["listing_exchange_code"].fillna("").astype(str).str.strip().str.upper()
    snapshots = snapshots[snapshots["listing_venue_code"] != ""].copy()
    if snapshots.empty:
        return _blank_frame()

    resolutions = _resolve_security_ids(store, snapshots["symbol"].tolist())
    snapshots = snapshots.merge(resolutions, on="symbol", how="left")
    snapshots["snapshot_date"] = pd.to_datetime(snapshots["as_of_date"])
    max_snapshot_date = snapshots["snapshot_date"].max()
    snapshots = snapshots.sort_values(["symbol", "listing_venue_code", "snapshot_date"])
    group_keys = ["symbol", "listing_venue_code", "listing_exchange_code"]
    snapshots["previous_snapshot_date"] = snapshots.groupby(group_keys)["snapshot_date"].shift()
    snapshots["new_streak"] = (
        snapshots["previous_snapshot_date"].isna()
        | ((snapshots["snapshot_date"] - snapshots["previous_snapshot_date"]).dt.days > 1)
    )
    snapshots["streak_id"] = snapshots.groupby(group_keys)["new_streak"].cumsum()

    rows: list[dict[str, Any]] = []
    for _, group in snapshots.groupby([*group_keys, "streak_id"], dropna=False):
        first = group.sort_values("snapshot_date").iloc[0]
        last = group.sort_values("snapshot_date").iloc[-1]
        valid_from = _date_or_nat(group["snapshot_date"].min())
        last_evidence_date = _date_or_nat(group["snapshot_date"].max())
        valid_to = pd.NaT if group["snapshot_date"].max() == max_snapshot_date else _date_or_nat(group["snapshot_date"].max() + pd.Timedelta(days=1))
        venue_code = str(first["listing_venue_code"])
        status_id = _stable_id(
            source,
            "nasdaq_symbol_directory",
            first["symbol"],
            venue_code,
            "active",
            valid_from,
            valid_to,
            first["directory"],
        )
        rows.append(
            {
                "listing_status_id": status_id,
                "security_id": first.get("security_id"),
                "symbol": first["symbol"],
                "listing_venue_code": venue_code,
                "listing_venue_name": _venue_name(venue_code),
                "listing_exchange_code": first["listing_exchange_code"],
                "status": "active",
                "valid_from": valid_from,
                "valid_to": valid_to,
                "as_of_date": valid_from,
                "available_at": group["source_loaded_at"].min(),
                "last_evidence_as_of_date": last_evidence_date,
                "last_evidence_at": group["source_loaded_at"].max(),
                "source": source,
                "evidence_source": "nasdaq_symbol_directory",
                "evidence_source_table": "nasdaq_symbol_directory",
                "source_event_id": pd.NA,
                "source_snapshot_directory": first["directory"],
                "source_url": first["source_url"],
                "method": "snapshot_presence_consecutive_days",
                "details_json": _details(
                    {
                        "security_name": last.get("security_name"),
                        "exchange": last.get("exchange"),
                        "market_category": last.get("market_category"),
                        "cqs_symbol": last.get("cqs_symbol"),
                        "nasdaq_symbol": last.get("nasdaq_symbol"),
                        "etf": last.get("etf"),
                        "test_issue": last.get("test_issue"),
                        "financial_status": last.get("financial_status"),
                        "round_lot_size": last.get("round_lot_size"),
                        "snapshot_count": int(len(group)),
                    }
                ),
                "run_id": run_id,
            }
        )
    return pd.DataFrame(rows, columns=OUTPUT_COLUMNS)


def _event_status(row: pd.Series) -> str | None:
    actions = {
        str(action).strip().lower()
        for action in (row.get("nasdaq_action"), row.get("bx_action"), row.get("psx_action"))
        if action is not None and not pd.isna(action) and str(action).strip()
    }
    if actions == {"add"}:
        return "active"
    if actions == {"delete"}:
        return "inactive"
    return None


def _event_intervals(store: DuckDBStore, *, source: str, run_id: str | None) -> pd.DataFrame:
    events = store.con.execute(
        """
        SELECT
            event_id,
            symbol,
            security_id,
            company_name,
            nasdaq_action,
            bx_action,
            psx_action,
            effective_date,
            primary_listing_market,
            as_of_date,
            source_file_created_at,
            source_url,
            source_loaded_at
        FROM nasdaq_listing_events
        WHERE symbol IS NOT NULL
          AND symbol <> ''
          AND effective_date IS NOT NULL
        """
    ).df()
    if events.empty:
        return _blank_frame()

    events["symbol"] = events["symbol"].map(symbol_key)
    resolutions = _resolve_security_ids(store, events["symbol"].tolist())
    events = events.merge(resolutions, on="symbol", how="left", suffixes=("", "_resolved"))
    events["security_id"] = events["security_id"].where(events["security_id"].notna(), events["security_id_resolved"])
    rows: list[dict[str, Any]] = []
    for _, row in events.iterrows():
        status = _event_status(row)
        if status is None:
            continue
        venue_code = str(row["primary_listing_market"]).strip().upper() if not pd.isna(row["primary_listing_market"]) else ""
        valid_from = _date_or_nat(row["effective_date"])
        status_id = _stable_id(source, "nasdaq_listing_events", row["event_id"], venue_code, status, valid_from)
        rows.append(
            {
                "listing_status_id": status_id,
                "security_id": row.get("security_id"),
                "symbol": row["symbol"],
                "listing_venue_code": venue_code,
                "listing_venue_name": _venue_name(venue_code),
                "listing_exchange_code": venue_code,
                "status": status,
                "valid_from": valid_from,
                "valid_to": pd.NaT,
                "as_of_date": _date_or_nat(row["as_of_date"]),
                "available_at": row["source_loaded_at"],
                "last_evidence_as_of_date": _date_or_nat(row["as_of_date"]),
                "last_evidence_at": row["source_loaded_at"],
                "source": source,
                "evidence_source": "nasdaq_trading_system_adds_deletes",
                "evidence_source_table": "nasdaq_listing_events",
                "source_event_id": row["event_id"],
                "source_snapshot_directory": pd.NA,
                "source_url": row["source_url"],
                "method": "trading_system_action_checkpoint",
                "details_json": _details(
                    {
                        "company_name": row.get("company_name"),
                        "nasdaq_action": row.get("nasdaq_action"),
                        "bx_action": row.get("bx_action"),
                        "psx_action": row.get("psx_action"),
                        "source_file_created_at": row.get("source_file_created_at"),
                    }
                ),
                "run_id": run_id,
            }
        )
    return pd.DataFrame(rows, columns=OUTPUT_COLUMNS)


def build_listing_status_intervals(
    store: DuckDBStore,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> int:
    store.initialize()
    frame = pd.concat(
        [
            _snapshot_intervals(store, source=source, run_id=run_id),
            _event_intervals(store, source=source, run_id=run_id),
        ],
        ignore_index=True,
    )
    if frame.empty:
        return 0
    with store.transaction():
        store.con.execute("DELETE FROM listing_status_intervals WHERE source = ?", [source])
        insert_frame(store, frame, "listing_status_intervals", "listing_status_intervals_insert")
    return int(len(frame))


class ListingStatusIntervalDataset(Dataset):
    dataset_id = "listing_status_intervals"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: ListingStatusIntervalOptions) -> DatasetLoadResult:
        rows = build_listing_status_intervals(store, source=options.source, run_id=options.run_id)
        summary = store.con.execute(
            """
            SELECT
                count(*) AS rows,
                count(DISTINCT symbol) AS symbols,
                count(security_id) AS resolved_security_rows,
                sum(CASE WHEN status = 'active' THEN 1 ELSE 0 END) AS active_rows,
                sum(CASE WHEN status = 'inactive' THEN 1 ELSE 0 END) AS inactive_rows,
                min(valid_from) AS min_valid_from,
                max(valid_from) AS max_valid_from
            FROM listing_status_intervals
            WHERE source = ?
            """,
            [options.source],
        ).fetchone()
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="listing_status_intervals",
            check_name="built_listing_status_intervals",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={
                "symbols": int(summary[1] or 0),
                "resolved_security_rows": int(summary[2] or 0),
                "active_rows": int(summary[3] or 0),
                "inactive_rows": int(summary[4] or 0),
                "min_valid_from": summary[5],
                "max_valid_from": summary[6],
            },
        )
