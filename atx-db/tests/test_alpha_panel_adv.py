"""ADV landing helpers (roster names, dates, clocks, column keeping) and the zip-by-range utilities in sec_docs."""

from __future__ import annotations

import datetime as dt
import io
import zipfile
from pathlib import Path

import pyarrow.parquet as pq

from atx_db.alpha_panel import adv as A
from atx_db.alpha_panel import sec_docs as D


def test_roster_date_formats() -> None:
    assert A.roster_date("ia07012026.zip") == dt.date(2026, 7, 1)
    assert A.roster_date("ia070122.zip") == dt.date(2022, 7, 1)
    assert A.roster_date("010118-exempt.zip") == dt.date(2018, 1, 1)
    assert A.roster_date("ia03052025-exempt.xlsx") == dt.date(2025, 3, 5)
    assert A.roster_date("formadv-part1a_1.pdf") is None


def test_roster_links_kinds_duplicates_and_window() -> None:
    html = "".join(f'<a href="{h}">x</a>' for h in (
        "/files/a/ia090117.zip", "/files/b/ia090117_2.zip", "/files/b/ia090117-exempt_0.zip",
        "/files/c/ia020226-exemptzip.zip", "/files/c/ia09012026-registered.zip",
        "/files/d/ia-no-data-100125.pdf", "/files/d/ia010324.xlsx", "/files/e/ia010117.zip"))
    links = A.roster_links(html, start=dt.date(2017, 6, 1), end=dt.date(2026, 12, 31))
    kinds = {(d.isoformat(), k): u.rsplit("/", 1)[-1] for d, k, u in links}
    assert kinds[("2017-09-01", "registered")] == "ia090117_2.zip"      # the re-posted correction wins
    assert kinds[("2017-09-01", "exempt")] == "ia090117-exempt_0.zip"
    assert kinds[("2026-02-02", "exempt")] == "ia020226-exemptzip.zip"
    assert kinds[("2026-09-01", "registered")] == "ia09012026-registered.zip"
    assert ("2017-01-01", "registered") not in kinds                    # before the window
    assert all(u.startswith("https://www.sec.gov/files/") for _d, _k, u in links)


def test_roster_available_at_rule() -> None:
    d = dt.date(2026, 7, 1)
    lm = dt.datetime(2026, 7, 1, 11, 41, tzinfo=dt.UTC)
    assert A.roster_available_at(d, lm) == (lm, "last_modified", False)
    late = dt.datetime(2025, 9, 15, tzinfo=dt.UTC)                        # a re-post years later
    av, basis, vr = A.roster_available_at(dt.date(2024, 1, 3), late)
    assert (av, basis, vr) == (dt.datetime(2024, 1, 17, tzinfo=dt.UTC), "date_plus_14d_floor", True)
    assert A.roster_available_at(d, None)[1] == "date_plus_14d_floor"


def _roster_csv() -> bytes:
    header = ["Organization CRD#", "SEC#", "Firm Type", "CIK#", "Primary Business Name", "Legal Name",
              "Main Office State", "5D(d)(3)", "5D(f)(3)", "5F(2)(c)", "6A(1)", "7B", "Any Hedge Funds",
              "Total number of Hedge funds", "Unrelated"]
    rows = [["148826", "801-70860", "Registered", "1423053", "CITADEL ADVISORS LLC", "CITADEL ADVISORS LLC", "FL",
             "", "570,621,709,022.00", "570,621,709,022.00", "N", "Y", "Y", "37", "x"],
            ["", "801-1", "Registered", "", "NO CRD", "NO CRD", "NY", "", "", "", "N", "N", "", "", ""]]
    buf = io.StringIO()
    import csv
    w = csv.writer(buf)
    w.writerow(header)
    w.writerows(rows)
    return buf.getvalue().encode("cp1252")


def test_read_roster_file_csv_zip_keeps_columns(tmp_path: Path) -> None:
    p = tmp_path / "ia07012026.zip"
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("IA_FIRM_ROSTER.CSV", _roster_csv())
    rows, rep = A.read_roster_file(p)
    assert len(rows) == 1                                                  # a row without CRD is dropped
    r = rows[0]
    assert (r["crd"], r["sec_number"], r["cik"], r["raum_f"], r["n_hedge_funds"]) == \
        ("148826", "801-70860", "1423053", "570,621,709,022.00", "37")
    assert "raum_a" in rep["missing"] and "crd" in rep["kept"]


def test_read_roster_file_notice_zip(tmp_path: Path) -> None:
    p = tmp_path / "ia010119.zip"
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("ia010119.txt", b"The file is unavailable due to the federal government shutdown.")
    rows, rep = A.read_roster_file(p)
    assert rows == [] and "shutdown" in rep["notice"]


def test_snake_names() -> None:
    assert A.snake("Total Gross Assets of Private Funds") == "total_gross_assets_of_private_funds"
    assert A.snake("1F1-State") == "1f1_state"


def _zip_bytes(members: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for n, b in members.items():
            z.writestr(n, b)
    return buf.getvalue()


def test_central_directory_member_range_and_inflate(tmp_path: Path) -> None:
    payload = b"FILINGID,CIK\n" + b"".join(f"{i},{1000 + i}\n".encode() for i in range(5000))
    blob = _zip_bytes({"dir/IA_1D3_CIK_20111105_20241231.csv": payload, "other.txt": b"x" * 100})
    entries = D.central_directory(blob[-4000:])
    e = next(x for x in entries if x["name"].endswith("IA_1D3_CIK_20111105_20241231.csv"))
    lo, hi = D.member_range(e, len(blob))
    rng = tmp_path / "m.range"
    rng.write_bytes(blob[lo:hi + 1])                                       # what a 206 response would carry
    out = tmp_path / "m.csv"
    assert D.inflate_range_file(rng, e, out) == len(payload)
    assert out.read_bytes() == payload


def test_inflate_rejects_corrupt_range(tmp_path: Path) -> None:
    blob = _zip_bytes({"a.csv": b"a,b\n1,2\n" * 1000})
    e = D.central_directory(blob)[0]
    lo, hi = D.member_range(e, len(blob))
    bad = bytearray(blob[lo:hi + 1])
    bad[30 + len(e["name"]) + e["csize"] // 2] ^= 0xFF                    # inside the deflated data
    (tmp_path / "r").write_bytes(bytes(bad))
    import pytest

    with pytest.raises(ValueError):
        D.inflate_range_file(tmp_path / "r", e, tmp_path / "o")
    assert not (tmp_path / "o").exists() and not (tmp_path / "o.partial").exists()


def test_csv_to_parquet_stream_strings_rename_filter(tmp_path: Path) -> None:
    src = tmp_path / "t.tsv"
    src.write_text("ASSET_CAT\tVALUE\tVALUE\n EC \t1\t2\nDBT\t3\t4\nEP\t5\t6\n", encoding="utf-8")
    import pyarrow.compute as pc

    res = D.csv_to_parquet_stream(src, tmp_path / "t.parquet", delimiter="\t", quoted=False, rename=A.snake,
                                  row_filter=lambda b: pc.is_in(pc.utf8_trim_whitespace(b.column("asset_cat")),
                                                                value_set=__import__("pyarrow").array(["EC", "EP"])))
    t = pq.read_table(tmp_path / "t.parquet")
    assert res["rows"] == 2 and t.column_names == ["asset_cat", "value", "value_2"]
    assert t.column("value_2").to_pylist() == ["2", "6"]


def test_structured_document_and_url() -> None:
    assert D.structured_document("xslSCHEDULE_13D_X01/primary_doc.xml") == "primary_doc.xml"
    assert D.structured_document("d355220dsc13d.htm") == "d355220dsc13d.htm"
    assert D.doc_url(41719, "0001193125-22-263767", "a.htm") == \
        "https://www.sec.gov/Archives/edgar/data/41719/000119312522263767/a.htm"
