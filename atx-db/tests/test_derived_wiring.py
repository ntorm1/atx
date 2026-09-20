"""Tier1-S3 T10: the derived engine is a first-class dataset and activation stage."""

from __future__ import annotations

import inspect

from atx_db.jobs import DATASET_DEPENDENCIES, DATASET_REGISTRY


def test_both_datasets_are_registered():
    assert "derived_metrics" in DATASET_REGISTRY
    assert "market_daily" in DATASET_REGISTRY


def test_dataset_classes_expose_the_expected_identity():
    from atx_db.derived_metrics import DerivedMetricsDataset
    from atx_db.market_daily import MarketDailyDataset

    derived_cls, _derived_factory = DATASET_REGISTRY["derived_metrics"]
    market_cls, _market_factory = DATASET_REGISTRY["market_daily"]
    assert derived_cls is DerivedMetricsDataset
    assert market_cls is MarketDailyDataset
    assert derived_cls.dataset_id == "derived_metrics"
    assert market_cls.dataset_id == "market_daily"


def test_option_factories_accept_a_plain_params_dict():
    _derived_cls, derived_factory = DATASET_REGISTRY["derived_metrics"]
    _market_cls, market_factory = DATASET_REGISTRY["market_daily"]
    derived = derived_factory({"metric_codes": ["revenue_ttm"], "batch_size": 7})
    assert derived.metric_codes == ("revenue_ttm",)
    assert derived.batch_size == 7
    market = market_factory({"start_date": "2020-01-02", "batch_size": 3})
    assert market.batch_size == 3
    assert market.start_date is not None
    assert market_factory({}).start_date is None


def test_dependency_edges_are_declared_and_resolvable():
    assert DATASET_DEPENDENCIES["derived_metrics"] == ("fundamental_standardized",)
    assert DATASET_DEPENDENCIES["market_daily"] == (
        "derived_metrics",
        "tbltickerhistory_daily",
        "shares_outstanding_history",
    )
    for dataset_id, dependencies in DATASET_DEPENDENCIES.items():
        for dependency in dependencies:
            assert dependency in DATASET_REGISTRY, f"{dataset_id} -> {dependency}"


def test_depends_on_is_applied_to_the_classes():
    derived_cls, _ = DATASET_REGISTRY["derived_metrics"]
    market_cls, _ = DATASET_REGISTRY["market_daily"]
    assert derived_cls.depends_on == ("fundamental_standardized",)
    assert "derived_metrics" in market_cls.depends_on


def test_activation_stage_order_places_the_engine_after_reconciliation():
    from atx_db.activation import STAGE_ORDER

    assert "derived_metrics" in STAGE_ORDER
    assert "market_daily" in STAGE_ORDER
    assert STAGE_ORDER.index("standardized") < STAGE_ORDER.index("derived_metrics")
    assert STAGE_ORDER.index("reconciliation") < STAGE_ORDER.index("derived_metrics")
    assert STAGE_ORDER.index("derived_metrics") < STAGE_ORDER.index("market_daily")
    assert STAGE_ORDER.index("market_daily") < STAGE_ORDER.index("provider_coverage")
    assert STAGE_ORDER.index("ticker_history_publish") < STAGE_ORDER.index("market_daily")


def test_both_stages_are_registered_with_the_uniform_signature():
    from atx_db.activation import STAGES, StageResult

    for name in ("derived_metrics", "market_daily"):
        function = STAGES[name]
        parameters = list(inspect.signature(function).parameters)
        assert parameters == ["store", "options"], f"{name} has {parameters}"
        assert inspect.signature(function).return_annotation in (StageResult, "StageResult")


def test_stage_order_and_stages_stay_in_lockstep():
    from atx_db.activation import STAGE_ORDER, STAGES

    assert set(STAGES) <= set(STAGE_ORDER)


def test_derived_stage_seeds_definitions_and_returns_a_row_count(tmp_store, derived_fixture):
    from atx_db.activation import ActivationOptions, stage_derived_metrics

    derived_fixture(tmp_store)
    result = stage_derived_metrics(tmp_store, ActivationOptions(db_path=tmp_store.path))
    assert result.rows > 0
    assert result.detail["definitions_seeded"] > 0
    assert result.detail["metric_count"] > 0
    seeded = tmp_store.con.execute("SELECT count(*) FROM derived_metric_definitions").fetchone()[0]
    assert seeded == result.detail["definitions_seeded"]


def test_market_stage_reports_the_shares_reconciliation(tmp_store, derived_fixture):
    from atx_db.activation import ActivationOptions, stage_derived_metrics, stage_market_daily

    derived_fixture(tmp_store)
    options = ActivationOptions(db_path=tmp_store.path)
    stage_derived_metrics(tmp_store, options)
    result = stage_market_daily(tmp_store, options)
    assert result.rows >= 0
    assert "pass_rate" in result.detail
    assert "meets_spec_gate" in result.detail


def test_stage_payloads_are_json_serialisable(tmp_store, derived_fixture):
    import json

    from atx_db.activation import ActivationOptions, stage_derived_metrics, stage_market_daily

    derived_fixture(tmp_store)
    options = ActivationOptions(db_path=tmp_store.path)
    for stage in (stage_derived_metrics, stage_market_daily):
        json.dumps(stage(tmp_store, options).detail, default=str)
