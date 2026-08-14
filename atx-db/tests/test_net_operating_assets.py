from __future__ import annotations

import datetime as dt
import json

import duckdb
import pandas as pd

from atx_db.migrations import bodies_0242
from atx_db.net_operating_assets import (
    FACTOR_ID,
    SOURCE_NAME,
    NetOperatingAssetsOptions,
    compute_net_operating_assets_rows,
)


def _row(security_id: str, *, liabilities: float, cash: float, st_debt: float, lt_debt: float) -> dict[str, object]:
    available_at = dt.datetime(2025, 2, 20, 22)
    row: dict[str, object] = {
        "security_id": security_id,
        "symbol": security_id,
        "trade_date": dt.date(2025, 3, 31),
        "decision_available_at": dt.datetime(2025, 3, 31, 22),
        "accession_number": f"{security_id}-current",
        "period_end": dt.date(2024, 12, 31),
        "prior_accession_number": f"{security_id}-prior",
        "prior_period_end": dt.date(2023, 12, 31),
        "prior_assets": 100.0,
        "prior_asset_id": f"{security_id}-prior-assets",
        "prior_asset_available_at": dt.datetime(2024, 2, 20, 22),
        "universe_id": "us_equity_research_v1",
        "universe_valid_from": dt.date(2025, 1, 1),
        "universe_valid_to": None,
        "universe_available_at": dt.datetime(2025, 1, 1),
        "universe_source": "test",
        "total_assets": 100.0,
        "total_liabilities": liabilities,
        "cash_st_inv": cash,
        "st_debt": st_debt,
        "lt_debt": lt_debt,
    }
    for metric in ("total_assets", "total_liabilities", "cash_st_inv", "st_debt", "lt_debt"):
        row[f"{metric}_id"] = f"{security_id}-{metric}"
        row[f"{metric}_available_at"] = available_at
    return row


def test_low_noa_orientation_and_complete_lineage() -> None:
    result = compute_net_operating_assets_rows(
        pd.DataFrame(
            [
                _row("LOW", liabilities=50.0, cash=20.0, st_debt=5.0, lt_debt=10.0),
                _row("HIGH", liabilities=40.0, cash=10.0, st_debt=10.0, lt_debt=20.0),
            ]
        ),
        NetOperatingAssetsOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")
    assert result.loc["LOW", "raw_value"] == -0.45
    assert result.loc["HIGH", "raw_value"] == -0.8
    assert result.loc["LOW", "value"] > result.loc["HIGH", "value"]
    lineage = json.loads(result.loc["LOW", "input_lineage_json"])
    assert lineage["current_statement"]["net_operating_assets"] == 45.0
    assert lineage["research_contract"]["missing_components_imputed"] is False
    assert "prioritized current-debt" in lineage["short_term_debt_policy"]


def test_missing_or_invalid_components_are_not_imputed() -> None:
    missing = _row("MISSING", liabilities=50.0, cash=20.0, st_debt=5.0, lt_debt=10.0)
    missing["st_debt"] = None
    invalid = _row("INVALID", liabilities=50.0, cash=20.0, st_debt=-1.0, lt_debt=10.0)
    result = compute_net_operating_assets_rows(
        pd.DataFrame(
            [
                _row("GOOD", liabilities=50.0, cash=20.0, st_debt=5.0, lt_debt=10.0),
                _row("GOOD2", liabilities=40.0, cash=10.0, st_debt=10.0, lt_debt=20.0),
                missing,
                invalid,
            ]
        ),
        NetOperatingAssetsOptions(minimum_names_per_date=2),
    )
    assert set(result["security_id"]) == {"GOOD", "GOOD2"}


def test_net_operating_assets_definition_is_governed(monkeypatch) -> None:
    conn = duckdb.connect(":memory:")
    conn.execute(
        """
        CREATE TABLE factor_definition (
            factor_id VARCHAR PRIMARY KEY,factor_name VARCHAR,family VARCHAR,
            description VARCHAR,expression VARCHAR,input_ids_json VARCHAR,
            direction INTEGER,lookback_days INTEGER,neutralization_spec_json VARCHAR,
            unit VARCHAR,sign VARCHAR,scale VARCHAR,is_point_in_time_safe BOOLEAN,
            available_at_policy VARCHAR,declared_in VARCHAR,owner VARCHAR,source VARCHAR,
            standardization_spec_json VARCHAR,valid_from DATE,valid_to DATE
        );
        CREATE TABLE factor_dependency_edges (
            dependency_id VARCHAR PRIMARY KEY,factor_id VARCHAR,dependency_type VARCHAR,
            dependency_name VARCHAR,dependency_factor_id VARCHAR,dependency_metric_id VARCHAR,
            dependency_source_id VARCHAR,dependency_depth INTEGER,expression VARCHAR,
            lookback_days INTEGER,is_direct BOOLEAN,source VARCHAR
        );
        """
    )
    monkeypatch.setattr(bodies_0242, "_refresh_schema_contract_v2_pin", lambda _conn: None)
    bodies_0242.MIGRATIONS[0].up(conn)
    definition = conn.execute(
        """SELECT expression,direction,is_point_in_time_safe,source,
                  standardization_spec_json FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[0].startswith("-((total_assets-cash_st_inv-total_liabilities")
    assert definition[1:4] == (1, True, SOURCE_NAME)
    spec = json.loads(definition[4])
    assert spec["missing_components_imputed"] is False
    assert spec["return_fitted_parameters"] is False
    dependencies = conn.execute(
        """SELECT dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("cash_st_inv",),
        ("lt_debt",),
        ("st_debt",),
        ("total_assets",),
        ("total_liabilities",),
    ]
    conn.close()
