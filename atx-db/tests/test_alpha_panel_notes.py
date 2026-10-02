"""Stage ``notes`` (SEC Financial Statement and Notes landing), on a fixture zip; the fixture serves S2.1 / S4.4 too."""

from __future__ import annotations

import datetime as dt
import zipfile
from pathlib import Path

import duckdb
import pytest

from atx_db.alpha_panel import notes_fetch as N

UTC = dt.UTC


def _words(s: str) -> list[str]:
    return s.split()


SUB_COLS = _words("adsh cik name sic countryba stprba cityba zipba bas1 bas2 baph countryma stprma cityma zipma mas1 mas2 "
            "countryinc stprinc ein former changed afs wksi fye form period fy fp filed accepted prevrpt detail instance "
            "nciks aciks pubfloatusd floatdate floataxis floatmems")
NUM_COLS = _words("adsh tag version ddate qtrs uom dimh iprx value footnote footlen dimn coreg durp datp dcml")
TXT_COLS = _words("adsh tag version ddate qtrs iprx lang dcml durp datp dimh dimn coreg escaped srclen txtlen footnote footlen "
            "context value")
A, B, C8, D, E = ("0000000001-21-000001", "0000000002-21-000002", "0000000003-21-000003", "0000000004-22-000004",
                  "0000000005-22-000005")
DIMS = {
    "0x00000000": "",
    "h_cls_a": "ClassOfStock=CommonClassA;",
    "h_cls_b": "ClassOfStock=CommonClassB;",
    "h_pref": "ClassOfStock=SeriesAPreferredStock;",
    "h_seg_alpha": "BusinessSegments=Alpha;",
    "h_seg_beta": "BusinessSegments=Beta;",
    "h_seg_beta_ops": "BusinessSegments=Beta;ConsolidationItems=OperatingSegments;",
    "h_seg_corp": "BusinessSegments=CorporateAndEliminations;",
    "h_seg_x": "BusinessSegments=SegX;",
    "h_seg_y": "BusinessSegments=SegY;",
    "h_seg_p": "BusinessSegments=SegP;",
    "h_seg_q": "BusinessSegments=SegQ;",
    "h_elim": "ConsolidationItems=IntersegmentElimination;",
    "h_geo_us": "Geographical=US;",
    "h_prod": "ProductOrService=Widgets;",
    "h_seg_alpha_prod": "BusinessSegments=Alpha;ProductOrService=Widgets;",
    "h_pension": "RetirementPlanType=PensionPlansDefinedBenefit;",
    "h_sub": "LegalEntity=SubCo;",
    "h_sub_cls": "ClassOfStock=PreferredStock;LegalEntity=SubCo;",
}


def _sub(adsh, cik, form, period, fy, fp, filed, accepted, pubfloat=""):
    row = dict.fromkeys(SUB_COLS, "")
    row.update(adsh=adsh, cik=str(cik), name=f"CO {cik}", sic="3571", countryba="US", cityba="X", countryinc="US",
               wksi="0", fye="1231", form=form, period=period, fy=fy, fp=fp, filed=filed, accepted=accepted,
               prevrpt="0", detail="1", instance="x.htm", nciks="1", pubfloatusd=pubfloat)
    return [row[c] for c in SUB_COLS]


def _num(adsh, tag, ddate, qtrs, value, dimh="0x00000000", uom="USD", iprx="0", version="us-gaap/2020", coreg="",
         datp="0.0"):
    d = DIMS[dimh]
    return [adsh, tag, version, ddate, str(qtrs), uom, dimh, iprx, value, "", "0", str(d.count(";")), coreg, "0.0",
            datp, "-6"]


def _txt(adsh, tag, value, dimh="0x00000000", ddate="20201231", qtrs="0", coreg="", version="dei/2020"):
    d = DIMS[dimh]
    return [adsh, tag, version, ddate, qtrs, "0", "en-US", "32767", "0.0", "0.0", dimh, str(d.count(";")), coreg, "0",
            str(len(value)), str(len(value)), "", "0", "c1", value]


def fixture_tables() -> dict[str, list[list[str]]]:
    sub = [
        _sub(A, 1, "10-K", "20201231", "2020", "FY", "20210226", "2021-02-26 16:05:00.0", "1500000000"),
        _sub(B, 2, "10-Q", "20210630", "2021", "Q2", "20210805", "2021-08-05 08:00:00.0"),
        _sub(C8, 3, "8-K", "20210310", "", "", "20210310", ""),
        _sub(D, 4, "10-K", "20211231", "2021", "FY", "20220301", "2022-03-01 17:45:00.0"),
        _sub(E, 5, "10-K", "20211231", "2021", "FY", "20220302", "2022-03-02 09:00:00.0"),
    ]
    txt = [
        # A: two listed securities + an unlisted class B share count
        _txt(A, "TradingSymbol", "ABC", "h_cls_a"), _txt(A, "SecurityExchangeName", "NYSE", "h_cls_a"),
        _txt(A, "Security12bTitle", "Class A Common Stock, $0.01 par value", "h_cls_a"),
        _txt(A, "TradingSymbol", "ABC PRA", "h_pref"), _txt(A, "SecurityExchangeName", "NYSE", "h_pref"),
        _txt(A, "Security12bTitle", "Depositary Shares, each representing 1/1000th of a share of 5.00% Series A "
                                    "Preferred Stock", "h_pref"),
        _txt(A, "AuditorName", "Big Four LLP"), _txt(A, "AuditorLocation", "New York, New York"),
        _txt(A, "EntityFilerCategory", "Large Accelerated Filer"), _txt(A, "EntityIncorporationStateCountryCode", "DE"),
        _txt(A, "EntityAddressCityOrTown", "Springfield"), _txt(A, "EntityRegistrantName", "ABC Corp"),
        _txt(A, "DocumentType", "10-K"),
        _txt(A, "TradingSymbol", "OLDABC", "h_cls_a", ddate="20191231"),     # older context: the latest wins
        _txt(A, "UnrelatedTag", "x", version="us-gaap/2020"),
        # B: non-dimensional cover
        _txt(B, "TradingSymbol", "XYZ", ddate="20210630"), _txt(B, "SecurityExchangeName", "NASDAQ", ddate="20210630"),
        _txt(B, "Security12bTitle", "Common Stock", ddate="20210630"),
        # C (8-K, no acceptance time): placeholder symbol + a co-registrant with its own CIK
        _txt(C8, "TradingSymbol", "N/A", ddate="20210310"),
        _txt(C8, "EntityCentralIndexKey", "0000000033", "h_sub", ddate="20210310", coreg="SubCo"),
        _txt(C8, "TradingSymbol", "SUBP", "h_sub_cls", ddate="20210310", coreg="SubCo"),
        _txt(C8, "SecurityExchangeName", "NYSE", "h_sub_cls", ddate="20210310", coreg="SubCo"),
        _txt(C8, "Security12bTitle", "6% Preferred Stock", "h_sub_cls", ddate="20210310", coreg="SubCo"),
    ]
    num = [
        # dei share counts and float
        _num(A, "EntityCommonStockSharesOutstanding", "20210228", 0, "100000000.0000", "h_cls_a", "shares",
             version="dei/2020", datp="9.0"),
        _num(A, "EntityCommonStockSharesOutstanding", "20210228", 0, "5000000.0000", "h_cls_b", "shares",
             version="dei/2020", datp="9.0"),
        _num(A, "EntityPublicFloat", "20200630", 0, "1400000000.0000", version="dei/2020"),
        _num(B, "EntityCommonStockSharesOutstanding", "20210731", 0, "2500000.0000", uom="shares", version="dei/2020"),
        # A segments: Alpha 60 + Beta 40 = 100 (Beta tagged twice: alone and with OperatingSegments)
        _num(A, "Revenues", "20201231", 4, "100.0000"),
        _num(A, "Revenues", "20201231", 4, "100.0000", iprx="1"),                 # repeated fact
        _num(A, "Revenues", "20191231", 4, "90.0000"),
        _num(A, "Revenues", "20201231", 4, "60.0000", "h_seg_alpha"),
        _num(A, "Revenues", "20201231", 4, "40.0000", "h_seg_beta"),
        _num(A, "Revenues", "20201231", 4, "40.0000", "h_seg_beta_ops"),
        _num(A, "Revenues", "20201231", 4, "25.0000", "h_seg_alpha_prod"),       # disaggregation: not a segment row
        _num(A, "Revenues", "20201231", 4, "80.0000", "h_geo_us"),
        _num(A, "Revenues", "20201231", 4, "55.0000", "h_prod"),                 # product only: not kept for segments
        _num(A, "OperatingIncomeLoss", "20201231", 4, "12.0000", "h_seg_alpha"),
        _num(A, "Assets", "20201231", 0, "300.0000", "h_seg_alpha"),
        _num(A, "SegmentAdjustedEbitda", "20201231", 4, "15.0000", "h_seg_alpha", version=A),   # custom, segment axis
        _num(A, "CustomUnrelated", "20201231", 4, "15.0000", version=A),                          # custom, no axis
        _num(A, "OperatingLeaseCost", "20191231", 4, "n/a"),                                      # unparsable value
        # A notes items
        _num(A, "LongTermDebtMaturitiesRepaymentsOfPrincipalInNextTwelveMonths", "20201231", 0, "10.0000"),
        _num(A, "LongTermDebtMaturitiesRepaymentsOfPrincipalInYearTwo", "20201231", 0, "11.0000"),
        _num(A, "LongTermDebtMaturitiesRepaymentsOfPrincipalInYearThree", "20201231", 0, "12.0000"),
        _num(A, "LongTermDebtMaturitiesRepaymentsOfPrincipalInYearFour", "20201231", 0, "13.0000"),
        _num(A, "LongTermDebtMaturitiesRepaymentsOfPrincipalInYearFive", "20201231", 0, "14.0000"),
        _num(A, "LongTermDebtMaturitiesRepaymentsOfPrincipalAfterYearFive", "20201231", 0, "50.0000"),
        _num(A, "ShareBasedCompensation", "20201231", 4, "7.0000"),
        _num(A, "AllocatedShareBasedCompensationExpense", "20201231", 4, "6.5000"),
        _num(A, "CurrentFederalTaxExpenseBenefit", "20201231", 4, "3.0000"),
        _num(A, "CurrentForeignTaxExpenseBenefit", "20201231", 4, "1.0000"),
        _num(A, "DeferredFederalIncomeTaxExpenseBenefit", "20201231", 4, "-0.5000"),
        _num(A, "DefinedBenefitPlanBenefitObligation", "20201231", 0, "200.0000", "h_pension"),
        _num(A, "DefinedBenefitPlanFairValueOfPlanAssets", "20201231", 0, "180.0000", "h_pension"),
        _num(A, "GoodwillImpairmentLoss", "20201231", 4, "5.0000"),
        _num(A, "ImpairmentOfIntangibleAssetsIndefinitelivedExcludingGoodwill", "20201231", 4, "2.0000"),
        _num(A, "OperatingLeaseLiability", "20201231", 0, "33.0000"),
        _num(A, "OperatingLeaseCost", "20201231", 4, "8.0000"),
        # B: 10-Q flows (YTD qtrs=2 wins over the discrete quarter)
        _num(B, "ShareBasedCompensation", "20210630", 2, "4.0000"),
        _num(B, "ShareBasedCompensation", "20210630", 1, "2.1000"),
        _num(B, "OperatingLeaseLiability", "20210630", 0, "9.0000"),
        # C: an 8-K revenue fact is not a periodic form -> not in num_items
        _num(C8, "Revenues", "20201231", 4, "999.0000"),
        # D: X 70 + Y 50 = 120 vs 100, eliminations -20 tagged without a segment member -> with_reconciling
        _num(D, "RevenueFromContractWithCustomerExcludingAssessedTax", "20211231", 4, "100.0000"),
        _num(D, "RevenueFromContractWithCustomerExcludingAssessedTax", "20211231", 4, "70.0000", "h_seg_x"),
        _num(D, "RevenueFromContractWithCustomerExcludingAssessedTax", "20211231", 4, "50.0000", "h_seg_y"),
        _num(D, "RevenueFromContractWithCustomerExcludingAssessedTax", "20211231", 4, "-20.0000", "h_elim"),
        _num(D, "OperatingIncomeLoss", "20211231", 4, "5.0000", "h_seg_corp"),
        # E: P 30 + Q 30 = 60 vs 100 -> segments_below_total
        _num(E, "Revenues", "20211231", 4, "100.0000"),
        _num(E, "Revenues", "20211231", 4, "30.0000", "h_seg_p"),
        _num(E, "Revenues", "20211231", 4, "30.0000", "h_seg_q"),
    ]
    dim = [[h, s, "0"] for h, s in DIMS.items()]
    return {"sub": sub, "num": num, "txt": txt, "dim": dim}


def write_zip(path: Path, tables: dict[str, list[list[str]]]) -> Path:
    heads = {"sub": SUB_COLS, "num": NUM_COLS, "txt": TXT_COLS, "dim": ["dimhash", "segments", "segt"]}
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, rows in tables.items():
            body = "\t".join(heads[name]) + "\n" + "".join("\t".join(r) + "\n" for r in rows)
            z.writestr(f"{name}.tsv", body)
        z.writestr("tag.tsv", "tag\tversion\n")
        z.writestr("notes-metadata.json", "{}")
    return path


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    """A fixture lake: one parsed data set ``2021q1`` with its ledger receipt and a finalized notes manifest."""
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    monkeypatch.setattr(N, "RAW_DIR", tmp_path / "raw")
    monkeypatch.setattr(N, "ARCHIVE_RECEIPTS", tmp_path / "archive")
    monkeypatch.setattr(N, "MIN_FREE_GB", 0.0)
    (tmp_path / "raw").mkdir()
    z = write_zip(tmp_path / "raw" / "2021q1_notes.zip", fixture_tables())
    res = N.parse_archive(z, "2021q1", tmp_path / "work")
    N.ledger().append({"key": "2021q1", "kind": "zip", "status": "parsed", "url": "u", "bytes": z.stat().st_size,
                       "sha256": "x", "span": ["2021-01-01", "2021-03-31"], "repost": 0, **res})
    N.finalize()
    return tmp_path, res


# ---------------------------------------------------------------- notes_fetch: pure helpers
def test_data_set_names_spans_and_order() -> None:
    q = N.data_set("https://www.sec.gov/files/dera/data/financial-statement-notes-data-sets/2019q1_notes.zip")
    assert (q["key"], q["cadence"], q["span_start"], q["span_end"], q["repost"]) == (
        "2019q1", "quarterly", dt.date(2019, 1, 1), dt.date(2019, 3, 31), 0)
    m = N.data_set("/files/dera/data/financial-statement-notes-data-sets/2025_12_notes.zip")
    assert (m["key"], m["cadence"], m["span_start"], m["span_end"]) == ("2025_12", "monthly", dt.date(2025, 12, 1),
                                                                       dt.date(2025, 12, 31))
    assert N.data_set("x/2010q1_notes_1.zip")["repost"] == 1
    keys = ["2025_07", "2025q2", "2019q4", "2026_01", "2025_12"]
    assert sorted(keys, key=N.order_key) == ["2019q4", "2025q2", "2025_07", "2025_12", "2026_01"]
    with pytest.raises(ValueError):
        N.data_set("x/2019q1.zip")


def test_discover_filters_first_key_and_prefers_repost() -> None:
    html = ('<a href="/files/dera/data/financial-statement-notes-data-sets/2018q4_notes.zip">a</a>'
            '<a href="/files/dera/data/financial-statement-notes-data-sets/2019q1_notes.zip">b</a>'
            '<a href="/files/dera/data/financial-statement-notes-data-sets/2019q1_notes_1.zip">b1</a>'
            "<a href='/files/dera/data/financial-statement-notes-data-sets/2025_07_notes.zip'>c</a>")
    got = N.discover(html)
    assert [d["key"] for d in got] == ["2019q1", "2025_07"]
    assert got[0]["repost"] == 1 and got[0]["url"].startswith("https://www.sec.gov/files/dera/")


def test_page_sizes_and_line_count() -> None:
    html = ('<a href="/files/dera/data/financial-statement-notes-data-sets/2026_08_notes.zip" download>2026 08</a>'
            '</td><td class="views-field views-field-filesize">298.24 MB </td>'
            '<a href="/x/2021q1_notes.zip">q</a></td><td class="views-field views-field-filesize">1.5 GB</td>')
    assert N._sizes_from_page(html) == {"2026_08_notes.zip": 298.24, "2021q1_notes.zip": 1536.0}
    assert N.count_lines(b"\n", 3, 10) == 3 and N.count_lines(b"x", 3, 10) == 4 and N.count_lines(b"", 0, 0) == 0


def test_disk_guard_refuses_below_floor(monkeypatch) -> None:
    monkeypatch.setattr(N, "free_gb", lambda: 45.0)
    N.require_free(4.0)
    with pytest.raises(RuntimeError, match="disk guard"):
        N.require_free(6.0)


# ---------------------------------------------------------------- notes_fetch: parse + verify
def test_parse_archive_filters_types_and_verifies(lake) -> None:
    tmp, res = lake
    v = res["verify"]
    assert v["ok"] and all(v["lines_consistent"].values()) and v["invalid_rows"] == dict.fromkeys(N.MEMBERS, 0)
    assert res["rows"]["sub"] == 5
    parts = N.parts_dir() / "source=2021q1"
    con = duckdb.connect()
    con.execute("SET TimeZone='UTC'")
    sub = {r[0]: r[1:] for r in con.execute(
        f"SELECT adsh, cik, accepted_utc, accepted_et_naive, period FROM read_parquet('{(parts / 'sub.parquet').as_posix()}')").fetchall()}
    assert sub[A][0] == 1 and sub[A][2] == dt.datetime(2021, 2, 26, 16, 5)
    assert sub[A][1] == dt.datetime(2021, 2, 26, 21, 5, tzinfo=UTC)          # EST: +5 h
    assert sub[B][1] == dt.datetime(2021, 8, 5, 12, 0, tzinfo=UTC)           # EDT: +4 h
    assert sub[C8][1] is None and sub[A][3] == dt.date(2020, 12, 31)
    tags = {r[0] for r in con.execute(f"SELECT tag FROM read_parquet('{(parts / 'txt_dei.parquet').as_posix()}')").fetchall()}
    assert "UnrelatedTag" not in tags and {"TradingSymbol", "AuditorName", "EntityCentralIndexKey"} <= tags
    dim = f"read_parquet('{(parts / 'dim.parquet').as_posix()}')"
    nd = con.execute(f"SELECT count(*), count(*) FILTER (WHERE segments = 'ClassOfStock=CommonClassA;') "
                     f"FROM read_parquet('{(parts / 'num_dei.parquet').as_posix()}') n LEFT JOIN {dim} d ON d.dimhash = n.dimh").fetchone()
    assert nd == (4, 1)
    items = con.execute(f"SELECT adsh, tag, segments FROM read_parquet('{(parts / 'num_items.parquet').as_posix()}') n "
                        f"LEFT JOIN {dim} d ON d.dimhash = n.dimh").fetchall()
    kept = {(a, t, s) for a, t, s in items}
    assert (A, "SegmentAdjustedEbitda", "BusinessSegments=Alpha;") in kept          # custom tag on a segment axis
    assert not any(t == "CustomUnrelated" for _, t, _ in kept)                     # custom tag, no axis
    assert not any(a == C8 for a, _, _ in kept)                                    # 8-K is not a periodic form
    assert not any(t.startswith("EntityCommonStock") for _, t, _ in kept)          # dei lives in num_dei
    assert (A, "Revenues", "ProductOrService=Widgets;") in kept                     # standard revenue, any dims
    assert v["orphan_rows"] == {"txt_dei": 0, "num_dei": 0, "num_items": 0} and v["dim_missing"] == 0
    kept_dims = {r[0] for r in con.execute(f"SELECT dimhash FROM {dim}").fetchall()}
    assert "h_prod" in kept_dims and "h_sub" in kept_dims and "0x00000000" not in kept_dims
    assert res["unparsed"]["num"] == {"value": 1}                                   # 'n/a' in a value column
    assert res["memory"] == {} or res["memory"]["peak_commit_gb"] < 0.25
    assert (tmp / "raw" / "meta" / "2021q1_notes.notes-metadata.json.gz").exists()
    man = (N.parts_dir().parent / "manifest.json")
    assert man.exists() and '"num_items"' in man.read_text()


def test_parse_archive_fails_on_short_member(tmp_path, monkeypatch) -> None:
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    monkeypatch.setattr(N, "RAW_DIR", tmp_path / "raw")
    monkeypatch.setattr(N, "MIN_FREE_GB", 0.0)
    t = fixture_tables()
    del t["dim"]
    z = tmp_path / "2021q2_notes.zip"
    write_zip(z, t)
    with pytest.raises(RuntimeError, match=r"dim.tsv missing"):
        N.parse_archive(z, "2021q2", tmp_path / "work")


def test_accepted_clock_dst_and_stream_counts_rejected_rows(tmp_path) -> None:
    naive, utc = N.accepted_clocks(["2021-02-26 16:05:00.0", "2021-08-05 08:00:00.0", None, "garbage",
                                    "2021-11-07 01:30:00.0"])
    assert utc[0] == dt.datetime(2021, 2, 26, 21, 5, tzinfo=UTC) and utc[1] == dt.datetime(2021, 8, 5, 12, tzinfo=UTC)
    assert utc[2] is None and naive[3] is None
    assert utc[4] == dt.datetime(2021, 11, 7, 5, 30, tzinfo=UTC)                   # ambiguous hour: first (EDT)
    z = tmp_path / "x.zip"
    with zipfile.ZipFile(z, "w") as zf:
        zf.writestr("dim.tsv", "dimhash\tsegments\tsegt\nh1\tA=B;\t0\nh2\tC=D;\t0\tEXTRA\nh3\t\t1")  # no final LF
    st: dict = {}
    with zipfile.ZipFile(z) as zf:
        rows = [r for b in N.stream_member(zf, "dim.tsv", "dim", st) for r in b.to_pylist()]
    assert rows == [{"dimhash": "h1", "segments": "A=B;", "segt": "0"}, {"dimhash": "h3", "segments": None, "segt": "1"}]
    assert (st["rows"], st["invalid_rows"], st["lines"]) == (2, 1, 4) and st["rows_plus_invalid_equal_lines_minus_header"]
    import pyarrow as pa
    un: dict = {}
    cols = N.cast_batch(pa.record_batch({"qtrs": ["4", "x", None], "value": ["1.5", "-2E3", "abc"],
                                         "ddate": ["20201231", "2020", None], "escaped": ["1", "0", "maybe"]}), "txt", un)
    assert cols["qtrs"].to_pylist() == [4, None, None] and un == {"qtrs": 1, "ddate": 1, "escaped": 1}
    assert cols["ddate"].to_pylist() == [dt.date(2020, 12, 31), None, None]
    assert cols["escaped"].to_pylist() == [True, False, None]
    unn: dict = {}
    assert N.cast_batch(pa.record_batch({"value": ["1.5", "-2E3", "abc", ".5"]}), "num", unn)["value"].to_pylist() == [1.5, -2000.0, None, 0.5]
    assert unn == {"value": 1}
