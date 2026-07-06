from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from db.factors.catalog import validate_catalog
from db.factors.cross_domain import (
    PRICE_LIQUIDITY_SPECS,
    compute_price_liquidity_factor_rows,
    price_liquidity_definition_frame,
    price_liquidity_factor_definitions,
    price_liquidity_rank_crosscheck,
)


def _price_metric(
    security_id: str,
    symbol: str,
    *,
    momentum_21d: float,
    momentum_126d: float,
    realized_vol_20d: float,
    realized_vol_60d: float,
    pct_from_high_252d: float,
    avg_dollar_volume_21d: float,
    amihud_illiquidity_21d: float,
    beta_60d: float,
    idiosyncratic_vol_60d: float,
    max_drawdown_126d: float,
    momentum_21d_cs_pct_rank: float,
    realized_vol_20d_cs_pct_rank: float,
    amihud_illiquidity_21d_cs_pct_rank: float,
    as_of_date: dt.date = dt.date(2024, 3, 1),
    available_at: str = "2024-03-01 21:00:00",
) -> dict[str, object]:
    return {
        "metric_id": f"metric-{security_id}",
        "security_id": security_id,
        "symbol": symbol,
        "trade_date": as_of_date,
        "as_of_date": as_of_date,
        "available_at": pd.Timestamp(available_at),
        "momentum_21d": momentum_21d,
        "momentum_126d": momentum_126d,
        "realized_vol_20d": realized_vol_20d,
        "realized_vol_60d": realized_vol_60d,
        "pct_from_high_252d": pct_from_high_252d,
        "avg_dollar_volume_21d": avg_dollar_volume_21d,
        "amihud_illiquidity_21d": amihud_illiquidity_21d,
        "beta_60d": beta_60d,
        "idiosyncratic_vol_60d": idiosyncratic_vol_60d,
        "max_drawdown_126d": max_drawdown_126d,
        "momentum_21d_cs_pct_rank": momentum_21d_cs_pct_rank,
        "realized_vol_20d_cs_pct_rank": realized_vol_20d_cs_pct_rank,
        "amihud_illiquidity_21d_cs_pct_rank": amihud_illiquidity_21d_cs_pct_rank,
    }


def _price_metrics_fixture() -> pd.DataFrame:
    return pd.DataFrame(
        [
            _price_metric(
                "SEC-A",
                "AAA",
                momentum_21d=0.20,
                momentum_126d=0.40,
                realized_vol_20d=0.10,
                realized_vol_60d=0.15,
                pct_from_high_252d=-0.01,
                avg_dollar_volume_21d=20_000_000.0,
                amihud_illiquidity_21d=0.10,
                beta_60d=0.80,
                idiosyncratic_vol_60d=0.12,
                max_drawdown_126d=-0.05,
                momentum_21d_cs_pct_rank=1.0,
                realized_vol_20d_cs_pct_rank=1.0 / 3.0,
                amihud_illiquidity_21d_cs_pct_rank=1.0 / 3.0,
            ),
            _price_metric(
                "SEC-B",
                "BBB",
                momentum_21d=0.10,
                momentum_126d=0.20,
                realized_vol_20d=0.20,
                realized_vol_60d=0.30,
                pct_from_high_252d=-0.08,
                avg_dollar_volume_21d=10_000_000.0,
                amihud_illiquidity_21d=0.20,
                beta_60d=1.10,
                idiosyncratic_vol_60d=0.18,
                max_drawdown_126d=-0.15,
                momentum_21d_cs_pct_rank=2.0 / 3.0,
                realized_vol_20d_cs_pct_rank=2.0 / 3.0,
                amihud_illiquidity_21d_cs_pct_rank=2.0 / 3.0,
            ),
            _price_metric(
                "SEC-C",
                "CCC",
                momentum_21d=0.00,
                momentum_126d=-0.10,
                realized_vol_20d=0.30,
                realized_vol_60d=0.45,
                pct_from_high_252d=-0.20,
                avg_dollar_volume_21d=5_000_000.0,
                amihud_illiquidity_21d=0.30,
                beta_60d=1.40,
                idiosyncratic_vol_60d=0.25,
                max_drawdown_126d=-0.30,
                momentum_21d_cs_pct_rank=1.0 / 3.0,
                realized_vol_20d_cs_pct_rank=1.0,
                amihud_illiquidity_21d_cs_pct_rank=1.0,
            ),
            _price_metric(
                "SEC-FUTURE",
                "FFF",
                momentum_21d=0.90,
                momentum_126d=0.90,
                realized_vol_20d=0.01,
                realized_vol_60d=0.01,
                pct_from_high_252d=0.0,
                avg_dollar_volume_21d=50_000_000.0,
                amihud_illiquidity_21d=0.01,
                beta_60d=0.10,
                idiosyncratic_vol_60d=0.01,
                max_drawdown_126d=0.0,
                momentum_21d_cs_pct_rank=1.0,
                realized_vol_20d_cs_pct_rank=1.0 / 4.0,
                amihud_illiquidity_21d_cs_pct_rank=1.0 / 4.0,
                available_at="2024-03-02 09:00:00",
            ),
        ]
    )


def test_price_liquidity_definitions_validate_for_price_source() -> None:
    definitions = price_liquidity_factor_definitions()
    frame = price_liquidity_definition_frame()

    validate_catalog(definitions, known_source_ids=("equity_price_metrics",))

    assert len(definitions) == len(PRICE_LIQUIDITY_SPECS) == 10
    assert set(frame["factor_id"]) >= {
        "price_momentum_21d",
        "price_realized_vol_20d",
        "price_avg_dollar_volume_21d",
        "price_amihud_illiquidity_21d",
        "price_pct_from_high_252d",
    }
    assert frame.loc[frame["factor_id"] == "price_momentum_21d", "scale"].iloc[0] == "percent_rank"
    assert json.loads(frame.loc[frame["factor_id"] == "price_momentum_21d", "input_ids_json"].iloc[0]) == [
        "source:equity_price_metrics"
    ]
    assert json.loads(frame.loc[frame["factor_id"] == "price_momentum_21d", "standardization_spec_json"].iloc[0])[
        "method"
    ] == "rank_cs"


def test_price_liquidity_mapper_emits_ranked_rows_with_lineage_and_native_crosscheck() -> None:
    rows = compute_price_liquidity_factor_rows(_price_metrics_fixture(), run_id="s9-price-run")
    lookup = {(row.factor_id, row.security_id): row for row in rows.itertuples(index=False)}
    momentum = lookup[("price_momentum_21d", "SEC-A")]
    volatility = lookup[("price_realized_vol_20d", "SEC-A")]
    liquidity = lookup[("price_amihud_illiquidity_21d", "SEC-A")]

    assert len(rows) == 3 * len(PRICE_LIQUIDITY_SPECS)
    assert "SEC-FUTURE" not in set(rows["security_id"])
    assert momentum.raw_value == pytest.approx(0.20)
    assert momentum.value == pytest.approx(1.0)
    assert volatility.value == pytest.approx(1.0)
    assert liquidity.value == pytest.approx(1.0)
    assert lookup[("price_momentum_21d", "SEC-B")].value == pytest.approx(0.5)
    assert momentum.available_at == pd.Timestamp("2024-03-01 21:00:00")
    assert momentum.source_row_id == "metric-SEC-A"
    assert json.loads(momentum.input_ids_json) == ["source:equity_price_metrics"]

    lineage = json.loads(momentum.input_lineage_json)
    assert lineage == [
        {
            "source_table": "equity_price_metrics",
            "source_column": "momentum_21d",
            "source_row_id": "metric-SEC-A",
            "as_of_date": "2024-03-01",
            "available_at": "2024-03-01T21:00:00",
            "raw_value": 0.2,
            "native_rank_column": "momentum_21d_cs_pct_rank",
            "native_rank_value": 1.0,
            "native_percent_rank_value": 1.0,
        }
    ]

    report = price_liquidity_rank_crosscheck(rows)
    assert report["status"] == "passed"
    assert report["checked_count"] == 9
    assert report["mismatch_count"] == 0


def test_price_liquidity_seed_rows_round_trip_into_catalog(tmp_store) -> None:
    seeded = tmp_store.con.execute(
        """
        SELECT family, direction, standardization_spec_json, valid_from, valid_to
        FROM factor_definition
        WHERE factor_id = 'price_momentum_21d'
        """
    ).fetchone()
    edge = tmp_store.con.execute(
        """
        SELECT dependency_type, dependency_source_id
        FROM factor_dependency_edges
        WHERE factor_id = 'price_momentum_21d'
        """
    ).fetchone()
    catalog = tmp_store.con.execute(
        """
        SELECT domain, dependency_count
        FROM v_cross_domain_price_liquidity_factor_catalog
        WHERE factor_id = 'price_momentum_21d'
        """
    ).fetchone()
    dataset_row = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM dataset_catalog
        WHERE dataset_id = 'cross_domain_price_liquidity_factors'
        """
    ).fetchone()[0]
    view_catalog_row = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM table_catalog
        WHERE table_name = 'v_cross_domain_price_liquidity_factor_catalog'
        """
    ).fetchone()[0]

    assert seeded[0:2] == ("price_momentum", 1)
    assert json.loads(seeded[2])["source_column"] == "momentum_21d"
    assert seeded[3] == dt.date(1900, 1, 1)
    assert seeded[4] is None
    assert edge == ("source", "equity_price_metrics")
    assert catalog == ("price_liquidity", 1)
    assert dataset_row == 1
    assert view_catalog_row == 1
