from __future__ import annotations

from dataclasses import dataclass

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check


SOURCE_NAME = "equity_daily_bars calendar"


@dataclass(frozen=True)
class TradingCalendarOptions:
    calendar_id: str = "XNYS"
    source: str = SOURCE_NAME
    run_id: str | None = None


class TradingCalendarDataset(Dataset):
    dataset_id = "trading_calendar"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: TradingCalendarOptions) -> DatasetLoadResult:
        with store.transaction():
            store.con.execute(
                "DELETE FROM trading_calendar WHERE calendar_id = ? AND source = ?",
                [options.calendar_id, options.source],
            )
            store.con.execute(
                """
                INSERT INTO trading_calendar (
                    calendar_id,
                    trade_date,
                    is_open,
                    open_time,
                    close_time,
                    source
                )
                SELECT DISTINCT
                    ? AS calendar_id,
                    trade_date,
                    true AS is_open,
                    '09:30:00' AS open_time,
                    '16:00:00' AS close_time,
                    ? AS source
                FROM equity_daily_bars
                WHERE trade_date IS NOT NULL
                ORDER BY trade_date
                """,
                [options.calendar_id, options.source],
            )
        rows = store.con.execute(
            "SELECT count(*) FROM trading_calendar WHERE calendar_id = ? AND source = ?",
            [options.calendar_id, options.source],
        ).fetchone()[0]
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="trading_calendar",
            check_name="open_days_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"calendar_id": options.calendar_id},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(rows),
            source=options.source,
            details={"calendar_id": options.calendar_id},
        )
