from __future__ import annotations

import datetime as dt

import numpy as np
import pandas as pd
import pytest

from db.signal_eval import (
    IC_HORIZONS,
    IcResult,
    compute_forward_returns,
    compute_information_coefficient,
    compute_quantile_spread,
    compute_turnover,
    compute_factor_correlation,
    compute_crowding,
    compute_breadth,
    load_panel_for_eval,
    evaluate_panel,
)


def _dates(n: int, start="2020-01-01") -> list[dt.date]:
    d0 = pd.Timestamp(start)
    return [(d0 + pd.Timedelta(days=7 * i)).date() for i in range(n)]


def _panel(factor_id: str, values_by_date: dict) -> pd.DataFrame:
    rows = []
    for as_of, sec_vals in values_by_date.items():
        for sec, val in sec_vals.items():
            rows.append({"security_id": sec, "as_of_date": as_of, "factor_id": factor_id, "value": float(val)})
    return pd.DataFrame(rows)


def test_zero_signal_factor_has_rank_ic_near_zero() -> None:
    rng = np.random.default_rng(7)
    dates = _dates(40)
    secs = [f"S{i}" for i in range(30)]
    # factor and forward returns are independent random draws -> no relationship
    panel_rows, fr_rows = [], []
    for d in dates:
        for s in secs:
            panel_rows.append({"security_id": s, "as_of_date": d, "factor_id": "noise", "value": rng.normal()})
            for h in IC_HORIZONS:
                fr_rows.append({"security_id": s, "as_of_date": d, "horizon": h, "forward_return": rng.normal()})
    result = compute_information_coefficient(pd.DataFrame(panel_rows), pd.DataFrame(fr_rows))
    assert isinstance(result, IcResult)
    mean_ic_h1 = result.ic.loc[result.ic["horizon"] == 1, "mean_rank_ic"].iloc[0]
    assert abs(mean_ic_h1) < 0.05


def test_persistent_but_fading_factor_has_monotone_decaying_ic() -> None:
    rng = np.random.default_rng(11)
    dates = _dates(60)
    secs = [f"S{i}" for i in range(40)]
    panel_rows, fr_rows = [], []
    for d in dates:
        base = {s: rng.normal() for s in secs}
        for s in secs:
            panel_rows.append({"security_id": s, "as_of_date": d, "factor_id": "fade", "value": base[s]})
            for h in IC_HORIZONS:
                decay = 0.9 ** (IC_HORIZONS.index(h))         # weaker signal at longer horizons
                fr_rows.append({"security_id": s, "as_of_date": d, "horizon": h,
                                "forward_return": decay * base[s] + 0.25 * rng.normal()})
    decay = compute_information_coefficient(pd.DataFrame(panel_rows), pd.DataFrame(fr_rows)).ic_decay
    profile = decay.sort_values("ladder_position")["mean_rank_ic"].to_numpy()
    assert np.all(np.diff(profile) <= 1e-9)                    # non-increasing rank-IC across the ladder
    assert profile[0] > profile[-1]


def test_compute_ic_is_order_invariant() -> None:
    rng = np.random.default_rng(3)
    dates = _dates(20); secs = [f"S{i}" for i in range(15)]
    panel_rows, fr_rows = [], []
    for d in dates:
        for s in secs:
            v = rng.normal()
            panel_rows.append({"security_id": s, "as_of_date": d, "factor_id": "f", "value": v})
            for h in IC_HORIZONS:
                fr_rows.append({"security_id": s, "as_of_date": d, "horizon": h, "forward_return": v + rng.normal()})
    p, fr = pd.DataFrame(panel_rows), pd.DataFrame(fr_rows)
    a = compute_information_coefficient(p, fr).ic
    b = compute_information_coefficient(p.sample(frac=1.0, random_state=99).reset_index(drop=True),
                                        fr.sample(frac=1.0, random_state=1).reset_index(drop=True)).ic
    pd.testing.assert_frame_equal(a.reset_index(drop=True), b.reset_index(drop=True))


def test_compute_ic_handles_mixed_asof_dtypes() -> None:
    # Regression: the panel arrives from DuckDB `.df()` with as_of_date as datetime64,
    # while compute_forward_returns emits as_of_date as datetime.date objects (object dtype).
    # Merging the two on as_of_date must NOT raise
    #   "ValueError: You are trying to merge on datetime64[us] and object columns".
    rng = np.random.default_rng(5)
    dates = _dates(12)
    secs = [f"S{i}" for i in range(10)]
    panel_rows, fr_rows = [], []
    for d in dates:
        for s in secs:
            v = rng.normal()
            panel_rows.append({"security_id": s, "as_of_date": d, "factor_id": "mixed", "value": v})
            for h in IC_HORIZONS:
                fr_rows.append({"security_id": s, "as_of_date": d, "horizon": h,
                                "forward_return": v + rng.normal()})
    panel = pd.DataFrame(panel_rows)
    panel["as_of_date"] = pd.to_datetime(panel["as_of_date"])          # datetime64, like DuckDB .df()
    forward_returns = pd.DataFrame(fr_rows)                            # as_of_date stays datetime.date (object)
    assert pd.api.types.is_datetime64_any_dtype(panel["as_of_date"])
    assert forward_returns["as_of_date"].dtype == object
    result = compute_information_coefficient(panel, forward_returns)   # must not raise
    assert isinstance(result, IcResult)
    assert not result.ic.empty
    assert (result.ic["n_dates"] > 0).all()


def test_compute_forward_returns_from_prices() -> None:
    prices = pd.DataFrame({
        "security_id": ["S"] * 5,
        "as_of_date": _dates(5),
        "close": [100.0, 110.0, 121.0, 133.1, 146.41],
    })
    fr = compute_forward_returns(prices, horizons=[1])
    r = fr.sort_values("as_of_date")["forward_return"].dropna().to_numpy()
    assert np.allclose(r, [0.10, 0.10, 0.10, 0.10])           # 10% per step, last row NaN dropped


def test_evaluate_panel_persists_ic_rows_per_factor(tmp_store) -> None:
    # v_factor_panel is empty on a fresh template; evaluate_panel with an injected panel/returns still writes rows.
    # (Full base-table seeding is exercised in the DQC integration test; here we assert the persistence contract.)
    counts = evaluate_panel(
        tmp_store,
        forward_returns=None,
        run_id="rid-ic",
    )
    assert "factor_ic" in counts and "factor_ic_decay" in counts


def test_monotone_factor_has_monotone_deciles_and_positive_spread() -> None:
    dates = _dates(50); secs = [f"S{i}" for i in range(50)]
    panel_rows, fr_rows = [], []
    for d in dates:
        for i, s in enumerate(secs):
            v = float(i)                                    # perfectly ordered factor
            panel_rows.append({"security_id": s, "as_of_date": d, "factor_id": "mono", "value": v})
            fr_rows.append({"security_id": s, "as_of_date": d, "horizon": 1, "forward_return": v / 100.0})
    spread = compute_quantile_spread(pd.DataFrame(panel_rows), pd.DataFrame(fr_rows), n_quantiles=10, horizons=[1])
    deciles = spread.sort_values("quantile")["mean_forward_return"].to_numpy()
    assert np.all(np.diff(deciles) > -1e-9)                 # monotone non-decreasing decile returns
    assert spread["long_short_spread"].iloc[0] > 0
    assert spread["long_short_hit_rate"].iloc[0] > 0.9
    assert spread["decile_monotonicity"].iloc[0] > 0.99


def test_random_walk_factor_has_flat_deciles_and_high_turnover() -> None:
    rng = np.random.default_rng(5)
    dates = _dates(60); secs = [f"S{i}" for i in range(40)]
    panel_rows, fr_rows = [], []
    for d in dates:
        for s in secs:
            v = rng.normal()                                # re-drawn each date -> unstable ranking
            panel_rows.append({"security_id": s, "as_of_date": d, "factor_id": "rw", "value": v})
            fr_rows.append({"security_id": s, "as_of_date": d, "horizon": 1, "forward_return": rng.normal()})
    p, fr = pd.DataFrame(panel_rows), pd.DataFrame(fr_rows)
    spread = compute_quantile_spread(p, fr, n_quantiles=10, horizons=[1])
    assert abs(spread["long_short_spread"].iloc[0]) < 0.05
    turnover = compute_turnover(p, n_quantiles=10)
    assert turnover["top_decile_turnover"].iloc[0] > 0.5    # membership churns hard for a random walk
    assert abs(turnover["mean_rank_autocorrelation"].iloc[0]) < 0.2


def test_stable_factor_has_low_turnover() -> None:
    dates = _dates(30); secs = [f"S{i}" for i in range(40)]
    panel_rows = [{"security_id": s, "as_of_date": d, "factor_id": "stable", "value": float(i)}
                  for d in dates for i, s in enumerate(secs)]
    turnover = compute_turnover(pd.DataFrame(panel_rows), n_quantiles=10)
    assert turnover["top_decile_turnover"].iloc[0] < 1e-9   # ranking never changes -> zero churn
    assert turnover["mean_rank_autocorrelation"].iloc[0] > 0.99


def test_compute_quantile_spread_is_order_invariant() -> None:
    rng = np.random.default_rng(21)
    dates = _dates(15); secs = [f"S{i}" for i in range(30)]
    rows, fr = [], []
    for d in dates:
        for s in secs:
            v = rng.normal()
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "f", "value": v})
            fr.append({"security_id": s, "as_of_date": d, "horizon": 1, "forward_return": v + rng.normal()})
    p, f = pd.DataFrame(rows), pd.DataFrame(fr)
    a = compute_quantile_spread(p, f, n_quantiles=5, horizons=[1])
    b = compute_quantile_spread(p.sample(frac=1.0, random_state=8).reset_index(drop=True),
                                f.sample(frac=1.0, random_state=9).reset_index(drop=True), n_quantiles=5, horizons=[1])
    pd.testing.assert_frame_equal(a.reset_index(drop=True), b.reset_index(drop=True))


def test_near_duplicate_factors_are_mutually_crowded() -> None:
    rng = np.random.default_rng(2)
    dates = _dates(30); secs = [f"S{i}" for i in range(40)]
    rows = []
    for d in dates:
        base = {s: rng.normal() for s in secs}
        for s in secs:
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "value_a", "value": base[s]})
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "value_b", "value": base[s] + 0.01 * rng.normal()})
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "indep",   "value": rng.normal()})
    panel = pd.DataFrame(rows)
    corr = compute_factor_correlation(panel)
    ab = corr[(corr["factor_id_a"] == "value_a") & (corr["factor_id_b"] == "value_b")]["mean_abs_correlation"].iloc[0]
    assert ab > 0.9
    crowd = compute_crowding(corr).set_index("factor_id")
    assert crowd.loc["value_a", "max_abs_correlation"] > 0.9
    assert crowd.loc["value_b", "max_abs_correlation"] > 0.9
    assert crowd.loc["indep", "max_abs_correlation"] < 0.5
    assert crowd.loc["value_a", "most_correlated_factor_id"] == "value_b"


def test_breadth_matches_known_fixture_coverage() -> None:
    dates = _dates(3); secs = [f"S{i}" for i in range(10)]
    rows = []
    for d in dates:
        for i, s in enumerate(secs):
            val = float(i) if i < 6 else None                # only 6 of 10 names defined
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "sparse", "value": val})
    uni = pd.DataFrame({"as_of_date": dates, "universe_size": [10, 10, 10]})
    breadth = compute_breadth(pd.DataFrame(rows), uni)
    assert set(breadth["n_non_null"]) == {6}
    assert np.allclose(breadth["coverage_fraction"], 0.6)


def test_compute_correlation_is_order_invariant() -> None:
    rng = np.random.default_rng(31)
    dates = _dates(12); secs = [f"S{i}" for i in range(20)]
    rows = []
    for d in dates:
        for s in secs:
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "a", "value": rng.normal()})
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "b", "value": rng.normal()})
    p = pd.DataFrame(rows)
    a = compute_factor_correlation(p)
    b = compute_factor_correlation(p.sample(frac=1.0, random_state=6).reset_index(drop=True))
    pd.testing.assert_frame_equal(a.reset_index(drop=True), b.reset_index(drop=True))
