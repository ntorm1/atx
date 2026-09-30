"""Stage F batch pipeline end to end on fixtures: Company Facts rows + FSDS-derived work files -> event, catalog,
history and SIC parts (the SQL of _base_sql: concept join with per-share units, class/pos/label tiers, clocks)."""

from __future__ import annotations

import datetime as dt

import pyarrow as pa
import pyarrow.parquet as pq

from atx_db.alpha_panel import common
from atx_db.alpha_panel import fund_extract as fx
from atx_db.alpha_panel import fund_items as fi
from atx_db.alpha_panel import fundamentals as fu

D = dt.date


def _cf(concept, start, end, value, unit="USD", accn="0001-24-000001", form="10-K", tax="us-gaap"):
    return {"cik": 42, "taxonomy": tax, "concept": concept, "unit": unit, "period_start": start, "period_end": end,
            "filed_date": D(2024, 2, 20), "fiscal_year": 2023, "fiscal_period": "FY", "form": form,
            "accession_number": accn, "value": value}


def test_process_batch_end_to_end(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    monkeypatch.setenv("ATX_FUND_STAGE", "fund_t")
    monkeypatch.delenv("ATX_FX_DAILY", raising=False)
    s, e = D(2023, 1, 1), D(2023, 12, 31)
    rows = [_cf("Assets", None, e, 1000.0), _cf("StockholdersEquity", None, e, 400.0),
            _cf("Revenues", s, e, 800.0), _cf("NetIncomeLoss", s, e, 50.0),
            _cf("EarningsPerShareBasic", s, e, 0.5, unit="USD/shares"),
            _cf("EarningsPerShareBasic", s, e, 999.0, unit="USD"),                  # wrong unit kind: dropped
            _cf("RetainedEarningsAccumulatedDeficit", None, e, 300.0),
            _cf("NetCashProvidedByUsedInOperatingActivities", s, e, 90.0),
            _cf("EntityCommonStockSharesOutstanding", None, D(2024, 2, 1), 100.0, unit="shares", tax="dei")]
    cf_dir = fx.out_dir()
    pq.write_table(pa.Table.from_pylist(rows, schema=fx.SCHEMA), cf_dir / "batch-0000.parquet")
    w = fu.work_dir()
    accepted = dt.datetime(2024, 2, 20, 21, 5)
    pq.write_table(pa.Table.from_pylist([{
        "adsh": "0001-24-000001", "sub_cik": 42, "sic": 3674, "sub_form": "10-K", "sub_fy": 2023, "sub_fp": "FY",
        "sub_filed": D(2024, 2, 20), "accepted_utc": accepted, "sub_quarter": "2024q1"}]), fu.sub_clock_path())
    pq.write_table(pa.table({"adsh": pa.array([], pa.string()), "tag": pa.array([], pa.string()),
                             "ddate": pa.array([], pa.date32()), "qtrs": pa.array([], pa.int32()),
                             "n_members": pa.array([], pa.int64()), "value": pa.array([], pa.float64())}),
                   fu.class_shares_path())
    pq.write_table(pa.table({"adsh": pa.array([], pa.string()), "tag": pa.array([], pa.string()),
                             "ddate": pa.array([], pa.date32()), "qtrs": pa.array([], pa.int32()),
                             "uom": pa.array([], pa.string()), "n_members": pa.array([], pa.int64()),
                             "value": pa.array([], pa.float64())}), fu.pos_sums_path())
    pq.write_table(pa.Table.from_pylist([{"adsh": "0001-24-000001", "concept": "CostOfRevenue", "ddate": e, "qtrs": 4,
                                          "uom": "USD", "n_lines": 1, "n_total": 0, "value": 500.0}]),
                   fu.label_lines_path())
    flags = {f: False for f in fi.PRE_FLAGS} | {"has_is": True, "has_cf": True, "has_bs": True, "is_rev": True}
    pq.write_table(pa.Table.from_pylist([{"adsh": "0001-24-000001", **flags}]), fu.pre_flags_path())
    con = common.connect(memory="200MB", threads=1)
    try:
        fu._register_static(con)
        r = fu.process_batch(con, cf_dir / "batch-0000.parquet")
    finally:
        con.close()
    assert r["events"] == 1 and r["filter_counts"]["label_fallback_rows"] == 1
    ev = pq.read_table(fu.parts_dir() / "events-0000.parquet").to_pylist()[0]
    cat = pq.read_table(fu.parts_dir() / "catalog-0000.parquet").to_pylist()[0]
    assert ev["clock_utc"] == accepted and ev["available_at"] == accepted and ev["fx_converted"] is False
    assert ev["sale_ttm"] == 800.0 and ev["cogs_ttm"] == 500.0 and ev["gp_ttm"] == 300.0     # label tier
    assert ev["is_amendment"] is False and ev["is_restated"] is False
    assert cat["epspx_ttm"] == 0.5 and cat["re"] == 300.0 and cat["tstk"] == 0.0
    assert cat["accession"] == ev["accession"] and "tstk" in cat["catalog_zero_filled"]
    assert fu._batch_done(cf_dir / "batch-0000.parquet", fu.parts_dir() / "batch-0000.json")


def test_prepare_step_cache(tmp_path):
    """A prepare step whose output exists with the same SQL fingerprint is skipped (guard stops resume prepare);
    a changed query recomputes."""
    import duckdb

    con = duckdb.connect()
    try:
        dest = tmp_path / "x.parquet"
        r: dict = {}
        assert fu.cached_copy(con, "SELECT 1 AS a UNION ALL SELECT 2", dest, r, "x") == 2 and r["cached"] == []
        r2: dict = {}
        assert fu.cached_copy(con, "SELECT 1 AS a UNION ALL SELECT 2", dest, r2, "x") == 2 and r2["cached"] == ["x"]
        r3: dict = {}
        assert fu.cached_copy(con, "SELECT 5 AS a", dest, r3, "x") == 1 and r3["cached"] == []
    finally:
        con.close()
