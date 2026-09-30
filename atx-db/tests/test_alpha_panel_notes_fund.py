"""Stage ``fundamentals_notes`` (S4.4: segments and notes items) on the notes fixture zip."""

from __future__ import annotations

import datetime as dt

import duckdb

from atx_db.alpha_panel import fund_notes as FN
from atx_db.alpha_panel import notes_fetch as N
from tests.test_alpha_panel_notes import A, B, D, E, lake  # noqa: F401  (fixture)

UTC = dt.UTC


# ---------------------------------------------------------------- fund_notes
def test_fp_quarters_and_recon_class_twins() -> None:
    assert [FN.fp_quarters(fp, f) for fp, f in [("FY", "10-K"), ("Q1", "10-Q"), ("H1", "10-Q"), ("M9", "10-Q"),
                                               (None, "10-K/A"), (None, "10-Q")]] == [4, 1, 2, 3, 4, None]
    cases = [(100.0, 100.5, None, 2), (120.0, 100.0, -20.0, 2), (120.0, 100.0, 20.0, 2), (60.0, 100.0, None, 2),
             (130.0, 100.0, None, 2), (200.0, 100.0, None, 3), (100000.0, 100.0, None, 2), (None, 100.0, None, 0),
             (50.0, None, None, 2), (0.0, 0.0, None, 2), (5.0, 0.0, None, 2)]
    con = duckdb.connect()
    for s, t, e, n in cases:
        sql = FN.recon_class_sql("?", "?", "?", "?").replace("?", "{}")
        lit = [("NULL" if x is None else f"CAST({x} AS DOUBLE)") for x in (s, t, e)] + [str(n)]
        want = FN.recon_class(s, t, e, n)
        got = con.execute("SELECT " + FN.recon_class_sql(lit[0], lit[1], lit[2], lit[3])).fetchone()[0]
        assert got == want, (s, t, e, n, sql)
    assert [FN.recon_class(*c) for c in cases] == [
        "direct", "with_reconciling", "with_reconciling", "segments_below_total", "segments_exceed_total",
        "double_count", "scale_or_unit", "no_segment_revenue", "no_total", "direct", "zero_total"]


def test_fund_notes_build_segments_recon_and_wide(lake) -> None:  # noqa: F811
    rec = FN.build()
    out = N.C.stage_dir(FN.STAGE)
    con = duckdb.connect()
    con.execute("SET TimeZone='UTC'")
    rc = {r[0]: r[1:] for r in con.execute(f"""
        SELECT adsh, n_members, multi_segment, revenue_tag, n_revenue_members, seg_sum, consolidated_revenue, recon_class,
               available_at, reconciling_items
        FROM read_parquet('{(out / 'segment_recon.parquet').as_posix()}')""").fetchall()}
    assert rc[A][:7] == (2, True, "Revenues", 2, 100.0, 100.0, "direct")
    assert rc[A][7] == dt.datetime(2021, 2, 26, 21, 5, tzinfo=UTC)
    assert rc[D][0] == 2 and rc[D][6] == "with_reconciling" and rc[D][8] == -20.0     # SegX, SegY; Corporate excluded
    assert rc[E][6] == "segments_below_total"
    assert B not in rc                                                                  # 10-Q: no reconciliation row
    segs = con.execute(f"""SELECT metric, axis_kind, member, consolidation_member, value FROM
                           read_parquet('{(out / 'segments.parquet').as_posix()}') WHERE adsh = '{A}' ORDER BY 1, 2, 3, 4""").fetchall()
    assert ("revenue", "business", "Alpha", None, 60.0) in segs and ("revenue", "geographic", "US", None, 80.0) in segs
    assert ("revenue", "business", "Beta", "OperatingSegments", 40.0) in segs
    assert not any(v == 25.0 for *_, v in segs)                                          # segment x product excluded
    assert ("operating_income", "business", "Alpha", None, 12.0) in segs and ("assets", "business", "Alpha", None, 300.0) in segs
    n_rev = con.execute(f"""SELECT count(*) FROM read_parquet('{(out / 'items' / 'year=*' / 'items.parquet').as_posix()}')
                            WHERE adsh = '{A}' AND tag = 'Revenues' AND segments IS NULL AND ddate = DATE '2020-12-31'""").fetchone()[0]
    assert n_rev == 1                                                                    # iprx repeat read once
    w = con.execute(f"SELECT * FROM read_parquet('{(out / 'notes_wide.parquet').as_posix()}') WHERE adsh = '{A}'").fetchdf().iloc[0]
    assert (w.debt_mat_y1, w.debt_mat_y2_5, w.debt_mat_after5) == (10.0, 50.0, 50.0)
    assert w.sbc == 7.0 and w.sbc_expense == 6.5 and w.tax_cur_federal == 3.0 and w.tax_def_federal == -0.5
    assert w.pension_pbo == 200.0 and w.pension_basis == "defined_benefit_pension_member"
    assert w.gw_impairment == 5.0 and w.intang_impairment == 2.0 and w.oplease_liab == 33.0 and w.flow_qtrs == 4
    assert w.currency == "USD"
    wb = con.execute(f"SELECT sbc, flow_qtrs, oplease_liab FROM read_parquet('{(out / 'notes_wide.parquet').as_posix()}') "
                     f"WHERE adsh = '{B}'").fetchone()
    assert wb == (4.0, 2, 9.0)                                                          # YTD flow of a Q2 10-Q
    cov = rec["coverage"]["segments"]
    assert cov["2021"]["multi_segment"] == 1 and cov["2021"]["share_segment_revenue"] == 1.0
    assert cov["2022"]["multi_segment"] == 2 and cov["2022"]["share_reconciled"] == 0.5
    man = (out / "manifest.json").read_text()
    assert '"input_manifests_sha256"' in man and "segment_recon.parquet" in man
