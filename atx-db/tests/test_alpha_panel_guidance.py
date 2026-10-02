"""Rule-based guidance extraction (events table ``guidance``, S6.4): values, measures, periods, priors, revisions."""

from __future__ import annotations

import datetime as dt

from atx_db.alpha_panel import events_sources as ES
from atx_db.alpha_panel import guidance as G

Q1_25 = G.ReportedPeriod(dt.date(2025, 3, 31), 12, 31)
Q2_23 = G.ReportedPeriod(dt.date(2023, 6, 30), 12, 31)
Q4_24 = G.ReportedPeriod(dt.date(2024, 12, 31), 12, 31)


def _one(s: str, rep: G.ReportedPeriod = Q1_25, **kw) -> list[G.GuidanceRow]:
    return G.extract_from_sentence(s, rep, (rep.period_end or dt.date(2025, 1, 1)).year, kw.get("heading"),
                                   kw.get("prefix"), kw.get("in_outlook", False))


def test_values_ranges_points_scales() -> None:
    v = G.find_values("revenue of $1.31 billion to $1.39 billion and EPS of $2.74 - $2.84")
    assert [(x.low, x.high, x.scale_word) for x in v] == [(1.31, 1.39, "billion"), (2.74, 2.84, None)]
    v = G.find_values("from a range of $27.5-$28.5 million")[0]
    assert (v.low, v.high, v.scale_word, v.raw) == (27.5, 28.5, "million", "$27.5-$28.5 million")
    v = G.find_values("between $0.68 to $0.72")[0]
    assert (v.low, v.high) == (0.68, 0.72)
    v = G.find_values("approximately $9.6 billion, plus or minus $300 million")[0]
    assert v.kind == "range" and abs(v.low - 9.3e9) < 1 and abs(v.high - 9.9e9) < 1
    v = G.find_values("a loss of $(0.10) to $(0.05)")[0]
    assert (v.low, v.high) == (-0.10, -0.05)
    assert G.find_values("C$1.20 per share") == []
    assert G.find_values("growth of 5% to 7%") == []


def test_sentence_eps_range_with_prior() -> None:
    rows = _one("The company now expects adjusted earnings from continuing operations, before special items, of "
                "$2.47 to $2.55 per diluted share, compared to prior guidance of $2.45 to $2.55 per diluted share.",
                prefix=G.PeriodMention(0, 0, "FY", 2025))
    assert len(rows) == 1
    r = rows[0]
    assert (r.measure, r.basis, r.period_type, r.fiscal_year, r.low, r.high) == ("EPS", "ADJUSTED", "FY", 2025, 2.47, 2.55)
    assert (r.prior_low, r.prior_high) == (2.45, 2.55)


def test_sentence_revenue_from_to_change() -> None:
    rows = _one('"With respect to fiscal 2023, we are increasing our guidance for revenue (net of iGaming royalties) '
                'from a range of $27.5-$28.5 million to a range of $29-$30 million."', rep=Q2_23)
    assert [(r.measure, r.low, r.high, r.prior_low, r.prior_high, r.text_direction) for r in rows] == [
        ("REVENUE", 29e6, 30e6, 27.5e6, 28.5e6, "raised")]


def test_sentence_quarter_inferred_and_both_measures() -> None:
    rows = _one("CTS expects full-year 2026 sales to be in the range of $550-$580 million and adjusted diluted EPS to "
                "be in the range of $2.30-$2.45.", rep=G.ReportedPeriod(dt.date(2025, 12, 31)))
    assert [(r.measure, r.basis, r.fiscal_year, r.low, r.high) for r in rows] == [
        ("REVENUE", "UNSPECIFIED", 2026, 550e6, 580e6), ("EPS", "ADJUSTED", 2026, 2.30, 2.45)]
    rows = _one("FMC expects adjusted earnings per diluted share to be in the range of $1.70 to $2.00 in the second "
                "quarter.")
    assert [(r.period_type, r.fiscal_year, r.fiscal_quarter, r.period_basis) for r in rows] == [("Q", 2025, 2, "stated")] \
        or [(r.period_type, r.fiscal_year, r.fiscal_quarter) for r in rows] == [("Q", 2025, 2)]


def test_historical_and_non_guidance_values_are_dropped() -> None:
    # a result measured against old guidance
    assert _one("Adjusted net income was $0.55 per diluted share and exceeded the company's original guidance of "
                "$0.47 to $0.50 per diluted share.") == []
    # a reported (past) period
    assert _one("TXNM Energy reported 2024 GAAP earnings of $2.67 per diluted share, in line with guidance.", Q4_24) == []
    # an FX impact per share and a change amount are not EPS / revenue guidance
    assert _one("Overall, we anticipate a full-year negative FX impact of approximately $0.45 per share.") == []
    assert _one("We expect the acquisition to increase full-year revenue by approximately $10 million.") == []
    # FFO per share is not EPS; EBITDA is not revenue
    assert _one("The company expects 2025 FFO of $1.20 to $1.25 per diluted share.") == []
    assert _one("We expect full-year 2025 adjusted EBITDA of $1.3 billion to $1.4 billion.") == []
    # a base-year actual in a growth statement
    assert _one("CPKC expects core adjusted diluted EPS to grow double digits versus 2023 core adjusted diluted EPS "
                "of $3.84.", Q4_24) == []
    # prior guidance alone
    assert _one("This compares to our previous 2025 adjusted earnings per share guidance of $19.10 to $19.50.") == []


def test_segment_revenue_is_skipped_but_headline_verbs_are_not() -> None:
    assert _one("2025 Outlook for Corporate & Other Net Sales of approximately $50 million is unchanged.") == []
    rows = _one("Maintains revenue outlook of $5.25 to $5.55 billion", heading=G.PeriodMention(0, 0, "FY0"),
                in_outlook=True)
    assert [(r.measure, r.fiscal_year, r.period_basis, r.text_direction) for r in rows] == [
        ("REVENUE", 2025, "heading", "maintained")]


def test_floor_and_point_with_percent_tolerance() -> None:
    rows = _one("Targeting Full Year 2025 EPS in Excess of $1.50")
    assert [(r.value_type, r.low, r.high, r.point) for r in rows] == [("floor", 1.5, None, None)]
    rows = _one("For the fourth quarter of 2025, we expect revenue to be approximately $337 million, plus or minus "
                "3 percent.")
    assert rows[0].value_type == "range" and abs(rows[0].low - 326.89e6) < 1 and abs(rows[0].high - 347.11e6) < 1


def test_period_after_value_wins() -> None:
    rows = _one("For 2025, NextEra Energy expects to grow 6% to 8% off the 2024 adjusted earnings per share "
                "expectations range, which translates to a range of $3.45 to $3.70 per share.",
                G.ReportedPeriod(dt.date(2023, 12, 31)))
    assert [(r.fiscal_year, r.low, r.high) for r in rows] == [(2025, 3.45, 3.70)]


def test_reported_label_and_forward_rule() -> None:
    lines = ["Target Corporation Reports Third Quarter 2024 Earnings", "Fiscal 2024 outlook"]
    rep = G.ReportedPeriod(dt.date(2024, 11, 2), 2, 1)
    assert G.detect_reported_label(lines, rep, 2024) == (2024, 3)
    lab = G.ReportedPeriod(rep.period_end, 2, 1, (2024, 3))
    assert G.is_forward("FY", 2024, None, lab) and not G.is_forward("FY", 2023, None, lab)
    assert G.is_forward("Q", 2024, 4, lab) and not G.is_forward("Q", 2024, 3, lab)
    assert not G.is_forward("FY", 2028, None, lab)  # more than two years ahead: a long-term target
    q4 = G.ReportedPeriod(dt.date(2024, 12, 31))
    assert not G.is_forward("FY", 2024, None, q4) and G.is_forward("FY", 2025, None, q4)


def test_table_outlook_with_prior_column_and_plus_minus() -> None:
    text = "\n".join([
        "Full-Year 2025 Guidance*",
        "Adjusted Metric* | FY25 Guidance as of September 11, 2025 | FY25 Guidance as of December 4, 2025 |",
        "Identical Sales without fuel | 2.7% - 3.4% | 2.8% - 3.0% |",
        "EPS | $4.70 - $4.80 | $4.75 - $4.80 |",
        "Business Outlook",
        "| | | Q4 FY2026 | | |",
        "(In millions, except per share amounts) | | | |",
        "Total revenue | | | $ | 10,250 | | +/- | $ | 500 | |",
        "Non-GAAP diluted EPS | | | $ | 4.02 | | +/- | $ | 0.20 | |",
    ])
    rows = G.extract_guidance(text, G.ReportedPeriod(dt.date(2025, 11, 8), 1, 31, (2025, 3)), 2025)
    eps = [r for r in rows if r.fiscal_year == 2025]
    assert [(r.measure, r.basis, r.low, r.high, r.prior_low, r.prior_high) for r in eps] == [
        ("EPS", "ADJUSTED", 4.75, 4.80, 4.70, 4.80)]
    amat = G.extract_guidance("\n".join(text.split("\n")[4:]), G.ReportedPeriod(dt.date(2026, 7, 26), 10, 26), 2026)
    got = {(r.measure, r.basis, round(r.low, 2), round(r.high, 2)) for r in amat}
    assert got == {("REVENUE", "UNSPECIFIED", 9.75e9, 10.75e9), ("EPS", "ADJUSTED", 3.82, 4.22)}


def test_results_table_is_not_guidance() -> None:
    text = "\n".join(["Guidance and results", "| Third-Quarter | Year-to-Date |", "Core EPS | $1.79 | $4.73 |"])
    assert G.extract_guidance(text, G.ReportedPeriod(dt.date(2021, 9, 4), 12, 26), 2021) == []


def test_revision_against_previous_release_and_text_prior() -> None:
    base = {"measure": "EPS", "basis": "ADJUSTED", "period_type": "FY", "fiscal_year": 2025, "fiscal_quarter": None,
            "prior_low": None, "prior_high": None}
    rows = [
        {**base, "accession": "a1", "available_at": dt.datetime(2025, 2, 1), "low": 2.0, "high": 2.2, "point": 2.1},
        {**base, "accession": "a2", "available_at": dt.datetime(2025, 5, 1), "low": 2.1, "high": 2.3, "point": 2.2},
        {**base, "accession": "a3", "available_at": dt.datetime(2025, 8, 1), "low": 2.1, "high": 2.3, "point": 2.2},
        {**base, "accession": "a4", "available_at": dt.datetime(2025, 11, 1), "low": 1.9, "high": 2.0, "point": 1.95},
        {**base, "fiscal_year": 2026, "accession": "a4", "available_at": dt.datetime(2025, 11, 1), "low": 2.4,
         "high": 2.5, "point": 2.45, "prior_low": 2.3, "prior_high": 2.4},
    ]
    out = G.revise(rows)
    assert [r["revision"] for r in out] == ["new", "raised", "maintained", "lowered", "raised"]
    assert [r["revision_basis"] for r in out] == ["none", "previous_release", "previous_release", "previous_release",
                                                  "text_prior"]
    assert out[1]["prev_accession"] == "a1"


def test_document_text_rows_and_footnotes() -> None:
    html = (b"<html><body><p>Outlook</p><p>Raises EPS guidance to $2.47 to $2.55"
            b"<font style='position:relative;top:-4.2pt'>1</font></p>"
            b"<table><tr><td><p>EPS</p></td><td><p>$4.70 -\n $4.80</p></td></tr></table></body></html>")
    lines = ES.document_text(html, "x.htm").split("\n")
    assert "Raises EPS guidance to $2.47 to $2.55" in lines
    assert any(line.startswith("EPS | $4.70 - $4.80") for line in lines)
