"""FSDS-derived inputs of alpha-panel stage F v2: class-of-stock share sums and statement-line flags."""

from __future__ import annotations

import datetime as dt
from decimal import Decimal

import duckdb
import pytest
import pyarrow as pa
import pyarrow.parquet as pq

from atx_db.alpha_panel import fundamentals as fu

NUM_SCHEMA = pa.schema([("adsh", pa.string()), ("tag", pa.string()), ("version", pa.string()),
                        ("ddate", pa.date32()), ("qtrs", pa.int32()), ("uom", pa.string()),
                        ("segments", pa.string()), ("coreg", pa.string()), ("value", pa.decimal128(28, 4))])


def _num(adsh, segments, value, tag="CommonStockSharesOutstanding", qtrs=0, coreg=None, version="us-gaap/2024"):
    return {"adsh": adsh, "tag": tag, "version": version, "ddate": dt.date(2024, 12, 31), "qtrs": qtrs,
            "uom": "shares", "segments": segments, "coreg": coreg, "value": Decimal(str(value))}


def _run(sql: str):
    con = duckdb.connect()
    try:
        return {r[0]: r[1:] for r in con.execute(f"SELECT adsh, tag, n_members, value FROM ({sql}) ORDER BY 1").fetchall()}
    finally:
        con.close()


def test_class_sum_rules(tmp_path):
    rows = [
        _num("meta", "ClassOfStock=CommonClassA;", 2211e6), _num("meta", "ClassOfStock=CommonClassB;", 350e6),
        # generic member equal to the sum of the classes is the total
        _num("gen", "ClassOfStock=CommonClassA;", 3436020), _num("gen", "ClassOfStock=CommonClassB;", 200000),
        _num("gen", "ClassOfStock=CommonStock;", 3636020),
        # voting + nonvoting: the generic member is one class, not the total
        _num("vote", "ClassOfStock=CommonStock;", 18e6), _num("vote", "ClassOfStock=NonvotingCommonStock;", 248.9e6),
        # Berkshire-style equivalents, preferred, ADRs are dropped
        _num("brk", "ClassOfStock=EquivalentClassA;", 1448880), _num("brk", "ClassOfStock=EquivalentClassB;", 2.17e9),
        _num("pref", "ClassOfStock=CommonClassA;", 100), _num("pref", "ClassOfStock=SeriesAPreferredStock;", 50),
        # other axes, co-registrants and custom tags never count
        _num("axis", "ClassOfStock=CommonClassA;LegalEntity=Sub;", 999), _num("axis", "ClassOfStock=CommonClassA;", 10),
        _num("coreg", "ClassOfStock=CommonClassA;", 7, coreg="Sub"),
        _num("custom", "ClassOfStock=CommonClassA;", 7, version="0001234567-24-000001"),
        _num("plain", None, 500),
    ]
    path = tmp_path / "num.parquet"
    pq.write_table(pa.Table.from_pylist(rows, schema=NUM_SCHEMA), path)
    out = _run(fu.class_sum_sql(path.as_posix()))
    assert out["meta"][2] == 2561e6 and out["meta"][1] == 2
    assert out["gen"][2] == 3636020
    assert out["vote"][2] == 266.9e6
    assert out["brk"][2] == 2.17e9                    # equivalents only: the larger total, never their sum
    assert out["pref"][2] == 100
    assert out["axis"][2] == 10
    assert "coreg" not in out and "custom" not in out and "plain" not in out


PRE_SCHEMA = pa.schema([("adsh", pa.string()), ("stmt", pa.string()), ("inpth", pa.bool_()), ("tag", pa.string())])


def test_pre_flags(tmp_path):
    rows = [
        {"adsh": "bio", "stmt": "IS", "inpth": False, "tag": "OperatingExpenses"},
        {"adsh": "bio", "stmt": "IS", "inpth": False, "tag": "NetIncomeLoss"},
        {"adsh": "bio", "stmt": "CF", "inpth": False, "tag": "NetCashProvidedByUsedInOperatingActivities"},
        {"adsh": "bio", "stmt": "IS", "inpth": True, "tag": "ResearchAndDevelopmentExpense"},   # parenthetical
        {"adsh": "tech", "stmt": "IS", "inpth": False, "tag": "Revenues"},
        {"adsh": "tech", "stmt": "IS", "inpth": False, "tag": "CostOfRevenue"},
        {"adsh": "tech", "stmt": "IS", "inpth": False, "tag": "ResearchAndDevelopmentExpenseCustom"},
        {"adsh": "tech", "stmt": "IS", "inpth": False, "tag": "IncomeTaxExpenseBenefit"},
        {"adsh": "tech", "stmt": "IS", "inpth": False, "tag": "InterestExpenseNonoperating"},
        {"adsh": "tech", "stmt": "CF", "inpth": False, "tag": "PaymentsToAcquirePropertyPlantAndEquipment"},
        {"adsh": "mkt", "stmt": "CI", "inpth": False, "tag": "SalesAndMarketingExpense"},
        {"adsh": "mkt", "stmt": "IS", "inpth": False, "tag": "CostOfGoodsAndServicesSold"},
        {"adsh": "asc606", "stmt": "IS", "inpth": False, "tag": "RevenueFromContractWithCustomerExcludingAssessedTax"},
    ]
    path = tmp_path / "pre.parquet"
    pq.write_table(pa.Table.from_pylist(rows, schema=PRE_SCHEMA), path)
    con = duckdb.connect()
    try:
        cols = ", ".join(("adsh",) + tuple(f for f in ("has_is", "has_cf", "is_rd", "is_rev", "is_tax", "is_int",
                                                           "cf_capx")))
        got = {r[0]: dict(zip(("has_is", "has_cf", "is_rd", "is_rev", "is_tax", "is_int", "cf_capx"), r[1:]))
               for r in con.execute(f"SELECT {cols} FROM ({fu.pre_flags_sql(path.as_posix())})").fetchall()}
    finally:
        con.close()
    assert got["bio"] == {"has_is": True, "has_cf": True, "is_rd": False, "is_rev": False, "is_tax": False,
                          "is_int": False, "cf_capx": False}
    assert got["tech"] == {"has_is": True, "has_cf": True, "is_rd": True, "is_rev": True, "is_tax": True,
                           "is_int": True, "cf_capx": True}
    assert got["mkt"]["is_rev"] is False            # marketing expense and cost of sales are not revenue lines
    assert got["mkt"]["has_cf"] is False
    assert got["asc606"]["is_rev"] is True           # the ASC 606 revenue tag ends in "Tax"


def test_structural_na_manifest_shape():
    snan = fu.structural_na()
    assert snan["gp_ttm"] == ["bank", "insurer", "reit", "utility"]
    assert snan["oi_ttm"] == ["bank"]
    assert all(set(v) <= {"bank", "insurer", "reit", "utility", "other"} for v in snan.values())


def test_product_or_service_cost_sums(tmp_path):
    rows = [_num("vse", "ProductOrService=Product;", 303.9, tag="CostOfGoodsAndServicesSold", qtrs=4),
            _num("vse", "ProductOrService=Service;", 321.1, tag="CostOfGoodsAndServicesSold", qtrs=4),
            _num("vse", "ProductOrService=Service;BusinessSegments=A;", 99, tag="CostOfGoodsAndServicesSold", qtrs=4),
            _num("vse", None, 1000, tag="CostOfGoodsAndServicesSold", qtrs=4),
            _num("inst", "ProductOrService=Product;", 5, tag="CostOfRevenue", qtrs=0)]
    for r in rows:
        r["uom"] = "USD"
    path = tmp_path / "num.parquet"
    pq.write_table(pa.Table.from_pylist(rows, schema=NUM_SCHEMA), path)
    con = duckdb.connect()
    try:
        got = con.execute(f"SELECT adsh, tag, qtrs, n_members, value FROM ({fu.pos_sum_sql(path.as_posix())})").fetchall()
    finally:
        con.close()
    assert got == [("vse", "CostOfGoodsAndServicesSold", 4, 2, pytest.approx(625.0))]
