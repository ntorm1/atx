from __future__ import annotations

from pathlib import Path

import pandas as pd

from .connection import DEFAULT_DB_PATH, connect


SHORT_INTEREST_WITH_13F_SQL = """
WITH recent_short AS (
    SELECT
        settlement_date,
        symbol,
        current_short_position_quantity,
        previous_short_position_quantity,
        change_previous_number,
        change_percent,
        average_daily_volume_quantity,
        days_to_cover_quantity,
        row_number() OVER (ORDER BY settlement_date DESC) AS rn
    FROM finra_short_interest
    WHERE symbol = upper(?)
),
security AS (
    SELECT id_value AS cusip
    FROM security_identifiers
    WHERE symbol = upper(?) AND id_type = 'CUSIP'
    ORDER BY updated_at DESC
    LIMIT 1
),
latest_13f AS (
    SELECT p.*
    FROM v_thirteenf_positioning_by_security p
    JOIN security s ON s.cusip = p.cusip
    ORDER BY p.report_period DESC NULLS LAST, p.source_period DESC
    LIMIT 1
)
SELECT
    r.settlement_date,
    r.symbol,
    r.current_short_position_quantity,
    r.previous_short_position_quantity,
    r.change_previous_number,
    r.change_percent,
    r.average_daily_volume_quantity,
    r.days_to_cover_quantity,
    l.report_period AS thirteenf_report_period,
    l.total_common_share_quantity AS thirteenf_total_common_shares,
    l.total_common_value_usd AS thirteenf_total_common_value_usd,
    l.filing_count AS thirteenf_filing_count,
    l.holding_rows AS thirteenf_holding_rows,
    l.call_share_quantity AS thirteenf_call_share_quantity,
    l.put_share_quantity AS thirteenf_put_share_quantity
FROM recent_short r
CROSS JOIN latest_13f l
WHERE r.rn <= ?
ORDER BY r.settlement_date DESC
"""


def short_interest_with_13f_positioning(
    symbol: str = "AAPL",
    periods: int = 10,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    with connect(db_path, read_only=True) as store:
        return store.con.execute(
            SHORT_INTEREST_WITH_13F_SQL,
            [symbol.upper(), symbol.upper(), periods],
        ).df()
