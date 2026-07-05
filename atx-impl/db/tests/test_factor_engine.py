from __future__ import annotations

import json
import datetime as dt
from dataclasses import replace

import pandas as pd
import pytest

from db.factors.catalog import (
    CatalogValidationError,
    FactorDefinition,
    factor_definitions_frame,
    legacy_factor_definitions,
    validate_catalog,
)
from db.factors.engine import (
    FactorGraphError,
    compute_factor_rows,
    factor_build_manifest_frame,
    factor_dependency_edges_frame,
    topological_factor_order,
)


def _row(frame, factor_id: str):
    selected = frame[frame["factor_id"] == factor_id]
    assert len(selected) == 1
    return selected.iloc[0]


def _factor(
    factor_id: str,
    expression: str = "source",
    inputs: tuple[str, ...] = (),
) -> FactorDefinition:
    return FactorDefinition(
        factor_id=factor_id,
        factor_name=factor_id,
        family="fixture",
        description=f"{factor_id} fixture factor",
        expression=expression,
        input_ids_json=json.dumps(list(inputs)),
        direction=1,
        lookback_days=0,
        neutralization_spec_json=json.dumps({"method": "none", "by": []}),
        unit="score",
        sign="signed",
        scale="1",
        is_point_in_time_safe=True,
        available_at_policy="fixture",
        declared_in="test",
        owner="test",
        source="test",
    )


def test_legacy_factor_catalog_reconciles_features_fundamentals_and_alphas() -> None:
    frame = factor_definitions_frame()

    ret = _row(frame, "ret_1d")
    revenue = _row(frame, "fund_revenue_ttm")
    alpha = _row(frame, "alpha_momentum_liquidity_v1")

    assert ret["family"] == "returns"
    assert ret["declared_in"] == "features.FEATURE_DEFINITIONS"
    assert json.loads(ret["input_ids_json"]) == ["source:equity_daily_bars"]
    assert revenue["declared_in"] == "features.FUNDAMENTAL_FEATURE_DEFINITIONS"
    assert revenue["unit"] == "currency"
    assert revenue["scale"] == "1"
    assert alpha["declared_in"] == "alpha_research.DEFAULT_ALPHA_SPECS"
    assert alpha["family"] == "alpha_research"
    assert set(json.loads(alpha["input_ids_json"])) == {"factor:mom_21d", "factor:adv_21d"}
    assert frame["family"].notna().all()
    assert frame["direction"].notna().all()
    assert frame["neutralization_spec_json"].notna().all()


def test_validate_catalog_rejects_undeclared_factor_input() -> None:
    rows = legacy_factor_definitions()
    bad = replace(rows[0], input_ids_json=json.dumps(["factor:not_declared"]))

    with pytest.raises(CatalogValidationError, match="not_declared"):
        validate_catalog((bad, *rows[1:]))


def test_factor_definition_migration_seeds_catalog_rows(tmp_store) -> None:
    expected_count = len(legacy_factor_definitions())
    count = tmp_store.con.execute("SELECT count(*) FROM factor_definition").fetchone()[0]
    ret = tmp_store.con.execute(
        """
        SELECT family, unit, sign, scale, declared_in
        FROM factor_definition
        WHERE factor_id = 'ret_1d'
        """
    ).fetchone()
    alpha = tmp_store.con.execute(
        """
        SELECT family, input_ids_json, neutralization_spec_json
        FROM factor_definition
        WHERE factor_id = 'alpha_momentum_liquidity_v1'
        """
    ).fetchone()
    catalog = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM table_catalog
        WHERE table_name = 'factor_definition'
        """
    ).fetchone()[0]
    semantic_fields = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT field_name
            FROM field_catalog
            WHERE table_name = 'factor_definition'
              AND field_name IN ('unit', 'sign', 'scale')
            """
        ).fetchall()
    }

    assert count == expected_count
    assert ret == ("returns", "ratio", "signed", "1", "features.FEATURE_DEFINITIONS")
    assert alpha[0] == "alpha_research"
    assert set(json.loads(alpha[1])) == {"factor:mom_21d", "factor:adv_21d"}
    assert json.loads(alpha[2])["rank_method"] == "cross_section_percent_rank"
    assert catalog == 1
    assert semantic_fields == {"unit", "sign", "scale"}


def test_factor_dependency_graph_orders_dependencies_and_rejects_cycles() -> None:
    rows = (
        _factor("base_a"),
        _factor("base_b"),
        _factor("spread", "base_a - base_b", ("factor:base_a", "factor:base_b")),
        _factor("combo", "spread + base_a", ("factor:spread", "factor:base_a")),
    )
    edges = factor_dependency_edges_frame(rows)

    assert topological_factor_order(rows, target_factor_ids=("combo",)) == ("base_a", "base_b", "spread", "combo")
    assert set(edges[edges["factor_id"] == "spread"]["dependency_name"]) == {"base_a", "base_b"}

    cyclic = (
        replace(rows[0], input_ids_json=json.dumps(["factor:combo"])),
        *rows[1:],
    )
    with pytest.raises(FactorGraphError, match="Cyclic factor dependency"):
        topological_factor_order(cyclic, target_factor_ids=("combo",))


def test_compute_factor_rows_uses_dependency_order_and_max_input_available_at() -> None:
    rows = (
        _factor("base_a"),
        _factor("base_b"),
        _factor("spread", "base_a - base_b", ("factor:base_a", "factor:base_b")),
        _factor("combo", "spread + base_a", ("factor:spread", "factor:base_a")),
    )
    input_values = pd.DataFrame(
        [
            {
                "factor_id": "base_a",
                "security_id": "SEC-A",
                "symbol": "AAA",
                "as_of_date": dt.date(2023, 1, 3),
                "value": 10.0,
                "available_at": pd.Timestamp("2023-01-03 09:30:00"),
            },
            {
                "factor_id": "base_b",
                "security_id": "SEC-A",
                "symbol": "AAA",
                "as_of_date": dt.date(2023, 1, 3),
                "value": 3.0,
                "available_at": pd.Timestamp("2023-01-03 10:00:00"),
            },
        ]
    )

    result = compute_factor_rows(input_values, rows, target_factor_ids=("combo",), run_id="factor-run")
    manifest = factor_build_manifest_frame(result)

    assert result.topological_order == ("base_a", "base_b", "spread", "combo")
    assert len(result.frame) == 1
    assert result.frame.iloc[0]["value"] == pytest.approx(17.0)
    assert result.frame.iloc[0]["available_at"] == pd.Timestamp("2023-01-03 10:00:00")
    assert json.loads(result.frame.iloc[0]["input_lineage_json"])[0]["factor_id"] == "spread"
    assert json.loads(manifest.iloc[0]["factor_ids_json"]) == ["combo"]
    assert json.loads(manifest.iloc[0]["topological_order_json"]) == ["base_a", "base_b", "spread", "combo"]


def test_factor_dependency_migration_seeds_legacy_edges(tmp_store) -> None:
    edge_count = tmp_store.con.execute("SELECT count(*) FROM factor_dependency_edges").fetchone()[0]
    vol_edge = tmp_store.con.execute(
        """
        SELECT dependency_type, dependency_name
        FROM factor_dependency_edges
        WHERE factor_id = 'vol_21d'
        """
    ).fetchone()
    alpha_edges = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT dependency_name
            FROM factor_dependency_edges
            WHERE factor_id = 'alpha_momentum_liquidity_v1'
            """
        ).fetchall()
    }
    manifest_catalog = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM table_catalog
        WHERE table_name IN ('factor_dependency_edges', 'factor_build_manifests')
        """
    ).fetchone()[0]

    assert edge_count > 0
    assert vol_edge == ("factor", "ret_1d")
    assert alpha_edges == {"mom_21d", "adv_21d"}
    assert manifest_catalog == 2
