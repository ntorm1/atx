from __future__ import annotations

import datetime as dt
import uuid
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .symbol_directory import IDENTITY_BASIS_CURRENT_TICKER, cutoff_end, normalize_nasdaq_action
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
    "is_latest_revision",
]

EVENT_ACTION_OUTCOMES = ("add", "delete", "mixed", "no_action", "unrecognized")


@dataclass(frozen=True)
class ListingStatusIntervalOptions:
    source: str = DEFAULT_SOURCE
    run_id: str | None = None
    # Report cutoff: evidence dated after it (snapshot/file as_of_date) is ignored.
    as_of_date: dt.date | None = None


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
    """Resolve symbols through the CURRENT ticker maps (``current_ticker_unverified``)."""
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


def _identity_details(security_id: Any) -> dict[str, Any]:
    resolved = security_id is not None and not pd.isna(security_id)
    return {
        "identity_basis": IDENTITY_BASIS_CURRENT_TICKER,
        "security_resolution": "resolved" if resolved else "unresolved",
    }


def _cutoff_sql(cutoff: dt.date | None) -> tuple[str, list[Any]]:
    """Evidence dated, and known, by the cutoff day (``as_of_date`` and ``available_at``)."""
    if cutoff is None:
        return "", []
    return (
        "AND as_of_date <= ? AND coalesce(available_at, source_loaded_at) <= ?",
        [cutoff, cutoff_end(cutoff)],
    )


def _snapshot_intervals(
    store: DuckDBStore, *, source: str, run_id: str | None, cutoff: dt.date | None = None
) -> pd.DataFrame:
    cutoff_sql, params = _cutoff_sql(cutoff)
    snapshots = store.con.execute(
        f"""
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
            coalesce(available_at, source_loaded_at) AS evidence_available_at
        FROM nasdaq_symbol_directory
        WHERE symbol IS NOT NULL
          AND symbol <> ''
          AND coalesce(is_latest_revision, true)
          {cutoff_sql}
        """,
        params,
    ).df()
    if snapshots.empty:
        return _blank_frame()

    snapshots["symbol"] = snapshots["symbol"].map(symbol_key)
    is_nasdaq = snapshots["directory"] == "nasdaqlisted"
    snapshots["listing_venue_code"] = (
        snapshots["market_category"].where(is_nasdaq, snapshots["exchange"])
        .fillna("").astype(str).str.strip().str.upper()
    )
    snapshots["listing_exchange_code"] = (
        snapshots["exchange"].where(~is_nasdaq, "NASDAQ").fillna("").astype(str).str.strip().str.upper()
    )
    snapshots = snapshots[snapshots["listing_venue_code"] != ""].copy()
    if snapshots.empty:
        return _blank_frame()

    resolutions = _resolve_security_ids(store, snapshots["symbol"].tolist())
    snapshots = snapshots.merge(resolutions, on="symbol", how="left")
    snapshots["snapshot_date"] = pd.to_datetime(snapshots["as_of_date"])
    max_snapshot_date = snapshots["snapshot_date"].max()
    group_keys = ["symbol", "listing_venue_code", "listing_exchange_code"]
    snapshots = snapshots.sort_values([*group_keys, "snapshot_date", "directory", "source_url"], kind="stable")
    snapshots["previous_snapshot_date"] = snapshots.groupby(group_keys)["snapshot_date"].shift()
    snapshots["new_streak"] = (
        snapshots["previous_snapshot_date"].isna()
        | ((snapshots["snapshot_date"] - snapshots["previous_snapshot_date"]).dt.days > 1)
    )
    snapshots["streak_id"] = snapshots.groupby(group_keys)["new_streak"].cumsum()

    # Vectorized per-streak aggregation (one row per symbol/venue streak); the
    # frame is sorted by snapshot date inside each streak, so head/tail are the
    # first and last snapshots.
    streak_keys = [*group_keys, "streak_id"]
    grouped = snapshots.groupby(streak_keys, sort=False)
    stats = grouped.agg(
        first_date=("snapshot_date", "min"),
        last_date=("snapshot_date", "max"),
        snapshot_count=("snapshot_date", "size"),
        available_at=("evidence_available_at", "min"),
        last_evidence_at=("evidence_available_at", "max"),
    )
    last_columns = ["security_name", "exchange", "market_category", "cqs_symbol", "nasdaq_symbol", "etf",
                    "test_issue", "financial_status", "round_lot_size"]
    streaks = (
        stats.join(grouped.head(1).set_index(streak_keys)[["directory", "source_url", "security_id"]])
        .join(grouped.tail(1).set_index(streak_keys)[last_columns].add_prefix("last_"))
        .reset_index()
    )

    rows: list[dict[str, Any]] = []
    for stat in streaks.to_dict("records"):
        symbol, venue_code, exchange_code = stat["symbol"], stat["listing_venue_code"], stat["listing_exchange_code"]
        # A snapshot interval starts at its first snapshot date: current-snapshot
        # presence is never extended backwards in time.
        valid_from = _date_or_nat(stat["first_date"])
        last_evidence_date = _date_or_nat(stat["last_date"])
        valid_to = (
            pd.NaT if stat["last_date"] == max_snapshot_date
            else _date_or_nat(stat["last_date"] + pd.Timedelta(days=1))
        )
        security_id = stat["security_id"]
        status_id = _stable_id(
            source,
            "nasdaq_symbol_directory",
            symbol,
            venue_code,
            "active",
            valid_from,
            valid_to,
            stat["directory"],
        )
        rows.append(
            {
                "listing_status_id": status_id,
                "security_id": security_id,
                "symbol": symbol,
                "listing_venue_code": venue_code,
                "listing_venue_name": _venue_name(venue_code),
                "listing_exchange_code": exchange_code,
                "status": "active",
                "valid_from": valid_from,
                "valid_to": valid_to,
                "as_of_date": valid_from,
                "available_at": stat["available_at"],
                "last_evidence_as_of_date": last_evidence_date,
                "last_evidence_at": stat["last_evidence_at"],
                "source": source,
                "evidence_source": "nasdaq_symbol_directory",
                "evidence_source_table": "nasdaq_symbol_directory",
                "source_event_id": pd.NA,
                "source_snapshot_directory": stat["directory"],
                "source_url": stat["source_url"],
                "method": "snapshot_presence_consecutive_days",
                "details_json": _details(
                    {
                        **{column: stat[f"last_{column}"] for column in last_columns},
                        "snapshot_count": int(stat["snapshot_count"]),
                        **_identity_details(security_id),
                    }
                ),
                "run_id": run_id,
                "is_latest_revision": True,
            }
        )
    return pd.DataFrame(rows, columns=OUTPUT_COLUMNS)


def event_action_outcome(row: pd.Series | dict[str, Any]) -> str:
    """Classify one Adds/Deletes row across its three venue action columns.

    Canonical (``add``/``delete``) and legacy (``A``/``Add``/``D``/``Delete``, any
    case) spellings are accepted. Returns ``add``, ``delete``, ``mixed`` (both add
    and delete present), ``no_action`` or ``unrecognized``.
    """
    actions = {
        normalize_nasdaq_action(row.get(column))
        for column in ("nasdaq_action", "bx_action", "psx_action")
    } - {None}
    if not actions:
        return "no_action"
    if not actions <= {"add", "delete"}:
        return "unrecognized"
    if len(actions) > 1:
        return "mixed"
    return next(iter(actions))


_OUTCOME_STATUS = {"add": "active", "delete": "inactive"}


def _event_intervals(
    store: DuckDBStore,
    *,
    source: str,
    run_id: str | None,
    cutoff: dt.date | None = None,
    outcomes: dict[str, int] | None = None,
) -> pd.DataFrame:
    cutoff_sql, params = _cutoff_sql(cutoff)
    events = store.con.execute(
        f"""
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
            coalesce(available_at, source_loaded_at) AS evidence_available_at
        FROM nasdaq_listing_events
        WHERE symbol IS NOT NULL
          AND symbol <> ''
          AND effective_date IS NOT NULL
          AND coalesce(is_latest_revision, true)
          {cutoff_sql}
        """,
        params,
    ).df()
    if events.empty:
        return _blank_frame()

    events["symbol"] = events["symbol"].map(symbol_key)
    resolutions = _resolve_security_ids(store, events["symbol"].tolist())
    events = events.merge(resolutions, on="symbol", how="left", suffixes=("", "_resolved"))
    events["security_id"] = events["security_id"].where(events["security_id"].notna(), events["security_id_resolved"])
    rows: list[dict[str, Any]] = []
    for _, row in events.iterrows():
        outcome = event_action_outcome(row)
        if outcomes is not None:
            outcomes[outcome] = outcomes.get(outcome, 0) + 1
        status = _OUTCOME_STATUS.get(outcome)
        if status is None:
            continue
        venue_code = str(row["primary_listing_market"]).strip().upper() if not pd.isna(row["primary_listing_market"]) else ""
        effective_date = _date_or_nat(row["effective_date"])
        snapshot_date = _date_or_nat(row["as_of_date"])
        # Never assert a (current-ticker-resolved) status before the snapshot that
        # evidences it; the published effective date is kept in details_json.
        floored = not pd.isna(snapshot_date) and effective_date < snapshot_date
        valid_from = snapshot_date if floored else effective_date
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
                "as_of_date": snapshot_date,
                "available_at": row["evidence_available_at"],
                "last_evidence_as_of_date": snapshot_date,
                "last_evidence_at": row["evidence_available_at"],
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
                        "nasdaq_action": normalize_nasdaq_action(row.get("nasdaq_action")),
                        "bx_action": normalize_nasdaq_action(row.get("bx_action")),
                        "psx_action": normalize_nasdaq_action(row.get("psx_action")),
                        "source_file_created_at": row.get("source_file_created_at"),
                        "effective_date": effective_date,
                        "valid_from_basis": "snapshot_date_floor" if floored else "effective_date",
                        **_identity_details(row.get("security_id")),
                    }
                ),
                "run_id": run_id,
                "is_latest_revision": True,
            }
        )
    return pd.DataFrame(rows, columns=OUTPUT_COLUMNS)


@dataclass(frozen=True)
class ListingStatusBuild:
    rows: int
    snapshot_rows: int
    event_rows: int
    event_action_outcomes: dict[str, int]


def build_listing_status(
    store: DuckDBStore,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
    cutoff: dt.date | None = None,
) -> ListingStatusBuild:
    """Rebuild ``listing_status_intervals`` for ``source`` from latest snapshot evidence.

    The table is derived: the prior build of ``source`` is replaced. Mixed add+delete
    and unrecognized action rows produce no status and are counted.
    """
    store.initialize()
    outcomes = dict.fromkeys(EVENT_ACTION_OUTCOMES, 0)
    snapshot_frame = _snapshot_intervals(store, source=source, run_id=run_id, cutoff=cutoff)
    event_frame = _event_intervals(store, source=source, run_id=run_id, cutoff=cutoff, outcomes=outcomes)
    frames = [frame for frame in (snapshot_frame, event_frame) if not frame.empty]
    frame = pd.concat(frames, ignore_index=True) if frames else _blank_frame()
    # The table is derived: an empty rebuild must still retire the prior build of
    # ``source`` so stale (e.g. post-cutoff) intervals never survive it.
    with store.transaction():
        store.con.execute("DELETE FROM listing_status_intervals WHERE source = ?", [source])
        insert_frame(store, frame, "listing_status_intervals", "listing_status_intervals_insert")
    return ListingStatusBuild(len(frame), len(snapshot_frame), len(event_frame), outcomes)


def build_listing_status_intervals(
    store: DuckDBStore,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> int:
    return build_listing_status(store, source=source, run_id=run_id).rows


class ListingStatusIntervalDataset(Dataset):
    dataset_id = "listing_status_intervals"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: ListingStatusIntervalOptions) -> DatasetLoadResult:
        build = build_listing_status(store, source=options.source, run_id=options.run_id, cutoff=options.as_of_date)
        rows = build.rows
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
        assert summary is not None
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="listing_status_intervals",
            check_name="built_listing_status_intervals",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source, "event_action_outcomes": build.event_action_outcomes},
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
                "snapshot_interval_rows": build.snapshot_rows,
                "event_interval_rows": build.event_rows,
                "event_action_outcomes": build.event_action_outcomes,
                "identity_basis": IDENTITY_BASIS_CURRENT_TICKER,
                "cutoff": options.as_of_date,
            },
        )
