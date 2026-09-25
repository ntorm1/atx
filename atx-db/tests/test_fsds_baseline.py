"""P12: FSDS loader -> canonical mapping -> coverage -> fact_disagreement harness, end to end.

One compact fixture (no network): a quarterly FSDS zip with two Apple 10-Ks and a non-panel filer.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import zipfile

import pandas as pd
import pytest

from atx_db.fsds_baseline import (
    BenchmarkIssuer,
    FsdsArchive,
    benchmark_coverage,
    canonical_fsds_facts,
    canonical_rules,
    fsds_alias_facts,
    load_fsds_subset,
    open_fsds_subset,
    run_fsds_comparison,
)

A1 = "0000320193-24-000123"  # 10-K FY2024 (original vintage)
A2 = "0000320193-25-000079"  # 10-K FY2025 (re-reports FY2024 balance sheet)
A9 = "0000320193-26-000010"  # later filing, outside the FSDS window
AAPL = BenchmarkIssuer("AAPL", "0000320193", "tech", 4)

SUB = [
    "adsh\tcik\tname\tform\tperiod\tfy\tfp\tfiled\taccepted\tprevrpt",
    f"{A2}\t320193\tApple Inc.\t10-K\t20250930\t2025\tFY\t20251031\t2025-10-31 06:01:00.0\t0",
    f"{A1}\t320193\tApple Inc.\t10-K\t20240930\t2024\tFY\t20241101\t2024-11-01 06:01:00.0\t0",
    "0000999999-25-000001\t999999\tOther Co\t10-K\t20250930\t2025\tFY\t20251101\t2025-11-01 06:01:00.0\t0",
]
NUM_ROWS = [
    (A2, "RevenueFromContractWithCustomerExcludingAssessedTax", "us-gaap/2025", "20250930", 4, "USD", "", "", "400000000000"),
    (A2, "Revenues", "us-gaap/2025", "20250930", 4, "USD", "", "", "410000000000"),
    (A2, "CostOfGoodsAndServicesSold", "us-gaap/2025", "20250930", 4, "USD", "", "", "220000000000"),
    (A2, "NetIncomeLoss", "us-gaap/2025", "20250930", 4, "USD", "", "", "100000000000"),
    (A2, "EarningsPerShareDiluted", "us-gaap/2025", "20250930", 4, "USD", "", "", "6.08"),  # FSDS uom
    (A2, "Assets", "us-gaap/2025", "20250930", 0, "USD", "", "", "360000000000"),
    (A2, "Assets", "us-gaap/2025", "20240930", 0, "USD", "", "", "350000000000"),
    (A2, "Assets", "us-gaap/2025", "20250930", 0, "USD", "BusinessSegments=Americas;", "", "100000000000"),
    (A2, "Assets", "us-gaap/2025", "20250930", 0, "USD", "", "AppleOpsLLC", "50000000000"),
    (A2, "NetCashProvidedByUsedInOperatingActivities", "us-gaap/2025", "20250930", 1, "USD", "", "", "30000000000"),
    (A2, "NetCashProvidedByUsedInOperatingActivities", "us-gaap/2025", "20250930", 4, "USD", "", "", "110000000000"),
    (A2, "OperatingProfitAdjusted", A2, "20250930", 4, "USD", "", "", "120000000000"),
    (A2, "PaymentsToAcquirePropertyPlantAndEquipment", "us-gaap/2025", "20250930", 4, "USD", "", "", "12000000000"),
    (A1, "Assets", "us-gaap/2024", "20240930", 0, "USD", "", "", "340000000000"),
    (A1, "RevenueFromContractWithCustomerExcludingAssessedTax", "us-gaap/2024", "20240930", 4, "USD", "", "", "390000000000"),
    ("0000999999-25-000001", "Assets", "us-gaap/2025", "20250930", 0, "USD", "", "", "1"),
]


def _fixture_zip(tmp_path) -> FsdsArchive:
    num = ["adsh\ttag\tversion\tddate\tqtrs\tuom\tsegments\tcoreg\tvalue\tfootnote"]
    num += ["\t".join(str(v) for v in row) + "\t" for row in NUM_ROWS]
    tags = ["tag\tversion\tcustom\tabstract\tdatatype\tiord\tcrdr\ttlabel\tdoc"]
    tags += [f"{row[1]}\t{row[2]}\t{1 if row[2] == A2 else 0}\t0\tmonetary\tD\tC\t{row[1]}\t" for row in NUM_ROWS]
    path = tmp_path / "2025q4.zip"
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("sub.txt", "\n".join(SUB) + "\n")
        archive.writestr("num.txt", "\n".join(num) + "\n")
        archive.writestr("tag.txt", "\n".join(sorted(set(tags), key=tags.index)) + "\n")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return FsdsArchive("2025q4", path, digest, path.stat().st_size, "fixture", False)


def _std(store, item_id, code, basis, period_end, value, accession, available_at, latest=True) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, upstream_source, security_id, symbol, cik, item_id, canonical_code,
            basis, period_end, value, unit_type, source_accession, filed_date, as_of_date, available_at,
            input_codes_json, input_item_ids_json, rule_id, combination_rule, is_latest_revision, run_id
        ) VALUES (?, 'fundamental_standardization_v1', 'fixture', 'SEC-AAPL', 'AAPL', '0000320193', ?, ?,
                  ?, ?, ?, 'USD', ?, ?, ?, ?, '[]', '[]', 'fixture_rule', 'coalesce_priority', ?, 'fixture')
        """,
        [f"{code}|{basis}|{period_end}|{accession}", item_id, code, basis, period_end, value, accession,
         available_at.date(), period_end, available_at, latest],
    )


def test_fsds_loader_mapping_coverage_and_parity_harness(tmp_path, tmp_store) -> None:
    subset = tmp_path / "P12-subset.duckdb"
    archive = _fixture_zip(tmp_path)
    stats = load_fsds_subset([archive], subset, ciks=[AAPL.cik])
    assert (stats[0]["sub_rows"], stats[0]["num_rows"]) == (2, 15)  # non-panel filer dropped
    assert load_fsds_subset([archive], subset, ciks=[AAPL.cik])[0]["reused"] is True  # same zip hash

    rules = canonical_rules()
    con = open_fsds_subset(subset, read_only=True)
    try:
        canon = canonical_fsds_facts(fsds_alias_facts(con, rules), rules)
        coverage = benchmark_coverage(con, (AAPL,), years=2, rules_map=rules)
    finally:
        con.close()

    got = {
        (r.benchmark_item, r.basis, r.ddate.isoformat(), r.adsh): (r.value, r.derivation, json.loads(r.source_tags_json))
        for r in canon.itertuples(index=False)
    }
    fy25, fy24 = "2025-09-30", "2024-09-30"
    assert got == {
        # priority 10 alias beats Revenues (20); qtrs=4 -> annual
        ("revenue", "annual", fy25, A2): (400e9, "direct", ["us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax"]),
        ("revenue", "annual", fy24, A1): (390e9, "direct", ["us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax"]),
        # no GrossProfit tag: coalesce_or_difference over raw revenue - COGS in the same filing
        ("gross_profit", "annual", fy25, A2): (180e9, "derived", [
            "us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax", "us-gaap:CostOfGoodsAndServicesSold"]),
        ("net_income", "annual", fy25, A2): (100e9, "direct", ["us-gaap:NetIncomeLoss"]),
        ("eps_diluted", "annual", fy25, A2): (6.08, "direct", ["us-gaap:EarningsPerShareDiluted"]),
        # qtrs=0 -> instant; segment and co-registrant facts excluded; comparative kept per filing
        ("total_assets", "instant", fy25, A2): (360e9, "direct", ["us-gaap:Assets"]),
        ("total_assets", "instant", fy24, A2): (350e9, "direct", ["us-gaap:Assets"]),
        ("total_assets", "instant", fy24, A1): (340e9, "direct", ["us-gaap:Assets"]),
        ("cfo", "annual", fy25, A2): (110e9, "direct", ["us-gaap:NetCashProvidedByUsedInOperatingActivities"]),
        ("cfo", "quarterly", fy25, A2): (30e9, "direct", ["us-gaap:NetCashProvidedByUsedInOperatingActivities"]),
        ("capex", "annual", fy25, A2): (12e9, "direct", ["us-gaap:PaymentsToAcquirePropertyPlantAndEquipment"]),
    }

    cells = coverage.cells.set_index(["benchmark_item", coverage.cells["fy_end"].astype(str)])
    assert coverage.summary["cells"] == 20 and coverage.summary["mapped"] == 9
    assert cells.loc[("operating_income", fy25), "reason"] == "custom_extension"
    assert json.loads(cells.loc[("operating_income", fy25), "detail"]) == ["OperatingProfitAdjusted"]
    assert cells.loc[("gross_profit", fy24), "reason"] == "derivation_incomplete"
    assert cells.loc[("total_assets", fy24), "n_distinct_values"] == 2  # re-reported within the window

    # Warehouse side (exact period ends 2025-09-27 / 2024-09-28 vs FSDS month-end rounding).
    p25, p24 = dt.date(2025, 9, 27), dt.date(2024, 9, 28)
    t1, t2, t9 = dt.datetime(2024, 11, 3), dt.datetime(2025, 11, 2), dt.datetime(2026, 2, 1)
    _std(tmp_store, 1001, "revenue", "annual", p25, 400.8e9, A2, t2)  # 0.2 % off -> agrees
    _std(tmp_store, 1001, "revenue", "annual", p24, 390e9, A1, t1)
    _std(tmp_store, 1004, "gross_profit__1004", "annual", p25, 180e9, A2, t2)
    _std(tmp_store, 1031, "net_income_total", "annual", p25, 99e9, A2, t2)  # 1 % off -> mismatch
    _std(tmp_store, 1035, "eps_diluted", "annual", p25, 6.20, A2, t2)  # $1M floor would hide this
    _std(tmp_store, 1101, "total_assets", "instant", p25, 360e9, A2, t2)
    _std(tmp_store, 1101, "total_assets", "instant", p24, 340e9, A1, t1, latest=False)
    _std(tmp_store, 1101, "total_assets", "instant", p24, 350e9, A2, t2, latest=False)
    _std(tmp_store, 1101, "total_assets", "instant", p24, 330e9, A9, t9)  # later restatement
    _std(tmp_store, 1305, "capex__1305", "annual", p24, 11e9, A1, t1)  # capex only for FY2024

    result = run_fsds_comparison(tmp_store, coverage.grid_facts, panel=(AAPL,))
    classes = {
        (r.benchmark_item, str(pd.Timestamp(r.period_end).date())): r.parity_class
        for r in result.rows.itertuples(index=False)
    }
    assert classes == {
        ("revenue", "2025-09-27"): "agrees",
        ("revenue", "2024-09-28"): "agrees",
        ("gross_profit", "2025-09-27"): "agrees",
        ("net_income", "2025-09-27"): "value_mismatch",
        ("eps_diluted", "2025-09-27"): "value_mismatch",
        ("total_assets", "2025-09-27"): "agrees",
        ("total_assets", "2024-09-28"): "vintage_difference",  # latest A9 differs; same-accession A2 agrees
        ("cfo", "2025-09-30"): "missing_warehouse_item",
        ("capex", "2025-09-30"): "missing_warehouse_period",
    }
    assert result.summary["agrees"] == 4
    assert result.summary["by_item"]["total_assets"]["same_filing_agreement_ratio"] == pytest.approx(1.0)
    assert tmp_store.con.execute(
        "SELECT count(*) FROM vendor_baseline_facts WHERE vendor = 'SEC_FSDS' AND source_accession = ?", [A2]
    ).fetchone()[0] == 8
