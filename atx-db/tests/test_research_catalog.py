"""R1a research anomaly catalog: coverage, engine consistency and loader rejections."""

from __future__ import annotations

import csv
import re
from dataclasses import replace
from pathlib import Path

import pytest

from atx_db.derived_registry import DerivedMetricDefinition, default_derived_definitions
from atx_db.research import ANOMALY_CLASSES, CONTROL_CLASSES, default_anomaly_catalog
from atx_db.research.catalog import (
    ANOMALY_CATALOG_COLUMNS,
    ANOMALY_CATALOG_PATH,
    CLOCK_BAR,
    CLOCK_FILING,
    CLOCK_MAX,
    EXCLUDED_SEED_METRICS,
    AnomalyCatalogError,
    anomaly_class_counts,
    derive_metric_shapes,
    load_anomaly_catalog,
    read_anomaly_catalog,
    render_anomaly_catalog_markdown,
    validate_anomaly_catalog,
)

DOC_PATH = Path(__file__).resolve().parents[1] / "docs" / "research" / "ANOMALY_CATALOG.md"

#: Values the engine labels value_origin='incomparable' for every quarterly row.
BLOCKED = {
    "eps_diluted_q_growth_qoq": "one_quarter_pair_per_share_without_split_guard",
    "eps_basic_q_growth_qoq": "one_quarter_pair_per_share_without_split_guard",
    "eps_diluted_growth_qoq": "one_quarter_pair_per_share_without_split_guard",
    "revenue_growth_qoq": "one_quarter_pair_without_provable_quarter_spans",
    # Twelve trailing-twelve-month growth rates: 365-day spans never form a
    # single-quarter chain (R1c _consecutive_window).
    "earnings_variability": "stdev_q_over_trailing_spans",
    # Split-sensitive comparisons other than flows exactly four quarters apart
    # stay incomparable until a split guard exists (R1c fix, pending R1d).
    "shares_growth_yoy": "share_basis_balance_pair_without_split_guard",
    "share_issuance_3y": "multi_year_share_basis_pair_without_split_guard",
    "eps_cagr_3y": "multi_year_share_basis_pair_without_split_guard",
    "piotroski_f": "incomparable_input:shares_growth_yoy",
}


def _by_id() -> dict[str, object]:
    return {entry.feature_id: entry for entry in default_anomaly_catalog()}


def test_every_seed_metric_has_exactly_one_reviewed_disposition():
    entries = default_anomaly_catalog()
    seed = {definition.metric_code: definition for definition in default_derived_definitions()}
    cataloged = [entry.metric_code for entry in entries if entry.source_kind == "seed_metric"]

    assert len(cataloged) == len(set(cataloged))
    assert set(cataloged).isdisjoint(EXCLUDED_SEED_METRICS)
    assert set(cataloged) | set(EXCLUDED_SEED_METRICS) == set(seed)
    for entry in entries:
        if entry.source_kind == "seed_metric":
            assert (entry.metric_code, entry.metric_window) == (
                seed[entry.metric_code].metric_code, seed[entry.metric_code].window)


def test_every_row_is_a_signed_referenced_hypothesis_and_all_classes_are_present():
    entries = default_anomaly_catalog()
    counts = anomaly_class_counts(entries)

    assert len({entry.feature_id for entry in entries}) == len(entries)
    assert all(entry.expected_sign in (-1, 1) for entry in entries)
    assert all(entry.reference and entry.sign_rationale for entry in entries)
    assert all(counts[name] >= 1 for name in ANOMALY_CLASSES + CONTROL_CLASSES), counts
    assert {entry.anomaly_class for entry in entries if entry.is_control} <= set(CONTROL_CLASSES)


def test_compositions_are_market_scaled_and_resolve_to_seed_operands():
    compositions = [entry for entry in default_anomaly_catalog() if entry.source_kind == "composition"]
    seed = {definition.metric_code: definition for definition in default_derived_definitions()}

    assert {entry.feature_id for entry in compositions} >= {
        "sales_to_price", "cfo_to_price", "ebitda_to_ev", "debt_to_market", "assets_to_market"}
    for entry in compositions:
        assert entry.feature_id not in seed
        assert entry.availability_clock == CLOCK_MAX
        metrics = [operand.split(":", 1)[1] for operand in entry.operands if operand.startswith("metric:")]
        assert any(seed[code].window == "daily" for code in metrics), entry.feature_id


def test_incomparable_by_construction_metrics_are_blocked_and_only_those():
    entries = default_anomaly_catalog()
    shapes = derive_metric_shapes()
    blocked = {entry.feature_id for entry in entries if not entry.is_research_eligible}

    assert blocked == set(BLOCKED)
    for code, reason in BLOCKED.items():
        assert shapes[code].incomparable_reason == reason
    # A4 ruling: per-share QoQ stays incomparable until a split guard exists, while
    # accelerations of basis-free growth can be labeled quarterly and stay testable.
    by_id = _by_id()
    for code in ("eps_diluted_q_growth_yoy_accel", "revenue_q_growth_yoy_accel", "revenue_q_growth_qoq"):
        assert by_id[code].is_research_eligible
        assert shapes[code].incomparable_reason is None


@pytest.mark.parametrize(
    ("feature_id", "quarters", "sessions", "clock"),
    [
        ("revenue_growth_yoy", 8, 0, CLOCK_FILING),  # a TTM sum (4) against the TTM 4 quarters back
        ("roa", 5, 0, CLOCK_FILING),  # average assets: current and 4 quarters back
        ("eps_diluted_q_growth_yoy_accel", 6, 0, CLOCK_FILING),  # 5-quarter yoy and the prior one
        ("revenue_cagr_3y", 16, 0, CLOCK_FILING),
        ("earnings_variability", 19, 0, CLOCK_FILING),  # 12 values of an 8-quarter growth series
        ("piotroski_f", 9, 0, CLOCK_FILING),  # change in ROA on average assets
        ("momentum_12_1", 0, 253, CLOCK_BAR),
        ("total_return_1m", 0, 22, CLOCK_BAR),
        ("realized_vol_60d", 0, 61, CLOCK_BAR),  # 60 log returns need 61 closes
        ("dollar_volume_20d", 0, 20, CLOCK_BAR),
        ("market_cap", 0, 1, CLOCK_MAX),  # DEI share counts carry the filing clock
        ("earnings_yield", 4, 1, CLOCK_MAX),
        ("debt_to_market", 1, 1, CLOCK_MAX),
        ("assets_to_market", 1, 1, CLOCK_MAX),
    ],
)
def test_history_and_clock_are_inherited_from_the_definition(feature_id, quarters, sessions, clock):
    entry = _by_id()[feature_id]

    assert (entry.min_history_quarters, entry.min_history_sessions) == (quarters, sessions)
    assert entry.availability_clock == clock


def _probe(code: str, expression: str, inputs: tuple[str, ...]) -> DerivedMetricDefinition:
    return DerivedMetricDefinition(code, "growth", expression, "q", inputs, False, "probe", "1")


def test_engine_mirror_follows_the_r1c_quarter_chain_proofs():
    seed = default_derived_definitions()
    probes = (
        # Provable since R1c: a stdev_q chain of basis-free single quarters, a flow
        # over a one-quarter-lagged instant, and instant after instant.
        _probe("sue_probe",
               "safe_div(net_income_total - lag(net_income_total, 4), stdev_q(net_income_q_growth_yoy, 8))",
               ("item:net_income_total", "metric:net_income_q_growth_yoy")),
        _probe("roe_q_lag1_probe", "safe_div(net_income_total, lag(common_equity_q, 1))",
               ("item:net_income_total", "metric:common_equity_q")),
        _probe("roe_q_lag4_probe", "safe_div(net_income_total, lag(common_equity_q, 4))",
               ("item:net_income_total", "metric:common_equity_q")),
        _probe("sue_child_probe", "abs(sue_probe)", ("metric:sue_probe",)),
        _probe("asset_qoq_probe", "total_assets - lag(total_assets, 1)", ("item:total_assets",)),
        # Share flows exactly four quarters apart; per-share x shares is basis-free.
        _probe("wavg_yoy_probe", "yoy(weighted_avg_shares_basic)", ("item:weighted_avg_shares_basic",)),
        _probe("earnings_qoq_probe",
               "eps_diluted * weighted_avg_shares_diluted - lag(eps_diluted * weighted_avg_shares_diluted, 1)",
               ("item:eps_diluted", "item:weighted_avg_shares_diluted")),
        # Never provable: trailing spans in a stdev_q chain; a split-sensitive chain,
        # one-quarter pair, balance pair or multi-year pair; an instant after a flow.
        _probe("ttm_vol_probe", "stdev_q(revenue_growth_yoy, 8)", ("metric:revenue_growth_yoy",)),
        _probe("eps_vol_probe", "stdev_q(eps_diluted, 8)", ("item:eps_diluted",)),
        _probe("share_qoq_probe", "weighted_avg_shares_basic - lag(weighted_avg_shares_basic, 1)",
               ("item:weighted_avg_shares_basic",)),
        _probe("share_balance_yoy_probe", "yoy(shares_outstanding_period_end)",
               ("item:shares_outstanding_period_end",)),
        _probe("wavg_2y_probe", "weighted_avg_shares_basic - lag(weighted_avg_shares_basic, 8)",
               ("item:weighted_avg_shares_basic",)),
        _probe("instant_after_flow_probe", "safe_div(total_assets, lag(net_income_total, 1))",
               ("item:total_assets", "item:net_income_total")),
    )
    shapes = derive_metric_shapes(seed + probes)

    comparable = ("sue_probe", "roe_q_lag1_probe", "roe_q_lag4_probe", "sue_child_probe", "asset_qoq_probe",
                  "wavg_yoy_probe", "earnings_qoq_probe")
    assert {code: shapes[code].incomparable_reason for code in comparable} == dict.fromkeys(comparable)
    assert shapes["sue_probe"].min_history_quarters == 12  # 5-quarter yoy series, 8 of them
    assert shapes["roe_q_lag1_probe"].min_history_quarters == 2
    assert shapes["ttm_vol_probe"].incomparable_reason == "stdev_q_over_trailing_spans"
    assert shapes["eps_vol_probe"].incomparable_reason == "stdev_q_over_share_basis_without_split_guard"
    assert shapes["share_qoq_probe"].incomparable_reason == "one_quarter_pair_per_share_without_split_guard"
    assert shapes["share_balance_yoy_probe"].incomparable_reason == "share_basis_balance_pair_without_split_guard"
    assert shapes["wavg_2y_probe"].incomparable_reason == "multi_year_share_basis_pair_without_split_guard"
    assert shapes["instant_after_flow_probe"].incomparable_reason == (
        "one_quarter_pair_without_provable_quarter_spans")

    template = _by_id()["revenue_q_growth_yoy"]
    extra = tuple(
        replace(template, feature_id=probe.metric_code, metric_code=probe.metric_code, supersedes=(),
                min_history_quarters=shapes[probe.metric_code].min_history_quarters)
        for probe in probes
    )
    with pytest.raises(AnomalyCatalogError) as caught:
        validate_anomaly_catalog(default_anomaly_catalog() + extra, definitions=seed + probes)
    message = str(caught.value)
    for code in ("ttm_vol_probe", "eps_vol_probe", "share_qoq_probe", "share_balance_yoy_probe", "wavg_2y_probe",
                 "instant_after_flow_probe"):
        assert f"feature {code!r}: every quarterly value is labeled incomparable" in message
    for code in comparable:
        assert f"feature {code!r}: every quarterly value" not in message


def _rows() -> list[dict[str, str]]:
    with ANOMALY_CATALOG_PATH.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def _write(tmp_path: Path, rows: list[dict[str, str]]) -> Path:
    path = tmp_path / "research_anomaly_catalog.csv"
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=ANOMALY_CATALOG_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
    return path


def _mutated(tmp_path: Path, target_id: str, changes: dict[str, str]) -> Path:
    rows = _rows()
    target = [row for row in rows if row["feature_id"] == target_id]
    assert len(target) == 1
    target[0].update(changes)
    return _write(tmp_path, rows)


@pytest.mark.parametrize(
    ("feature_id", "changes", "fragment"),
    [
        ("roa", {"feature_id": "roa_x", "metric_code": "roa_x"}, "unknown seed metric_code 'roa_x'"),
        ("roa", {"metric_window": "q"}, "metric_window 'q' but the seed window of 'roa' is 'ttm'"),
        ("sales_to_price", {"numerator": "metric:revenue_ttm_x"}, "unknown seed metric operand"),
        ("assets_to_market", {"numerator": "item:total_assets_x"}, "unknown fundamental item operand"),
        ("sales_to_price", {"denominator": "metric:total_debt_q"}, "must be market-scaled"),
        ("sales_to_price", {"feature_id": "gross_margin"}, "collides with a seed metric"),
        ("roa", {"reference": ""}, "reference must be non-empty"),
        ("roa", {"anomaly_class": "alpha"}, "unknown anomaly_class 'alpha'"),
        ("roa", {"supersedes": "factor:no_such_legacy_factor"}, "not a known legacy factor id"),
        ("roe", {"supersedes": "factor:profitability_roa"}, "already superseded by 'roa'"),
        ("roa", {"availability_clock": CLOCK_BAR}, "the inherited clock is 'conservative_filing_46h'"),
        ("momentum_12_1", {"min_history_sessions": "252"}, "needs (0q, 253s)"),
        ("eps_basic_q_growth_qoq", {"admission": "eligible", "admission_note": ""},
         "admission must be blocked_incomparable_origin"),
        ("revenue_q_growth_qoq", {"admission": "blocked_incomparable_origin"},
         "the engine can label it comparable"),
        ("market_cap", {"preferred_transform": "winsor_z"}, "log_winsor_z is required"),
        ("roa", {"admission": "eligible_with_caveat"}, "admission_note is required"),
    ],
)
def test_loader_rejects_unknown_codes_and_engine_disagreements(tmp_path, feature_id, changes, fragment):
    path = _mutated(tmp_path, feature_id, changes)

    with pytest.raises(AnomalyCatalogError, match=re.escape(fragment)):
        load_anomaly_catalog(path)


def test_loader_rejects_a_dropped_metric_duplicates_and_unsigned_rows(tmp_path):
    rows = _rows()
    dropped = [row for row in rows if row["feature_id"] != "gross_profitability"]
    with pytest.raises(AnomalyCatalogError, match="'gross_profitability' has no catalog row and no exclusion"):
        load_anomaly_catalog(_write(tmp_path, dropped))

    with pytest.raises(AnomalyCatalogError, match="duplicate feature_id"):
        load_anomaly_catalog(_write(tmp_path, [*rows, rows[0]]))

    with pytest.raises(AnomalyCatalogError, match="expected_sign must be"):
        read_anomaly_catalog(_mutated(tmp_path, "roa", {"expected_sign": "0"}))


def test_exclusions_must_name_seed_metrics_and_cataloged_targets():
    entries = default_anomaly_catalog()
    broken = {
        **EXCLUDED_SEED_METRICS,
        "pe_ttm": "inverse_cataloged:no_such_feature",
        "not_a_metric": "per_share_level",
    }

    with pytest.raises(AnomalyCatalogError) as caught:
        validate_anomaly_catalog(entries, exclusions=broken)
    assert "exclusion 'pe_ttm' has an invalid reason" in str(caught.value)
    assert "exclusion 'not_a_metric' is not a seed metric" in str(caught.value)


def test_anomaly_catalog_doc_is_generated_from_the_catalog():
    assert DOC_PATH.read_text(encoding="utf-8") == render_anomaly_catalog_markdown()
