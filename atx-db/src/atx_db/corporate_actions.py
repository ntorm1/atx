from __future__ import annotations

from dataclasses import dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "tbltickerhistory inferred corporate actions"


@dataclass(frozen=True)
class CorporateActionsOptions:
    source: str = SOURCE_NAME
    min_cash_amount: float = 0.0001
    max_dividend_factor: float = 0.999999
    run_id: str | None = None


class CorporateActionsDataset(Dataset):
    dataset_id = "corporate_actions"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: CorporateActionsOptions) -> DatasetLoadResult:
        frame = self._build_actions(store, options)
        rows = self._replace_actions(store, frame, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="corporate_actions",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source": options.source,
                "min_cash_amount": options.min_cash_amount,
                "max_dividend_factor": options.max_dividend_factor,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={
                "min_cash_amount": options.min_cash_amount,
                "max_dividend_factor": options.max_dividend_factor,
            },
        )

    def _build_actions(self, store: DuckDBStore, options: CorporateActionsOptions) -> pd.DataFrame:
        rows = store.con.execute(
            """
            SELECT
                security_id,
                coalesce(today_ticker, ticker_tk) AS symbol,
                trading_date AS ex_date,
                close_pr,
                close_unadj_pr,
                return_factor,
                total_return,
                cumul_return_factor
            FROM tbltickerhistory_daily
            WHERE trading_date IS NOT NULL
              AND security_id IS NOT NULL
              AND return_factor IS NOT NULL
              AND close_pr IS NOT NULL
              AND close_unadj_pr IS NOT NULL
              AND return_factor > 0
              AND return_factor < ?
              AND close_unadj_pr - close_pr >= ?
            ORDER BY security_id, trading_date
            """,
            [options.max_dividend_factor, options.min_cash_amount],
        ).fetchall()
        actions = []
        for (
            security_id,
            symbol,
            ex_date,
            close_pr,
            close_unadj_pr,
            return_factor,
            total_return,
            cumul_return_factor,
        ) in rows:
            details = {
                "inference": "cash dividend inferred from close_unadj_pr - close_pr and return_factor < 1",
                "close_pr": close_pr,
                "close_unadj_pr": close_unadj_pr,
                "return_factor": return_factor,
                "total_return": total_return,
                "cumul_return_factor": cumul_return_factor,
            }
            actions.append(
                {
                    "source": options.source,
                    "security_id": security_id,
                    "symbol": symbol,
                    "action_type": "cash_dividend_inferred",
                    "ex_date": ex_date,
                    "declaration_date": pd.NaT,
                    "record_date": pd.NaT,
                    "payable_date": pd.NaT,
                    "cash_amount": float(close_unadj_pr) - float(close_pr),
                    "split_from": pd.NA,
                    "split_to": pd.NA,
                    "adjustment_factor": return_factor,
                    "details_json": json_dumps(details),
                    "available_at": pd.Timestamp(ex_date) + pd.Timedelta(hours=22),
                    "run_id": options.run_id,
                }
            )
        return pd.DataFrame(actions)

    def _replace_actions(
        self,
        store: DuckDBStore,
        frame: pd.DataFrame,
        options: CorporateActionsOptions,
    ) -> int:
        with store.transaction():
            store.con.execute(
                """
                DELETE FROM corporate_actions
                WHERE source = ?
                """,
                [options.source],
            )
            if frame.empty:
                return 0
            store.con.register("corporate_actions_load", frame)
            try:
                insert_frame(store, frame, "corporate_actions", "corporate_actions_insert")
            finally:
                store.con.unregister("corporate_actions_load")
        return int(len(frame))
