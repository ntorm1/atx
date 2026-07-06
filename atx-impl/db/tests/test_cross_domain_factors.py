from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from db.factors.catalog import validate_catalog
from db.factors.cross_domain import (
    ESTIMATE_13F_SPECS,
    PRICE_LIQUIDITY_SPECS,
    compute_estimate_revision_factor_rows,
    compute_price_liquidity_factor_rows,
    compute_thirteenf_flow_factor_rows,
    estimate_13f_definition_frame,
    estimate_13f_factor_definitions,
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


def _surprise(
    security_id: str,
    symbol: str,
    *,
    sue: float,
    surprise_pct: float,
    actual: float,
    consensus_mean: float,
    available_at: str = "2024-04-20 08:00:00",
) -> dict[str, object]:
    return {
        "security_id": security_id,
        "symbol": symbol,
        "measure_code": "EPS_DILUTED",
        "fiscal_year": 2024,
        "fiscal_period": "Q1",
        "period_end": dt.date(2024, 3, 31),
        "actual": actual,
        "expected": consensus_mean,
        "surprise": actual - consensus_mean,
        "sue": sue,
        "consensus_mean": consensus_mean,
        "surprise_pct": surprise_pct,
        "as_of_date": dt.date(2024, 3, 31),
        "available_at": pd.Timestamp(available_at),
        "source": "fixture_est_surprise",
    }


def _consensus(
    security_id: str,
    symbol: str,
    *,
    row_id: str,
    consensus_date: str,
    mean: float,
    num_up: int | None,
    num_down: int | None,
    num_estimates: int | None,
    available_at: str,
) -> dict[str, object]:
    date = dt.date.fromisoformat(consensus_date)
    return {
        "est_consensus_id": row_id,
        "security_id": security_id,
        "symbol": symbol,
        "measure_code": "EPS_DILUTED",
        "fiscal_year": 2024,
        "fiscal_period": "Q1",
        "period_end": dt.date(2024, 3, 31),
        "consensus_date": date,
        "mean": mean,
        "num_up": num_up,
        "num_down": num_down,
        "num_estimates": num_estimates,
        "as_of_date": date,
        "available_at": pd.Timestamp(available_at),
        "source": "fixture_est_consensus",
    }


def _estimate_surprises_fixture() -> pd.DataFrame:
    return pd.DataFrame(
        [
            _surprise("SEC-A", "AAA", sue=2.0, surprise_pct=0.20, actual=1.20, consensus_mean=1.00),
            _surprise("SEC-B", "BBB", sue=0.5, surprise_pct=0.05, actual=1.05, consensus_mean=1.00),
            _surprise("SEC-C", "CCC", sue=-1.0, surprise_pct=-0.10, actual=0.90, consensus_mean=1.00),
        ]
    )


def _estimate_consensus_fixture() -> pd.DataFrame:
    rows = []
    for security_id, symbol, latest_mean, num_up, num_down, num_estimates in (
        ("SEC-A", "AAA", 1.20, 5, 1, 6),
        ("SEC-B", "BBB", 1.00, 2, 2, 4),
        ("SEC-C", "CCC", 0.80, 1, 5, 6),
    ):
        rows.append(
            _consensus(
                security_id,
                symbol,
                row_id=f"{security_id}-prior",
                consensus_date="2024-03-15",
                mean=1.00,
                num_up=None,
                num_down=None,
                num_estimates=None,
                available_at="2024-03-15 13:00:00",
            )
        )
        rows.append(
            _consensus(
                security_id,
                symbol,
                row_id=f"{security_id}-latest",
                consensus_date="2024-04-15",
                mean=latest_mean,
                num_up=num_up,
                num_down=num_down,
                num_estimates=num_estimates,
                available_at="2024-04-15 13:00:00",
            )
        )
    return pd.DataFrame(rows)


def test_estimate_13f_definitions_validate_for_source_surfaces() -> None:
    definitions = estimate_13f_factor_definitions()
    frame = estimate_13f_definition_frame()

    validate_catalog(
        definitions,
        known_source_ids=("est_surprise", "est_consensus", "thirteenf_concentration_metrics"),
    )

    assert len(definitions) == len(ESTIMATE_13F_SPECS) == 9
    assert {"estimate_sue", "estimate_consensus_revision_mean_pct", "thirteenf_value_hhi"} <= set(frame["factor_id"])
    assert json.loads(frame.loc[frame["factor_id"] == "estimate_sue", "input_ids_json"].iloc[0]) == [
        "source:est_surprise"
    ]
    assert json.loads(frame.loc[frame["factor_id"] == "thirteenf_value_hhi", "standardization_spec_json"].iloc[0])[
        "source_table"
    ] == "thirteenf_concentration_metrics"


def test_estimate_revision_mapper_emits_surprise_and_consensus_revision_rows() -> None:
    rows = compute_estimate_revision_factor_rows(
        surprises=_estimate_surprises_fixture(),
        consensus=_estimate_consensus_fixture(),
        run_id="s9-est-run",
    )
    lookup = {(row.factor_id, row.security_id): row for row in rows.itertuples(index=False)}
    sue = lookup[("estimate_sue", "SEC-A")]
    revision = lookup[("estimate_consensus_revision_mean_pct", "SEC-A")]
    breadth = lookup[("estimate_consensus_revision_breadth", "SEC-A")]

    assert len(rows) == 12
    assert sue.raw_value == pytest.approx(2.0)
    assert sue.value == pytest.approx(1.0)
    assert lookup[("estimate_sue", "SEC-B")].value == pytest.approx(0.5)
    assert revision.raw_value == pytest.approx(0.20)
    assert revision.value == pytest.approx(1.0)
    assert breadth.raw_value == pytest.approx((5 - 1) / 6)

    revision_lineage = json.loads(revision.input_lineage_json)[0]
    assert revision_lineage["source_table"] == "est_consensus"
    assert revision_lineage["source_row_id"] == "SEC-A-latest"
    assert revision_lineage["mean"] == 1.2
    assert revision_lineage["prior_mean"] == 1.0
    assert revision_lineage["consensus_date"] == "2024-04-15"

    before = compute_estimate_revision_factor_rows(
        surprises=_estimate_surprises_fixture(),
        consensus=_estimate_consensus_fixture(),
        as_of_date=dt.date(2024, 4, 10),
    )
    assert before.empty


def _thirteenf_metric(
    security_id: str,
    symbol: str,
    *,
    value_hhi: float,
    top_10_holder_value_pct: float,
    effective_holder_count_value: float,
    value_hhi_change: float,
    holder_count_change: int,
) -> dict[str, object]:
    return {
        "metric_id": f"13f-{security_id}",
        "security_id": security_id,
        "symbol": symbol,
        "cusip": f"CUSIP-{security_id[-1]}",
        "report_period": dt.date(2024, 3, 31),
        "as_of_date": dt.date(2024, 3, 31),
        "filing_date": dt.date(2024, 5, 15),
        "filing_count": 6,
        "holder_count": 20 + holder_count_change,
        "value_hhi": value_hhi,
        "top_10_holder_value_pct": top_10_holder_value_pct,
        "effective_holder_count_value": effective_holder_count_value,
        "prior_report_period": dt.date(2023, 12, 31),
        "prior_value_hhi": value_hhi - value_hhi_change,
        "value_hhi_change": value_hhi_change,
        "prior_holder_count": 20,
        "holder_count_change": holder_count_change,
        "available_at": pd.Timestamp("2024-05-15 22:00:00"),
        "is_latest_revision": True,
        "source": "fixture_thirteenf_concentration",
    }


def _thirteenf_fixture() -> pd.DataFrame:
    return pd.DataFrame(
        [
            _thirteenf_metric(
                "SEC-A",
                "AAA",
                value_hhi=0.10,
                top_10_holder_value_pct=0.30,
                effective_holder_count_value=10.0,
                value_hhi_change=-0.02,
                holder_count_change=5,
            ),
            _thirteenf_metric(
                "SEC-B",
                "BBB",
                value_hhi=0.20,
                top_10_holder_value_pct=0.50,
                effective_holder_count_value=5.0,
                value_hhi_change=0.00,
                holder_count_change=0,
            ),
            _thirteenf_metric(
                "SEC-C",
                "CCC",
                value_hhi=0.30,
                top_10_holder_value_pct=0.70,
                effective_holder_count_value=3.33,
                value_hhi_change=0.04,
                holder_count_change=-3,
            ),
        ]
    )


def test_thirteenf_flow_mapper_respects_filing_lag_and_ranks_crowding_direction() -> None:
    before = compute_thirteenf_flow_factor_rows(_thirteenf_fixture(), as_of_date=dt.date(2024, 5, 14))
    after = compute_thirteenf_flow_factor_rows(
        _thirteenf_fixture(),
        as_of_date=dt.date(2024, 5, 16),
        run_id="s9-13f-run",
    )
    lookup = {(row.factor_id, row.security_id): row for row in after.itertuples(index=False)}
    hhi = lookup[("thirteenf_value_hhi", "SEC-A")]
    effective_count = lookup[("thirteenf_effective_holder_count_value", "SEC-A")]

    assert before.empty
    assert len(after) == 3 * 5
    assert hhi.raw_value == pytest.approx(0.10)
    assert hhi.value == pytest.approx(1.0)
    assert lookup[("thirteenf_value_hhi", "SEC-C")].value == pytest.approx(0.0)
    assert effective_count.value == pytest.approx(1.0)

    lineage = json.loads(hhi.input_lineage_json)[0]
    assert lineage["source_table"] == "thirteenf_concentration_metrics"
    assert lineage["source_row_id"] == "13f-SEC-A"
    assert lineage["filing_date"] == "2024-05-15"
    assert lineage["holder_count"] == 25


def test_estimate_13f_seed_rows_round_trip_into_catalog(tmp_store) -> None:
    estimate_seeded = tmp_store.con.execute(
        """
        SELECT family, direction, standardization_spec_json
        FROM factor_definition
        WHERE factor_id = 'estimate_sue'
        """
    ).fetchone()
    thirteenf_seeded = tmp_store.con.execute(
        """
        SELECT family, direction, standardization_spec_json
        FROM factor_definition
        WHERE factor_id = 'thirteenf_value_hhi'
        """
    ).fetchone()
    edge_sources = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT dependency_source_id
            FROM factor_dependency_edges
            WHERE factor_id IN ('estimate_sue', 'estimate_consensus_revision_mean_pct', 'thirteenf_value_hhi')
            """
        ).fetchall()
    }
    catalog_domains = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT DISTINCT domain
            FROM v_cross_domain_estimate_13f_factor_catalog
            """
        ).fetchall()
    }
    dataset_row = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM dataset_catalog
        WHERE dataset_id = 'cross_domain_estimate_13f_factors'
        """
    ).fetchone()[0]

    assert estimate_seeded[0:2] == ("estimate_surprise", 1)
    assert json.loads(estimate_seeded[2])["source_table"] == "est_surprise"
    assert thirteenf_seeded[0:2] == ("thirteenf_crowding", -1)
    assert json.loads(thirteenf_seeded[2])["source_table"] == "thirteenf_concentration_metrics"
    assert edge_sources == {"est_surprise", "est_consensus", "thirteenf_concentration_metrics"}
    assert catalog_domains == {"estimate_revision", "13f_flow"}
    assert dataset_row == 1
