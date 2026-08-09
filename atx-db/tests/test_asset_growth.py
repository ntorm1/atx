from __future__ import annotations

import datetime as dt
import json

import pandas as pd

from atx_db.asset_growth import (
    FACTOR_ID,
    SOURCE_NAME,
    AssetGrowthOptions,
    compute_asset_growth_rows,
)


def _input_row(
    security_id: str,
    current_assets: float,
    prior_assets: float = 100.0,
) -> dict[str, object]:
    return {
        "security_id": security_id,
        "symbol": security_id,
        "trade_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "current_assets": current_assets,
        "prior_assets": prior_assets,
        "current_asset_id": f"{security_id}-assets-current",
        "prior_asset_id": f"{security_id}-assets-prior",
        "current_period_end": dt.date(2024, 12, 31),
        "prior_period_end": dt.date(2023, 12, 31),
        "current_asset_available_at": dt.datetime(2025, 1, 30, 22),
        "prior_asset_available_at": dt.datetime(2024, 1, 30, 22),
        "current_accession_number": f"{security_id}-filing-current",
        "prior_accession_number": f"{security_id}-filing-prior",
        "universe_id": "us_common_equity_liquid_v1",
        "universe_valid_from": dt.date(2025, 1, 1),
        "universe_valid_to": None,
        "universe_available_at": dt.datetime(2025, 1, 1),
        "universe_source": "test",
    }


def test_conservative_asset_growth_orientation_and_lineage() -> None:
    inputs = pd.DataFrame(
        [
            _input_row("SHRINK", 80.0),
            _input_row("FLAT", 100.0),
            _input_row("EXPAND", 150.0),
        ]
    )
    rows = compute_asset_growth_rows(
        inputs,
        AssetGrowthOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")

    assert rows.loc["SHRINK", "raw_value"] > rows.loc["FLAT", "raw_value"]
    assert rows.loc["FLAT", "raw_value"] > rows.loc["EXPAND", "raw_value"]
    lineage = json.loads(rows.loc["SHRINK", "input_lineage_json"])
    assert lineage["assets"]["asset_growth"] == -0.2
    assert lineage["assets"]["current"]["statement_point_id"] == (
        "SHRINK-assets-current"
    )
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_conservative_asset_growth_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,is_point_in_time_safe,source,standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[1:3] == (True, SOURCE_NAME)
    assert definition[0].startswith("-((total_assets_t")
    standardization = json.loads(definition[3])
    assert standardization["winsor_limits"] == [0.01, 0.01]
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [("metric", "total_assets")]
