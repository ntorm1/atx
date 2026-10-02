"""metrics.measure on a fixture panel (coverage bases, distribution, no return stats from 2023) and the run() scratch connection."""

from __future__ import annotations

import datetime as dt

import pyarrow as pa
import pyarrow.parquet as pq

from atx_db.alpha_panel import common
from atx_db.alpha_panel import metrics as M


def test_measure_fixture(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    rows = []
    for y in (2022, 2023):
        for i in range(4):
            rows.append({"session_date": dt.date(y, 3, 1), "security_id": i, "sidx": i, "ticker": "T", "member": True,
                         "member_equity": i < 3, "cik": 10 + i if i < 2 else None,
                         "link_tier": "strict" if i == 0 else ("name" if i == 1 else None),
                         "ret": float(i) - 1.0 if i != 2 else float("nan"), "at": None if i == 2 else float(i - 1),
                         "fin_template": "bank" if i == 0 else "other", "currency": "USD"})
    f = tmp_path / "p.parquet"
    pq.write_table(pa.Table.from_pylist(rows), f)
    con = common.connect(memory="200MB", threads=1, db_file="metrics_test.duckdb")
    glob = f.as_posix()
    cov, dist = M.measure(con, glob, M._schema(con, glob), {"at": ("bank",)})
    c = {(r["field"], r["window"]): r for r in cov}
    r = c[("ret", "2022")]
    assert r["cells"] == 3 and r["member_equity"] == round(2 / 3, 4)        # NaN is not finite
    assert r["linked"] == 1.0 and r["linked_strict"] == 1.0 and r["linked_pit"] == 1.0
    a = c[("at", "2022")]
    assert a["member_equity"] == round(2 / 3, 4) and a["excl_structural"] == 0.5   # bank row excluded from basis
    d = {(r["field"], r["window"]): r for r in dist}
    assert ("ret", "2022") in d and ("ret", "2023") not in d                 # no return statistic from 2023 on
    assert ("at", "2023") in d and d[("at", "2022")]["share_zero"] == 0.5
