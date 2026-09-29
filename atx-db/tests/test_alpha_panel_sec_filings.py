"""Pure helpers of the alpha panel's sec_filings stage: zip streaming, JSON flattening, items, events, regimes."""

from __future__ import annotations

import datetime as dt
import io
import json
import zipfile

import pytest

from atx_db.alpha_panel import sec_filings as S


def _zip_bytes(members: dict[str, bytes], zip64: bool = False) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for name, blob in members.items():
            if zip64:
                with zf.open(name, "w", force_zip64=True) as fh:
                    fh.write(blob)
            else:
                zf.writestr(name, blob)
    return buf.getvalue()


@pytest.mark.parametrize("zip64", [False, True])
def test_streaming_zip_reader_matches_zipfile(zip64: bool) -> None:
    members = {f"CIK{i:010d}.json": json.dumps({"cik": i, "pad": "x" * (i * 37)}).encode() for i in range(1, 40)}
    members["CIK0000000001-submissions-001.json"] = b'{"accessionNumber": []}'
    blob = _zip_bytes(members, zip64=zip64)
    fh = io.BytesIO(blob)
    seen = {}
    for e in S.iter_zip_entries(fh, block=257):  # tiny blocks exercise the carry-over path
        seen[e.name] = S.read_zip_member(fh, e)
    assert seen == members
    assert [e.index for e in S.iter_zip_entries(io.BytesIO(blob))] == list(range(len(members)))


def test_read_zip_member_rejects_crc_mismatch() -> None:
    blob = _zip_bytes({"CIK0000000001.json": b'{"a": 1}'})
    fh = io.BytesIO(blob)
    entry = next(S.iter_zip_entries(fh))
    bad = S.ZipEntry(entry.index, entry.name, entry.method, entry.crc ^ 1, entry.compressed_size, entry.size,
                     entry.header_offset)
    with pytest.raises(ValueError, match="CRC"):
        S.read_zip_member(fh, bad)


def test_member_kind() -> None:
    assert S.member_kind("CIK0000320193.json") == (320193, 0)
    assert S.member_kind("CIK0000019617-submissions-012.json") == (19617, 12)
    assert S.member_kind("placeholder.txt") is None


def test_flatten_filings_pads_and_types() -> None:
    block = {
        "accessionNumber": ["0000320193-24-000001", "0000320193-24-000002"],
        "filingDate": ["2024-02-01", "2024-02-02"],
        "form": ["8-K", "4"],
        "items": ["2.02,9.01", ""],
        "size": [1234, None],
        "isXBRL": [1, 0],
        "acceptanceDateTime": ["2024-02-01T21:30:00.000Z"],  # short array: padded with None
    }
    out = S.flatten_filings(block, 320193, 0)
    assert out["cik"] == [320193, 320193]
    assert out["source"] == [0, 0]
    assert out["accession"] == ["0000320193-24-000001", "0000320193-24-000002"]
    assert out["acceptance_raw"] == ["2024-02-01T21:30:00.000Z", None]
    assert out["size"] == [1234, None]
    assert out["is_xbrl"] == [1, 0]
    assert out["report_date"] == [None, None]
    assert out["primary_document"] == [None, None]


def test_columnar_block_main_vs_extra() -> None:
    main = {"filings": {"recent": {"accessionNumber": ["a"]}, "files": []}}
    extra = {"accessionNumber": ["b"]}
    assert S.columnar_block(main, 0) == {"accessionNumber": ["a"]}
    assert S.columnar_block(extra, 3) == {"accessionNumber": ["b"]}


def test_profile_row() -> None:
    doc = {
        "cik": "0000320193", "entityType": "operating", "sic": "3571", "sicDescription": "Electronic Computers",
        "name": "Apple Inc.", "tickers": ["AAPL"], "exchanges": ["Nasdaq"], "category": "Large accelerated filer",
        "fiscalYearEnd": "0926", "stateOfIncorporation": "CA", "insiderTransactionForIssuerExists": 1,
        "insiderTransactionForOwnerExists": 0, "lei": None, "ein": "",
        "addresses": {"business": {"stateOrCountry": "CA", "isForeignLocation": 0}},
        "formerNames": [{"name": "APPLE INC", "from": "2007-01-10T05:00:00.000Z", "to": "2019-08-05T04:00:00.000Z"}],
        "filings": {"recent": {"accessionNumber": ["x", "y"]}, "files": [{"name": "f", "filingCount": 1247}]},
    }
    row = S.profile_row(doc, 320193)
    assert row["name"] == "Apple Inc." and row["sic"] == "3571" and row["fiscal_year_end"] == "0926"
    assert row["tickers"] == ["AAPL"] and row["exchanges"] == ["Nasdaq"]
    assert row["ein"] is None and row["lei"] is None
    assert row["insider_tx_for_issuer_exists"] is True and row["insider_tx_for_owner_exists"] is False
    assert row["business_is_foreign"] is False
    assert row["former_names"] == [{"name": "APPLE INC", "from_date": dt.date(2007, 1, 10),
                                    "to_date": dt.date(2019, 8, 5)}]
    assert row["n_extra_files"] == 1 and row["n_filings_total"] == 1249


def test_parse_items_normalises_and_dedupes() -> None:
    assert S.parse_items("2.02,9.01") == ["2.02", "9.01"]
    assert S.parse_items("2.2, 9.1") == ["2.02", "9.01"]
    assert S.parse_items("Item 2.02;Item 2.02") == ["2.02"]
    assert S.parse_items("") == [] and S.parse_items(None) == []
    assert S.parse_items("5,7") == ["5", "7"]  # pre-2004 item numbering kept verbatim


def test_forms_and_amendments() -> None:
    assert S.base_form("8-K/A") == "8-K" and S.is_amendment("8-K/A") and not S.is_amendment("8-K")
    assert S.is_eight_k("8-K12B") and S.is_eight_k("8-K/A") and not S.is_eight_k("10-K")
    assert S.classify_regime("10-Q") == "domestic"
    assert S.classify_regime("10-K/A") is None  # amendments never set a regime
    assert S.classify_regime("20-F") == "fpi_20f"
    assert S.classify_regime("40-F") == "canadian_40f"
    assert S.classify_regime("N-CSR") == "fund" and S.classify_regime("485BPOS") == "fund"
    assert S.classify_regime("8-K") is None


def test_event_types() -> None:
    assert S.event_types("8-K", ["2.02", "9.01"]) == [("earnings_release", "2.02")]
    assert S.event_types("8-K/A", ["1.03", "3.01"]) == [("bankruptcy", "1.03"), ("delisting_notice", "3.01")]
    assert S.event_types("25-NSE", []) == [("form25nse_delisting", None)]
    assert S.event_types("15-12B", []) == [("form15_12b_deregistration", None)]
    assert S.event_types("NT 10-K", []) == [("late_filing_nt_10k", None)]
    assert S.event_types("10-K", ["2.02"]) == []  # items only count on 8-K forms


def _d(s: str) -> dt.date:
    return dt.date.fromisoformat(s)


def test_regime_segments_switch_lapse_and_clip() -> None:
    t = dt.datetime(2010, 3, 1, 21, 0)
    filings = [
        (_d("2008-03-01"), "domestic", t), (_d("2008-05-01"), "domestic", t), (_d("2008-08-01"), "domestic", t),
        (_d("2011-04-01"), "fpi_20f", t), (_d("2012-04-01"), "fpi_20f", t),
    ]
    segs = S.regime_segments(filings, horizon=_d("2014-12-31"), grace_days=400, start=_d("2009-01-01"))
    got = [(s.regime, s.valid_from, s.valid_to) for s in segs]
    assert got == [
        ("domestic", _d("2009-01-01"), _d("2009-09-05")),  # 2008-08-01 + 400 days, clipped at the start
        ("none", _d("2009-09-06"), _d("2011-03-31")),
        ("fpi_20f", _d("2011-04-01"), _d("2013-05-06")),  # 2012-04-01 + 400 days
        ("none", _d("2013-05-07"), _d("2014-12-31")),
    ]
    assert segs[2].n_filings == 2 and segs[2].first_filing == _d("2011-04-01")


def test_regime_segments_switch_without_gap_ends_day_before() -> None:
    filings = [(_d("2015-03-01"), "domestic", None), (_d("2015-11-01"), "fpi_20f", None)]
    segs = S.regime_segments(filings, horizon=_d("2016-06-30"), grace_days=400)
    assert [(s.regime, s.valid_from, s.valid_to) for s in segs] == [
        ("domestic", _d("2015-03-01"), _d("2015-10-31")),
        ("fpi_20f", _d("2015-11-01"), _d("2016-06-30")),
    ]
    assert S.regime_segments([], horizon=_d("2016-06-30")) == []


def test_items_sql_matches_parse_items() -> None:
    import duckdb

    samples = ["2.02,9.01", "2.2, 9.1", "Item 2.02;Item 2.02", "5.02", "7.01,9.01,7.01", "1.01 2.03 9.01"]
    con = duckdb.connect()
    con.execute("CREATE TABLE f (k INT, items VARCHAR)")
    con.executemany("INSERT INTO f VALUES (?, ?)", list(enumerate(samples)))
    rows = con.execute(S.items_explode_sql("f", "k")).fetchall()
    got: dict[int, set[str]] = {}
    for k, item in rows:
        got.setdefault(k, set()).add(item)
    assert got == {k: set(S.parse_items(s)) for k, s in enumerate(samples)}


def test_file_clock_decision() -> None:
    assert S.file_clock(0, 0) == "unresolved"
    assert S.file_clock(9, 1) == "et" and S.file_clock(1, 9) == "utc"
    assert S.file_clock(5, 5) == "conflict"


def test_clock_resolution_per_file_and_consensus() -> None:
    """File A holds true UTC, file B the ET wall clock labelled Z (the EDGAR index-page cases); D has no evidence but
    shares an accession with A (consensus); E has no evidence at all (conservative ET reading, flagged)."""
    import duckdb

    con = duckdb.connect()
    con.execute("SET TimeZone='UTC'")
    rows = [
        # cik, source, accession, form, filing_date, raw
        (1, 0, "X", "4", "2026-09-17", "2026-09-17 22:30:24"),
        (1, 0, "Z", "8-K", "2024-03-01", "2024-03-01 15:00:00"),
        (2, 0, "X", "4", "2026-09-17", "2026-09-17 18:30:24"),
        (2, 0, "Y", "8-K", "2023-09-22", "2023-09-21 19:43:38"),
        (4, 0, "Z", "8-K", "2024-03-01", "2024-03-01 15:00:00"),
        (5, 0, "W", "8-K", "2024-03-01", "2024-03-01 15:00:00"),
        (5, 2, "W", "8-K", "2024-03-01", "2024-03-01 15:00:00"),
    ]
    con.execute("CREATE TABLE src (cik BIGINT, source SMALLINT, accession VARCHAR, form VARCHAR, fd VARCHAR, r VARCHAR)")
    con.executemany("INSERT INTO src VALUES (?, ?, ?, ?, ?, ?)", rows)
    con.execute(f"""
        CREATE TABLE raw AS SELECT cik, source, accession, {S.ACC_KEY_SQL} AS acc_key, form,
               CAST(fd AS DATE) AS filing_date,
               CAST(r AS TIMESTAMP) AS ts, {S.UTC_TO_ET.format(x='CAST(r AS TIMESTAMP)')} AS tu,
               r AS acceptance_raw, NULL::DATE AS report_date, NULL::VARCHAR AS items, false AS is_amendment,
               NULL::VARCHAR AS primary_document, NULL::VARCHAR AS file_number, NULL::VARCHAR AS act,
               false AS is_xbrl, false AS is_inline_xbrl, 0 AS size FROM src""")
    con.execute("CREATE TABLE files AS " + S.clock_decision_sql("(" + S.clock_evidence_sql("raw") + ")"))
    clocks = dict(((c, s), k) for c, s, k in con.execute("SELECT cik, source, clock FROM files").fetchall())
    assert clocks == {(1, 0): "utc", (2, 0): "et", (4, 0): "unresolved", (5, 0): "unresolved", (5, 2): "unresolved"}
    S.resolve_filings(con, "raw", "files", "out")
    got = {(c, a): (str(t), k, v) for c, a, t, k, v in con.execute(
        "SELECT cik, accession, acceptance_utc, acceptance_clock, vintage_risk FROM out").fetchall()}
    assert len(got) == con.execute("SELECT count(*) FROM out").fetchone()[0] == 6
    assert got[(1, "X")] == ("2026-09-17 22:30:24", "file_utc", None)
    assert got[(2, "X")] == ("2026-09-17 22:30:24", "file_et", None)       # 18:30:24 EDT
    assert got[(2, "Y")] == ("2023-09-21 23:43:38", "file_et", None)       # index page: accepted 19:43:38 ET
    assert got[(4, "Z")] == ("2024-03-01 15:00:00", "accession_consensus", None)
    assert got[(5, "W")] == ("2024-03-01 20:00:00", "unresolved_conservative", "acceptance_clock_unresolved")


def test_delisting_causes_sql() -> None:
    import duckdb

    con = duckdb.connect()
    con.execute("""CREATE TABLE ev (cik BIGINT, accession VARCHAR, form VARCHAR, event_type VARCHAR,
                   filing_date DATE, available_at TIMESTAMP, is_amendment BOOLEAN)""")
    con.execute("CREATE TABLE fil (cik BIGINT, form VARCHAR, filing_date DATE, available_at TIMESTAMP)")
    ev = [
        (1, "m25", "25-NSE", "form25nse_delisting", "2022-10-28"), (1, "m801", "8-K", "change_in_control", "2022-10-28"),
        (2, "b25", "25-NSE", "form25nse_delisting", "2020-06-01"), (2, "b103", "8-K", "bankruptcy", "2020-01-10"),
        (3, "d25", "25-NSE", "form25nse_delisting", "2019-05-01"), (3, "d301", "8-K", "delisting_notice", "2018-12-01"),
        (4, "s25", "25", "form25_delisting", "2021-03-01"),
        (5, "v25", "25", "form25_delisting", "2021-03-01"), (5, "v15", "15-12B", "form15_12b_deregistration", "2021-03-12"),
        (6, "u25", "25", "form25_delisting", "2021-03-01"),
        (7, "f25", "25-NSE", "form25nse_delisting", "2021-03-01"),
    ] + [(876661, f"0000876661-21-{i:06d}", "25-NSE", "form25nse_delisting", "2021-03-01") for i in range(25)]
    con.executemany("INSERT INTO ev VALUES (?, ?, ?, ?, CAST(? AS DATE), CAST(? AS DATE) + INTERVAL 20 HOUR, false)",
                    [(c, a, f, t, d, d) for c, a, f, t, d in ev])
    con.execute("INSERT INTO fil VALUES (4, '10-Q', DATE '2021-05-10', TIMESTAMP '2021-05-10 20:00:00'),"
                " (3, '10-Q', DATE '2019-08-10', TIMESTAMP '2019-08-10 20:00:00')")
    con.execute("""CREATE TABLE rg AS SELECT 7::BIGINT AS cik, 'fund' AS regime, DATE '2015-01-01' AS valid_from,
                   DATE '2026-09-19' AS valid_to, TIMESTAMP '2015-01-01 20:00:00' AS available_at""")
    sql = S.delisting_causes_sql("ev", "fil", "rg")
    got = dict(con.execute(f"SELECT cik, cause FROM ({sql})").fetchall())
    # the exchange's own CIK (self-filed 25-NSEs) is not a subject issuer and is dropped
    assert got == {1: "merger_or_acquisition", 2: "bankruptcy", 3: "exchange_deficiency", 4: "still_reporting",
                   5: "voluntary_deregistration", 6: "unknown", 7: "fund_or_trust"}
    av = dict(con.execute(f"SELECT cik, cause_available_at FROM ({sql})").fetchall())
    assert str(av[4]) == "2021-05-10 20:00:00" and str(av[6]) == "2021-03-01 20:00:00"


def test_regime_segments_per_regime_grace() -> None:
    # annual 20-F filings 407 days apart: one segment under the 550-day fpi grace, a lapse under a flat 400
    filings = [(_d("2017-06-15"), "fpi_20f", None), (_d("2018-07-27"), "fpi_20f", None)]
    flat = S.regime_segments(filings, horizon=_d("2018-12-31"), grace_days=400)
    assert [s.regime for s in flat] == ["fpi_20f", "none", "fpi_20f"]
    per = S.regime_segments(filings, horizon=_d("2018-12-31"), grace_days=S.REGIME_GRACE)
    assert [(s.regime, s.valid_from, s.valid_to, s.n_filings) for s in per] == [
        ("fpi_20f", _d("2017-06-15"), _d("2018-12-31"), 2)]
    assert S.classify_regime("N-PX") is None  # 13F managers file N-PX too since 2024: not a fund marker
