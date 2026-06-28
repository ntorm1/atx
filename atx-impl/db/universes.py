from __future__ import annotations

import datetime as dt
from dataclasses import asdict, dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check, symbol_key


SOURCE_NAME = "atx-impl universe builder"


@dataclass(frozen=True)
class UniverseBuildOptions:
    universe_id: str = "us_liquid_equity_v1"
    name: str = "US liquid equity research universe"
    description: str = "Trailing-liquidity PIT universe derived from canonical daily bars."
    symbols: tuple[str, ...] | None = None
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    lookback_days: int = 20
    min_history_days: int = 20
    min_price: float = 5.0
    min_dollar_volume: float = 10_000_000.0
    source: str = SOURCE_NAME
    run_id: str | None = None


class UniverseMembershipDataset(Dataset):
    dataset_id = "universe_memberships"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: UniverseBuildOptions) -> DatasetLoadResult:
        if options.lookback_days < 1:
            raise ValueError("lookback_days must be positive")
        if options.min_history_days < 1:
            raise ValueError("min_history_days must be positive")
        if options.min_history_days > options.lookback_days:
            raise ValueError("min_history_days cannot exceed lookback_days")

        frame = self._build_memberships(store, options)
        rows = self._replace_memberships(store, frame, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="universe_memberships",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "universe_id": options.universe_id,
                "symbols": None if options.symbols is None else sorted({symbol_key(symbol) for symbol in options.symbols}),
                "rules": self._rules(options),
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={
                "universe_id": options.universe_id,
                "start_date": options.start_date,
                "end_date": options.end_date,
                "rules": self._rules(options),
            },
        )

    def _rules(self, options: UniverseBuildOptions) -> dict[str, object]:
        return {
            "lookback_days": options.lookback_days,
            "min_history_days": options.min_history_days,
            "min_price": options.min_price,
            "min_dollar_volume": options.min_dollar_volume,
        }

    def _build_memberships(self, store: DuckDBStore, options: UniverseBuildOptions) -> pd.DataFrame:
        lookback_preceding = options.lookback_days - 1
        joins = ""
        base_filters = [
            "b.trade_date IS NOT NULL",
            "b.security_id IS NOT NULL",
            "b.symbol IS NOT NULL",
            "b.close IS NOT NULL",
            "b.close > 0",
            "b.volume IS NOT NULL",
            "b.volume >= 0",
        ]
        output_filters = [
            "close >= ?",
            "avg_dollar_volume >= ?",
            "history_days >= ?",
        ]
        params: list[object] = []
        if options.symbols is not None:
            symbols = sorted({symbol_key(symbol) for symbol in options.symbols if symbol_key(symbol)})
            store.con.register("universe_symbol_filter", pd.DataFrame({"symbol": symbols}))
            joins = "JOIN universe_symbol_filter sf ON sf.symbol = b.symbol"
        if options.end_date is not None:
            base_filters.append("b.trade_date <= ?")
            params.append(options.end_date)
        output_params: list[object] = [options.min_price, options.min_dollar_volume, options.min_history_days]
        if options.start_date is not None:
            output_filters.append("trade_date >= ?")
            output_params.append(options.start_date)
        if options.end_date is not None:
            output_filters.append("trade_date <= ?")
            output_params.append(options.end_date)

        rules_json = json_dumps(self._rules(options))
        sql = f"""
            WITH base AS (
                SELECT
                    b.security_id,
                    b.symbol,
                    b.trade_date,
                    b.close,
                    b.close * b.volume AS dollar_volume,
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
                {joins}
                WHERE {" AND ".join(base_filters)}
            )
            SELECT
                ? AS universe_id,
                security_id,
                symbol,
                trade_date AS effective_date,
                trade_date AS as_of_date,
                NULL::DOUBLE AS weight,
                ? AS reason,
                trade_date::TIMESTAMP + INTERVAL 22 HOURS AS available_at,
                ? AS source,
                ? AS run_id
            FROM base
            WHERE {" AND ".join(output_filters)}
            ORDER BY as_of_date, security_id
        """
        query_params = [
            *params,
            options.universe_id,
            rules_json,
            options.source,
            options.run_id,
            *output_params,
        ]
        try:
            return store.con.execute(sql, query_params).df()
        finally:
            if options.symbols is not None:
                store.con.unregister("universe_symbol_filter")

    def _replace_memberships(
        self,
        store: DuckDBStore,
        frame: pd.DataFrame,
        options: UniverseBuildOptions,
    ) -> int:
        rules_json = json_dumps(self._rules(options))
        with store.transaction():
            store.con.execute("DELETE FROM universes WHERE universe_id = ?", [options.universe_id])
            store.con.execute(
                """
                INSERT INTO universes (
                    universe_id,
                    name,
                    description,
                    rules_json
                )
                VALUES (?, ?, ?, ?)
                """,
                [options.universe_id, options.name, options.description, json_dumps(asdict(options) | {"rules": self._rules(options)})],
            )

            predicates = ["universe_id = ?", "source = ?"]
            params: list[object] = [options.universe_id, options.source]
            if options.start_date is not None:
                predicates.append("as_of_date >= ?")
                params.append(options.start_date)
            if options.end_date is not None:
                predicates.append("as_of_date <= ?")
                params.append(options.end_date)
            store.con.execute(
                f"DELETE FROM universe_memberships WHERE {' AND '.join(predicates)}",
                params,
            )
            if frame.empty:
                return 0
            store.con.register("universe_memberships_load", frame)
            try:
                insert_frame(store, frame, "universe_memberships", "universe_memberships_insert")
            finally:
                store.con.unregister("universe_memberships_load")
        return int(len(frame))
