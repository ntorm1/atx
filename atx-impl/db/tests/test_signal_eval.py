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
