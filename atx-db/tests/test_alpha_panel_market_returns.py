"""S3.2 internal total-return identity (rule returns-identity-v1) on a fixture lake."""

from __future__ import annotations

import datetime as dt
import importlib

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

D = dt.date


def sessions(start, end):
    return [start + dt.timedelta(days=i) for i in range((end - start).days + 1)
            if (start + dt.timedelta(days=i)).weekday() < 5]


def write(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.Table.from_pylist(rows), path)


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    cal = sessions(D(2019, 1, 2), D(2019, 4, 30))
    write(tmp_path / "calendar.parquet", [{"session_date": d} for d in cal])
    prices, panel, events = [], [], []
    for sid in range(1, 13):
        prev, adj = None, 1.0
        for d in cal:
            split = sid == 1 and d == D(2019, 3, 4)
            div = sid == 2 and d == D(2019, 2, 11)
            close = (20.0 + sid + d.day * 0.1) / (2 if sid == 1 and d >= D(2019, 3, 4) else 1)
            f = 0.5 if split else (0.99 if div else 1.0)
            ret = None if prev is None else close / (prev * f) - 1
            adj = adj * (1 + ret) if ret is not None else adj
            prices.append({"security_id": sid, "session_date": d, "close": close, "prev_raw_close": prev, "ret": ret,
                           "ret_guarded": False, "ret_source": "vendor", "fb_action": "none", "sid0_repaired": False,
                           "return_factor": f, "adj_close": adj})
            panel.append({"session_date": d, "security_id": sid, "member_equity": True, "me_line": 1e6 * sid})
            if split:
                events.append({"security_id": sid, "ex_date": d, "kind": "split", "split_ratio": 2.0, "implied_cash": None})
            if div:
                events.append({"security_id": sid, "ex_date": d, "kind": "cash_distribution", "split_ratio": None,
                               "implied_cash": prev * (1 - f)})
            prev = close
    write(tmp_path / "prices" / "year=2019" / "prices.parquet", prices)
    write(tmp_path / "panel" / "year=2019" / "panel-01.parquet", panel)
    write(tmp_path / "corporate_actions" / "vendor_events.parquet", events)
    return tmp_path


def test_identity_holds_off_events_and_explains_the_dividend_convention(lake):
    from atx_db.alpha_panel import common, market_common, returns_validation
    importlib.reload(common)
    importlib.reload(market_common)
    importlib.reload(returns_validation)
    out = returns_validation.build(n=20)
    assert out["sample"]["line_months"] == 20
    assert out["chain_within_1bp"] == pytest.approx(1.0) and out["monthly_within_1bp"] == pytest.approx(1.0)
    assert out["identity_within_1bp"] > 0.97
    assert set(out["exceptions_by_reason"]) <= {"cash_distribution_convention"}
    assert (lake / "validation" / "returns.json").exists()


def test_allocation_is_even_capped_and_redistributed():
    from atx_db.alpha_panel.returns_validation import allocate
    sizes = {(2019, 1, True): 1, (2019, 2, True): 50, (2019, 1, False): 50, (2019, 2, False): 50}
    a = allocate(20, sizes, event_share=0.2)
    assert sum(a.values()) == 20 and a[(2019, 1, True)] == 1
    assert all(a[k] <= sizes[k] for k in sizes)
