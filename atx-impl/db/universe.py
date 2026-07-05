from __future__ import annotations

import datetime as dt
from dataclasses import asdict, dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check, symbol_key


SOURCE_NAME = "atx-impl governed universe builder"
DEFAULT_UNIVERSE_ID = "us_common_equity_liquid_v1"
DEFAULT_SNAPSHOT_UNIVERSE_ID = "us_liquid_equity_v1"

OUTPUT_COLUMNS = [
    "universe_id",
    "security_id",
    "symbol",
    "valid_from",
    "valid_to",
    "as_of_date",
    "is_member",
    "reason",
    "rules_json",
    "decision_count",
    "available_at",
    "source",
    "run_id",
]


@dataclass(frozen=True)
class UniverseMembershipOptions:
    universe_id: str = DEFAULT_UNIVERSE_ID
    name: str = "US common-equity liquid PIT universe"
    description: str = (
        "Interval-keyed PIT common-equity universe decisions reconciled to the "
        "legacy liquidity snapshot universe."
    )
    snapshot_universe_id: str = DEFAULT_SNAPSHOT_UNIVERSE_ID
    symbols: tuple[str, ...] | None = None
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    lookback_days: int = 20
    min_history_days: int = 20
    min_price: float = 5.0
    min_dollar_volume: float = 10_000_000.0
    source: str = SOURCE_NAME
    run_id: str | None = None


def _rules(options: UniverseMembershipOptions) -> dict[str, object]:
    return {
        "snapshot_universe_id": options.snapshot_universe_id,
        "lookback_days": options.lookback_days,
        "min_history_days": options.min_history_days,
        "min_price": options.min_price,
        "min_dollar_volume": options.min_dollar_volume,
    }


def _empty_output() -> pd.DataFrame:
    return pd.DataFrame(columns=OUTPUT_COLUMNS)


def _bool_series(frame: pd.DataFrame, column: str, default: bool) -> pd.Series:
    if column not in frame.columns:
        return pd.Series(default, index=frame.index, dtype=bool)
    series = frame[column]
    if series.dtype == bool:
        return series.fillna(default).astype(bool)
    normalized = series.astype("string").str.strip().str.lower()
    mapped = normalized.map(
        {
            "1": True,
            "true": True,
            "t": True,
            "yes": True,
            "y": True,
            "0": False,
            "false": False,
            "f": False,
            "no": False,
            "n": False,
        }
    )
    return mapped.fillna(default).astype(bool)


def _text(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series("", index=frame.index, dtype="string")
    return frame[column].fillna("").astype("string")


def _common_equity_mask(frame: pd.DataFrame) -> pd.Series:
    asset_class = _text(frame, "asset_class").str.strip().str.upper()
    security_type = _text(frame, "security_type").str.strip().str.upper()
    name_blob = (
        _text(frame, "symbol")
        + " "
        + _text(frame, "security_name")
        + " "
        + _text(frame, "name")
    ).str.upper()

    equity_asset = asset_class.isin(("", "EQUITY", "COMMON", "COMMON_STOCK", "COMMON EQUITY"))
    common_type = security_type.isin(("", "COMMON", "COMMON_STOCK", "COMMON EQUITY", "CS"))
    non_common_text = name_blob.str.contains(
        r"\b(?:ADR|ADS|PREFERRED|PREFERENCE|PREF|WARRANT|RIGHT|UNIT|ETF|ETN|NOTE|BOND)\b",
        regex=True,
        na=False,
    )
    explicit_etf = _bool_series(frame, "is_etf", False) | _bool_series(frame, "etf", False)
    return equity_asset & common_type & ~non_common_text & ~explicit_etf


def compute_universe_membership_intervals(
    daily_decisions: pd.DataFrame,
    options: UniverseMembershipOptions = UniverseMembershipOptions(),
) -> pd.DataFrame:
    """Compress daily PIT universe decisions into deterministic validity intervals."""
    if daily_decisions is None or daily_decisions.empty:
        return _empty_output()

    frame = daily_decisions.copy()
    if "as_of_date" not in frame.columns:
        if "trade_date" not in frame.columns:
            raise ValueError("daily_decisions must contain as_of_date or trade_date")
        frame["as_of_date"] = frame["trade_date"]

    frame["security_id"] = frame["security_id"].astype("string")
    frame["symbol"] = frame.get("symbol", pd.Series(pd.NA, index=frame.index)).map(symbol_key)
    frame["as_of_date"] = pd.to_datetime(frame["as_of_date"], errors="coerce").dt.date
    frame = frame.dropna(subset=["security_id", "as_of_date"]).reset_index(drop=True)
    if frame.empty:
        return _empty_output()

    available = pd.to_datetime(frame.get("available_at"), errors="coerce")
    default_available = pd.to_datetime(frame["as_of_date"]) + pd.Timedelta(hours=22)
    frame["available_at"] = available.where(available.notna(), default_available)

    common = (
        _bool_series(frame, "is_common_equity", True)
        if "is_common_equity" in frame.columns
        else _common_equity_mask(frame)
    )
    active_listing = (
        _bool_series(frame, "is_active_listing", True)
        if "is_active_listing" in frame.columns
        else _bool_series(frame, "active", True)
    )
    snapshot_member = _bool_series(frame, "liquidity_snapshot_member", False)
    close = pd.to_numeric(frame.get("close"), errors="coerce")
    avg_dollar_volume = pd.to_numeric(
        frame.get("avg_dollar_volume", frame.get("avg_dollar_volume_21d")),
        errors="coerce",
    )
    history_days = pd.to_numeric(frame.get("history_days"), errors="coerce")
    liquidity_pass = snapshot_member | (
        close.ge(options.min_price)
        & avg_dollar_volume.ge(options.min_dollar_volume)
        & history_days.ge(options.min_history_days)
    )

    frame["is_member"] = common & active_listing & liquidity_pass
    frame["reason"] = "member"
    frame.loc[~common, "reason"] = "not_common_equity"
    frame.loc[common & ~active_listing, "reason"] = "inactive_listing"
    frame.loc[common & active_listing & ~liquidity_pass, "reason"] = "liquidity_screen_fail"

    rules_json = json_dumps(_rules(options))
    rows: list[dict[str, object]] = []
    sort_columns = ["security_id", "as_of_date", "is_member", "reason"]
    for security_id, group in frame.sort_values(sort_columns, kind="stable").groupby(
        "security_id",
        sort=True,
        dropna=False,
    ):
        current: dict[str, object] | None = None
        previous_date: dt.date | None = None
        for row in group.itertuples(index=False):
            state = (bool(row.is_member), str(row.reason), getattr(row, "symbol", None))
            row_date = row.as_of_date
            row_available = pd.Timestamp(row.available_at).to_pydatetime()
            if current is None or current["_state"] != state:
                if current is not None:
                    current["valid_to"] = previous_date
                    rows.append({k: v for k, v in current.items() if k != "_state"})
                current = {
                    "_state": state,
                    "universe_id": options.universe_id,
                    "security_id": str(security_id),
                    "symbol": getattr(row, "symbol", None),
                    "valid_from": row_date,
                    "valid_to": row_date,
                    "as_of_date": row_date,
                    "is_member": bool(row.is_member),
                    "reason": str(row.reason),
                    "rules_json": rules_json,
                    "decision_count": 0,
                    "available_at": row_available,
                    "source": options.source,
                    "run_id": options.run_id,
                }
            current["decision_count"] = int(current["decision_count"]) + 1
            previous_date = row_date
        if current is not None:
            current["valid_to"] = previous_date
            rows.append({k: v for k, v in current.items() if k != "_state"})

    if not rows:
        return _empty_output()
    return pd.DataFrame.from_records(rows, columns=OUTPUT_COLUMNS).sort_values(
        ["valid_from", "security_id", "is_member", "reason"],
        kind="stable",
    ).reset_index(drop=True)


class GovernedUniverseMembershipDataset(Dataset):
    dataset_id = "universe_membership"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: UniverseMembershipOptions) -> DatasetLoadResult:
        if options.lookback_days < 1:
            raise ValueError("lookback_days must be positive")
        if options.min_history_days < 1:
            raise ValueError("min_history_days must be positive")
        if options.min_history_days > options.lookback_days:
            raise ValueError("min_history_days cannot exceed lookback_days")

        daily = self._daily_decisions(store, options)
        intervals = compute_universe_membership_intervals(daily, options)
        rows = self._replace_intervals(store, intervals, options)
        members = int(intervals["is_member"].sum()) if not intervals.empty else 0
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="universe_membership",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "universe_id": options.universe_id,
                "member_intervals": members,
                "symbols": None
                if options.symbols is None
                else sorted({symbol_key(symbol) for symbol in options.symbols}),
                "rules": _rules(options),
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={
                "universe_id": options.universe_id,
                "member_intervals": members,
                "start_date": options.start_date,
                "end_date": options.end_date,
                "rules": _rules(options),
            },
        )

    def _daily_decisions(
        self,
        store: DuckDBStore,
        options: UniverseMembershipOptions,
    ) -> pd.DataFrame:
        lookback_preceding = options.lookback_days - 1
        filters = [
            "b.trade_date IS NOT NULL",
            "b.security_id IS NOT NULL",
            "b.close IS NOT NULL",
            "b.close > 0",
            "b.volume IS NOT NULL",
            "b.volume >= 0",
        ]
        params: list[object] = []
        symbol_join = ""
        registered = False
        if options.symbols is not None:
            symbols = sorted({symbol_key(symbol) for symbol in options.symbols if symbol_key(symbol)})
            store.con.register("governed_universe_symbol_filter", pd.DataFrame({"symbol": symbols}))
            registered = True
            symbol_join = "JOIN governed_universe_symbol_filter sf ON sf.symbol = b.symbol"
        if options.start_date is not None:
            filters.append("b.trade_date >= ?")
            params.append(options.start_date)
        if options.end_date is not None:
            filters.append("b.trade_date <= ?")
            params.append(options.end_date)

        sql = f"""
            WITH base AS (
                SELECT
                    b.security_id,
                    b.symbol,
                    b.trade_date AS as_of_date,
                    b.close,
                    b.close * b.volume AS dollar_volume,
                    b.available_at,
                    count(*) OVER (
                        PARTITION BY b.security_id
                        ORDER BY b.trade_date
                        ROWS BETWEEN {lookback_preceding} PRECEDING AND CURRENT ROW
                    ) AS history_days,
                    avg(b.close * b.volume) OVER (
                        PARTITION BY b.security_id
                        ORDER BY b.trade_date
                        ROWS BETWEEN {lookback_preceding} PRECEDING AND CURRENT ROW
                    ) AS avg_dollar_volume
                FROM equity_daily_bars b
                {symbol_join}
                WHERE {" AND ".join(filters)}
            ),
            listing AS (
                SELECT
                    base.security_id,
                    base.as_of_date,
                    l.status AS listing_status,
                    row_number() OVER (
                        PARTITION BY base.security_id, base.as_of_date
                        ORDER BY l.available_at DESC NULLS LAST, l.valid_from DESC
                    ) AS rn
                FROM base
                LEFT JOIN listing_status_intervals l
                  ON l.security_id = base.security_id
                 AND l.valid_from <= base.as_of_date
                 AND (l.valid_to IS NULL OR l.valid_to >= base.as_of_date)
                 AND (l.available_at IS NULL OR l.available_at <= base.available_at)
            )
            SELECT
                base.security_id,
                base.symbol,
                base.as_of_date,
                base.close,
                base.avg_dollar_volume,
                base.history_days,
                base.available_at,
                coalesce(s.asset_class, 'EQUITY') AS asset_class,
                coalesce(s.name, base.symbol) AS security_name,
                coalesce(s.active, true) AS active,
                CASE
                    WHEN listing.listing_status IS NULL THEN coalesce(s.active, true)
                    ELSE lower(listing.listing_status) = 'active'
                END AS is_active_listing,
                lm.security_id IS NOT NULL AS liquidity_snapshot_member
            FROM base
            LEFT JOIN securities s ON s.security_id = base.security_id
            LEFT JOIN listing
              ON listing.security_id = base.security_id
             AND listing.as_of_date = base.as_of_date
             AND listing.rn = 1
            LEFT JOIN universe_memberships lm
              ON lm.universe_id = ?
             AND lm.security_id = base.security_id
             AND lm.as_of_date = base.as_of_date
             AND (lm.available_at IS NULL OR lm.available_at <= base.available_at)
            ORDER BY base.as_of_date, base.security_id
        """
        try:
            return store.con.execute(sql, [*params, options.snapshot_universe_id]).df()
        finally:
            if registered:
                store.con.unregister("governed_universe_symbol_filter")

    def _replace_intervals(
        self,
        store: DuckDBStore,
        frame: pd.DataFrame,
        options: UniverseMembershipOptions,
    ) -> int:
        with store.transaction():
            store.con.execute("DELETE FROM universes WHERE universe_id = ?", [options.universe_id])
            store.con.execute(
                """
                INSERT INTO universes (
                    universe_id, name, description, rules_json
                )
                VALUES (?, ?, ?, ?)
                """,
                [
                    options.universe_id,
                    options.name,
                    options.description,
                    json_dumps(asdict(options) | {"rules": _rules(options)}),
                ],
            )

            predicates = ["universe_id = ?", "source = ?"]
            params: list[object] = [options.universe_id, options.source]
            if options.start_date is not None:
                predicates.append("coalesce(valid_to, valid_from) >= ?")
                params.append(options.start_date)
            if options.end_date is not None:
                predicates.append("valid_from <= ?")
                params.append(options.end_date)
            store.con.execute(
                f"DELETE FROM universe_membership WHERE {' AND '.join(predicates)}",
                params,
            )
            if frame.empty:
                return 0
            insert_frame(store, frame, "universe_membership", "universe_membership_insert")
        return int(len(frame))
