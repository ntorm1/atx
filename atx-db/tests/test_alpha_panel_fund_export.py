"""Stage F consumer export (fund_export.build) on a fixture stage: v1 contract columns, catalog items joined,
accepted_utc = available_at, NaN-filled items, manifest listing."""

from __future__ import annotations

import datetime as dt
import json
import math

import pyarrow as pa
import pyarrow.parquet as pq

from atx_db.alpha_panel import common
from atx_db.alpha_panel import fund_catalog as fcat
from atx_db.alpha_panel import fund_export as fe
from atx_db.alpha_panel import fundamentals as fu

T = dt.datetime
D = dt.date


def _event(accn, clock, avail, **items):
    row = {f.name: None for f in fu.EVENT_SCHEMA}
    row.update({"cik": 7, "accession": accn, "form": "10-K", "filed": clock.date(), "clock_utc": clock,
                "clock_basis": "fsds_accepted_utc", "period_end": D(2023, 12, 31), "fiscal_year": 2023,
                "fiscal_period": "FY", "currency": "USD", "fin_template": "other", "staleness_days": 200,
                "zero_filled": "", "available_at": avail, "fx_converted": False, "is_amendment": False,
                "is_restated": False, "restated_items": ""})
    row.update(items)
    return row


def test_export_build_fixture(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    monkeypatch.setenv("ATX_FUND_STAGE", "fund_t")
    monkeypatch.setattr(fe, "EXPORT_NAME", "fe_test")
    st = fu.fund_dir()
    c1, c2 = T(2024, 2, 20, 21), T(2024, 5, 1, 21)
    ev_schema = fu.EVENT_SCHEMA.append(pa.field("nonreliance_402_at", pa.timestamp("us")))
    evs = [_event("a", c1, c1, at=100.0, sale_ttm=50.0), _event("b", c2, c2 + dt.timedelta(days=2), at=110.0)]
    pq.write_table(pa.Table.from_pylist(evs, schema=ev_schema), st / "events.parquet")
    cat = [{"cik": 7, "accession": "a", "clock_utc": c1, "available_at": c1, "re": 30.0, "catalog_zero_filled": "tstk",
            "tstk": 0.0}]
    pq.write_table(pa.Table.from_pylist(cat, schema=fu.CATALOG_SCHEMA), st / "catalog.parquet")
    sic = [{"cik": 7, "clock_utc": c1, "sic": 3674, "sic2": 36, "ff12": "BusEq", "ff49": "Chips", "accession": "a",
            "sic_basis": "fsds_sub"}]
    pq.write_table(pa.Table.from_pylist(sic), st / "sic_events.parquet")
    common.write_json_atomic(st / "manifest.json", {"status": "complete", "rule": fu.RULE, "files": {}})
    m = fe.build()
    out = pq.read_table(tmp_path / "export" / "fe_test" / "fundamental_events.parquet")
    names = out.schema.names
    for c in ("cik", "accepted_utc", "accession", "clock_basis", "period_end", "staleness_days", "filing_accepted_utc"):
        assert c in names
    assert all(c in names for c in fcat.CAT_COLUMNS) and m["items"][-len(fcat.CAT_COLUMNS):] == list(fcat.CAT_COLUMNS)
    rows = {r["accession"]: r for r in out.to_pylist()}
    assert rows["a"]["re"] == 30.0 and rows["a"]["tstk"] == 0.0 and math.isnan(rows["b"]["re"])
    assert rows["b"]["accepted_utc"].replace(tzinfo=None) == c2 + dt.timedelta(days=2)       # the FX-aware clock
    assert rows["b"]["filing_accepted_utc"].replace(tzinfo=None) == c2
    assert out.schema.field("staleness_days").type == pa.int32()
    assert all(v is not None for v in out.column("sale_ttm").to_pylist())                  # NaN-filled, never null
    man = json.loads((tmp_path / "export" / "fe_test" / "manifest.json").read_text())
    assert man["schema"] == "atx.fundamental-events/v1" and "fundamental_events.parquet" in man["files"]
