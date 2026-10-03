"""Stage ``fundamentals_notes`` (S4.4: segments and notes items) on the notes fixture zip."""

from __future__ import annotations

import datetime as dt
import hashlib
from types import SimpleNamespace

import duckdb
import pytest

from atx_db.alpha_panel import fund_notes as FN
from atx_db.alpha_panel import notes_fetch as N
from tests.test_alpha_panel_notes import SUB_COLS, A, B, D, E, _sub, fixture_tables, lake, write_zip  # noqa: F401

UTC = dt.UTC
F6, G7, H8, I9 = "0000000006-22-000006", "0000000007-22-000007", "0000000001-21-000008", "0000000009-22-000009"
J10, K11, L12 = "0000000010-23-000010", "0000000011-23-000011", "0000000012-23-000012"


def _numd(tables: dict, adsh: str, tag: str, ddate: str, qtrs: int, value: str, segments: str = "",
          custom: bool = False) -> None:
    """Append a NUM row with an arbitrary DIM string (and its DIM row) to fixture tables (``custom``: a filer
    extension tag, version = adsh)."""
    h = "0x00000000" if not segments else "t14_" + hashlib.md5(segments.encode()).hexdigest()[:12]
    if segments and not any(r[0] == h for r in tables["dim"]):
        tables["dim"].append([h, segments, "0"])
    tables["num"].append([adsh, tag, adsh if custom else "us-gaap/2020", ddate, str(qtrs), "USD", h, "0", value, "", "0",
                          str(segments.count(";")), "", "0.0", "0.0", "-6"])


def seg_tables() -> dict:
    """The notes fixture plus the segment-feature cases: customer, foreign, 10-Q quarter fallback, a Canadian
    filer, out-of-range shares, a later 10-K/A of A with one lease fact."""
    t = fixture_tables()
    f = _sub(F6, 6, "10-K", "20211231", "2021", "FY", "20220303", "2022-03-03 10:00:00.0")
    f[SUB_COLS.index("countryba")] = "CA"
    t["sub"] += [f, _sub(G7, 7, "10-K", "20211231", "2021", "FY", "20220304", ""),
                 _sub(H8, 1, "10-K/A", "20201231", "2020", "FY", "20210415", "2021-04-15 10:00:00.0"),
                 _sub(I9, 9, "10-K", "20211231", "2021", "FY", "20220307", "2022-03-07 16:30:00.0"),
                 _sub(J10, 10, "10-K", "20221231", "2022", "FY", "20230301", "2023-03-01 16:00:00.0"),
                 _sub(K11, 11, "10-K", "20221231", "2022", "FY", "20230302", "2023-03-02 16:00:00.0"),
                 _sub(L12, 12, "10-K", "20221231", "2022", "FY", "20230303", "2023-03-03 16:00:00.0")]
    cust = "ConcentrationRiskByBenchmark=SalesRevenueNet;ConcentrationRiskByType=CustomerConcentrationRisk;"
    _numd(t, A, "Revenues", "20201231", 4, "15.0", "MajorCustomers=CustomerA;")
    _numd(t, A, "Revenues", "20201231", 4, "12.0", cust + "MajorCustomers=CustomerB;")
    _numd(t, A, "Revenues", "20201231", 4, "50.0", "MajorCustomers=Residential;")           # customer class
    _numd(t, A, "Revenues", "20201231", 4, "14.0", "ConcentrationRiskByType=SupplierConcentrationRisk;MajorCustomers=VendorX;")
    _numd(t, B, "Revenues", "20210630", 1, "40.0")                                           # 10-Q: discrete quarter only
    _numd(t, B, "Revenues", "20210630", 1, "30.0", "BusinessSegments=Alpha;")
    _numd(t, B, "Revenues", "20210630", 1, "10.0", "BusinessSegments=Beta;")
    _numd(t, E, "Revenues", "20211231", 4, "40.0", "Geographical=NonUs;")
    _numd(t, F6, "Revenues", "20211231", 4, "100.0")
    _numd(t, F6, "Revenues", "20211231", 4, "30.0", "Geographical=CA;")
    _numd(t, F6, "Revenues", "20211231", 4, "70.0", "Geographical=US;")
    _numd(t, F6, "Revenues", "20211231", 4, "60.0", "MajorCustomers=Residential;")
    _numd(t, G7, "Revenues", "20211231", 4, "100.0")
    _numd(t, G7, "Revenues", "20211231", 4, "150.0", "Geographical=US;")
    _numd(t, G7, "Revenues", "20211231", 4, "120.0", "MajorCustomers=CustomerA;")
    _numd(t, G7, "LongTermDebtMaturitiesRepaymentsOfPrincipalInYearTwo", "20211231", 0, "5.0")
    _numd(t, G7, "LongTermDebtMaturitiesRepaymentsOfPrincipalRemainderOfFiscalYear", "20211231", 0, "3.0")
    _numd(t, H8, "OperatingLeaseLiability", "20201231", 0, "35.0")
    _numd(t, A, "OperatingLeaseWeightedAverageDiscountRatePercent", "20201231", 0, "0.045")
    _numd(t, G7, "OperatingLeaseWeightedAverageDiscountRatePercent", "20211231", 0, "4.5")      # 4.5%: NULL
    _numd(t, I9, "Revenues", "20211231", 4, "95.0")                                          # Group = SubA + SubB
    for m, v in (("Group", "70.0"), ("SubA", "40.0"), ("SubB", "30.0"), ("Other", "25.0")):
        _numd(t, I9, "Revenues", "20211231", 4, v, f"BusinessSegments={m};")
    _numd(t,I9,'Revenues','20211231',4,'10','ConsolidationItems=CorporateNonSegment;')
    _numd(t,I9,'Revenues','20211231',4,'-10','ConsolidationItems=IntersegmentElimination;')
    prem = "SupplementaryInsuranceInformationPremiumRevenue"                                 # tier 2 (insurer)
    _numd(t, J10, "Revenues", "20221231", 4, "100.0")
    _numd(t, J10, prem, "20221231", 4, "60.0", "BusinessSegments=Alpha;")
    _numd(t, J10, prem, "20221231", 4, "40.0", "BusinessSegments=Beta;")
    _numd(t, K11, "Revenues", "20221231", 4, "50.0")                                          # tier 3 (extension)
    _numd(t, K11, "SegmentRevenue", "20221231", 4, "35.0", "BusinessSegments=SegA;", custom=True)
    _numd(t, K11, "SegmentRevenue", "20221231", 4, "15.0", "BusinessSegments=SegB;", custom=True)
    _numd(t, K11, "SegmentCostOfSales", "20221231", 4, "9.0", "BusinessSegments=SegA;", custom=True)  # not revenue
    rfc = "RevenueFromContractWithCustomerExcludingAssessedTax"                              # zero 'Revenues' total
    _numd(t, L12, "Revenues", "20221231", 4, "0.0")
    _numd(t, L12, "Revenues", "20221231", 4, "5.0", "BusinessSegments=X;")
    _numd(t, L12, "Revenues", "20221231", 4, "3.0", "BusinessSegments=Y;")
    _numd(t, L12, rfc, "20221231", 4, "100.0")
    _numd(t, L12, rfc, "20221231", 4, "60.0", "BusinessSegments=X;")
    _numd(t, L12, rfc, "20221231", 4, "40.0", "BusinessSegments=Y;")
    # A partial amendment repeats the consolidated total without a segment disclosure.
    _numd(t, H8, "Revenues", "20201231", 4, "105.0")
    for cik, accession, total, values in [
        (13, "0000000013-24-000013", 90, [50, 50, 40]),
        (14, "0000000014-24-000014", 30, [10, 20, 30]),
    ]:
        t['sub'].append(_sub(accession, cik, '10-K', '20231231', '2023', 'FY', '20240301', '2024-03-01 10:00:00.0'))
        _numd(t, accession, 'Revenues', '20231231', 4, str(total))
        for name, value in zip(['A', 'B', 'C'], values):
            _numd(t, accession, 'Revenues', '20231231', 4, str(value), f'BusinessSegments={name};')
    for cik in (15,16):
        accession = f'00000000{cik:02d}-25-0000{cik:02d}'
        t['sub'].append(_sub(accession,cik,'10-K','20241231','2024','FY','20250301','2025-03-01 10:00:00.0'))
        _numd(t,accession,'Revenues','20241231',4,'100')
        for member, point, over in [('A',40,20),('B',30,10)]:
            for timing,value in [('TransferredAtPointInTime',point),('TransferredOverTime',over)]:
                if cik==16 and timing=='TransferredOverTime':
                    continue
                _numd(t,accession,'Revenues','20241231',4,str(value),
                      f'BusinessSegments={member};TimingOfTransferOfGoodOrService={timing};')
    # Unknown metric facts still establish a multi-segment disclosure denominator.
    accession='0000000017-25-000017'
    t['sub'].append(_sub(accession,17,'10-K','20241231','2024','FY','20250301','2025-03-01 10:00:00.0'))
    for member in ('A','B'):
        _numd(t,accession,'SegmentUnmappedMetric','20241231',0,'5',f'BusinessSegments={member};', custom=True)
    for cik,container in [(18,'ReportableLegalEntities'),(19,'ReportableGeographicalComponents'),(20,'ReportableGeographicalComponents')]:
        accession=f'00000000{cik:02d}-26-0000{cik:02d}'
        t['sub'].append(_sub(accession,cik,'10-K','20251231','2025','FY','20260301','2026-03-01 10:00:00.0'))
        _numd(t,accession,'Revenues','20251231',4,'100')
        for member,value in [('A',60),('B',40)]:
            _numd(t,accession,'Revenues','20251231',4,str(value),f'BusinessSegments={member};ConsolidationItems={container};')
    return t


@pytest.fixture()
def lake_seg(tmp_path, monkeypatch):
    """A fixture lake with :func:`seg_tables` parsed as ``2021q1``, plus a calendar, panel rows and a link table."""
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    monkeypatch.setattr(N, "RAW_DIR", tmp_path / "raw")
    monkeypatch.setattr(N, "ARCHIVE_RECEIPTS", tmp_path / "archive")
    monkeypatch.setattr(N, "MIN_FREE_GB", 0.0)
    (tmp_path / "raw").mkdir()
    z = write_zip(tmp_path / "raw" / "2021q1_notes.zip", seg_tables())
    res = N.parse_archive(z, "2021q1", tmp_path / "work")
    N.ledger().append({"key": "2021q1", "kind": "zip", "status": "parsed", "url": "u", "bytes": z.stat().st_size,
                       "sha256": "x", "span": ["2021-01-01", "2021-03-31"], "repost": 0, **res})
    N.finalize()
    root = N.C.build_root()
    con = duckdb.connect()
    days = ["2021-02-25", "2021-02-26", "2021-03-30", "2021-03-31", "2021-04-29", "2021-04-30", "2022-09-29", "2022-09-30"]
    con.execute(f"COPY (SELECT CAST(d AS DATE) AS session_date FROM (VALUES {', '.join(f'(DATE {d!r})' for d in days)}) v(d)) "
                f"TO '{(root / 'calendar.parquet').as_posix()}' (FORMAT PARQUET)")
    for y, rows in {2021: [("2021-02-26", 101, True), ("2021-03-30", 101, True), ("2021-03-31", 101, True),
                           ("2021-03-31", 102, True), ("2021-03-31", 104, False), ("2021-04-30", 101, True)],
                    2022: [("2022-09-30", 101, True)]}.items():
        pm = root / "panel" / f"year={y}"
        pm.mkdir(parents=True)
        vals = ", ".join(f"(DATE '{d}', {s}::BIGINT, NULL::BIGINT, {m})" for d, s, m in rows)
        con.execute(f"COPY (SELECT * FROM (VALUES {vals}) v(session_date, security_id, cik, member_equity)) "
                    f"TO '{(pm / 'panel-01.parquet').as_posix()}' (FORMAT PARQUET)")
    (root / "identity").mkdir(parents=True, exist_ok=True)
    con.execute(f"""COPY (SELECT 101::BIGINT AS security_id, 1::BIGINT AS cik, DATE '2019-01-01' AS valid_from,
                                 NULL::DATE AS valid_to, true AS is_issuer_primary)
                    TO '{(root / 'identity' / 'link_table_v3.parquet').as_posix()}' (FORMAT PARQUET)""")
    return tmp_path


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


# ---------------------------------------------------------------- segment features (tier1-v4 Task 14)
def _features(out) -> dict:
    con = duckdb.connect()
    con.execute("SET TimeZone='UTC'")
    cur = con.execute(f"SELECT * FROM read_parquet('{(out / 'segment_features.parquet').as_posix()}')")
    cols = [c[0] for c in cur.description]
    rows = [SimpleNamespace(**dict(zip(cols, r, strict=True))) for r in cur.fetchall()]
    return {r.adsh: r for r in rows}


def test_segment_features_segments_geo_customers(lake_seg) -> None:
    FN.build()
    x = _features(N.C.stage_dir(FN.STAGE))
    a = x[A]
    assert (a.bseg_n, a.bseg_n_tagged, a.bseg_rev_n, a.bseg_rev_tag, a.bseg_recon_class) == (2, 2, 2, "Revenues", "direct")
    assert a.bseg_rev_hhi == pytest.approx(0.6 ** 2 + 0.4 ** 2)               # Beta read once (segment-only context)
    assert (a.foreign_sales_share, a.foreign_basis, a.geo_rev_n) == (pytest.approx(0.2), "domestic_member", 1)
    assert (a.major_cust_n, a.major_cust_share) == (2, pytest.approx(0.15))   # class and supplier rows excluded
    assert a.rev_consolidated == 100.0 and bool(a.is_annual) and not bool(a.is_amendment) and a.carry_n == 0
    assert a.available_at == dt.datetime(2021, 2, 26, 21, 5, tzinfo=UTC) and a.clock_basis == "accepted_utc"
    d = x[D]                                                                  # Corporate member is not a segment
    assert (d.bseg_n, d.bseg_rev_n, d.bseg_recon_class) == (2, 2, "with_reconciling")
    assert d.bseg_rev_hhi == pytest.approx((70 ** 2 + 50 ** 2) / 120 ** 2) and d.geo_rev_n is None
    e = x[E]
    assert e.bseg_rev_hhi == pytest.approx(0.5)
    assert (e.foreign_sales_share, e.foreign_basis) == (pytest.approx(0.4), "foreign_member")   # NonUs / total
    f = x[F6]                                                                 # Canadian filer: CA is domestic
    assert (f.countryba, f.foreign_sales_share, f.foreign_basis) == ("CA", pytest.approx(0.7), "domestic_member")
    assert (f.bseg_n, f.bseg_n_tagged, f.bseg_rev_hhi) == (1, 0, 1.0)          # single segment by default
    assert f.major_cust_n is None and f.major_cust_share is None              # only a customer class tagged
    g = x[G7]
    assert g.foreign_sales_share is None and g.foreign_basis == "out_of_range"  # US 150 > total 100
    assert g.major_cust_n == 1 and g.major_cust_share is None                   # customer above total
    assert g.clock_basis == "filed_plus_46h" and g.available_at == dt.datetime(2022, 3, 5, 22, 0, tzinfo=UTC)
    b = x[B]                                                                  # 10-Q: discrete-quarter fallback
    assert (b.bseg_n, b.bseg_rev_n, b.flow_qtrs, bool(b.is_annual)) == (2, 2, 2, False)
    assert b.bseg_rev_hhi == pytest.approx((30 ** 2 + 10 ** 2) / 40 ** 2) and b.stkco == 4.0 and b.oplease_liab == 9.0


def test_segment_features_debt_ladder_leases_sbc(lake_seg) -> None:
    FN.build()
    x = _features(N.C.stage_dir(FN.STAGE))
    a = x[A]
    assert (a.dd1, a.dd2, a.dd3, a.dd4, a.dd5, a.dd_after5) == (10.0, 11.0, 12.0, 13.0, 14.0, 50.0)
    assert (a.dd_n_buckets, a.dd_sched_total) == (6, 110.0) and a.dd1_share == pytest.approx(10 / 110)
    assert (a.oplease_liab, a.oplease_cost, a.stkco, a.mrc1) == (33.0, 8.0, 7.0, None)
    g = x[G7]                                                                 # remainder bucket counts when dd1 absent
    assert (g.dd2, g.dd_rem_fy, g.dd_n_buckets, g.dd_sched_total, g.dd1_share) == (5.0, 3.0, 1, 8.0, None)
    assert a.oplease_rate == 0.045 and g.oplease_rate is None                 # a rate must be a decimal in [0, 1]


def test_segment_features_amendment_is_new_vintage_with_carry(lake_seg) -> None:
    FN.build()
    x = _features(N.C.stage_dir(FN.STAGE))
    h, a = x[H8], x[A]
    assert bool(h.is_amendment) and h.cik == a.cik and h.period == a.period
    assert h.available_at == dt.datetime(2021, 4, 15, 14, 0, tzinfo=UTC)
    assert h.oplease_liab == 35.0 and a.oplease_liab == 33.0                  # own value; the original row is unchanged
    assert (h.bseg_n, h.bseg_rev_hhi, h.foreign_sales_share, h.major_cust_share, h.dd1) == (
        a.bseg_n, a.bseg_rev_hhi, a.foreign_sales_share, a.major_cust_share, a.dd1)
    assert (h.oplease_cost, h.stkco, h.oplease_rate, h.carry_n) == (8.0, 7.0, 0.045, 7)   # 4 blocks + 3 columns
    assert C8_not_in(x)


def C8_not_in(x: dict) -> bool:
    return all(not k.startswith("0000000003") for k in x)                    # an 8-K never gets a row


def test_segment_features_member_coverage_pit_and_gates(lake_seg) -> None:
    rec = FN.build()
    cov = rec["coverage"]
    m = cov["cells"]["member_equity"]["by_year"]
    assert "not published" in cov["cells"]["universe"]
    # Month-end clock includes A accepted on 02-26 at 21:05, strictly before m 22:00.
    assert (m["2021"]["cells"], m["2021"]["linked"], m["2021"]["annual_row"]) == (4, 0.75, 0.75)
    assert m["2021"]["bseg_rev_hhi"] == 0.75 and m["2021"]["major_cust_share"] == 0.75 and m["2021"]["oplease_liab"] == 0.75
    assert (m["2022"]["cells"], m["2022"]["annual_row"]) == (1, 0.0)          # period 2020-12 is stale by 2022-09
    assert cov["segments_member"]["2021"] == {"multi_segment": 1, "with_segment_revenue": 1, "share_segment_revenue": 1.0,
                                             "reconciled_1pct": 1, "share_reconciled": 1.0}
    f10k = cov["features_10k"]["2022"]                                        # D, E, F6, G7, I9
    assert (f10k["ten_k"], f10k["with_row"], f10k["bseg_rev_hhi"], f10k["foreign_sales_share"]) == (5, 1.0, 1.0, 0.4)
    gs = rec["gate_status"]
    assert gs["segment_revenue_multi_segment_10k"]["measured"]["2021"] == 1.0
    assert gs["segment_sum_within_1pct"]["measured"]["2022"] == 0.3333          # only D; I9 arithmetic candidate unverified
    assert "2022" in gs["segment_sum_within_1pct"]["fails"]
    assert cov["segments"]["2022"]["share_reconciled_strict"] == 0.3333
    assert gs["pass"] is False and "2020" in gs["segment_revenue_multi_segment_10k"]["fails"]   # no data -> FAIL
    man = (N.C.stage_dir(FN.STAGE) / "manifest.json").read_text()
    assert '"gate_status"' in man and "segment_features.parquet" in man


def test_gate_status_thresholds() -> None:
    cov = {"denominator_certified": True,
           "segments": {str(y): {"share_segment_revenue": 0.95, "share_reconciled": 0.96} for y in FN.GATE_YEARS}}
    assert FN.gate_status(cov)["pass"] is True
    cov["segments"]["2023"]["share_reconciled"] = 0.949
    gs = FN.gate_status(cov)
    assert gs["pass"] is False and gs["segment_sum_within_1pct"]["fails"] == ["2023"]
    assert gs["segment_revenue_multi_segment_10k"]["pass"] is True
    cov['denominator_certified'] = False
    gs = FN.gate_status(cov)
    assert gs['segment_revenue_multi_segment_10k']['observable_threshold_pass'] is True
    assert gs['segment_revenue_multi_segment_10k']['pass'] is False


# ---------------------------------------------------------------- reconciliation search (strict misses)
def test_dedup_members_rollups_and_duplicates() -> None:
    # Hyster-Yale 2022 10-K: Lifttruck = AmericasHY + EMEAHY + JAPIC on the same axis
    hy = {"AmericasHY": 1984.6, "Bolzoni": 347.8, "EMEAHY": 678.9, "JAPIC": 233.9, "Lifttruckbusiness": 2897.4, "Nuvera": 0.7}
    kept, dropped = FN.dedup_members(hy)
    assert dropped == ["Lifttruckbusiness"] and sum(kept.values()) == pytest.approx(3245.9)
    # Bruker: a two-child roll-up plus a one-child group (duplicate value)
    br = {"BioSpin": 691.0, "Calid": 819.6, "NanoGroup": 697.5, "BsiNano": 697.5, "ScientificInstruments": 1510.6,
          "Beest": 223.8}
    kept, dropped = FN.dedup_members(br)
    assert dropped == ["NanoGroup", "ScientificInstruments"] and sum(kept.values()) == pytest.approx(2431.9)
    assert FN.dedup_members(br, dups=False)[1] == ["ScientificInstruments"]
    assert FN.dedup_members({"A": 10.0, "B": 20.0, "C": 35.0}) == ({"A": 10.0, "B": 20.0, "C": 35.0}, [])   # 30 != 35
    assert FN.dedup_members({"A": 10.0, "B": 20.0, "C": 30.01})[1] == []            # outside 0.01%
    assert FN.dedup_members({"P": 30.0, "A": 10.0, "B": 20.0})[1] == ["P"]


def test_recon_search_rules() -> None:
    rev = "Revenues"
    # overlapping consolidation members: S - one component reconciles, the summed E does not (Viacom-like)
    r = FN.recon_search({rev: {"Cable": 14200.0, "Film": 3070.0, "TV": 12931.0}}, 28586.0, rev, [-1615.0, -1615.0, -3230.0])
    assert r["rule"] == "components" and r["reconciling"] == -1615.0 and r["dropped"] == []
    # duplicate member (Mattel: International tagged twice under two names)
    r = FN.recon_search({rev: {"AmericanGirl": 270.274, "International": 2219.189, "InternationalSegment": 2219.189,
                               "NorthAmerica": 2968.278}}, 5457.741, rev, [])
    assert r["rule"] == "dedup" and r["dropped"] == ["InternationalSegment"]
    # roll-up plus eliminations (Hyster-Yale)
    r = FN.recon_search({rev: {"AmericasHY": 1984.6, "Bolzoni": 347.8, "EMEAHY": 678.9, "JAPIC": 233.9,
                               "Lifttruckbusiness": 2897.4, "Nuvera": 0.7}}, 3075.7, rev, [-170.2])
    assert r["rule"] == "dedup+components" and r["seg_sum"] == pytest.approx(3245.9) and r["reconciling"] == -170.2
    # mixed tags: one segment tagged only with the lease-revenue chain tag
    r = FN.recon_search({rev: {"Land": 79.9, "Materials": 126.2}, "RevenueFromContractWithCustomerExcludingAssessedTax":
                         {"Land": 79.9, "Materials": 126.2, "CommercialRE": 173.2}}, 379.3, rev, [])
    assert r["rule"] == "mixed_tags" and set(r["members"]) == {"Land", "Materials", "CommercialRE"}
    # genuine misses stay misses: untagged eliminations, partial tagging, no total
    assert FN.recon_search({rev: {"DSS": 35.7, "Retail": 1439.7, "Wholesale": 112.2}}, 1531.4, rev, []) is None
    assert FN.recon_search({rev: {"A": 50.0, "B": 30.0}}, None, rev, []) is None


def test_recon_search_overlay_in_build(lake_seg) -> None:
    rec = FN.build()
    out = N.C.stage_dir(FN.STAGE)
    con = duckdb.connect()
    r = con.execute(f"""SELECT recon_class, recon_class_strict, recon_rule, dropped_members, seg_sum, seg_sum_reconciled
                        FROM read_parquet('{(out / 'segment_recon.parquet').as_posix()}') WHERE adsh = '{I9}'""").fetchone()
    assert r == ("segments_exceed_total", "segments_exceed_total", "dedup", ["Group"], 165.0, 95.0)
    x = _features(out)[I9]
    assert (x.bseg_n, x.bseg_n_tagged, x.bseg_rev_n, x.bseg_rev_rule, x.bseg_recon_class) == (4, 4, 4, None, "segments_exceed_total")
    assert x.bseg_rev_hhi == pytest.approx((40 ** 2 + 30 ** 2 + 25 ** 2 + 70 ** 2) / 165 ** 2)
    assert rec["coverage"]["recon_rules"]["2022"] == {"dedup": 1}
    a = _features(out)[A]
    assert a.bseg_rev_rule is None and a.bseg_recon_class == "direct"


def test_remeasure_adds_universe_panels(lake_seg) -> None:
    FN.build()
    root = N.C.build_root()
    u = root / "universe" / "year=2021"
    u.mkdir(parents=True)
    duckdb.connect().execute(f"""COPY (SELECT * FROM (VALUES (DATE '2021-03-31', 101::BIGINT, 1::BIGINT, true, false),
                                                       (DATE '2021-03-31', 103::BIGINT, 9::BIGINT, true, true))
                                       v(session_date, security_id, cik, univ_all_common, univ_r3000p))
                                 TO '{(u / 'universe.parquet').as_posix()}' (FORMAT PARQUET)""")
    (root / "universe" / "manifest.json").write_text("{}")
    cov = FN.remeasure()
    a, r = cov["cells"]["univ_all_common"]["by_year"]["2021"], cov["cells"]["univ_r3000p"]["by_year"]["2021"]
    assert (a["cells"], a["linked"], a["annual_row"]) == (2, 1.0, 0.5)      # cik 9's 10-K (2022-03-07) not yet visible
    assert (r["cells"], r["annual_row"]) == (1, 0.0)
    man = N.C.read_json(N.C.stage_dir(FN.STAGE) / "manifest.json")
    assert "remeasured_at" in man and "univ_r3000p" in man["receipt"]["coverage"]["cells"]


def test_revenue_tiers_and_zero_totals(lake_seg) -> None:
    rec = FN.build()
    out = N.C.stage_dir(FN.STAGE)
    con = duckdb.connect()
    rc = {r[0]: r[1:] for r in con.execute(f"""
        SELECT adsh, revenue_tag, revenue_tier, recon_class, consolidated_revenue, seg_sum
        FROM read_parquet('{(out / 'segment_recon.parquet').as_posix()}')""").fetchall()}
    assert rc[J10] == ("SupplementaryInsuranceInformationPremiumRevenue", 2, "no_total", None, 100.0)
    assert rc[K11] == ("SegmentRevenue", 3, "no_total", None, 50.0)       # extension equivalence unverified
    assert rc[L12] == ("RevenueFromContractWithCustomerExcludingAssessedTax", 1, "direct", 100.0, 100.0)
    assert rc[A][:2] == ("Revenues", 1)
    x = _features(out)
    assert x[J10].bseg_rev_hhi == pytest.approx(0.52) and x[J10].bseg_rev_tag.startswith("Supplementary")
    assert x[K11].bseg_rev_hhi == pytest.approx((35 ** 2 + 15 ** 2) / 50 ** 2) and x[K11].rev_consolidated == 50.0
    assert x[L12].bseg_rev_hhi == pytest.approx(0.52) and x[L12].rev_consolidated == 100.0
    custom = con.execute(f"""SELECT DISTINCT tag, is_custom FROM read_parquet('{(out / 'items' / 'year=*' / 'items.parquet').as_posix()}')
                             WHERE adsh = '{K11}' AND segments IS NOT NULL""").fetchall()
    assert custom == [("SegmentRevenue", True)]
    seg = rec["coverage"]["segments"]["2023"]
    assert (seg["multi_segment"], seg["share_segment_revenue"], seg["share_segment_revenue_tier1"]) == (3, 1.0, 0.3333)


def test_recon_search_reportable_variant() -> None:
    r = FN.recon_search({"Revenues": {"A": 50.0, "B": 30.0, "IntercompanyRevenue": 20.0}}, 80.0, "Revenues", [])
    assert r["rule"] == "reportable" and r["dropped"] == ["IntercompanyRevenue"] and r["seg_sum"] == 80.0


def test_arithmetic_candidates_preserve_economic_segments(lake_seg) -> None:
    FN.build()
    out = N.C.stage_dir(FN.STAGE)
    x = _features(out)
    con = duckdb.connect()
    for accession, values, total in [
        ('0000000013-24-000013', [50, 50, 40], 90),
        ('0000000014-24-000014', [10, 20, 30], 30),
    ]:
        row = x[accession]
        assert (row.bseg_n, row.bseg_rev_n, row.bseg_rev_rule) == (3, 3, None)
        assert row.bseg_rev_hhi == pytest.approx(sum(v*v for v in values) / sum(values)**2)
        candidate, status = con.execute(f"SELECT candidate_only,recon_class FROM read_parquet('{out.as_posix()}/segment_recon.parquet') WHERE adsh=?", [accession]).fetchone()
        assert candidate is True and status != 'reconciled_search'


def test_partial_amendment_total_only_carries_segments(lake_seg) -> None:
    FN.build()
    x = _features(N.C.stage_dir(FN.STAGE))
    assert x[H8].rev_consolidated == 105
    assert x[H8].bseg_n == x[A].bseg_n == 2
    assert x[H8].bseg_rev_hhi == x[A].bseg_rev_hhi == pytest.approx(.52)
    assert x[H8].available_at > x[A].available_at


def test_exhaustive_timing_projection_and_independent_census(lake_seg) -> None:
    rec=FN.build()
    out=N.C.stage_dir(FN.STAGE)
    con=duckdb.connect()
    complete='0000000015-25-000015'
    incomplete='0000000016-25-000016'
    projection=con.execute(f"SELECT member,value,context_basis,len(context_provenance) FROM read_parquet('{out.as_posix()}/segments.parquet') WHERE adsh=? ORDER BY member",[complete]).fetchall()
    assert projection==[('A',60.,'timing_transfer_partition',2),('B',40.,'timing_transfer_partition',2)]
    assert con.execute(f"SELECT count(*) FROM read_parquet('{out.as_posix()}/segments.parquet') WHERE adsh=?",[incomplete]).fetchone()[0]==0
    assert _features(out)[complete].bseg_rev_hhi==pytest.approx(.52)
    status=con.execute(f"SELECT recon_class,n_members FROM read_parquet('{out.as_posix()}/segment_recon.parquet') WHERE adsh='0000000017-25-000017'").fetchone()
    assert status==('no_segment_revenue',2)
    assert rec['coverage']['segments']['2025']['multi_segment']==3
    assert rec['coverage']['segments']['2025']['with_segment_revenue']==1


def test_monthly_clock_strict_boundary(lake_seg) -> None:
    FN.build()
    out=N.C.stage_dir(FN.STAGE)
    con=duckdb.connect()
    features=out/'segment_features.parquet'
    con.execute(f"CREATE TABLE features AS SELECT * REPLACE (CASE WHEN adsh='{A}' THEN TIMESTAMPTZ '2021-02-26 22:00:00+00' ELSE available_at END AS available_at) FROM read_parquet('{features.as_posix()}')")
    con.execute(f"COPY features TO '{features.as_posix()}' (FORMAT PARQUET)")
    cov=FN.remeasure()['cells']['member_equity']['by_year']['2021']
    assert cov['annual_row']==.5   # exact m22 boundary excluded, later months included


def test_restricted_year_rebuild_preserves_omitted_year_census(lake) -> None:  # noqa: F811
    first=FN.build(years=[2021,2022])
    out=N.C.stage_dir(FN.STAGE)
    con=duckdb.connect()
    def snapshot():
        census=con.execute(f"SELECT adsh,n_members,multi_segment FROM read_parquet('{out.as_posix()}/segment_census.parquet') WHERE year(filed)=2021 ORDER BY adsh").fetchall()
        features=con.execute(f"SELECT adsh,bseg_n,bseg_rev_hhi,bseg_basis FROM read_parquet('{out.as_posix()}/segment_features.parquet') WHERE year(filed)=2021 ORDER BY adsh").fetchall()
        recon=con.execute(f"SELECT adsh,n_members,multi_segment,n_revenue_members,recon_class FROM read_parquet('{out.as_posix()}/segment_recon.parquet') WHERE year(filed)=2021 ORDER BY adsh").fetchall()
        return census,features,recon
    before=snapshot()
    second=FN.build(years=[2022])
    assert snapshot()==before
    assert second['coverage']['segments']['2021']==first['coverage']['segments']['2021']
    a=_features(out)[A]
    assert a.bseg_n==2 and a.bseg_rev_hhi==pytest.approx(.52)


def test_explicit_reportable_container_identity(lake_seg, monkeypatch) -> None:
    monkeypatch.setattr(FN,'DISCLOSED_REPORTABLE_CONTAINERS', {
        '0000000018-26-000018':'ReportableLegalEntities',
        '0000000019-26-000019':'ReportableGeographicalComponents'})
    FN.build()
    out=N.C.stage_dir(FN.STAGE)
    features=_features(out)
    for cik in (18,19):
        row=features[f'00000000{cik:02d}-26-0000{cik:02d}']
        assert (row.bseg_n,row.bseg_rev_n,row.bseg_recon_class)==(2,2,'direct')
        assert row.bseg_rev_hhi==pytest.approx(.52)
    # The same generic geographic container is insufficient without filing evidence.
    unrelated=features['0000000020-26-000020']
    assert unrelated.bseg_n==2 and unrelated.bseg_rev_hhi is None


def _role(edges, concept='us-gaap_Revenues', name='role-a'):
    from atx_db.alpha_panel import fund_notes_structure as S
    return {'role':name,'kind':'presentationLink','invalid':[],
            'edges':[(concept,S.AXIS,'parent-child'),(S.AXIS,S.DOMAIN,'parent-child')]+[(a,b,'parent-child') for a,b in edges]}


def test_disclosed_nested_structure_and_equal_siblings():
    from atx_db.alpha_panel import fund_notes_structure as S
    role=_role([(S.DOMAIN,'x_TotalMember'),('x_TotalMember','x_AMember'),('x_TotalMember','x_BMember'),(S.DOMAIN,'x_CMember')])
    result=S.select_hierarchy([role],'us-gaap_Revenues',{'Total','A','B','C'})
    assert result['status']=='verified_structure' and result['members']==['A','B','C']
    # Values deliberately absent: equal-valued distinct siblings cannot change identity.
    assert S.select_hierarchy([role],'us-gaap_Revenues',{'Total','A','C'})['status']=='incomplete_or_unmapped_partition'


def test_structure_conflicts_overlap_ambiguity_and_missing_links():
    from atx_db.alpha_panel import fund_notes_structure as S
    nested=_role([(S.DOMAIN,'x_TotalMember'),('x_TotalMember','x_AMember'),('x_TotalMember','x_BMember')])
    other=_role([(S.DOMAIN,'x_AMember'),(S.DOMAIN,'x_CMember')],name='role-b')
    assert S.select_hierarchy([nested,other],'us-gaap_Revenues',{'Total','A','B','C'})['status']=='conflicting_roles'
    overlap=_role([(S.DOMAIN,'x_PMember'),(S.DOMAIN,'x_QMember'),('x_PMember','x_AMember'),('x_QMember','x_AMember')])
    assert S.select_hierarchy([overlap],'us-gaap_Revenues',{'P','Q','A'})['status']=='invalid_role_graph'
    flat=_role([(S.DOMAIN,'x_TotalMember'),(S.DOMAIN,'x_AMember'),(S.DOMAIN,'x_BMember')])
    assert S.select_hierarchy([flat],'us-gaap_Revenues',{'Total','A','B'})['status']=='flat_ambiguous'
    assert S.select_hierarchy([],'us-gaap_Revenues',{'A','B'})['status']=='missing_links'
    # Cowen-style custom Economic Income concept is not equivalent to GAAP Revenues.
    assert S.select_hierarchy([_role([(a,b) for a,b,_ in nested['edges'][2:]],concept='x_EconomicIncomeRevenue')],'us-gaap_Revenues',{'Total','A','B'})['status']=='missing_links'


def test_structure_primary_context_period_and_concept():
    from atx_db.alpha_panel import fund_notes_structure as S
    html=b'''<xbrli:context id="current"><xbrli:period><xbrli:startDate>2024-01-01</xbrli:startDate><xbrli:endDate>2024-12-31</xbrli:endDate></xbrli:period><xbrldi:explicitMember dimension="us-gaap:StatementBusinessSegmentsAxis">x:AMember</xbrldi:explicitMember></xbrli:context><ix:nonFraction name="us-gaap:Revenues" contextRef="current">50</ix:nonFraction>'''
    assert S.primary_contexts(html,'us-gaap_Revenues','2024-12-31')=={'A':['current']}
    assert S.primary_contexts(html,'us-gaap_Revenues','2023-12-31')=={}
    assert S.primary_contexts(html,'x_EconomicIncomeRevenue','2024-12-31')=={}


@pytest.mark.parametrize('extra_name_filter',[False,True])
def test_source_structure_production_economics_and_clock(lake_seg,monkeypatch,extra_name_filter):
    from atx_db.alpha_panel import fund_notes_structure as S
    if extra_name_filter:monkeypatch.setattr(FN,'RECONCILING_RE',FN.RECONCILING_RE+'|^Other$')
    monkeypatch.setattr(S,'cached_accessions',lambda root:{I9})
    monkeypatch.setattr(S,'cached_structure',lambda root,adsh,tag,period,members:{
        'status':'verified_structure','members':['SubA','SubB','Other'],
        'concept':'us-gaap_Revenues','period':period,'roles':[{'role':'fixture-note-8'}],
        'sources':[{'sha256':'fixture','url':'fixture'}],
        'bridge_context_ids':{'CorporateNonSegment':['corporate'],'IntersegmentElimination':['elimination']},
        'context_ids':{'SubA':['original-a'],'SubB':['original-b'],'Other':['original-c']}})
    FN.build()
    out=N.C.stage_dir(FN.STAGE); row=_features(out)[I9]
    assert (row.bseg_n,row.bseg_rev_n,row.bseg_recon_class)==(3,3,'reconciled_search')
    assert row.bseg_n_tagged==(3 if extra_name_filter else 4) and row.bseg_basis=='source_role_structure'
    assert row.bseg_partition_quality=='source_verified_reportable' and row.bseg_partition_source_adsh==I9
    assert row.bseg_rev_hhi==pytest.approx((40**2+30**2+25**2)/95**2)
    assert row.available_at==dt.datetime(2022,3,7,21,30,tzinfo=UTC)
    con=duckdb.connect()
    census=con.execute(f"SELECT n_members,n_members_source_tagged,member_basis FROM read_parquet('{out/'segment_census.parquet'}') WHERE adsh=?",[I9]).fetchone()
    assert census==(3,3 if extra_name_filter else 4,'source_role_structure')
    assert con.execute(f"SELECT count(*) FROM read_parquet('{out/'segment_structure.parquet'}')").fetchone()[0]==1
    import json
    proof=json.loads(con.execute(f"SELECT proof_json FROM read_parquet('{out/'segment_structure.parquet'}')").fetchone()[0])
    assert [t['value'] for t in proof['source_bridge_terms']]==[10,-10]
    assert con.execute(f"SELECT rel_diff_after_source_bridge,rel_diff_strict FROM read_parquet('{out/'segment_recon.parquet'}') WHERE adsh=?",[I9]).fetchone()==pytest.approx((0,70/95))
    con.close()


def test_cached_structure_requires_exact_source_role_and_cut(tmp_path,monkeypatch):
    import gzip,json
    from atx_db.alpha_panel import fund_notes_structure as S
    primary=b'''<xbrli:context id="a"><xbrli:period><xbrli:startDate>2024-01-01</xbrli:startDate><xbrli:endDate>2024-12-31</xbrli:endDate></xbrli:period><xbrldi:explicitMember dimension="us-gaap:StatementBusinessSegmentsAxis">x:AMember</xbrldi:explicitMember></xbrli:context><xbrli:context id="b"><xbrli:period><xbrli:startDate>2024-01-01</xbrli:startDate><xbrli:endDate>2024-12-31</xbrli:endDate></xbrli:period><xbrldi:explicitMember dimension="us-gaap:StatementBusinessSegmentsAxis">x:BMember</xbrldi:explicitMember></xbrli:context><ix:nonFraction name="us-gaap:Revenues" contextRef="a">50</ix:nonFraction><ix:nonFraction name="us-gaap:Revenues" contextRef="b">50</ix:nonFraction>'''
    nodes=['us-gaap_Revenues',S.AXIS,S.DOMAIN,'x_TotalMember','x_AMember','x_BMember']
    loc=''.join(f'<link:loc xlink:label="n{i}" xlink:href="x.xsd#{n}"/>' for i,n in enumerate(nodes))
    arcs=''.join(f'<link:presentationArc xlink:from="n{a}" xlink:to="n{b}" xlink:arcrole="http://www.xbrl.org/2003/arcrole/parent-child"/>' for a,b in [(0,1),(1,2),(2,3),(2,4),(2,5)])
    xml=f'<link:linkbase xmlns:link="http://www.xbrl.org/2003/linkbase" xmlns:xlink="http://www.w3.org/1999/xlink"><link:presentationLink xlink:role="fixture-role">{loc}{arcs}</link:presentationLink></link:linkbase>'.encode()
    receipts=[]
    for kind,body in [('primary',primary),('linkbase',xml)]:
        filename=kind+'.gz';(tmp_path/filename).write_bytes(gzip.compress(body))
        receipts.append({'adsh':'fixture-original','http_status':200,'kind':kind,'path':filename,'url':'fixture-'+kind,'sha256':hashlib.sha256(body).hexdigest()})
    (tmp_path/'receipts.jsonl').write_text('\n'.join(json.dumps(r) for r in receipts),encoding='utf-8')
    monkeypatch.setattr(S,'CORROBORATED',{'fixture-original':{'primary_sha256':receipts[0]['sha256'],'role':'fixture-role','concept':'us-gaap_Revenues','members':['A','B'],'basis':'explicit primary reported level'}})
    result=S.cached_structure(tmp_path,'fixture-original','Revenues','2024-12-31',{'A','B','Total'})
    assert result['status']=='verified_structure' and result['context_ids']=={'A':['a'],'B':['b']}
    assert S.cached_structure(tmp_path,'fixture-amendment','Revenues','2024-12-31',{'A','B'})['status']=='missing_links'
    (tmp_path/'primary.gz').write_bytes(gzip.compress(primary+b'changed'))
    assert S.cached_structure(tmp_path,'fixture-original','Revenues','2024-12-31',{'A','B','Total'})['status']=='source_hash_mismatch'


def test_disclosure_period_conditional_reorganization_uncertainty():
    from atx_db.alpha_panel.fund_notes_disclosure import disclosure_claims
    for prefix in ['Previously, ','If conditions change, ','Following our reorganization, ','In 2020, ']:
        text=prefix+'we have two reportable segments.'
        claims=disclosure_claims(text,'2024-12-31')
        assert claims[0]['quality']=='temporal_or_reorganization_ambiguous' and not claims[0]['certified']
        assert text[claims[0]['start_offset']:claims[0]['end_offset']]==claims[0]['claim']
    assert disclosure_claims('We currently have two reportable segments.','2024-12-31')[0]['quality']=='explicit_claim_period_uncorroborated'
    assert disclosure_claims('We have two operating segments.','2024-12-31')[0]['quality']=='operating_only_reportable_unverified'
