"""R1a research anomaly catalog: coverage, engine consistency and loader rejections."""

from __future__ import annotations

import csv
import re
from collections import defaultdict
from dataclasses import replace
from pathlib import Path

import pytest

from atx_db.derived_registry import DerivedMetricDefinition, default_derived_definitions
from atx_db.research import ANOMALY_CLASSES, CONTROL_CLASSES, default_anomaly_catalog
from atx_db.research.catalog import (
    ANOMALY_CATALOG_COLUMNS,
    ANOMALY_CATALOG_PATH,
    CAVEAT_CODES,
    CLOCK_BAR,
    CLOCK_FILING,
    CLOCK_MAX,
    DOMAIN_RULES,
    EXCLUDED_SEED_METRICS,
    AnomalyCatalogError,
    anomaly_catalog_sha256,
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
    # Two trailing-twelve-month sums one quarter apart overlap by three quarters.
    "revenue_growth_qoq": "one_quarter_pair_without_provable_quarter_spans",
    "eps_diluted_growth_qoq": "one_quarter_pair_without_provable_quarter_spans",
    # Twelve trailing-twelve-month growth rates: 365-day spans never form a
    # single-quarter chain (R1c _consecutive_window).
    "earnings_variability": "stdev_q_over_trailing_spans",
}
#: Share-basis comparisons and windows: since R1d (split epochs from daily bars)
#: the engine labels them per row, comparable on one filing's clock or on a split
#: basis proven from bars, so they are admitted with the split_basis caveat.
SPLIT_GATED = {
    "eps_diluted_growth_yoy", "eps_diluted_q_growth_yoy", "eps_basic_q_growth_yoy",
    "eps_diluted_q_growth_qoq", "eps_basic_q_growth_qoq", "eps_diluted_q_growth_yoy_accel",
    "eps_cagr_3y", "shares_growth_yoy", "share_issuance_1y", "share_issuance_3y", "piotroski_f",
    # Blocked for their spans; the share basis is labeled per row as well.
    "earnings_variability", "eps_diluted_growth_qoq",
}
#: Share-basis rows whose comparability rests only on R1d's cross-filing split
#: rebasing (trailing EPS windows, balance and multi-year share pairs, per-share
#: QoQ): blocked for a known bias until the R1d split guard passes review
#: (controller ruling for eps_diluted_growth_yoy, R1a I2).
KNOWN_BIAS = {
    "eps_diluted_growth_yoy", "eps_cagr_3y", "eps_diluted_q_growth_qoq", "eps_basic_q_growth_qoq",
    "shares_growth_yoy", "share_issuance_3y", "piotroski_f",
}
#: Contested signs pre-registered two-sided (R1a I3/I5, R1b J1).
TWO_SIDED = {"revenue_growth_yoy", "debt_to_market", "assets_to_market", "sga_to_sales"}


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
    assert all(entry.expected_sign in (-1, 0, 1) for entry in entries)
    assert all(entry.reference and entry.sign_rationale for entry in entries)
    assert all(counts[name] >= 1 for name in ANOMALY_CLASSES + CONTROL_CLASSES), counts
    assert {entry.anomaly_class for entry in entries if entry.is_control} <= set(CONTROL_CLASSES)
    two_sided = {entry.feature_id for entry in entries if entry.is_two_sided}
    assert two_sided == TWO_SIDED
    for code in two_sided:
        entry = _by_id()[code]
        assert entry.prior_evidence != "published_anomaly"
        assert "mixed_evidence" in entry.caveat_codes


def test_domains_caveats_and_families_are_machine_readable():
    entries = default_anomaly_catalog()
    by_id = _by_id()

    for entry in entries:
        assert entry.domain_rule in DOMAIN_RULES, entry.feature_id
        assert set(entry.caveat_codes) <= set(CAVEAT_CODES), entry.feature_id
        assert bool(entry.caveat_codes) == (entry.admission != "eligible"), entry.feature_id
        assert entry.hypothesis_family, entry.feature_id
    # R1a I4: the published constructions' domains, enforced by the feature store (R2b).
    assert (by_id["book_to_market"].domain_rule, by_id["book_to_market"].domain_operand) == (
        "negative_book_excluded", None)
    for code in ("earnings_yield", "fcf_yield", "cfo_to_price"):
        assert by_id[code].domain == "loss_firms_separate", code
    assert by_id["dividend_yield"].domain == "zero_payer_separate"
    assert by_id["dividend_yield"].prior_evidence == "published_analogue"
    for code in ("gross_profit_to_ev", "cfo_to_ev", "ebit_to_ev", "sales_to_ev", "ebitda_to_ev"):
        assert by_id[code].domain == "positive_denominator_required:metric:enterprise_value", code
    assert by_id["roe"].domain_operand == "metric:common_equity_avg2"
    for code in ("market_cap", "dollar_volume_20d", "realized_vol_60d", "realized_vol_252d"):
        assert by_id[code].domain == "positive_value_required", code
    # R1b J1: near-duplicates and same-construct variants share one family.
    families: dict[str, set[str]] = defaultdict(set)
    for entry in entries:
        families[entry.hypothesis_family].add(entry.feature_id)
    assert {"net_income_q_growth_yoy", "sue_ni", "earnings_surprise_to_market"} <= families["earnings_surprise"]
    assert families["asset_turnover_change"] == {"asset_turnover_change_yoy", "noa_turnover_change_yoy"}
    assert families["capex_growth"] == {
        "capex_growth_yoy", "capex_q_growth_yoy", "capex_q_growth_qoq", "capex_growth_2y", "capex_growth_3y"}
    assert {"net_equity_issuance", "shares_growth_yoy", "share_issuance_1y", "share_issuance_3y",
            "buyback_yield"} <= families["equity_issuance"]
    assert {"sales_growth_less_gross_profit_growth", "gross_margin_q_change_yoy"} <= families["gross_margin_change"]


def test_presence_rule_caveat_marks_exactly_the_rows_that_can_read_an_imputed_zero():
    """R1b J4 / R1a M5: a zero read from a concept's absence is an imputation and is labeled."""
    entries = default_anomaly_catalog()
    shapes = derive_metric_shapes()
    presence = {entry.feature_id for entry in entries if "presence_rule" in entry.caveat_codes}

    # The presence rules themselves: debt, short-term debt, long-term debt, inventory
    # (change and level) and cash-flow equity issuance.
    for code in ("total_debt_q", "operating_working_capital_q", "long_term_debt_to_assets", "change_in_inventory_yoy",
                 "quick_ratio", "no_equity_issuance_ttm"):
        assert shapes[code].reads_absence, code
    # Every catalog row reading them, directly or through debt in NOA, EV or invested capital.
    assert {"debt_to_assets", "debt_to_assets_change_yoy", "long_term_debt_to_assets", "net_debt_to_book_equity",
            "debt_to_market", "working_capital_accruals", "rsst_accruals", "delta_noa", "noa_to_assets", "rnoa_q",
            "ebitda_to_ev", "gross_profit_to_ev", "roic", "quick_ratio", "investment_to_assets",
            "inventory_change_to_assets", "cash_profitability", "piotroski_f_cash_issuance"} <= presence
    # Rows on reported concepts only never carry it.
    for code in ("roa", "current_ratio", "cash_ratio", "book_to_market", "market_cap", "net_equity_issuance",
                 "dividend_yield"):
        assert code not in presence, code
        assert not shapes[code].reads_absence, code
    assert "sales_to_price" not in presence


def test_duplicates_negations_and_conflicting_priors_are_reasoned_exclusions():
    # R1a I3 / R1b J1: one hypothesis is never tested twice or with both signs.
    assert EXCLUDED_SEED_METRICS["revenue_cagr_1y"] == "duplicate_of:revenue_growth_yoy"
    assert EXCLUDED_SEED_METRICS["buyback_ratio"] == "negation_of:net_equity_issuance"
    assert EXCLUDED_SEED_METRICS["sustainable_growth"] == "conflicting_prior:book_value_growth_yoy"
    by_id = _by_id()
    for code in ("revenue_cagr_1y", "buyback_ratio", "sustainable_growth"):
        assert code not in by_id
    # I5 / m2: leverage evidence is mixed or an analogue, never asserted as settled.
    for code in ("debt_to_market", "assets_to_market", "net_debt_to_book_equity"):
        assert by_id[code].prior_evidence == "published_analogue", code


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

    by_id = _by_id()
    assert blocked == set(BLOCKED) | KNOWN_BIAS
    for code, reason in BLOCKED.items():
        assert shapes[code].incomparable_reason == reason
        assert by_id[code].admission == "blocked_incomparable_origin"
    # R1d: the share-basis rows (R1a I2's trailing EPS growth included) are labeled
    # per row by the engine and carry a split_basis caveat. Those that only R1d's
    # cross-filing rebasing makes comparable stay blocked until its split guard
    # passes review; the rest were comparable on restated comparatives before R1d.
    assert {entry.feature_id for entry in entries if "split_basis" in entry.caveat_codes} == SPLIT_GATED
    assert KNOWN_BIAS.issubset(SPLIT_GATED - set(BLOCKED))
    for code in SPLIT_GATED:
        assert shapes[code].split_gated, code
    for code in SPLIT_GATED - set(BLOCKED):
        assert shapes[code].incomparable_reason is None, code
        expected = "blocked_known_bias" if code in KNOWN_BIAS else "eligible_with_caveat"
        assert by_id[code].admission == expected, code
    assert {entry.feature_id for entry in entries if entry.admission == "blocked_known_bias"} == KNOWN_BIAS
    # Accelerations of basis-free growth are labeled quarterly and stay testable.
    for code in ("revenue_q_growth_yoy_accel", "revenue_q_growth_qoq"):
        assert by_id[code].is_research_eligible
        assert shapes[code].incomparable_reason is None
        assert not shapes[code].split_gated


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
        # Per-share x shares within one bucket is basis-free.
        _probe("earnings_qoq_probe",
               "eps_diluted * weighted_avg_shares_diluted - lag(eps_diluted * weighted_avg_shares_diluted, 1)",
               ("item:eps_diluted", "item:weighted_avg_shares_diluted")),
        # Share-basis pairs and windows (R1d): span-comparable, labeled per row by
        # one filing clock or a split basis proven from daily bars.
        _probe("wavg_yoy_probe", "yoy(weighted_avg_shares_basic)", ("item:weighted_avg_shares_basic",)),
        _probe("eps_vol_probe", "stdev_q(eps_diluted, 8)", ("item:eps_diluted",)),
        _probe("share_qoq_probe", "weighted_avg_shares_basic - lag(weighted_avg_shares_basic, 1)",
               ("item:weighted_avg_shares_basic",)),
        _probe("share_balance_yoy_probe", "yoy(shares_outstanding_period_end)",
               ("item:shares_outstanding_period_end",)),
        _probe("wavg_2y_probe", "weighted_avg_shares_basic - lag(weighted_avg_shares_basic, 8)",
               ("item:weighted_avg_shares_basic",)),
        # Never provable: trailing spans in a stdev_q chain; an instant after a flow.
        _probe("ttm_vol_probe", "stdev_q(revenue_growth_yoy, 8)", ("metric:revenue_growth_yoy",)),
        _probe("instant_after_flow_probe", "safe_div(total_assets, lag(net_income_total, 1))",
               ("item:total_assets", "item:net_income_total")),
        # Two balances one quarter apart span that quarter (_combine), so against a
        # trailing span of the same bucket the starts differ for every row.
        _probe("balance_quarter_vs_ttm_probe", "revenue_ttm - (total_assets - lag(total_assets, 1))",
               ("metric:revenue_ttm", "item:total_assets")),
        # A constant coalesce branch (a presence-rule zero) has no dates: not "never".
        _probe("constant_branch_probe", "safe_div(revenue_ttm, coalesce(revenue * 0, 1))",
               ("metric:revenue_ttm", "item:revenue")),
        # A change guarded by a five-quarter presence chain (the idiom of the
        # missing-is-not-zero rules): comparable, five quarters of history.
        _probe("presence_chain_probe",
               "pp_and_e_gross - lag(pp_and_e_gross, 4) + 0 * stdev_q(pp_and_e_gross, 5)",
               ("item:pp_and_e_gross",)),
    )
    shapes = derive_metric_shapes(seed + probes)

    gated = ("wavg_yoy_probe", "eps_vol_probe", "share_qoq_probe", "share_balance_yoy_probe", "wavg_2y_probe")
    comparable = ("sue_probe", "roe_q_lag1_probe", "roe_q_lag4_probe", "sue_child_probe", "asset_qoq_probe",
                  "earnings_qoq_probe", "constant_branch_probe", "presence_chain_probe", *gated)
    assert {code: shapes[code].incomparable_reason for code in comparable} == dict.fromkeys(comparable)
    assert {code for code in comparable if shapes[code].split_gated} == set(gated)
    assert shapes["sue_probe"].min_history_quarters == 12  # 5-quarter yoy series, 8 of them
    assert shapes["roe_q_lag1_probe"].min_history_quarters == 2
    assert shapes["ttm_vol_probe"].incomparable_reason == "stdev_q_over_trailing_spans"
    assert shapes["instant_after_flow_probe"].incomparable_reason == (
        "one_quarter_pair_without_provable_quarter_spans")
    assert shapes["balance_quarter_vs_ttm_probe"].incomparable_reason == "quarter_vs_trailing_span_same_bucket"
    assert shapes["presence_chain_probe"].min_history_quarters == 5
    assert shapes["presence_chain_probe"].reads_absence and shapes["constant_branch_probe"].reads_absence
    assert not shapes["asset_qoq_probe"].reads_absence

    template = _by_id()["revenue_q_growth_yoy"]
    extra = tuple(
        replace(template, feature_id=probe.metric_code, metric_code=probe.metric_code, supersedes=(),
                min_history_quarters=shapes[probe.metric_code].min_history_quarters)
        for probe in probes
    )
    with pytest.raises(AnomalyCatalogError) as caught:
        validate_anomaly_catalog(default_anomaly_catalog() + extra, definitions=seed + probes)
    message = str(caught.value)
    for code in ("ttm_vol_probe", "instant_after_flow_probe", "balance_quarter_vs_ttm_probe"):
        assert f"feature {code!r}: every quarterly value is labeled incomparable" in message
    for code in comparable:
        assert f"feature {code!r}: every quarterly value" not in message
    # An uncaveated share-basis row is rejected for its missing split_basis caveat.
    for code in gated:
        assert f"feature {code!r}: caveat split_basis goes with a share-basis comparison" in message


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
        # R1a M2: a quoted string in a legacy module is not a factor id unless declared as one.
        ("roa", {"supersedes": "factor:revenue"}, "not a known legacy factor id"),
        ("roe", {"supersedes": "factor:profitability_roa"}, "already superseded by 'roa'"),
        ("roa", {"availability_clock": CLOCK_BAR}, "the inherited clock is 'conservative_filing_46h'"),
        ("momentum_12_1", {"min_history_sessions": "252"}, "needs (0q, 253s)"),
        ("revenue_growth_qoq", {"admission": "eligible", "admission_note": "", "caveat_code": ""},
         "admission must be blocked_incomparable_origin"),
        ("revenue_q_growth_qoq", {"admission": "blocked_incomparable_origin"},
         "the engine can label it comparable"),
        ("market_cap", {"preferred_transform": "winsor_z"}, "log_winsor_z is required"),
        ("roa", {"admission": "eligible_with_caveat"}, "admission_note is required"),
        ("roa", {"domain": "positive_only"}, "unknown domain rule 'positive_only'"),
        ("roa", {"domain": "positive_denominator_required"}, "requires an operand"),
        ("roa", {"domain": "unrestricted:metric:total_assets_avg2"}, "takes no operand"),
        ("roe", {"domain": "negative_book_excluded:metric:no_such_metric"},
         "is not a seed metric or fundamental item"),
        ("revenue_growth_yoy", {"prior_evidence": "published_anomaly"}, "cannot claim published_anomaly"),
        ("roa", {"expected_sign": "two_sided"}, "two_sided goes with caveat mixed_evidence"),
        ("roe", {"caveat_code": "sign_flip|made_up"}, "unknown caveat_code 'made_up'"),
        ("roa", {"caveat_code": "fiscal_seasonality"}, "caveat_code is required unless admission is 'eligible'"),
        ("roe", {"caveat_code": ""}, "caveat_code is required unless admission is 'eligible'"),
        ("roe", {"domain": "unrestricted"}, "caveat sign_flip goes with a domain rule"),
        ("earnings_yield", {"domain": "unrestricted"}, "caveat non_monotone goes with a domain rule"),
        ("market_cap", {"domain": "unrestricted"}, "log_winsor_z goes with domain positive_value_required"),
        ("roa", {"hypothesis_family": ""}, "hypothesis_family must match"),
        ("revenue_growth_qoq", {"admission": "blocked_known_bias"},
         "admission must be blocked_incomparable_origin"),
        # R1d: the split_basis caveat marks exactly the share-basis rows.
        ("eps_basic_q_growth_qoq", {"caveat_code": "sequential_quarter"},
         "caveat split_basis goes with a share-basis comparison"),
        ("roa", {"admission": "eligible_with_caveat", "caveat_code": "split_basis", "admission_note": "x"},
         "caveat split_basis goes with a share-basis comparison"),
        # R1b J4: an imputed zero is always labeled, and only an imputed zero.
        ("debt_to_assets", {"admission": "eligible", "caveat_code": "", "admission_note": ""},
         "caveat presence_rule goes with a definition that reads a zero"),
        ("roa", {"admission": "eligible_with_caveat", "caveat_code": "presence_rule", "admission_note": "x"},
         "caveat presence_rule goes with a definition that reads a zero"),
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


def test_composition_clock_is_the_latest_leg_clock():
    """R1a M9: bar-only legs keep the bar clock; a fundamental leg makes it the max clock."""
    template = _by_id()["sales_to_price"]
    probe = replace(template, feature_id="reversal_to_volatility_probe", numerator="metric:total_return_1m",
                    denominator="metric:realized_vol_60d", anomaly_class="reversal",
                    hypothesis_family="reversal_to_volatility_probe", availability_clock=CLOCK_BAR,
                    min_history_quarters=0, min_history_sessions=61)
    validate_anomaly_catalog((*default_anomaly_catalog(), probe))

    with pytest.raises(AnomalyCatalogError, match=re.escape("the inherited clock is 'modeled_trade_date_22h'")):
        validate_anomaly_catalog((*default_anomaly_catalog(), replace(probe, availability_clock=CLOCK_MAX)))


def test_catalog_digest_ignores_line_endings(tmp_path):
    """R1a M1: a CRLF checkout or archive export hashes the same as the LF blob."""
    content = ANOMALY_CATALOG_PATH.read_bytes().replace(b"\r\n", b"\n")
    lf, crlf = tmp_path / "lf.csv", tmp_path / "crlf.csv"
    lf.write_bytes(content)
    crlf.write_bytes(content.replace(b"\n", b"\r\n"))

    assert anomaly_catalog_sha256(crlf) == anomaly_catalog_sha256(lf) == anomaly_catalog_sha256()
    assert anomaly_catalog_sha256(lf) != anomaly_catalog_sha256(_mutated(tmp_path, "roa", {"reference": "x"}))


def test_anomaly_catalog_doc_is_generated_from_the_catalog():
    assert DOC_PATH.read_text(encoding="utf-8") == render_anomaly_catalog_markdown()
