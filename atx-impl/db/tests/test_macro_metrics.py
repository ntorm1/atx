"""Tests for the derived ``macro_metrics`` dataset (S13).

The engine splits into a pure, DB-free transform (``compute_macro_metrics``) that maps
raw FRED observations to typed macro metric rows (change, year-over-year change/growth,
expanding z-score, synthetic 10Y-2Y term spread), and a thin DuckDB materializer
(``refresh_macro_metrics`` / ``MacroMetricsDataset``) that feeds it the cached
observations and writes the result.

No network: metrics derive purely from already-cached warehouse tables.
"""
from __future__ import annotations

import datetime as dt

import numpy as np
import pandas as pd
import pytest

from db.macro_metrics import (
    MacroMetricsDataset,
    MacroMetricsOptions,
    compute_macro_metrics,
    macro_metrics_asof,
    refresh_macro_metrics,
)


def _ts(s: str) -> pd.Timestamp:
    return pd.Timestamp(s)


def _obs(series_id, date, value, av="2026-06-28", frequency="monthly", units="x"):
    return {
        "series_id": series_id,
        "observation_date": pd.Timestamp(date),
        "value": value,
        "available_at": _ts(av),
        "frequency": frequency,
        "units": units,
    }


class TestComputeMacroMetrics:
    def test_change_abs_vs_prior_observation(self):
        out = compute_macro_metrics(pd.DataFrame([
            _obs("UNRATE", "2025-01-01", 4.0),
            _obs("UNRATE", "2025-02-01", 4.2),
        ]))
        by = {r.observation_date: r for r in out.itertuples(index=False)}
        assert pd.isna(by[dt.date(2025, 1, 1)].change_abs)
        assert by[dt.date(2025, 2, 1)].change_abs == pytest.approx(0.2)

    def test_year_over_year_change_and_growth(self):
        rows = [_obs("CPIAUCSL", f"2024-{m:02d}-01", 100.0 + m) for m in range(1, 13)]
        rows.append(_obs("CPIAUCSL", "2025-01-01", 110.0))  # vs 2024-01-01 base 101.0
        out = compute_macro_metrics(pd.DataFrame(rows))
        last = {r.observation_date: r for r in out.itertuples(index=False)}[dt.date(2025, 1, 1)]
        assert last.change_yoy == pytest.approx(110.0 - 101.0)
        assert last.yoy_growth == pytest.approx(110.0 / 101.0 - 1.0)

    def test_yoy_growth_null_when_base_nonpositive(self):
        rows = [_obs("X", f"2024-{m:02d}-01", 0.0) for m in range(1, 13)]
        rows.append(_obs("X", "2025-01-01", 5.0))  # base (2024-01-01) = 0 -> growth undefined
        out = compute_macro_metrics(pd.DataFrame(rows))
        last = {r.observation_date: r for r in out.itertuples(index=False)}[dt.date(2025, 1, 1)]
        assert pd.isna(last.yoy_growth)
        assert last.change_yoy == pytest.approx(5.0)

    def test_expanding_zscore_respects_min_periods_and_sign(self):
        base = dt.date(2024, 1, 1)
        rows = [_obs("DGS10", base + dt.timedelta(days=i), float(i + 1), frequency="daily") for i in range(30)]
        out = compute_macro_metrics(pd.DataFrame(rows)).sort_values("observation_date").reset_index(drop=True)
        # first observation: fewer than ZSCORE_MIN_PERIODS history -> NaN
        assert pd.isna(out.iloc[0]["zscore"])
        # last observation is the series max -> z-score is finite and positive
        assert np.isfinite(out.iloc[-1]["zscore"])
        assert out.iloc[-1]["zscore"] > 0

    def test_expanding_pct_rank_regime(self):
        # ascending values each set a new high (regime percentile 1.0); a later dip
        # ranks at the fraction of prior+current history at or below it.
        base = dt.date(2024, 1, 1)
        vals = [10.0, 20.0, 30.0, 15.0]
        rows = [_obs("VIXCLS", base + dt.timedelta(days=i), v, frequency="daily")
                for i, v in enumerate(vals)]
        out = compute_macro_metrics(pd.DataFrame(rows)).sort_values("observation_date").reset_index(drop=True)
        # first observation: alone in its history -> top of its own (1-point) distribution
        assert out.iloc[0]["expanding_pct_rank"] == pytest.approx(1.0)
        # each new high stays at 1.0
        assert out.iloc[2]["expanding_pct_rank"] == pytest.approx(1.0)
        # the dip to 15 sits at {10, 15} <= 15 out of {10,20,30,15} -> 2/4 = 0.5
        assert out.iloc[3]["expanding_pct_rank"] == pytest.approx(0.5)

    def test_synthetic_term_spread(self):
        rows = [
            _obs("DGS10", "2024-01-02", 4.0, frequency="daily"),
            _obs("DGS10", "2024-01-03", 4.1, frequency="daily"),
            _obs("DGS2", "2024-01-02", 4.5, frequency="daily"),
            _obs("DGS2", "2024-01-03", 4.4, frequency="daily"),
        ]
        out = compute_macro_metrics(pd.DataFrame(rows))
        spread = out[out["series_id"] == "T10Y2Y"]
        assert len(spread) == 2
        assert bool(spread.iloc[0]["is_synthetic"]) is True
        by = {r.observation_date: r for r in spread.itertuples(index=False)}
        assert by[dt.date(2024, 1, 2)].value == pytest.approx(-0.5)  # inverted curve
        assert by[dt.date(2024, 1, 3)].value == pytest.approx(-0.3)

    def test_synthetic_real_fed_funds(self):
        rows = [
            _obs("CPIAUCSL", "2024-01-01", 100.0, av="2024-01-15"),
            _obs("CPIAUCSL", "2024-02-01", 101.0, av="2024-02-15"),
            _obs("CPIAUCSL", "2025-01-01", 110.0, av="2025-01-15"),
            _obs("FEDFUNDS", "2025-01-01", 5.0, av="2025-02-01", units="percent"),
        ]
        out = compute_macro_metrics(pd.DataFrame(rows))
        real = out[out["series_id"] == "REAL_FEDFUNDS"]
        assert len(real) == 1
        row = real.iloc[0]
        assert bool(row["is_synthetic"]) is True
        assert row["frequency"] == "monthly"
        assert row["units"] == "percentage_points"
        assert row["observation_date"] == dt.date(2025, 1, 1)
        assert row["value"] == pytest.approx(5.0 - 10.0)
        assert row["available_at"] == pd.Timestamp("2025-02-01")

    def test_synthetic_real_fed_funds_requires_cpi_yoy_base(self):
        rows = [
            _obs("CPIAUCSL", "2025-01-01", 110.0, av="2025-01-15"),
            _obs("FEDFUNDS", "2025-01-01", 5.0, av="2025-02-01", units="percent"),
        ]
        out = compute_macro_metrics(pd.DataFrame(rows))
        assert "REAL_FEDFUNDS" not in set(out["series_id"])

    def test_metric_id_deterministic_and_unique(self):
        rows = pd.DataFrame([
            _obs("UNRATE", "2025-01-01", 4.0),
            _obs("UNRATE", "2025-02-01", 4.2),
        ])
        a = compute_macro_metrics(rows)
        b = compute_macro_metrics(rows)
        assert list(a["metric_id"]) == list(b["metric_id"])
        assert a["metric_id"].is_unique

    def test_empty_returns_typed_empty(self):
        out = compute_macro_metrics(pd.DataFrame())
        assert out.empty
        assert "zscore" in out.columns


# --------------------------------------------------------------------------- #
# Integration: cached observations -> macro metrics
# --------------------------------------------------------------------------- #


def _insert_series(store, series_id, frequency, units):
    store.con.execute(
        "INSERT INTO macro_series (source, series_id, title, frequency, units, source_loaded_at) "
        "VALUES ('test', ?, ?, ?, ?, now())",
        [series_id, f"{series_id} title", frequency, units],
    )


def _insert_obs(store, series_id, date, value, av):
    store.con.execute(
        "INSERT INTO macro_observations (source, series_id, observation_date, as_of_date, available_at, value, source_loaded_at) "
        "VALUES ('test', ?, ?, ?, ?, ?, now())",
        [series_id, date, date, av, value],
    )


class TestRefreshIntegration:
    def test_materializes_metrics_and_term_spread(self, tmp_store):
        _insert_series(tmp_store, "DGS10", "daily", "percent")
        _insert_series(tmp_store, "DGS2", "daily", "percent")
        av = dt.datetime(2026, 6, 28, 7, 0)
        for d, v10, v2 in [
            (dt.date(2024, 1, 2), 4.0, 4.5),
            (dt.date(2024, 1, 3), 4.1, 4.4),
            (dt.date(2024, 1, 4), 4.2, 4.3),
        ]:
            _insert_obs(tmp_store, "DGS10", d, v10, av)
            _insert_obs(tmp_store, "DGS2", d, v2, av)

        n = refresh_macro_metrics(tmp_store, MacroMetricsOptions())
        assert n == 9  # 3 dates x (DGS10 + DGS2 + synthetic T10Y2Y)
        spread = tmp_store.con.execute(
            "SELECT observation_date, value FROM macro_metrics WHERE series_id='T10Y2Y' ORDER BY observation_date"
        ).df()
        assert spread.iloc[0]["value"] == pytest.approx(-0.5)
        # change_abs on DGS10 between first two dates
        chg = tmp_store.con.execute(
            "SELECT change_abs FROM macro_metrics WHERE series_id='DGS10' AND observation_date = DATE '2024-01-03'"
        ).fetchone()[0]
        assert chg == pytest.approx(0.1)

    def test_materializes_real_fed_funds(self, tmp_store):
        _insert_series(tmp_store, "CPIAUCSL", "monthly", "index")
        _insert_series(tmp_store, "FEDFUNDS", "monthly", "percent")
        _insert_obs(tmp_store, "CPIAUCSL", dt.date(2024, 1, 1), 100.0, dt.datetime(2024, 1, 15))
        _insert_obs(tmp_store, "CPIAUCSL", dt.date(2025, 1, 1), 106.0, dt.datetime(2025, 1, 15))
        _insert_obs(tmp_store, "FEDFUNDS", dt.date(2025, 1, 1), 5.25, dt.datetime(2025, 2, 1))

        n = refresh_macro_metrics(tmp_store, MacroMetricsOptions())
        assert n == 4  # CPI x2 + FEDFUNDS + synthetic REAL_FEDFUNDS
        row = tmp_store.con.execute(
            """
            SELECT value, available_at, is_synthetic
            FROM macro_metrics
            WHERE series_id = 'REAL_FEDFUNDS'
            """
        ).fetchone()
        assert row[0] == pytest.approx(5.25 - 6.0)
        assert row[1] == dt.datetime(2025, 2, 1)
        assert row[2] is True

    def test_dataset_run_is_idempotent(self, tmp_store):
        _insert_series(tmp_store, "UNRATE", "monthly", "percent")
        _insert_obs(tmp_store, "UNRATE", dt.date(2025, 1, 1), 4.0, dt.datetime(2025, 2, 1))
        ds = MacroMetricsDataset()
        r1 = ds.run(tmp_store, MacroMetricsOptions())
        n1 = tmp_store.con.execute("SELECT count(*) FROM macro_metrics").fetchone()[0]
        r2 = ds.run(tmp_store, MacroMetricsOptions())
        n2 = tmp_store.con.execute("SELECT count(*) FROM macro_metrics").fetchone()[0]
        assert r1.rows_loaded == r2.rows_loaded
        assert n1 == n2 == 1


class TestAsofReader:
    def test_filters_by_available_at(self, tmp_store):
        _insert_series(tmp_store, "UNRATE", "monthly", "percent")
        _insert_obs(tmp_store, "UNRATE", dt.date(2025, 1, 1), 4.0, dt.datetime(2025, 2, 1, 12, 0))
        MacroMetricsDataset().run(tmp_store, MacroMetricsOptions())
        early = macro_metrics_asof(dt.date(2025, 1, 15), store=tmp_store, series_ids=["UNRATE"])
        assert early.empty
        late = macro_metrics_asof(dt.date(2025, 3, 1), store=tmp_store, series_ids=["UNRATE"])
        assert not late.empty
        assert set(late["series_id"]) == {"UNRATE"}


class TestSahmRule:
    def _unrate_obs(self):
        # 14 stable months at 3.5%, then a rising spike.
        dates = pd.date_range("2024-01-01", periods=17, freq="MS")
        values = [3.5] * 14 + [4.0, 4.2, 4.5]
        return [
            _obs("UNRATE", d, v, av=(d + pd.Timedelta(days=35)).strftime("%Y-%m-%d"))
            for d, v in zip(dates, values)
        ]

    def test_sahm_rule_series_emitted_with_recession_signal(self):
        out = compute_macro_metrics(pd.DataFrame(self._unrate_obs()), source="fixture")
        sahm = out[out["series_id"] == "SAHM_RULE"].sort_values("observation_date")
        assert not sahm.empty
        # 3mo MA at last month = (4.0+4.2+4.5)/3 = 4.23333; trailing-12mo min of the
        # 3mo MA = 3.5; Sahm = 0.73333 -> above the 0.5 recession-onset threshold.
        last = sahm.iloc[-1]
        assert last["value"] == pytest.approx(0.733333, abs=1e-4)
        assert last["value"] >= 0.5
        assert bool(last["is_synthetic"]) is True

    def test_sahm_rule_skips_warmup_months(self):
        out = compute_macro_metrics(pd.DataFrame(self._unrate_obs()), source="fixture")
        sahm = out[out["series_id"] == "SAHM_RULE"].sort_values("observation_date")
        # First valid value needs a full 3mo MA plus a 12mo trailing window -> the
        # 14th UNRATE month is the earliest Sahm row.
        assert sahm.iloc[0]["observation_date"] == dt.date(2025, 2, 1)

    def test_sahm_rule_available_at_tracks_unrate(self):
        out = compute_macro_metrics(pd.DataFrame(self._unrate_obs()), source="fixture")
        sahm = out[out["series_id"] == "SAHM_RULE"]
        unrate = out[out["series_id"] == "UNRATE"]
        merged = sahm.merge(unrate, on="observation_date", suffixes=("_s", "_u"))
        assert (merged["available_at_s"] == merged["available_at_u"]).all()
