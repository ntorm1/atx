"""Stage F validation suite (fund_validate) on fixtures: TTM gaps, FX shares, top-3000 coverage, FSDS benchmark,
A = L + E, the cutoff rebuild through the as-of views, and the member-cell exit measurement."""

from __future__ import annotations

import datetime as dt
from decimal import Decimal

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

from atx_db.alpha_panel import common
from atx_db.alpha_panel import fund_catalog as fcat
from atx_db.alpha_panel import fund_extract as fx
from atx_db.alpha_panel import fund_items as fi
from atx_db.alpha_panel import fund_validate as fv
from atx_db.alpha_panel import fundamentals as fu

T = dt.datetime
D = dt.date
EV_SCHEMA = fu.EVENT_SCHEMA.append(pa.field("nonreliance_402_at", pa.timestamp("us")))


def _ev(cik, accn, clock, pe, form="10-Q", **kw):
    r = {f.name: None for f in EV_SCHEMA}
    r.update({"cik": cik, "accession": accn, "form": form, "filed": clock.date(), "clock_utc": clock,
              "available_at": clock, "clock_basis": "fsds_accepted_utc", "period_end": pe, "fiscal_period": "FY"
              if form == "10-K" else "Q1", "currency": "USD", "fin_template": "other", "staleness_days": 200,
              "fx_converted": False, "is_amendment": False, "is_restated": False, "restated_items": "",
              "zero_filled": ""})
    r.update(kw)
    return r


def _write_stage(st, events, catalog=()):
    pq.write_table(pa.Table.from_pylist(events, schema=EV_SCHEMA), st / "events.parquet")
    cat = [{f.name: e.get(f.name) for f in fu.CATALOG_SCHEMA} | dict(c) for e, c in catalog] or [
        {f.name: e.get(f.name) for f in fu.CATALOG_SCHEMA} for e in events]
    pq.write_table(pa.Table.from_pylist(cat, schema=fu.CATALOG_SCHEMA), st / "catalog.parquet")


def test_ttm_gaps_excludes_short_histories(tmp_path):
    evs = []
    for k, m in enumerate((3, 6, 9, 12)):                   # issuer 1: four quarters, TTM missing at the 4th
        pe = D(2021, m, 28)
        evs.append(_ev(1, f"a{k}", T(2021, m, 28) + dt.timedelta(days=40), pe, sale_q=1.0,
                       sale_ttm=None if k == 3 else 4.0))
    evs.append(_ev(2, "b0", T(2021, 5, 1), D(2021, 3, 31), sale_q=1.0))  # issuer 2: one quarter, no TTM
    _write_stage(tmp_path, evs)
    con = duckdb.connect()
    r = fv.ttm_gaps(con, tmp_path / "events.parquet")["sale"]
    assert (r["events_with_q"], r["missing_ttm"]) == (5, 2)
    assert (r["events_with_q_excl_lt4q"], r["missing_ttm_excl_lt4q"]) == (1, 1)


def test_balance_identity_and_classes(tmp_path):
    pe = D(2022, 12, 31)
    evs = [_ev(1, "ok", T(2023, 2, 1), pe, **{"at": 100e6, "lt": 60e6, "seq": 40e6}),
           _ev(2, "mez", T(2023, 2, 1), pe, **{"at": 100e6, "lt": 50e6, "seq": 40e6}),
           _ev(3, "nolt", T(2023, 2, 1), pe, **{"at": 100e6, "seq": 40e6})]
    cat = [(evs[0], {"teq": 40e6}), (evs[1], {"mibt": 10e6, "lse": 100e6}), (evs[2], {})]
    _write_stage(tmp_path, evs, cat)
    r = fv.balance(duckdb.connect(), tmp_path)
    assert r["issuer_periods"] == 3 and r["ok"] == 2 and r["residual_classes"] == {"lt_missing": 1}


def test_structural_sql_predicate():
    con = duckdb.connect()
    q = "SELECT {} FROM (SELECT 'bank' AS fin_template, 6211 AS sic_in_force)"
    assert con.execute(q.format(fv._structural_sql("gp_ttm"))).fetchone()[0] is True
    assert con.execute(q.format(fv._structural_sql("dptc"))).fetchone()[0] is True      # broker: no deposits
    assert con.execute(q.format(fv._structural_sql("pcl_ttm"))).fetchone()[0] is True   # 6211 outside 6000-6199
    assert con.execute(q.format(fv._structural_sql("re"))).fetchone()[0] is False


def test_exit_measure_linked_usd_basis(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    days = [D(2021, 3, 1) + dt.timedelta(days=k) for k in range(5)]
    pq.write_table(pa.table({"session_date": pa.array(days, pa.date32())}), tmp_path / "calendar.parquet")
    (tmp_path / "_tmp" / "panel_member").mkdir(parents=True)
    mem = [{"session_date": d, "security_id": s, "member": True} for d in days[1:] for s in (10, 20, 30)]
    pq.write_table(pa.Table.from_pylist(mem), tmp_path / "_tmp" / "panel_member" / "year=2021.parquet")
    (tmp_path / "identity").mkdir()
    lt = [{"security_id": 10, "cik": 1, "valid_from": D(2020, 1, 1), "valid_to": None, "link_tier": "strict",
           "is_issuer_primary": True},
          {"security_id": 20, "cik": 2, "valid_from": D(2020, 1, 1), "valid_to": None, "link_tier": "strict",
           "is_issuer_primary": True}]
    pq.write_table(pa.Table.from_pylist(lt, schema=pa.schema([
        ("security_id", pa.int64()), ("cik", pa.int64()), ("valid_from", pa.date32()), ("valid_to", pa.date32()),
        ("link_tier", pa.string()), ("is_issuer_primary", pa.bool_())])), tmp_path / "identity" / "link_table.parquet")
    st = tmp_path / "stage_x"
    st.mkdir()
    evs = [_ev(1, "a", T(2021, 2, 10), D(2020, 12, 31), gp_ttm=5.0, sale_ttm=9.0),
           _ev(2, "b", T(2021, 3, 2, 23), D(2020, 12, 31), gp_ttm=None, sale_ttm=3.0)]   # visible from 03-04 only
    _write_stage(st, evs)
    r = fv.exit_measure(duckdb.connect(), st, years=(2021,))
    y = r["by_year"]["2021"]
    assert y["member_cells"] == 12 and y["linked_cells"] == 8
    # sale: cik 1 on 4 sessions, cik 2 on 03-04 and 03-05 (cutoff 03-03 22:00 > 03-02 23:00) -> 6 of 6 USD cells
    assert y["sale_ttm"]["linked_usd_cells"] == 6 and y["sale_ttm"]["linked_usd"] == 1.0
    assert y["gp_ttm"]["linked_usd"] == round(4 / 6, 4)
    assert y["sale_ttm"]["linked"] == round(6 / 8, 4)                                    # linked, not yet visible
    assert y["sale_ttm"]["member_all"] == round(6 / 12, 4) and y["sale_ttm"]["member_all_cells"] == 12
    # v9-style stage: no available_at column -> clock_utc is the visibility clock
    st9 = tmp_path / "stage_v9"
    st9.mkdir()
    t = pq.read_table(st / "events.parquet")
    pq.write_table(t.drop_columns(["available_at"]), st9 / "events.parquet")
    y9 = fv.exit_measure(duckdb.connect(), st9, years=(2021,))["by_year"]["2021"]
    assert y9["sale_ttm"]["linked_usd"] == y["sale_ttm"]["linked_usd"]
    r2 = fv.exit_measure(duckdb.connect(), st, years=(2021,), extra_fields=("ni_ttm",))
    assert r2["by_year"]["2021"]["ni_ttm"]["linked"] == 0.0 and "gp_ttm" in r2["by_year"]["2021"]


def test_benchmark_own_period_cells(monkeypatch, tmp_path):
    fsds = tmp_path / "fsds"
    (fsds / "num").mkdir(parents=True)
    (fsds / "sub").mkdir()
    monkeypatch.setattr(common, "FSDS_DIR", fsds)
    sub = [{"adsh": "k1", "cik": "5", "form": "10-K", "period": D(2023, 12, 31), "fy": 2023}]
    pq.write_table(pa.Table.from_pylist(sub), fsds / "sub" / "2024q1.parquet")
    ns = pa.schema([("adsh", pa.string()), ("tag", pa.string()), ("version", pa.string()), ("ddate", pa.date32()),
                    ("qtrs", pa.int32()), ("uom", pa.string()), ("segments", pa.string()), ("coreg", pa.string()),
                    ("value", pa.decimal128(28, 4))])

    def n(tag, q, v, dd=D(2023, 12, 31), uom="USD"):
        return {"adsh": "k1", "tag": tag, "version": "us-gaap/2023", "ddate": dd, "qtrs": q, "uom": uom,
                "segments": None, "coreg": None, "value": Decimal(str(v))}
    num = [n("Assets", 0, 1000), n("Revenues", 4, 800), n("RevenueFromContractWithCustomerExcludingAssessedTax", 4, 790),
           n("NetIncomeLoss", 4, 51), n("Assets", 0, 900, dd=D(2022, 12, 31)), n("GrossProfit", 4, 300)]
    pq.write_table(pa.Table.from_pylist(num, schema=ns), fsds / "num" / "2024q1.parquet")
    st = tmp_path / "st"
    st.mkdir()
    _write_stage(st, [_ev(5, "k1", T(2024, 2, 20), D(2023, 12, 31), form="10-K",
                          **{"at": 1000.0, "sale_ttm": 800.0, "ni_ttm": 50.0, "gp_ttm": None})])
    r = fv.benchmark(duckdb.connect(), st)
    assert r["cells"] == 4                                                  # the comparative and the ASC 606 cell drop
    assert r["mapped"] == 3 and r["matched"] == 2                           # ni 50 vs 51 is outside 0.5%
    assert r["by_item"]["gp_ttm"]["mapped"] == 0.0


def test_cutoff_rebuild_matches_asof_views(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    monkeypatch.setenv("ATX_FUND_STAGE", "fund_t")
    monkeypatch.setenv("ATX_FX_DAILY", str(tmp_path / "none.parquet"))
    rows = []
    for k, (y, v) in enumerate(((2021, 100.0), (2022, 110.0), (2023, 120.0))):
        s, e = D(y, 1, 1), D(y, 12, 31)
        accn = f"000{k}-{y}"
        for concept, st_, val in (("Assets", None, v * 10), ("Revenues", s, v), ("NetIncomeLoss", s, v / 10)):
            rows.append({"cik": 9, "taxonomy": "us-gaap", "concept": concept, "unit": "USD", "period_start": st_,
                         "period_end": e, "filed_date": D(y + 1, 2, 20), "fiscal_year": y, "fiscal_period": "FY",
                         "form": "10-K", "accession_number": accn, "value": val})
    pq.write_table(pa.Table.from_pylist(rows, schema=fx.SCHEMA), fx.out_dir() / "batch-0000.parquet")
    subs = [{"adsh": f"000{k}-{y}", "sub_cik": 9, "sic": 3674, "sub_form": "10-K", "sub_fy": y, "sub_fp": "FY",
             "sub_filed": D(y + 1, 2, 20), "accepted_utc": T(y + 1, 2, 20, 21), "sub_quarter": "q"}
            for k, y in enumerate((2021, 2022, 2023))]
    pq.write_table(pa.Table.from_pylist(subs), fu.sub_clock_path())
    for path, schema in ((fu.class_shares_path(), [("adsh", pa.string()), ("tag", pa.string()), ("ddate", pa.date32()),
                                                   ("qtrs", pa.int32()), ("n_members", pa.int64()), ("value", pa.float64())]),
                         (fu.pos_sums_path(), [("adsh", pa.string()), ("tag", pa.string()), ("ddate", pa.date32()),
                                               ("qtrs", pa.int32()), ("uom", pa.string()), ("n_members", pa.int64()),
                                               ("value", pa.float64())]),
                         (fu.label_lines_path(), [("adsh", pa.string()), ("concept", pa.string()), ("ddate", pa.date32()),
                                                  ("qtrs", pa.int32()), ("uom", pa.string()), ("n_lines", pa.int64()),
                                                  ("n_total", pa.int64()), ("value", pa.float64())])):
        pq.write_table(pa.schema(schema).empty_table(), path)
    pq.write_table(pa.Table.from_pylist([{"adsh": "x", **{f: False for f in fi.PRE_FLAGS}}]), fu.pre_flags_path())
    con = common.connect(memory="200MB", threads=1)
    fu._register_static(con)
    fu.process_batch(con, fx.out_dir() / "batch-0000.parquet")
    st = fu.fund_dir()
    parts = fu.parts_dir()
    con.execute(f"COPY (SELECT *, CAST(NULL AS TIMESTAMP) AS nonreliance_402_at FROM read_parquet('{parts.as_posix()}/events-*.parquet')) "
                f"TO '{(st / 'events.parquet').as_posix()}' (FORMAT PARQUET)")
    for name, glob in (("catalog.parquet", "catalog-*"), ("quarterly_history.parquet", "history-*")):
        con.execute(f"COPY (SELECT * FROM read_parquet('{parts.as_posix()}/{glob}.parquet')) "
                    f"TO '{(st / name).as_posix()}' (FORMAT PARQUET)")
    r = fv.cutoff(con, st, n_events=3, seed=1)
    con.close()
    assert r["events_sampled"] == 3 and r["compared_rows"] > 3 and r["differences"] == 0, r["examples"]
    assert len(fcat.CAT_COLUMNS) > 0


def test_validate_memory_env_override(monkeypatch):
    """ATX_FUND_VALIDATE_MEM overrides the default DuckDB memory limit (real-data coverage OOMed at 350MB)."""
    import importlib
    monkeypatch.setenv("ATX_FUND_VALIDATE_MEM", "480MB")
    try:
        assert importlib.reload(fv).MEM == "480MB"
    finally:
        monkeypatch.delenv("ATX_FUND_VALIDATE_MEM")
        assert importlib.reload(fv).MEM == "350MB"
