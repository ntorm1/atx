from __future__ import annotations

import datetime as dt
import json

import pandas as pd

from atx_db.continuous_financial_strength import (
    FACTOR_ID,
    HIGH_BOOK_TO_MARKET_FACTOR_ID,
    SOURCE_NAME,
    ContinuousFinancialStrengthOptions,
    compute_continuous_financial_strength_rows,
)


def _input_row(
    security_id: str,
    strength: float,
    book_percentile: float,
) -> dict[str, object]:
    available_at = dt.datetime(2025, 1, 31, 22)
    return {
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "available_at": available_at,
        "upstream_piotroski_factor_value_id": f"{security_id}-piotroski",
        "roa": -0.04 + 0.16 * strength,
        "prior_roa": 0.05,
        "cfo_to_beginning_assets": -0.02 + 0.19 * strength,
        "leverage": 0.30 - 0.20 * strength,
        "prior_leverage": 0.12,
        "current_ratio": 0.50 + 1.50 * strength,
        "prior_current_ratio": 1.60,
        "gross_margin": 0.20 + 0.35 * strength,
        "prior_gross_margin": 0.45,
        "asset_turnover": 0.80 + 0.30 * strength,
        "prior_asset_turnover": 1.00,
        "low_net_issuance": -0.05 + 0.10 * strength,
        "book_to_market_percentile": book_percentile,
        "is_high_book_to_market": book_percentile >= 0.80,
    }


def test_continuous_strength_equal_weight_ranks_and_condition() -> None:
    inputs = pd.DataFrame(
        [
            _input_row("STRONG", 1.0, 0.95),
            _input_row("MID", 0.5, 0.50),
            _input_row("WEAK", 0.0, 0.85),
        ]
    )
    rows = compute_continuous_financial_strength_rows(
        inputs,
        ContinuousFinancialStrengthOptions(
            minimum_names_per_date=2,
            minimum_high_book_to_market_names_per_date=2,
        ),
    )
    main = rows[rows["factor_id"] == FACTOR_ID].set_index("security_id")
    conditional = rows[
        rows["factor_id"] == HIGH_BOOK_TO_MARKET_FACTOR_ID
    ].set_index("security_id")

    assert main.loc["STRONG", "raw_value"] > main.loc["MID", "raw_value"]
    assert main.loc["MID", "raw_value"] > main.loc["WEAK", "raw_value"]
    assert set(conditional.index) == {"STRONG", "WEAK"}
    lineage = json.loads(main.loc["STRONG", "input_lineage_json"])
    assert len(lineage["components"]) == 9
    assert lineage["research_contract"]["return_fitted_weights"] is False
    assert lineage["components"]["delta_roa"]["raw"] > 0


def test_continuous_strength_definitions_are_governed(tmp_store) -> None:
    definitions = tmp_store.con.execute(
        """SELECT factor_id,expression,is_point_in_time_safe,source
           FROM factor_definition WHERE factor_id IN (?,?) ORDER BY factor_id""",
        [FACTOR_ID, HIGH_BOOK_TO_MARKET_FACTOR_ID],
    ).fetchall()
    assert len(definitions) == 2
    assert all(row[2:] == (True, SOURCE_NAME) for row in definitions)
    assert "mean(zrank" in next(row[1] for row in definitions if row[0] == FACTOR_ID)
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [("factor", "quality_piotroski_f_score")]
