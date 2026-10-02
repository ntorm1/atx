"""S7.1 classification (rule classification-v1): FF schemes from French ranges, SIC -> NAICS concordance picks."""

from __future__ import annotations

import datetime as dt

from atx_db.alpha_panel import classification as K

SIC_ROWS = [
    ["SIC", "SIC Title (and note)", "2002 NAICS", "2002 NAICS Title"],
    ["111", "Wheat", "111140", "Wheat Farming"],
    ["119", "Cash Grains, NEC (oilseed farming, except soybeans)", "111120", "Oilseed (except Soybean) Farming"],
    ["119", "Cash Grains, NEC (except popcorn, dry pea and bean)", "111199", "All Other Grain Farming"],
    ["3571", "Electronic Computers", "334111", "Electronic Computer Manufacturing"],
    ["3572", "Computer Storage Devices", "334112", "Computer Storage Device Manufacturing"],
    ["3577", "Computer Peripheral Equipment, NEC (printers)", "334119", "Other Computer Peripheral"],
    ["3577", "Computer Peripheral Equipment, NEC (scanners)", "334119", "Other Computer Peripheral"],
    ["3861", "Photographic Equipment (films)", "325992", "Photographic Film"],
    ["3861", "Photographic Equipment (cameras)", "333315", "Photographic Equipment"],
]
STEP = [
    ["2002 NAICS U.S. Matched to 2007 NAICS U.S."], ["(Note: ...)"],
    ["2002 NAICS Code", "2002 NAICS Title", "2007 NAICS Code", "2007 NAICS Title"],
    [334111, "Electronic Computer Manufacturing", 334111, "Electronic Computer Manufacturing"],
    [334112, "Computer Storage Device Manufacturing", 334112, "Computer Storage Device Manufacturing"],
    [334119, "Other Computer Peripheral (except x)", 334118, "Computer Terminal and Other Peripheral"],
    [334119, "Other Computer Peripheral (ATMs)", 333318, "Other Commercial Machinery"],
]


def test_concordance_rows_skip_banner_and_header():
    rows = K.concordance_rows(STEP)
    assert rows[0] == (334111, "Electronic Computer Manufacturing", 334111, "Electronic Computer Manufacturing")
    assert len(rows) == 4


def test_primary_piece_rule():
    pm = K.primary_map(K.concordance_rows(SIC_ROWS))
    assert pm[111] == (111140, "Wheat Farming", "whole", 1)
    assert pm[119][:3] == (111199, "All Other Grain Farming", "except_piece")   # the SIC's main body
    assert pm[3577][:3] == (334119, "Other Computer Peripheral", "whole")        # every piece to one NAICS
    assert pm[3861][:3] == (325992, "Photographic Film", "first_piece") and pm[3861][3] == 2


def test_sec_group_codes_fall_back_to_group_mode():
    pm = K.primary_map(K.concordance_rows(SIC_ROWS))
    assert K.sic_to_naics02(3571, pm)[:3] == (334111, "Electronic Computer Manufacturing", "sic4_whole")
    code, _, basis, n, prefix = K.sic_to_naics02(3570, pm)   # SEC industry-group code: hierarchical mode of 357x
    assert basis == "sic3_group" and code == 334111 and n == 3 and prefix == "33411"
    assert K.sic_to_naics02(3500, pm)[2] == "sic2_group"
    assert K.sic_to_naics02(8880, pm)[:3] == (None, None, "no_concordance")


def test_chain_to_later_vintages_flags_splits():
    step = K.primary_map(K.concordance_rows(STEP))
    assert K.chain_naics(334111, [step]) == (334111, "Electronic Computer Manufacturing", False)
    assert K.chain_naics(334119, [step]) == (334118, "Computer Terminal and Other Peripheral", True)
    assert K.chain_naics(None, [step]) == (None, None, False)


SICCODES = [
    {"scheme": 12, "industry_no": 1, "industry_short": "NoDur", "sic_lo": 100, "sic_hi": 999},
    {"scheme": 12, "industry_no": 6, "industry_short": "BusEq", "sic_lo": 3570, "sic_hi": 3579},
    {"scheme": 12, "industry_no": 12, "industry_short": "Other", "sic_lo": None, "sic_hi": None},
    {"scheme": 49, "industry_no": 35, "industry_short": "Hardw", "sic_lo": 3570, "sic_hi": 3579},
    {"scheme": 49, "industry_no": 49, "industry_short": "Other", "sic_lo": 4950, "sic_hi": 4959},
]


def test_ff_ranges_residual_and_unlisted_other():
    ff = K.ff_assigner(SICCODES)
    assert K.ff_of(3571, ff[12]) == (6, "BusEq", True)
    assert K.ff_of(1311, ff[12]) == (12, "Other", True)       # residual industry without ranges
    assert K.ff_of(3571, ff[49]) == (35, "Hardw", True)
    assert K.ff_of(1311, ff[49]) == (49, "Other", False)      # unlisted in 49: Other, flagged


def test_sic_runs_keep_first_clock_of_each_sic_run():
    t = dt.datetime
    ev = [(1, t(2020, 1, 1), 3571, "a", "fsds_sub"), (1, t(2020, 4, 1), 3571, "b", "fsds_sub"),
          (1, t(2020, 7, 1), 3572, "c", "fsds_sub"), (1, t(2020, 10, 1), 3571, "d", "carried"),
          (2, t(2020, 1, 1), 0, "e", "fsds_sub")]
    assert [r[3] for r in K.sic_runs(ev)] == ["a", "c", "d"]


def test_sic_runs_sql_matches_python_rule(tmp_path):
    import pyarrow as pa
    import pyarrow.parquet as pq

    t = dt.datetime
    ev = [(1, t(2020, 1, 1), 3571, "a", "fsds_sub"), (1, t(2020, 4, 1), 3571, "b", "fsds_sub"),
          (1, t(2020, 7, 1), 3572, "c", "fsds_sub"), (1, t(2020, 10, 1), 3571, "d", "carried"),
          (2, t(2020, 1, 1), 0, "e", "fsds_sub"), (2, t(2020, 2, 1), 2834, "f", "fsds_sub")]
    cols = ["cik", "clock_utc", "sic", "accession", "sic_basis"]
    path = tmp_path / "sic_events.parquet"
    pq.write_table(pa.table({c: [e[i] for e in ev] for i, c in enumerate(cols)}), path)
    runs, n = K.sic_runs_sql(path)
    assert n == 6 and [r[3] for r in runs] == [r[3] for r in K.sic_runs(ev)] == ["a", "c", "d", "f"]


def test_curated_primary_hier_mode_and_prefix():
    rows = K.concordance_rows(SIC_ROWS + [["7372", "Prepackaged Software (mass reproduction)", "334611",
                                           "Software Reproducing"],
                                          ["7372", "Prepackaged Software (software publishing)", "511210",
                                           "Software Publishers"]])
    pm = K.primary_map(rows)
    got = K.sic_to_naics02(7372, pm, K.candidates_map(rows), {b: t for _, _, b, t in rows})
    assert got == (511210, "Software Publishers", "curated_primary", 2, None)
    assert K.hier_mode([561612, 561621, 519110]) == 561612
    assert K.common_prefix({221111, 221122}) == "2211" and K.common_prefix({334611, 511210}) is None
