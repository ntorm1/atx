from __future__ import annotations

import json
import datetime as dt
import os
import subprocess
import sys
from dataclasses import replace
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from atx_db.factors.catalog import (
    CatalogValidationError,
    FactorDefinition,
    factor_definitions_frame,
    legacy_factor_definitions,
    validate_catalog,
)
from atx_db.factors.cross_domain import cross_domain_factor_definitions
from atx_db.factors.fundamental_families import factor_seed_definitions
from atx_db.factors.engine import (
    FactorGraphError,
    compute_factor_rows,
    factor_build_manifest_frame,
    factor_dependency_edges_frame,
    topological_factor_order,
)
from atx_db.factors.cross_section import neutralize, pit_safety_report
from atx_db.factors.cross_section import rank as cs_rank
from atx_db.factors.cross_section import winsorize, zscore


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
    # factor_definition is seeded from three authoritative sources, matching the
    # migrations that populate it: legacy S7-0 rows (migration 0152), the S8
    # definition-as-data seed CSV (migrations 0156-0159), and the S9 cross-domain
    # specs (migrations 0160-0163).
    expected_count = (
        len(legacy_factor_definitions())
        + len(factor_seed_definitions())
        + len(cross_domain_factor_definitions())
    )
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


def test_compute_factor_rows_selects_one_latest_revision_per_key() -> None:
    rows = (
        _factor("base_a"),
        _factor("base_b"),
        _factor("spread", "base_a - base_b", ("factor:base_a", "factor:base_b")),
    )

    # For each dependency, the chronologically-latest revision (V_new) is placed
    # BEFORE the stale revision (V_old) in the input frame. A buggy
    # `pivot_table(aggfunc="last")` selects whichever row is positionally last in
    # the frame -- V_old -- even though `available_at` (computed via `.max()`)
    # correctly reflects V_new's later timestamp. The fix must select V_new's
    # value (and V_new's available_at) for both dependencies, regardless of row
    # order.
    base_rows = [
        {
            "factor_id": "base_a",
            "security_id": "SEC-A",
            "symbol": "AAA",
            "as_of_date": dt.date(2023, 1, 3),
            "value": 10.0,  # V_new
            "available_at": pd.Timestamp("2023-01-03 09:30:00"),
        },
        {
            "factor_id": "base_a",
            "security_id": "SEC-A",
            "symbol": "AAA",
            "as_of_date": dt.date(2023, 1, 3),
            "value": 999.0,  # V_old (stale), positioned after V_new
            "available_at": pd.Timestamp("2023-01-03 09:00:00"),
        },
        {
            "factor_id": "base_b",
            "security_id": "SEC-A",
            "symbol": "AAA",
            "as_of_date": dt.date(2023, 1, 3),
            "value": 3.0,  # V_new
            "available_at": pd.Timestamp("2023-01-03 10:00:00"),
        },
        {
            "factor_id": "base_b",
            "security_id": "SEC-A",
            "symbol": "AAA",
            "as_of_date": dt.date(2023, 1, 3),
            "value": 888.0,  # V_old (stale), positioned after V_new
            "available_at": pd.Timestamp("2023-01-03 09:15:00"),
        },
    ]

    input_values = pd.DataFrame(base_rows)
    shuffled_values = pd.DataFrame(list(reversed(base_rows))).reset_index(drop=True)

    result = compute_factor_rows(input_values, rows, target_factor_ids=("spread",), run_id="revision-run")
    shuffled_result = compute_factor_rows(shuffled_values, rows, target_factor_ids=("spread",), run_id="revision-run")

    assert len(result.frame) == 1
    row = result.frame.iloc[0]
    assert row["value"] == pytest.approx(7.0)  # 10.0 (base_a V_new) - 3.0 (base_b V_new)
    assert row["available_at"] == pd.Timestamp("2023-01-03 10:00:00")

    pd.testing.assert_frame_equal(
        result.frame.reset_index(drop=True),
        shuffled_result.frame.reset_index(drop=True),
    )


_MANIFEST_ID_PROBE_SOURCE = '''
import datetime as dt
import json
import sys

import pandas as pd

from atx_db.factors.catalog import FactorDefinition
from atx_db.factors.engine import compute_factor_rows, factor_build_manifest_frame


def _factor(factor_id, expression="source", inputs=()):
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


rows = (
    _factor("base_a"),
    _factor("base_b"),
    _factor("combo_sum", "base_a + base_b", ("factor:base_a", "factor:base_b")),
    _factor("combo_diff", "base_a - base_b", ("factor:base_a", "factor:base_b")),
    _factor("combo_prod", "base_a * base_b", ("factor:base_a", "factor:base_b")),
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
targets = tuple(sys.argv[1:])
result = compute_factor_rows(input_values, rows, target_factor_ids=targets, run_id="factor-run")
manifest = factor_build_manifest_frame(result)
print(manifest.iloc[0]["manifest_id"])
'''


def test_factor_build_manifest_id_is_hashseed_independent(tmp_path) -> None:
    # A multi-target (>=3 factor ids) build's manifest_id must not depend on
    # PYTHONHASHSEED or on the iteration order of `target_factor_ids` -- both of
    # which perturb a plain `set` build. Force distinct interpreter hash seeds via
    # subprocesses so the assertion is deterministic regardless of the ambient
    # hash seed this pytest process happens to run under.
    repo_root = Path(__file__).resolve().parents[2]
    script = tmp_path / "manifest_id_probe.py"
    script.write_text(_MANIFEST_ID_PROBE_SOURCE, encoding="utf-8")

    def manifest_id(order: tuple[str, ...], hashseed: str) -> str:
        env = dict(os.environ, PYTHONHASHSEED=hashseed, PYTHONPATH=str(repo_root))
        completed = subprocess.run(
            [sys.executable, str(script), *order],
            cwd=repo_root,
            env=env,
            capture_output=True,
            text=True,
            check=True,
        )
        return completed.stdout.strip()

    targets = ("combo_sum", "combo_diff", "combo_prod")
    permuted_targets = ("combo_prod", "combo_sum", "combo_diff")

    manifest_seed0 = manifest_id(targets, "0")
    manifest_seed0_again = manifest_id(targets, "0")
    manifest_seed1 = manifest_id(targets, "1")
    manifest_seed0_permuted = manifest_id(permuted_targets, "0")

    assert manifest_seed0 == manifest_seed0_again
    assert manifest_seed0 == manifest_seed1
    assert manifest_seed0 == manifest_seed0_permuted


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


def _cross_section_fixture() -> pd.DataFrame:
    return pd.DataFrame(
        [
            {"factor_id": "value", "security_id": "A", "as_of_date": dt.date(2023, 1, 3), "value": 1.0},
            {"factor_id": "value", "security_id": "B", "as_of_date": dt.date(2023, 1, 3), "value": 2.0},
            {"factor_id": "value", "security_id": "C", "as_of_date": dt.date(2023, 1, 3), "value": 3.0},
            {"factor_id": "value", "security_id": "A", "as_of_date": dt.date(2023, 1, 4), "value": 10.0},
            {"factor_id": "value", "security_id": "B", "as_of_date": dt.date(2023, 1, 4), "value": 20.0},
            {"factor_id": "value", "security_id": "C", "as_of_date": dt.date(2023, 1, 4), "value": 30.0},
        ]
    )


def _ordered_values(frame: pd.DataFrame) -> pd.Series:
    return frame.sort_values(["as_of_date", "security_id"])["value"].reset_index(drop=True)


def test_cross_section_operators_match_per_date_isolated_results() -> None:
    frame = _cross_section_fixture()
    operators = (cs_rank, zscore, winsorize)

    for operator in operators:
        full = operator(frame)
        isolated = pd.concat([operator(group) for _, group in frame.groupby("as_of_date")], ignore_index=True)

        pd.testing.assert_series_equal(_ordered_values(full), _ordered_values(isolated), check_names=False)


def test_winsorize_caps_declared_cross_section_percentiles() -> None:
    frame = pd.DataFrame(
        [
            {"factor_id": "value", "security_id": "A", "as_of_date": dt.date(2023, 1, 3), "value": 1.0},
            {"factor_id": "value", "security_id": "B", "as_of_date": dt.date(2023, 1, 3), "value": 2.0},
            {"factor_id": "value", "security_id": "C", "as_of_date": dt.date(2023, 1, 3), "value": 3.0},
            {"factor_id": "value", "security_id": "D", "as_of_date": dt.date(2023, 1, 3), "value": 100.0},
        ]
    )

    capped = winsorize(frame, limits=0.25).sort_values("security_id")

    assert capped.iloc[0]["value"] == pytest.approx(1.75)
    assert capped.iloc[-1]["value"] == pytest.approx(27.25)


def test_factor_operator_metadata_migration_seeds_pit_safe_operators(tmp_store) -> None:
    rows = {
        row[0]: (row[1], json.loads(row[2]), bool(row[3]))
        for row in tmp_store.con.execute(
            """
            SELECT operator_id, kind, partition_keys_json, is_point_in_time_safe
            FROM factor_operator
            ORDER BY operator_id
            """
        ).fetchall()
    }
    table_catalog_count = tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'factor_operator'"
    ).fetchone()[0]

    assert rows["rank"] == ("cross_section_rank", ["factor_id", "as_of_date"], True)
    assert rows["zscore"] == ("cross_section_zscore", ["factor_id", "as_of_date"], True)
    assert rows["winsorize"] == ("cross_section_winsorize", ["factor_id", "as_of_date"], True)
    assert table_catalog_count == 1


def test_neutralize_residualizes_within_asof_sector_groups() -> None:
    frame = pd.DataFrame(
        [
            {"factor_id": "value", "security_id": "A", "sector": "tech", "as_of_date": dt.date(2023, 1, 3), "value": 1.0},
            {"factor_id": "value", "security_id": "B", "sector": "tech", "as_of_date": dt.date(2023, 1, 3), "value": 3.0},
            {"factor_id": "value", "security_id": "C", "sector": "energy", "as_of_date": dt.date(2023, 1, 3), "value": 10.0},
            {"factor_id": "value", "security_id": "D", "sector": "energy", "as_of_date": dt.date(2023, 1, 3), "value": 14.0},
        ]
    )

    residuals = neutralize(frame, by=("sector",))
    means = residuals.groupby(["as_of_date", "sector"])["value"].mean()

    assert all(abs(value) < 1e-12 for value in means)


def test_pit_safety_report_flags_future_inputs_and_cross_date_pooling() -> None:
    raw = _cross_section_fixture()
    raw["available_at"] = pd.to_datetime(raw["as_of_date"].astype(str))
    clean = zscore(raw)
    future = raw.copy()
    future.loc[0, "available_at"] = pd.Timestamp("2023-01-04")
    pooled = raw.copy()
    pooled["value"] = (raw["value"] - raw["value"].mean()) / raw["value"].std(ddof=1)

    clean_report = pit_safety_report(raw, transformed_frame=clean, operator="zscore")
    future_report = pit_safety_report(future)
    pooled_report = pit_safety_report(raw, transformed_frame=pooled, operator="zscore")

    assert clean_report["status"] == "passed"
    assert future_report["status"] == "failed"
    assert future_report["future_input_count"] == 1
    assert pooled_report["status"] == "failed"
    assert pooled_report["operator_mismatch_count"] > 0


def test_zscore_guards_inf_and_pit_safety_covers_neutralize() -> None:
    # (a) A cross-section whose standardization overflows: three securities that
    # each report a huge-magnitude value push mean/std computation past float64
    # range. Today `_z` only guards std == 0 / NaN, so std landing on `inf`
    # (rather than 0 or NaN) slips past that guard and the division proceeds,
    # leaking a raw (non-nullable) non-finite float into the "value" column
    # instead of the `pd.NA` sentinel the std==0 branch already returns.
    overflow_frame = pd.DataFrame(
        [
            {"factor_id": "value", "security_id": "A", "as_of_date": dt.date(2023, 1, 3), "value": 1e308},
            {"factor_id": "value", "security_id": "B", "as_of_date": dt.date(2023, 1, 3), "value": 1e308},
            {"factor_id": "value", "security_id": "C", "as_of_date": dt.date(2023, 1, 3), "value": 1e308},
        ]
    )

    standardized = zscore(overflow_frame)

    assert not np.isinf(pd.to_numeric(standardized["value"], errors="coerce")).any()
    assert standardized["value"].dtype == "Float64"
    assert standardized["value"].isna().all()

    # (b) `neutralize` is a valid PIT-safe operator but is missing from the
    # `_operator_by_name` map used by `pit_safety_report`'s recompute, so the
    # report cannot validate it today.
    neutralize_frame = pd.DataFrame(
        [
            {
                "factor_id": "value",
                "security_id": "A",
                "sector": "tech",
                "as_of_date": dt.date(2023, 1, 3),
                "value": 1.0,
                "available_at": pd.Timestamp("2023-01-03"),
            },
            {
                "factor_id": "value",
                "security_id": "B",
                "sector": "tech",
                "as_of_date": dt.date(2023, 1, 3),
                "value": 3.0,
                "available_at": pd.Timestamp("2023-01-03"),
            },
            {
                "factor_id": "value",
                "security_id": "C",
                "sector": "energy",
                "as_of_date": dt.date(2023, 1, 3),
                "value": 10.0,
                "available_at": pd.Timestamp("2023-01-03"),
            },
            {
                "factor_id": "value",
                "security_id": "D",
                "sector": "energy",
                "as_of_date": dt.date(2023, 1, 3),
                "value": 14.0,
                "available_at": pd.Timestamp("2023-01-03"),
            },
        ]
    )
    neutralized = neutralize(neutralize_frame, by=("sector",))

    report = pit_safety_report(
        neutralize_frame,
        transformed_frame=neutralized,
        operator="neutralize",
        operator_kwargs={"by": ("sector",)},
    )

    assert report["status"] == "passed"


def test_factor_engine_catalog_view_and_pit_safety_gate_are_registered(tmp_store) -> None:
    vol = tmp_store.con.execute(
        """
        SELECT family, dependency_count, factor_dependency_count, source_dependency_count
        FROM v_factor_engine_catalog
        WHERE factor_id = 'vol_21d'
        """
    ).fetchone()
    gate = tmp_store.con.execute(
        """
        SELECT severity, enabled, failure_status
        FROM quality_check_registry
        WHERE check_name = 'factor_operator_pit_safety'
          AND source = 'pf3_s7'
        """
    ).fetchone()
    view_catalog = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM table_catalog
        WHERE table_name = 'v_factor_engine_catalog'
        """
    ).fetchone()[0]

    assert vol == ("volatility", 1, 1, 0)
    assert gate == ("critical", True, "failed")
    assert view_catalog == 1
