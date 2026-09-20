"""Tier1-S3 T5: the quarterly derived-metric engine."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.derived_metrics import (
    DerivedMetricsOptions,
    refresh_derived_metrics,
    select_security_batches,
)
from atx_db.derived_registry import seed_derived_metric_definitions

_QUARTERS = (
    dt.date(2019, 3, 31),
    dt.date(2019, 6, 30),
    dt.date(2019, 9, 30),
    dt.date(2019, 12, 31),
    dt.date(2020, 3, 31),
    dt.date(2020, 6, 30),
    dt.date(2020, 9, 30),
    dt.date(2020, 12, 31),
)


def _insert_fact(store, security_id, code, basis, period_end, value, available_at, revision=1):
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, item_id, canonical_code, basis,
            period_end, value, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, revision_sequence,
            is_latest_revision
        ) VALUES (?, 'test', ?, 1, ?, ?, ?, ?, ?, ?, '[]', '[]', 'r', 'direct', ?, true)
        """,
        [
            f"{security_id}|{code}|{basis}|{period_end}|{revision}",
            security_id,
            code,
            basis,
            period_end,
            value,
            available_at.date(),
            available_at,
            revision,
        ],
    )


def _available(period_end: dt.date, *, lag_days: int = 40) -> dt.datetime:
    return dt.datetime.combine(period_end + dt.timedelta(days=lag_days), dt.time(21, 0))


@pytest.fixture
def seeded(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    for index, period_end in enumerate(_QUARTERS):
        revenue = 100.0 + 10.0 * index
        cogs = 60.0 + 4.0 * index
        assets = 1000.0 + 50.0 * index
        _insert_fact(tmp_store, "S1", "revenue", "quarterly", period_end, revenue, _available(period_end))
        _insert_fact(tmp_store, "S1", "cost_of_revenue_cogs", "quarterly", period_end, cogs, _available(period_end))
        _insert_fact(
            tmp_store, "S1", "total_assets", "instant", period_end, assets, _available(period_end, lag_days=50)
        )
    return tmp_store


def _value(store, metric_code, period_end):
    row = store.con.execute(
        "SELECT value, available_at, inputs_hash FROM derived_metric_values "
        "WHERE metric_code = ? AND period_end = ? AND security_id = 'S1'",
        [metric_code, period_end],
    ).fetchone()
    return row


def test_batches_are_deterministic_and_bounded(seeded):
    batches = select_security_batches(seeded, DerivedMetricsOptions(batch_size=1))
    assert batches == [("S1",)]


def test_ttm_sums_exactly_four_quarters(seeded):
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    assert _value(seeded, "revenue_ttm", _QUARTERS[2]) is None  # fewer than 4 observations
    value, _available_at, _hash = _value(seeded, "revenue_ttm", _QUARTERS[3])
    assert value == pytest.approx(100.0 + 110.0 + 120.0 + 130.0)
    value, _available_at, _hash = _value(seeded, "revenue_ttm", _QUARTERS[7])
    assert value == pytest.approx(140.0 + 150.0 + 160.0 + 170.0)


def test_available_at_is_the_max_over_the_ttm_frame(seeded):
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    _, available_at, _ = _value(seeded, "revenue_ttm", _QUARTERS[3])
    assert available_at == _available(_QUARTERS[3])


def test_avg2_averages_the_current_and_year_ago_balance(seeded):
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("total_assets_avg2",)))
    assert _value(seeded, "total_assets_avg2", _QUARTERS[3]) is None
    value, available_at, _hash = _value(seeded, "total_assets_avg2", _QUARTERS[4])
    assert value == pytest.approx((1000.0 + 1200.0) / 2.0)
    assert available_at == _available(_QUARTERS[4], lag_days=50)


def test_dependency_metrics_are_computed_in_topological_order(seeded):
    refresh_derived_metrics(
        seeded,
        DerivedMetricsOptions(metric_codes=("gross_profit_q", "gross_profit_ttm", "revenue_ttm", "gross_margin")),
    )
    gross_profit, _at, _hash = _value(seeded, "gross_profit_ttm", _QUARTERS[3])
    revenue, _at2, _hash2 = _value(seeded, "revenue_ttm", _QUARTERS[3])
    margin, _at3, _hash3 = _value(seeded, "gross_margin", _QUARTERS[3])
    assert margin == pytest.approx(gross_profit / revenue)


def test_yoy_uses_the_absolute_base(seeded):
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm", "revenue_growth_yoy")))
    current, _a, _h = _value(seeded, "revenue_ttm", _QUARTERS[7])
    prior, _b, _i = _value(seeded, "revenue_ttm", _QUARTERS[3])
    growth, _c, _j = _value(seeded, "revenue_growth_yoy", _QUARTERS[7])
    assert growth == pytest.approx((current - prior) / abs(prior))


def test_a_zero_denominator_yields_no_row_rather_than_infinity(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    for period_end in _QUARTERS[:4]:
        _insert_fact(tmp_store, "S2", "revenue", "quarterly", period_end, 0.0, _available(period_end))
        _insert_fact(tmp_store, "S2", "cost_of_revenue_cogs", "quarterly", period_end, 0.0, _available(period_end))
    refresh_derived_metrics(
        tmp_store,
        DerivedMetricsOptions(metric_codes=("gross_profit_q", "gross_profit_ttm", "revenue_ttm", "gross_margin")),
    )
    rows = tmp_store.con.execute(
        "SELECT count(*) FROM derived_metric_values WHERE metric_code = 'gross_margin'"
    ).fetchone()[0]
    assert rows == 0


def test_inputs_hash_is_stable_across_reruns(seeded):
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    first = seeded.con.execute(
        "SELECT inputs_hash FROM derived_metric_values WHERE metric_code = 'revenue_ttm' ORDER BY period_end"
    ).fetchall()
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    second = seeded.con.execute(
        "SELECT inputs_hash FROM derived_metric_values WHERE metric_code = 'revenue_ttm' ORDER BY period_end"
    ).fetchall()
    assert first == second
    assert all(len(str(row[0])) == 64 for row in first)


def test_inputs_hash_changes_when_an_input_value_changes(seeded):
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    before = _value(seeded, "revenue_ttm", _QUARTERS[7])[2]
    seeded.con.execute(
        "UPDATE fundamental_standardized SET value = value + 1 WHERE canonical_code = 'revenue' AND period_end = ?",
        [_QUARTERS[6]],
    )
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    after = _value(seeded, "revenue_ttm", _QUARTERS[7])[2]
    assert before != after


def test_rerun_is_idempotent_on_row_counts(seeded):
    options = DerivedMetricsOptions(metric_codes=("revenue_ttm", "total_assets_avg2"))
    first = refresh_derived_metrics(seeded, options)
    second = refresh_derived_metrics(seeded, options)
    assert first == second
    total = seeded.con.execute("SELECT count(*) FROM derived_metric_values").fetchone()[0]
    assert total == first


def test_batch_size_does_not_change_the_result(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    for security_id in ("S1", "S2", "S3"):
        for index, period_end in enumerate(_QUARTERS):
            _insert_fact(
                tmp_store,
                security_id,
                "revenue",
                "quarterly",
                period_end,
                100.0 + index + len(security_id),
                _available(period_end),
            )
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions(metric_codes=("revenue_ttm",), batch_size=3))
    wide = tmp_store.con.execute(
        "SELECT security_id, period_end, value, inputs_hash FROM derived_metric_values ORDER BY 1, 2"
    ).fetchall()
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions(metric_codes=("revenue_ttm",), batch_size=1))
    narrow = tmp_store.con.execute(
        "SELECT security_id, period_end, value, inputs_hash FROM derived_metric_values ORDER BY 1, 2"
    ).fetchall()
    assert wide == narrow


def test_every_emitted_value_is_finite_and_has_an_availability(seeded):
    refresh_derived_metrics(seeded)
    bad = seeded.con.execute(
        "SELECT count(*) FROM derived_metric_values WHERE value IS NULL OR NOT isfinite(value) OR available_at IS NULL"
    ).fetchone()[0]
    assert bad == 0


def test_as_of_date_never_precedes_availability(seeded):
    refresh_derived_metrics(seeded)
    violations = seeded.con.execute(
        "SELECT count(*) FROM derived_metric_values WHERE as_of_date < CAST(available_at AS DATE)"
    ).fetchone()[0]
    assert violations == 0


def test_full_catalog_runs_without_error(seeded):
    rows = refresh_derived_metrics(seeded)
    assert rows > 0
    codes = seeded.con.execute("SELECT count(DISTINCT metric_code) FROM derived_metric_values").fetchone()[0]
    assert codes >= 5


def test_no_wall_clock_in_the_module_source():
    import inspect

    import atx_db.derived_metrics as module

    source = inspect.getsource(module)
    for forbidden in ("date.today", "utcnow", "now()", "Timestamp.now", "time.time"):
        assert forbidden not in source, forbidden
