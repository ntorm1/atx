from __future__ import annotations

import json
from dataclasses import replace

import pytest

from db.factors.catalog import (
    CatalogValidationError,
    factor_definitions_frame,
    legacy_factor_definitions,
    validate_catalog,
)


def _row(frame, factor_id: str):
    selected = frame[frame["factor_id"] == factor_id]
    assert len(selected) == 1
    return selected.iloc[0]


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
