"""Govern predicted-announcement earnings seasonality."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT five-year earnings seasonality v1"
FACTOR_ID = "earnings_seasonality_predicted_announcement"


def _earnings_seasonality(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT five-year earnings seasonality','fundamental_earnings',
            'Five-year EarnRank for the fiscal quarter predicted to announce next month from its filing month one year earlier.',
            'mean(rank(split_adjusted_EPS[t-23:t-4])) for t-4,t-8,t-12,t-16,t-20',
            '["metric:eps_diluted_quarterly","market:equity_daily_bars"]',
            1,2200,'{"method":"within_issuer_twenty_quarter_rank_then_cross_sectional_standardization"}',
            'earnings_rank','higher_is_better','zscore',true,
            'Uses first-filed quarterly EPS from six to one years before the predicted announcement month; formation is the preceding month-end and never conditions on the future announcement.',
            'atx_db.earnings_seasonality','atx-db',?,
            '{"method":"twenty_quarter_earnrank_then_winsorize_zscore_cs","history_quarters":20,"same_quarter_observations":5,"minimum_price_usd":5,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    conn.executemany(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','earnings_seasonality',?,?,?)),
                  ?,?,?,?,?,?,1,?,2200,true,?)
        """,
        [
            (
                FACTOR_ID,"metric","eps_diluted_quarterly",FACTOR_ID,"metric",
                "eps_diluted_quarterly",None,None,None,"metric:eps_diluted_quarterly",SOURCE,
            ),
            (
                FACTOR_ID,"market","equity_daily_bars",FACTOR_ID,"market",
                "equity_daily_bars",None,None,"equity_daily_bars","market:equity_daily_bars",SOURCE,
            ),
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=266, name="predicted_announcement_earnings_seasonality", up=_earnings_seasonality)]
