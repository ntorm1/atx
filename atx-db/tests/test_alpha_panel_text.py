"""Filing text landing (section parser, receipts, parts) and the Lazy Prices / readability helpers (lane TXT)."""

from __future__ import annotations

import datetime as dt
import math
from collections import Counter
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import filing_text as FT
from atx_db.alpha_panel import text_features as TF

FILLER = "The Company designs and sells industrial pumps to customers in many markets. " * 12

TOC_10K = """
<html><head><title>10-K</title><style>.x{color:red}</style></head><body>
<div style="display:none"><ix:header><ix:hidden>dei:Hidden 99999 facts that are hidden</ix:hidden></ix:header></div>
<p>UNITED STATES SECURITIES AND EXCHANGE COMMISSION</p>
<p>TABLE OF CONTENTS</p>
<table>
<tr><td>PART I</td><td></td></tr>
<tr><td><a href="#i1">Item 1.</a></td><td><a href="#i1">Business</a></td><td>4</td></tr>
<tr><td>Item 1A.</td><td>Risk Factors</td><td>12</td></tr>
<tr><td>Item 1B.</td><td>Unresolved Staff Comments</td><td>30</td></tr>
<tr><td>Item 2.</td><td>Properties</td><td>31</td></tr>
<tr><td>Item 3.</td><td>Legal Proceedings</td><td>31</td></tr>
<tr><td>Item 4.</td><td>Mine Safety Disclosures</td><td>32</td></tr>
<tr><td>PART II</td></tr>
<tr><td>Item 5.</td><td>Market for Registrant's Common Equity</td><td>33</td></tr>
<tr><td>Item 7.</td><td>Management's Discussion and Analysis</td><td>35</td></tr>
<tr><td>Item 7A.</td><td>Quantitative and Qualitative Disclosures About Market Risk</td><td>50</td></tr>
<tr><td>Item 8.</td><td>Financial Statements and Supplementary Data</td><td>51</td></tr>
</table>
<p>PART I</p>
<div><span>Item</span></div><div><span>1. Business</span></div>
<p>{filler}</p>
<p>Item 7 of this report discusses results; this sentence starts with a cross reference.</p>
<p>Acme Pump Co. | 2024 Form 10-K | 5</p>
<p>{filler}</p>
<p>Acme Pump Co. | 2024 Form 10-K | 6</p>
<p><b>ITEM 1A.RISK FACTORS</b></p>
<p>Our pumps may fail. {filler}</p>
<p>Acme Pump Co. | 2024 Form 10-K | 7</p>
<p>12</p>
<p>Item 1B. Unresolved Staff Comments</p><p>None.</p>
<p>Item 2. Properties</p><p>We own a plant.</p>
<p>Item 3. Legal Proceedings</p><p>None.</p>
<p>Item 4. Mine Safety Disclosures</p><p>Not applicable.</p>
<p>PART II</p>
<p>Item 5. Market for Registrant&#8217;s Common Equity</p><p>Our stock trades on Nasdaq under the symbol ACME.</p>
<table><tr><td>Item 7.</td><td>Management&#8217;s Discussion and Analysis of Financial Condition</td></tr></table>
<p>Revenue grew because of pumps. {filler}</p>
<table><tr><td>Revenue</td><td>$</td><td>1,234,567</td><td>$</td><td>1,111,222</td></tr>
<tr><td>Cost</td><td>$</td><td>834,567</td><td>$</td><td>711,222</td></tr></table>
<p>Acme Pump Co. | 2024 Form 10-K | 36</p>
<p>Item 7A. Quantitative and Qualitative Disclosures About Market Risk</p><p>We have little market risk.</p>
<p>Item 8. Financial Statements and Supplementary Data</p><p>See the statements.</p>
</body></html>
""".replace("{filler}", FILLER)

TWENTY_F = """
<p>Item 3.</p><p>Key Information</p><p>Item 4.</p><p>Information on the Company</p><p>Item 4A.</p><p>Unresolved</p>
<p>Item 5.</p><p>Operating and Financial Review and Prospects</p><p>Item 6.</p><p>Directors</p>
<p>Item 7.</p><p>Major Shareholders</p><p>Item 8.</p><p>Financial Information</p><p>Item 9.</p><p>The Offer</p>
<p>ITEM 3. KEY</p><p>INFORMATION.</p>
<p>A. [Reserved]</p><p>B. Capitalization and Indebtedness</p><p>Not applicable.</p>
<p>D. Risk</p><p>Factors</p>
<p>Investing in our ADSs is risky. {filler}</p>
<p>ITEM 4. INFORMATION OF THE COMPANY</p>
<p>We make pumps in Israel. {filler}</p>
<p>ITEM 4A. UNRESOLVED STAFF COMMENTS</p><p>None.</p>
<p>Item 5. Operating</p><p>and Financial Review and Prospects</p>
<p>Revenue rose in 2024. {filler}</p>
<p>ITEM 6. DIRECTORS, SENIOR MANAGEMENT AND EMPLOYEES</p><p>Our board.</p>
<p>ITEM 7. MAJOR SHAREHOLDERS</p><p>Holders.</p>
""".replace("{filler}", FILLER)


def _sections(html: str, form: str = "10-K") -> tuple[dict[str, FT.Section], dict[str, Any]]:
    return FT.extract_sections(FT.html_to_text(html.encode("utf-8")), form)


# ------------------------------------------------------------------ html_to_text


def test_html_to_text_drops_hidden_numeric_tables_and_page_lines() -> None:
    text = FT.html_to_text(TOC_10K.encode("utf-8"))
    assert "hidden" not in text and "color:red" not in text  # ix:header and style
    assert "1,234,567" not in text  # numeric table dropped
    assert "Management's Discussion and Analysis of Financial Condition" in text  # text table kept, entity decoded
    assert "\n12\n" not in text  # page-number line
    assert "Item 1. Business 4\n" in text  # a text table row is one line
    assert "\nItem\n1. Business\n" in text


def test_html_to_text_inline_tags_join_and_cp1252() -> None:
    raw = b"<p>I<font size=1>TEM</font>&nbsp;1A.</p><p>Caf\xe9 \x93quoted\x94</p>"
    text = FT.html_to_text(raw)
    lines = text.split("\n")
    assert lines[0] == "ITEM 1A."
    assert lines[1].startswith("Caf") and '"quoted"' in lines[1]


def test_numeric_table_share() -> None:
    assert FT._numeric_share(b"<tr><td>Revenue</td><td>1,234,567</td><td>999,111</td></tr>") > FT.NUMERIC_TABLE_SHARE
    assert FT._numeric_share(b"<tr><td>Item 1A.</td><td>Risk Factors</td></tr>") < FT.NUMERIC_TABLE_SHARE
    assert FT._numeric_share(b"<td>&#8217;&#160;</td>") == 1.0  # entities are not digits; no letters -> dropped


# ------------------------------------------------------------------ item headings


def test_find_items_flags_table_of_contents_and_joins_split_heading() -> None:
    lines = FT.html_to_text(TOC_10K.encode("utf-8")).split("\n")
    heads = FT.find_items(lines, "10-K")
    toc = [h for h in heads if h.toc]
    body = [h for h in heads if not h.toc]
    assert {h.key for h in toc} >= {"1", "1A", "7", "8"}
    assert [h.key for h in body][:3] == ["1", "1A", "1B"]  # "Item" / "1. Business" joined; "1A.RISK" parsed
    assert not any(h.key == "7" and "cross reference" in lines[h.line] for h in heads)  # title check rejects


def test_packed_short_body_items_are_not_a_toc() -> None:
    lines = ["Item 1. Business", *(["We sell pumps. " * 40] * 3)]
    lines += ["Item 9B. Other Information", "None.", "Item 9C. Disclosure", "None.", "Item 10. Directors",
              "Proxy.", "Item 11. Executive Compensation", "Proxy.", "Item 12. Security Ownership", "Proxy.",
              "Item 13. Certain Relationships", "Proxy.", "Item 14. Principal Accountant Fees", "Proxy."]
    heads = FT.find_items(lines, "10-K")
    assert len(heads) == 8 and not any(h.toc for h in heads)


def test_heading_variants() -> None:
    lines = ["PART 1", "ITEM I. BUSINESS", "x", "Item 1.A. Risk Factors", "x", "ITEM 1. OUR BUSINESS", "x",
             "Item 7. Combined Management's Discussion and Analysis", "x", "Item 4.A. Executive Officers", "x",
             "Item 7 of this report describes our results", "x"]
    got = [(h.line, h.key) for h in FT.find_items(lines, "10-K")]
    assert got == [(1, "1"), (3, "1A"), (5, "1"), (7, "7"), (9, "4A")]
    f20 = ["ITEM 4. INFORMATION ON THE PARTNERSHIP", "x", "Item 3. D. Risk Factors", "x", "ITEM 16.A. AUDIT", "x"]
    assert [(h.key, h.sub) for h in FT.find_items(f20, "20-F")] == [("4", None), ("3", "D"), ("16A", None)]


def test_item_key_20f_sub_items() -> None:
    assert FT.item_key("3", "D", "20-F") == ("3", "D")
    assert FT.item_key("16", "K", "20-F") == ("16K", None)
    assert FT.item_key("1", "A", "10-K") == ("1A", None)
    assert FT.item_key("1", "D", "10-K") is None


# ------------------------------------------------------------------ sections


def test_extract_sections_10k_with_toc() -> None:
    secs, diag = _sections(TOC_10K)
    assert set(secs) == {"business", "risk", "mdna"}
    b, r, m = secs["business"], secs["risk"], secs["mdna"]
    assert b.text.startswith("Item") and "industrial pumps" in b.text and "Our pumps may fail" not in b.text
    assert "cross reference" in b.text  # the "Item 7 of this report" line stays inside Item 1
    assert "Acme Pump Co." not in b.text  # running footer dropped
    assert r.text.startswith("ITEM 1A.RISK FACTORS") and "Our pumps may fail" in r.text and "plant" not in r.text
    assert m.text.startswith("Item 7.") and "Revenue grew" in m.text and "little market risk" not in m.text
    assert all(s.method == "item" for s in secs.values())
    assert diag["toc_headings"] >= 10


def test_extract_sections_20f() -> None:
    secs, _ = _sections(TWENTY_F, "20-F")
    assert secs["risk"].text.startswith("D. Risk") and "risky" in secs["risk"].text
    assert "Capitalization" not in secs["risk"].text and "item3d" in secs["risk"].flags
    assert "pumps in Israel" in secs["business"].text and "UNRESOLVED" not in secs["business"].text
    assert secs["mdna"].text.startswith("Item 5. Operating") and "Revenue rose" in secs["mdna"].text
    assert "Our board" not in secs["mdna"].text


def test_cross_reference_index_is_not_a_section() -> None:
    lines = ["Cover page", "Item 1. Business 4-7, 9-10", "Item 1A. Risk Factors 24-31", "Item 1B. Unresolved None",
             "Item 2. Properties 4", "Item 3. Legal Proceedings 70", "Item 4. Mine Safety (a)",
             "Item 5. Market for equity 33", "Item 6. [Reserved]", "Item 7. Management's Discussion 8-30",
             "Item 7A. Quantitative 13", "Item 8. Financial Statements 36-73", "end"]
    secs, diag = FT.extract_sections("\n".join(lines), "10-K")
    assert "risk" not in secs and "business" not in secs
    assert set(diag["index_only"]) >= {"business", "risk", "mdna"}


def test_by_reference_and_in_document_reference() -> None:
    body = "We lend money to customers. " * 30
    ex13 = ["Item 1. Business", body, "Item 1A. Risk Factors", "Loans may default. " * 30,
            "Item 7. Management's Discussion and Analysis",
            "The information required by this item is incorporated by reference to the Annual Report.",
            "Item 8. Financial Statements", "See exhibit."]
    secs, _ = FT.extract_sections("\n".join(ex13), "10-K")
    assert "by_reference" in secs["mdna"].flags and secs["mdna"].method == "item"
    in_doc = ex13[:5] + ["Management's discussion and analysis appears on pages 40-90.", "Item 8. Financial Statements",
                         "See pages.", "Management's discussion and analysis", "Net interest income rose. " * 30,
                         "Consolidated balance sheets", "Assets."]
    secs, _ = FT.extract_sections("\n".join(in_doc), "10-K")
    assert secs["mdna"].method == "title" and "Net interest income rose" in secs["mdna"].text
    assert "Assets." not in secs["mdna"].text


def test_running_lines() -> None:
    lines = ["Acme | 2024 Form 10-K | 5", "text.", "Acme | 2024 Form 10-K | 6", "Acme | 2024 Form 10-K | 7",
             "Acme | 2024 Form 10-K | 8", "None.", "None.", "None.", "None."]
    assert FT.running_lines(lines) == {"Acme | # Form #-K | #"}


def test_exhibit_links() -> None:
    html = (b'<table><tr><td>1</td><td>10-K</td><td><a href="/Archives/edgar/data/1/0001/a10k.htm">a10k.htm</a></td>'
            b'<td>10-K</td></tr><tr><td>2</td><td>Annual report</td><td><a href="/Archives/edgar/data/1/0001/ex13.htm">'
            b'ex13.htm</a></td><td>EX-13</td></tr><tr><td>3</td><td>pdf</td><td><a href="/x/ex13.pdf">p</a></td>'
            b'<td>EX-13</td></tr></table>')
    assert FT.exhibit_links(html) == ["https://www.sec.gov/Archives/edgar/data/1/0001/ex13.htm"]


def test_extract_titled_annual_report() -> None:
    text = "\n".join(["Letter to shareholders", "Management's Discussion and Analysis of Financial Condition and "
                      "Results of Operations", "Deposits grew. " * 40, "Report of Independent Registered Public "
                      "Accounting Firm", "Opinion."])
    secs = FT.extract_titled(text, ["mdna", "risk"])
    assert set(secs) == {"mdna"} and "Deposits grew" in secs["mdna"].text and "Opinion" not in secs["mdna"].text


# ------------------------------------------------------------------ landing (no network)


class _FakeInner:
    lock_path = "fake"

    def __init__(self) -> None:
        self.n = 0

    def acquire(self, label: str | None = None) -> float:
        self.n += 1
        return 0.0

    def record_response(self, *a: Any, **k: Any) -> None:
        return None


def test_counting_limiter_budget() -> None:
    lim = FT.CountingLimiter(_FakeInner(), 2)
    lim.acquire()
    lim.acquire()
    assert lim.thread_count() == 2 and lim.exhausted
    with pytest.raises(FT.BudgetExhausted):
        lim.acquire()


def _row(acc: str, form: str = "10-K") -> dict[str, Any]:
    return {"accession": acc, "cik": 42, "ciks": [42], "form": form, "filing_date": dt.date(2024, 2, 1),
            "report_date": dt.date(2023, 12, 31), "acceptance_utc": dt.datetime(2024, 2, 1, 21, 5),
            "acceptance_clock": "file_utc", "vintage_risk": None, "available_at": dt.datetime(2024, 2, 1, 21, 5),
            "primary_document": "a.htm", "priority": 1}


def test_land_batches_receipts_parts_and_resume(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("ATX_SEC_TEXT_ROOT", str(tmp_path))
    pages = {"000004224024000001": TOC_10K.encode("utf-8")}

    def fake_get(url: str, limiter: Any, maximum: int = 0, timeout: float = 0.0) -> tuple[int, bytes, str | None, int, str]:
        key = url.rsplit("/", 2)[1]
        if key in pages:
            return 200, pages[key], None, 1, "2026-09-29T00:00:00+00:00"
        return (404, b"", None, 1, "2026-09-29T00:00:00+00:00") if key.endswith("2") else (
            503, b"", None, 6, "2026-09-29T00:00:00+00:00")

    monkeypatch.setattr(FT, "http_get", fake_get)
    lim = FT.CountingLimiter(_FakeInner(), 100)
    rows = [_row("0000042240-24-000001"), _row("0000042240-24-000002"), _row("0000042240-24-000003")]
    out = FT._run_batches(rows, FT.land_one, lim, 2, "docs", FT.DOC_SCHEMA, "doc")
    assert out["ok"] == 1 and out["failed"] == 2 and out["sections"] == 3
    state = FT.landing_state()
    assert state["landed"] == {"0000042240-24-000001"}
    assert state["failed"] == {"0000042240-24-000002"}  # 404 is terminal; 503 is retried next run
    assert state["requests_used"] == 8
    docs = pq.read_table(FT._parts("docs")[0]).to_pylist()
    assert docs[0]["sections_found"] == ["business", "mdna", "risk"] and docs[0]["sha256"]
    assert docs[0]["available_at"] == dt.datetime(2024, 2, 1, 21, 5)
    secs = pq.read_table(FT._parts("sections")[0]).to_pylist()
    assert {s["section"] for s in secs} == {"business", "risk", "mdna"}
    assert all(s["accession"] == "0000042240-24-000001" and s["source"] == "primary" for s in secs)
    receipts = FT.read_receipts()
    assert {r["http_status"] for r in receipts} == {200, 404, 503}
    assert all({"url", "bytes", "sha256", "http_status", "fetched_at"} <= set(r) for r in receipts)


def test_select_filings_member_scope_and_priority(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    root = tmp_path / "lake"
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(root))
    (root / "panel" / "year=2020").mkdir(parents=True)
    (root / "identity").mkdir()
    (root / "sec_filings").mkdir()
    # line 1 -> CIK 10 (panel cik); line 2 is member_equity with no panel cik but a backfill link to CIK 20;
    # line 3 is a member but not member_equity (CIK 30); CIK 40 is linked to nothing in the panel
    pq.write_table(pa.table({"session_date": [dt.date(2020, 3, 2)] * 3, "security_id": [1, 2, 3],
                             "member_equity": [True, True, False], "cik": [10, None, 30]}),
                   root / "panel" / "year=2020" / "panel-03.parquet")
    pq.write_table(pa.table({"security_id": [2, 3, 4], "cik": [20, 30, 40], "link_tier": ["backfill", "strict",
                                                                                          "strict"]}),
                   root / "identity" / "link_table.parquet")
    rows = []
    for cik in (10, 20, 30, 40):
        for n, (form, day) in enumerate((("10-K", dt.date(2018, 3, 1)), ("10-K", dt.date(2019, 3, 1)),
                                         ("40-F", dt.date(2020, 3, 1)), ("10-K/A", dt.date(2020, 4, 1)))):
            rows.append({"cik": cik, "accession": f"{cik:010d}-{n}", "form": form if cik != 20 or n != 1 else "20-F",
                         "filing_date": day, "report_date": None, "acceptance_utc": dt.datetime(2020, 1, 1),
                         "acceptance_clock": "file_utc", "vintage_risk": None, "available_at": dt.datetime(2020, 1, 1),
                         "primary_document": "a.htm"})
    pq.write_table(pa.Table.from_pylist(rows), root / "sec_filings" / "filings.parquet")
    con = FT.C.connect(memory="100MB", threads=1)
    got = FT.select_filings(con)
    assert {r["cik"] for r in got} == {10, 20}  # 30 is not member_equity, 40 is not a member line
    assert {r["form"] for r in got} == {"10-K", "20-F"}  # no 40-F, no amendments
    assert [(r["cik"], r["filing_date"].year, r["priority"]) for r in got] == [
        (10, 2019, 1), (20, 2019, 1), (10, 2018, 2), (20, 2018, 2)]


# ------------------------------------------------------------------ text features helpers


def test_words_paragraphs_sentences() -> None:
    assert TF.words("The Company's long-term plan: 2024 sales rose.") == ["the", "company's", "long-term", "plan",
                                                                          "sales", "rose"]
    text = "If we fail to compete, it\nwill hurt us. Sales fell.\nRisks Related to Our Business\nWe may lose."
    assert TF.paragraphs(text) == ["If we fail to compete, it will hurt us. Sales fell.",
                                   "Risks Related to Our Business", "We may lose."]
    assert TF.sentences(text) == ["If we fail to compete, it will hurt us.", "Sales fell.",
                                  "Risks Related to Our Business", "We may lose."]


def test_syllables_and_fog() -> None:
    assert TF.syllables("the") == 1 and TF.syllables("company") == 3 and TF.syllables("reliable") == 3
    assert TF.syllables("financial") >= 3 and TF.syllables("pumps") == 1
    text = ("The company sells pumps. " * 20).strip()
    r = TF.readability(text)
    assert r["words"] == 80 and r["sentences"] == 20
    assert r["fog"] == pytest.approx(0.4 * (4 + 100 * 20 / 80))
    assert TF.readability("Too short.")["fog"] is None


def test_similarity_measures() -> None:
    a = Counter({"x": 2, "y": 1})
    b = Counter({"x": 2, "y": 1})
    assert TF.cosine_tf(a, b) == pytest.approx(1.0)
    assert TF.cosine_tf(Counter({"x": 1}), Counter({"y": 1})) == 0.0
    assert TF.cosine_tf(Counter(), a) is None
    assert TF.jaccard({"a", "b"}, {"b", "c"}) == pytest.approx(1 / 3)
    s1 = ["One.", "Two.", "Three.", "Four."]
    assert TF.edit_similarity(s1, s1) == 1.0
    assert TF.edit_similarity(s1, s1[:2] + ["New."] + s1[2:]) == pytest.approx(2 * 4 / 9)
    cur, prev = TF.section_profile("Risk one. Risk two."), TF.section_profile("Risk one.")
    cmp = TF.compare(cur, prev)
    assert cmp["words_chg"] == pytest.approx(math.log(4 / 2)) and 0 < cmp["sim_cosine"] < 1
    assert TF.compare(cur, None)["sim_cosine"] is None


def test_compact_profile_matches_counter_measures() -> None:
    a = "Our pumps may fail. Demand may fall.\nWe compete with larger firms. Prices may drop."
    b = "Our pumps may fail. Demand may fall sharply.\nWe compete with larger firms."
    ref = TF.compare(TF.section_profile(a), TF.section_profile(b))
    got = TF.compare_compact(TF.compact_profile(a), TF.compact_profile(b))
    for k in ("sim_cosine", "sim_jaccard", "sim_minedit", "words_chg"):
        assert got[k] == pytest.approx(ref[k]), k


def test_prior_filing_window_and_family() -> None:
    hist = [{"accession": "a", "form": "10-K", "filing_date": dt.date(2020, 3, 1)},
            {"accession": "b", "form": "10-KT", "filing_date": dt.date(2020, 9, 1)},
            {"accession": "c", "form": "20-F", "filing_date": dt.date(2021, 1, 5)},
            {"accession": "d", "form": "10-K", "filing_date": dt.date(2021, 3, 1)}]
    assert TF.prior_filing(hist, hist[3])["accession"] == "b"  # 181 days, latest in window
    assert TF.prior_filing(hist, hist[1])["accession"] == "a"  # 184 days: in the window
    assert TF.prior_filing(hist, hist[2]) is None  # no earlier 20-F


def _text_landing(land: Path, root: Path) -> None:
    risk = ["Our pumps may fail. Demand may fall. Competition is intense. " * 20,
            "Our pumps may fail. Demand may fall. Tariffs may rise. " * 20]
    docs, secs = [], []
    for k, year in enumerate((2019, 2020)):
        acc = f"0000000007-{year % 100}-000001"
        clock = dt.datetime(year, 2, 20, 21, 30)
        base = {"accession": acc, "cik": 7, "ciks": [7], "form": "10-K", "filing_date": clock.date(),
                "report_date": dt.date(year - 1, 12, 31)}
        docs.append({**base, "acceptance_utc": clock, "acceptance_clock": "file_utc", "vintage_risk": None,
                     "available_at": clock, "priority": 1, "primary_document": "a.htm", "url": "u", "http_status": 200,
                     "doc_bytes": 1000, "sha256": "x", "fetched_at": "t", "requests": 1, "error": None,
                     "text_chars": 10, "doc_words": 500, "doc_sentences": 40, "doc_complex_words": 50, "doc_fog": 12.0,
                     "headings": 20, "toc_headings": 10, "running_lines": 0, "index_only": [],
                     "sections_found": ["risk"], "parser": "t"})
        secs.append({**base, "available_at": clock, "section": "risk", "item": "1A", "method": "item", "flags": [],
                     "n_chars": len(risk[k]), "source": "primary", "parser": "t", "text": risk[k]})
    for kind, rows, schema in (("docs", docs, FT.DOC_SCHEMA), ("sections", secs, FT.SECTION_SCHEMA)):
        (land / kind).mkdir(parents=True)
        pq.write_table(pa.Table.from_pylist(rows, schema=schema), land / kind / "part-00001.parquet")
    sessions = [dt.date(2020, 1, 1) + dt.timedelta(days=i) for i in range(366) if
                (dt.date(2020, 1, 1) + dt.timedelta(days=i)).weekday() < 5]
    root.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.table({"session_date": sessions}), root / "calendar.parquet")
    (root / "_tmp" / "panel_member").mkdir(parents=True)
    pq.write_table(pa.table({"session_date": sessions, "security_id": [70] * len(sessions),
                             "member": [True] * len(sessions)}), root / "_tmp" / "panel_member" / "year=2020.parquet")
    (root / "identity").mkdir()
    pq.write_table(pa.Table.from_pylist([{
        "security_id": 70, "cik": 7, "valid_from": dt.date(2019, 1, 1), "valid_to": dt.date(2026, 1, 1),
        "link_tier": "strict", "available_at": dt.datetime(2019, 1, 1), "evidence_at": dt.datetime(2019, 1, 1)}]),
        root / "identity" / "link_table.parquet")


def test_features_build_and_member_coverage(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    land, root = tmp_path / "land", tmp_path / "lake"
    monkeypatch.setenv("ATX_SEC_TEXT_ROOT", str(land))
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(root))
    _text_landing(land, root)
    assert TF.profile_pass()["rows"] == 2
    assert TF.profile_pass()["skipped"] == 1  # resumable per part
    out = TF.build_features()
    assert out["rows"] == 2 and out["comparisons"] == 1
    rows = pq.read_table(root / TF.STAGE / "features.parquet").to_pylist()
    r20 = next(r for r in rows if r["filing_date"].year == 2020)
    assert r20["prior_accession"] == "0000000007-19-000001" and 0.5 < r20["risk_sim_cosine"] < 1.0
    assert r20["risk_sim_minedit"] == pytest.approx(2 * 40 / 120, abs=0.05)
    assert r20["risk_words_chg"] == pytest.approx(0.0, abs=0.05) and r20["business_words"] is None
    cov = TF.member_coverage([2020], bases=("panel_member_linked",))["panel_member_linked"]["2020"]
    # 2019 filing (2019-02-20) is > 400 days old from 2020-03-26; the 2020 filing is visible from 2020-02-21's
    # session only when accepted before 22:00 UTC of the previous session (2020-02-20 21:30 < 22:00) -> 2020-02-21
    sessions = [d for d in (dt.date(2020, 1, 1) + dt.timedelta(days=i) for i in range(366)) if d.weekday() < 5]
    fresh = [d for d in sessions if d >= dt.date(2020, 2, 21) or (d - dt.date(2019, 2, 20)).days <= 400]
    assert cov["cells"] == len(sessions)
    assert cov["any_section"] == pytest.approx(round(len(fresh) / len(sessions), 4))
    sim = [d for d in sessions if d >= dt.date(2020, 2, 21)]
    assert cov["risk_sim_cosine"] == pytest.approx(round(len(sim) / len(sessions), 4))
