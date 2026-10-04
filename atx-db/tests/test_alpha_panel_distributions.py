"""Bounded offline tests of native declarations and the rate/PIT distinction."""
import datetime as dt
import json

import pytest

from atx_db.alpha_panel import distributions as D
from atx_db.alpha_panel import events_buyback as B


def declaration(extra="regular quarterly"):
    return f"The board declared on January 3, 2021 a {extra} cash dividend of US$0.25 per share of Class A common stock, payable on February 3, 2021 to holders of record on January 20, 2021."


def test_native_declaration_dates_rate_and_provenance():
    row, = D.extract_declarations(declaration())
    assert row["declare_date"] == dt.date(2021, 1, 3)
    assert row["record_date"] == dt.date(2021, 1, 20)
    assert row["pay_date"] == dt.date(2021, 2, 3)
    assert row["amount"] == .25
    assert row["indicated_annual_dividend_rate"] == 1
    assert row["security_class"] == "Class A common stock"
    assert row["evidence"] == declaration()


@pytest.mark.parametrize("description", ["quarterly", "regular", "special quarterly", "one-time quarterly", ""])
def test_unknown_and_special_never_annualized(description):
    row, = D.extract_declarations(declaration(description))
    assert row["indicated_annual_dividend_rate"] is None


def test_paid_currency_class_and_date_are_not_inferred():
    assert D.extract_declarations(declaration().replace("declared", "paid")) == []
    assert D.extract_declarations(declaration().replace("US$", "$")) == []
    assert D.extract_declarations(declaration().replace("Class A common stock", "preferred stock")) == []
    row, = D.extract_declarations(declaration().replace(" on January 3, 2021", ""))
    assert row["declare_date"] is None
    assert row["pay_date"] == dt.date(2021, 2, 3)


def test_duration_fact_is_not_indicated_rate():
    row = D.xbrl_declared({"concept": "CommonStockDividendsPerShareDeclared", "value": 1,
                          "unit": "USD/shares", "start": "2020-01-01", "end": "2020-12-31"})
    assert row["amount"] == 1
    assert row["indicated_annual_dividend_rate"] is None
    assert row["declare_date"] is None
    with pytest.raises(ValueError):
        D.xbrl_declared({"concept": "CommonStockDividendsPerShareDeclared", "value": 1, "unit": "USD"})


def test_strict_daily_monthly_clock():
    row = {"available_at": D.clock("2021-01-03T22:00:00Z")}
    assert not D.visible(row, dt.date(2021, 1, 4))
    assert D.visible(row, dt.date(2021, 1, 5))
    assert not D.visible(row, dt.date(2021, 1, 3), monthly=True)
    with pytest.raises(ValueError):
        D.clock("2021-01-03T21:00:00")


def test_exact_full_denominator_and_200_independent_checks():
    rows = [{"declare_date": "2020-01-01", "source_amount": 1.01, "vendor_cash": 1} for _ in range(95)]
    rows += [{} for _ in range(5)]
    checks = [{"accession": str(i), "event_id": str(i), "matches": i < 190,
               "native_source_sha256": "a" * 64, "evidence": "native sentence"} for i in range(200)]
    result = D.gates(rows, checks)
    assert all(g["status"] == "PASS" for g in result.values())
    assert D.gates(rows + [{}], checks)["vendor_cash_within_1pct"]["status"] == "FAIL"
    assert D.gates(rows, checks[:-1])["buyback_handcheck"]["status"] == "FAIL"
    assert D.gates(rows, [checks[0]] * 200)["buyback_handcheck"]["status"] == "FAIL"
    assert all(g["status"] == "FAIL" for g in D.gates().values())


def test_retained_product_hash_and_vintage(tmp_path, monkeypatch):
    from atx_db.alpha_panel import common as C
    import pyarrow.parquet as pq
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    native = tmp_path / "release.txt"
    native.write_text(declaration(), encoding="utf-8")
    catalog = tmp_path / "documents.jsonl"
    record = {"source_path": str(native), "source_sha256": C.sha256_file(native),
              "source_url": "https://www.sec.gov/Archives/fixture", "accession": "0000000001-21-000001",
              "available_at": "2021-01-03T21:00:00Z", "security_id": 1, "cik": 1,
              "security_class": "Class A common stock", "currency": "USD",
              "link_basis": "explicit native class to fixture security", "link_tier": "fixture"}
    catalog.write_text(json.dumps(record) + "\n", encoding="utf-8")
    upstream = tmp_path / "manifest.json"
    upstream.write_text('{"fixture": true}', encoding="utf-8")
    result = D.build(catalog, [upstream])
    assert result["rows"] == 1
    assert result["input_manifests_sha256"][str(upstream)] == C.sha256_file(upstream)
    target = tmp_path / "lake/corporate_actions/distributions.parquet"
    assert pq.read_table(target).to_pylist()[0]["available_at"].tzinfo is not None
    assert pq.read_table(target).to_pylist()[0]["indicated_annual_dividend_rate"] == 1
    assert all(g["status"] == "FAIL" for g in result["gate_status"].values())
    with pytest.raises(FileExistsError):
        D.build(catalog, [upstream])
    record["security_class"] = "unverified common class"
    catalog.write_text(json.dumps(record) + "\n", encoding="utf-8")
    second = D.build(catalog, [upstream])
    assert second["product_id"] != result["product_id"]
    assert pq.read_table(target).to_pylist()[0]["indicated_annual_dividend_rate"] is None
    original = tmp_path / "lake/corporate_actions/distributions_products" / result["product_id"] / "distributions.parquet"
    assert pq.read_table(original).to_pylist()[0]["indicated_annual_dividend_rate"] == 1
    record["available_at"] = "2021-01-02T21:00:00Z"
    catalog.write_text(json.dumps(record) + "\n", encoding="utf-8")
    with pytest.raises(ValueError, match="future declaration"):
        D.build(catalog, [upstream])
    native.write_text("changed", encoding="utf-8")
    with pytest.raises(ValueError, match="hash mismatch"):
        D.build(catalog, [upstream])


def test_explicit_spin_off_ratio():
    row, = D.extract_spin_offs("The company will distribute 1 share of SpinCo for every 4 shares of Class A common stock.")
    assert row["spin_off_ratio"] == .25
    assert row["distributed_security"] == "SpinCo"
    assert row["indicated_annual_dividend_rate"] is None
    assert D.extract_spin_offs("distribute 1 share of SpinCo for every 0 shares of common stock") == []


def test_part_ii_execution_is_not_authorization():
    text = """Part II Item 2
Period | Total shares purchased | Average price paid per share (USD) | Shares purchased under publicly announced plans | Remaining authorization (USD)
January 2021 | 1,000 | 12.50 | 900 | 1,000,000
Total | 1,000 | 12.50 | 900 | 1,000,000
Item 3
February 2021 | 2,000 | 13.00 | 2,000 | 900,000
"""
    row, = B.extract_part_ii(text)
    assert row["execution_shares"] == 1000
    assert row["execution_average_price"] == 12.5
    assert row["public_program_shares"] == 900
    assert row["remaining_authorization_usd"] == 1000000
    assert B.extract_authorizations(text) == []
    assert B.extract_part_ii(text.replace("(USD)", "(millions)")) == []
    assert B.extract_part_ii(text.replace("| 900 |", "| 1,001 |")) == []


def test_buyback_pinned_publish_excludes_execution_announcement(tmp_path, monkeypatch):
    import pyarrow as pa
    import pyarrow.parquet as pq
    from atx_db.alpha_panel import common as C
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    stamp = dt.datetime(2021, 2, 1, 20)
    fields = {"BIGINT": pa.int64(), "VARCHAR": pa.string(), "DATE": pa.date32(),
              "TIMESTAMP": pa.timestamp("us"), "DOUBLE": pa.float64()}
    schema = pa.schema([(name, fields[typ]) for name, typ in B.OUT_COLUMNS])
    base = {"cik": 1, "accession": "a", "available_at": stamp, "filing_date": stamp.date(),
            "source_sha256": "a" * 64, "evidence": "fixture native", "event_type": "new", "amount_usd": 1000000}
    path = B._tmp() / "buyback_rows.parquet"
    pq.write_table(pa.Table.from_pylist([base, {**base, "event_type": "execution", "amount_usd": None,
                                               "execution_period": "January 2021", "execution_shares": 100}], schema=schema), path)
    C.write_json_atomic(B._tmp() / "buyback_parse_receipt.json", {"parse_rows_sha256": C.sha256_file(path)})
    fund = C.stage_dir("fundamentals")
    pq.write_table(pa.Table.from_pylist([{"cik": 1, "accession": "earlier", "clock_utc": stamp - dt.timedelta(days=1),
        "buyback_authorized": 900000., "buyback_remaining": 800000.}, {"cik": 1, "accession": "later",
        "clock_utc": stamp + dt.timedelta(days=1), "buyback_authorized": 1000000., "buyback_remaining": 900000.}]), fund / "events.parquet")
    C.write_json_atomic(fund / "manifest.json", {"fixture": True})
    identity = C.stage_dir("identity")
    pq.write_table(pa.Table.from_pylist([{"security_id": 1, "cik": 1, "valid_from": dt.date(2020, 1, 1),
        "valid_to": dt.date(2022, 1, 1), "link_tier": "strict", "is_issuer_primary": True, "sessions": 500,
        "ever_member": True}]), identity / "link_table.parquet")
    C.write_json_atomic(identity / "manifest.json", {"fixture": True})
    result = B.phase_publish()
    assert result["rows"] == 2
    rows = pq.read_table(tmp_path / "events/buyback.parquet").to_pylist()
    assert next(r for r in rows if r["event_type"] == "execution")["first_announcement"] is False
    assert next(r for r in rows if r["event_type"] == "new")["first_announcement"] is True
    assert rows[0]["xbrl_after_available_at"] > rows[0]["available_at"]
    products = list((tmp_path / "events/buyback_products").iterdir())
    assert len(products) == 1
    assert json.loads((products[0] / "manifest.json").read_text())["gate_status"]["measured"] == 0
    with pytest.raises(FileExistsError):
        B.phase_publish()
