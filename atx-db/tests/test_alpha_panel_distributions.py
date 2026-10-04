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
    sessions = [dt.date(2021, 1, 1), dt.date(2021, 1, 4), dt.date(2021, 1, 5), dt.date(2021, 1, 29), dt.date(2021, 2, 1)]
    row = {"available_at": D.clock("2021-01-02T12:00:00Z")}
    assert not D.visible(row, dt.date(2021, 1, 4), sessions=sessions)
    assert D.visible(row, dt.date(2021, 1, 5), sessions=sessions)
    row["available_at"] = D.clock("2021-01-29T22:00:00Z")
    assert not D.visible(row, dt.date(2021, 1, 29), sessions=sessions, monthly=True)
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


def admitted_fixture(tmp_path, catalog):
    from atx_db.alpha_panel import common as C, distributions_native as N
    records = [json.loads(line) for line in catalog.read_text().splitlines()]
    for index, record in enumerate(records):
        if "receipt" not in record:
            receipt_path = tmp_path / f"fixture-receipt-{index}.jsonl"
            raw = (json.dumps({"sha256": record["source_sha256"], "url": record["source_url"], "status": 200,
                "fetched_at": record["fetched_at"]}) + "\n").encode()
            receipt_path.write_bytes(raw)
            record["receipt"] = {"path": str(receipt_path), "sha256": C.sha256_file(receipt_path), "line": 1,
                "line_sha256": __import__("hashlib").sha256(raw).hexdigest()}
    catalog.write_text("".join(json.dumps(record) + "\n" for record in records))
    inputs = {"catalog": {"path": str(catalog), "sha256": C.sha256_file(catalog)}}
    import pyarrow as pa
    import pyarrow.parquet as pq
    parent = tmp_path / "source_catalog.parquet"
    pq.write_table(pa.Table.from_pylist([{"accession": r["accession"], "doc_sha256": r["source_sha256"],
        "source_doc": r["source_doc"], "form": r.get("form", "8-K"), "filing_date": dt.date(2021, 1, 3)} for r in records]), parent)
    inputs["source_catalog"] = {"path": str(parent), "sha256": C.sha256_file(parent)}
    for role, value in (("calendar", ["2021-01-01", "2021-01-04", "2021-02-01"]), ("class", []), ("clock", [])):
        path = tmp_path / f"{role}.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        inputs[role] = {"path": str(path), "sha256": C.sha256_file(path)}
    for role, filename in (("fundamentals", "events.parquet"), ("identity", "link_table.parquet")):
        path = tmp_path / role / "manifest.json"
        path.parent.mkdir(exist_ok=True)
        if not (path.parent / filename).exists():
            (path.parent / filename).write_bytes(b"fixture-unused")
        path.write_text(json.dumps({"files": {filename: {"sha256": C.sha256_file(path.parent / filename)}}}))
        inputs[role] = {"path": str(path), "sha256": C.sha256_file(path)}
    admission = tmp_path / "admission.json"
    admission.write_text(json.dumps({"schema": "atx.native-admission/v1", "inputs": inputs,
        "code_sha256": N.code_pins(), "population": {"catalog_rows": len(records), "selection": "all_eligible_source_rows_hash_partition",
        "eligible_source_rows": len(records), "partition_index": 0, "partition_count": 1}}))
    return admission, C.sha256_file(admission)


def test_retained_product_hash_and_vintage(tmp_path, monkeypatch):
    from atx_db.alpha_panel import common as C
    import pyarrow.parquet as pq
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    native = tmp_path / "release.txt"
    native.write_text(declaration(), encoding="utf-8")
    catalog = tmp_path / "documents.jsonl"
    record = {"source_path": str(native), "source_sha256": C.sha256_file(native),
              "stored_sha256": C.sha256_file(native), "storage_encoding": "plain", "source_doc": "release.txt", "fetched_at": "2021-01-03T21:00:00Z",
              "source_url": "https://www.sec.gov/Archives/fixture", "accession": "0000000001-21-000001",
              "available_at": "2021-01-03T21:00:00Z", "security_id": 1, "cik": 1,
              "security_class": "Class A common stock", "currency": "USD",
              "link_basis": "explicit native class to fixture security", "link_tier": "fixture"}
    catalog.write_text(json.dumps(record) + "\n", encoding="utf-8")
    admission, sha = admitted_fixture(tmp_path, catalog)
    result = D.build(catalog, [], admission_path=admission, expected_sha256=sha)
    assert result["rows"] == 1
    target = tmp_path / "lake/corporate_actions/distributions.parquet"
    assert pq.read_table(target).to_pylist()[0]["available_at"].tzinfo is not None
    assert pq.read_table(target).to_pylist()[0]["indicated_annual_dividend_rate"] is None
    assert D.validate_product(target, admission, sha)["native_replay"] == "PASS"
    changed = pq.read_table(target).to_pylist()
    changed[0]["amount"] = 999
    pq.write_table(__import__("pyarrow").Table.from_pylist(changed, schema=D.output_schema()), target)
    with pytest.raises(ValueError, match="replay mismatch"):
        D.validate_product(target, admission, sha)
    pq.write_table(__import__("pyarrow").Table.from_pylist(D.replay_rows(admission, sha)[0], schema=D.output_schema()), target)
    assert all(g["status"] == "FAIL" for g in result["gate_status"].values())
    with pytest.raises(FileExistsError):
        D.build(catalog, [], admission_path=admission, expected_sha256=sha)
    record["security_class"] = "unverified common class"
    catalog.write_text(json.dumps(record) + "\n", encoding="utf-8")
    admission, sha = admitted_fixture(tmp_path, catalog)
    second = D.build(catalog, [], admission_path=admission, expected_sha256=sha)
    assert second["product_id"] != result["product_id"]
    assert pq.read_table(target).to_pylist()[0]["indicated_annual_dividend_rate"] is None
    original = tmp_path / "lake/corporate_actions/distributions_products" / result["product_id"] / "distributions.parquet"
    assert pq.read_table(original).to_pylist()[0]["declared_regular_rate"] == .25
    record["fetched_at"] = "2021-01-02T21:00:00Z"
    catalog.write_text(json.dumps(record) + "\n", encoding="utf-8")
    admission, sha = admitted_fixture(tmp_path, catalog)
    with pytest.raises(ValueError, match="future declaration"):
        D.build(catalog, [], admission_path=admission, expected_sha256=sha)
    native.write_text("changed", encoding="utf-8")
    with pytest.raises(ValueError, match="hash mismatch"):
        D.build(catalog, [], admission_path=admission, expected_sha256=sha)


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
    with pytest.raises(ValueError, match="hash-only"):
        B.phase_publish()
    native = tmp_path / "partii.htm"
    native.write_text("""<html><body><p>The board authorized a new share repurchase program of up to $1 million of common stock.</p><pre>Part II Item 2
Period | Total shares purchased | Average price paid per share (USD) | Shares purchased under publicly announced plans | Remaining authorization (USD)
January 2021 | 100 | 12.5 | 90 | 800000
Item 3</pre></body></html>""", encoding="utf-8")
    catalog = tmp_path / "documents.jsonl"
    catalog.write_text(json.dumps({"cik": 1, "accession": "a", "form": "10-Q", "filing_date": "2021-02-01",
        "fetched_at": "2021-02-01T20:00:00Z", "source_path": str(native), "source_doc": "partii.htm",
        "source_url": "https://www.sec.gov/fixture", "link_basis": "unknown", "storage_encoding": "plain",
        "stored_sha256": C.sha256_file(native), "source_sha256": C.sha256_file(native)}) + "\n")
    admission, sha = admitted_fixture(tmp_path, catalog)
    with pytest.raises(ValueError, match="abbreviated"):
        B.phase_publish(admission_path=admission, expected_sha256=sha)
    B.parse_native(admission, sha)
    result = B.phase_publish(admission_path=admission, expected_sha256=sha)
    assert result["rows"] == 2
    rows = pq.read_table(tmp_path / "events/buyback.parquet").to_pylist()
    assert next(r for r in rows if r["event_type"] == "execution")["first_announcement"] is False
    assert next(r for r in rows if r["event_type"] == "new")["first_announcement"] is None
    assert next(r for r in rows if r["event_type"] == "new")["first_announcement_basis"] == "unknown_incomplete_global_history"
    assert rows[0]["xbrl_after_available_at"] > rows[0]["available_at"]
    products = list((tmp_path / "events/buyback_products").iterdir())
    assert len(products) == 1
    assert json.loads((products[0] / "manifest.json").read_text())["gate_status"]["measured"] == 0
    assert B.validate_product(tmp_path / "events/buyback.parquet", admission, sha)["native_replay"] == "PASS"
    target = tmp_path / "events/buyback.parquet"
    table = pq.read_table(target)
    corrupted = table.to_pylist()
    corrupted[0]["xbrl_authorized_before"] = -1
    pq.write_table(pa.Table.from_pylist(corrupted, schema=table.schema), target)
    with pytest.raises(ValueError, match="join replay mismatch"):
        B.validate_product(target, admission, sha)
    pq.write_table(table, target)
    with pytest.raises(FileExistsError):
        B.phase_publish(admission_path=admission, expected_sha256=sha)
    # Even adding an earlier matching authorization to the admitted partition
    # does not certify completeness of global prior history.
    earlier = tmp_path / "earlier.htm"
    earlier.write_text("<html><p>The board authorized a new share repurchase program of up to $1 million of common stock.</p></html>")
    record = {"cik": 1, "accession": "earlier-authorization", "form": "8-K", "filing_date": "2021-01-01",
        "fetched_at": "2021-01-01T20:00:00Z", "source_path": str(earlier), "source_doc": "earlier.htm",
        "source_url": "https://www.sec.gov/earlier-fixture", "link_basis": "unknown", "storage_encoding": "plain",
        "stored_sha256": C.sha256_file(earlier), "source_sha256": C.sha256_file(earlier)}
    with catalog.open("a") as stream:
        stream.write(json.dumps(record) + "\n")
    admission, sha = admitted_fixture(tmp_path, catalog)
    B.parse_native(admission, sha)
    B.phase_publish(admission_path=admission, expected_sha256=sha)
    with_prior = pq.read_table(target).to_pylist()
    assert all(row["first_announcement"] is None for row in with_prior if row["event_type"] != "execution")
    assert B.validate_product(target, admission, sha)["native_replay"] == "PASS"


def test_increase_new_amount_and_ambiguous_classes():
    text = "The board declared a regular quarterly cash dividend increased from USD0.50 to USD0.60 per share of Class A common stock."
    row, = D.extract_declarations(text)
    assert row["amount"] == .60
    assert row["indicated_annual_dividend_rate"] == pytest.approx(2.4)
    assert D.extract_declarations(text.replace("increased from", "including")) == []
    assert D.extract_declarations(text.replace("common stock.", "common stock and Class B common stock.")) == []
    assert D.extract_declarations("The company will not pay a dividend pursuant to its variable dividend policy.") == []


@pytest.mark.parametrize("value", ["USD600 million", "USD600 thousand", "USD600", "USD600,000"])
def test_aggregate_or_unresolved_not_a_per_share_rate(value):
    text = f"The board declared a regular quarterly cash dividend totaling {value} for Class A common stock, with the per share amount to be determined."
    assert D.extract_declarations(text) == []
    assert D.extract_declarations(f"The board declared a regular quarterly cash dividend of {value} million per share of Class A common stock.") == []
    assert D.extract_declarations("The board declared a regular quarterly cash dividend increased from USD600 million to USD0.60 per share of Class A common stock.") == []
    assert D.extract_declarations("The board declared a regular quarterly cash dividend of USD0.60 per share of Class A common stock, with the final amount to be determined.") == []


def test_scaled_surrounding_part_ii_rejected():
    text = """Part II Item 2
Shares and amounts in millions except price per share
Period | Total shares purchased | Average price paid per share (USD) | Shares purchased under publicly announced plans | Remaining authorization (USD)
January 2021 | 2 | 12.5 | 1 | 5
Item 3"""
    assert B.extract_part_ii(text) == []
    assert B.extract_part_ii("Shares and amounts in millions except price per share\n" + text.replace("Shares and amounts in millions except price per share\n", "")) == []


def test_original_gzip_admission_duplicates_and_stale_code(tmp_path):
    import gzip
    from atx_db.alpha_panel import common as C, distributions_native as N
    body = ("<html><p>" + declaration() + "</p></html>").encode()
    path = tmp_path / "source.gz"
    path.write_bytes(gzip.compress(body))
    record = {"accession": "a", "source_doc": "ex99.htm", "source_path": str(path),
        "stored_sha256": C.sha256_file(path), "source_sha256": __import__("hashlib").sha256(body).hexdigest(),
        "storage_encoding": "gzip", "cik": 1, "source_url": "https://www.sec.gov/fixture",
        "link_basis": "unknown", "fetched_at": "2021-01-04T20:00:00Z"}
    catalog = tmp_path / "documents.jsonl"
    catalog.write_text(json.dumps(record) + "\n")
    admission, sha = admitted_fixture(tmp_path, catalog)
    rows, pins, admitted = D.replay_rows(admission, sha)
    assert rows[0]["amount"] == .25
    assert rows[0]["source_sha256"] != rows[0]["stored_sha256"]
    assert rows[0]["indicated_annual_dividend_rate"] is None
    payload = json.loads(admission.read_text())
    payload["population"]["eligible_source_rows"] = 2
    admission.write_text(json.dumps(payload))
    with pytest.raises(ValueError, match="denominator"):
        D.replay_rows(admission, C.sha256_file(admission))
    with pytest.raises(ValueError, match="independent"):
        D.build(catalog, [])
    catalog.write_text((json.dumps(record) + "\n") * 2)
    admission, sha = admitted_fixture(tmp_path, catalog)
    with pytest.raises(ValueError, match="duplicate"):
        D.replay_rows(admission, sha)
    payload = json.loads(admission.read_text())
    payload["code_sha256"] = {}
    admission.write_text(json.dumps(payload))
    with pytest.raises(ValueError, match="reader/parser"):
        N.admission(admission, C.sha256_file(admission))


def test_no_pay_policy_and_holiday_cutoff():
    source = "Pursuant to Cal-Maine Foods' variable dividend policy, for each quarter for which the Company reports net income, the Company pays a cash dividend to shareholders in an amount equal to one-third of such quarterly income. Therefore, the Company will not pay a dividend with respect to the second quarter of fiscal 2020."
    assert D.extract_declarations(source) == []
    sessions = [dt.date(2021, 1, 15), dt.date(2021, 1, 19), dt.date(2021, 1, 20)]
    assert not D.visible({"available_at": D.clock("2021-01-16T12:00:00Z")}, dt.date(2021, 1, 19), sessions=sessions)
    assert not D.visible({"available_at": D.clock("2021-01-15T22:00:00Z")}, dt.date(2021, 1, 19), sessions=sessions)
    with pytest.raises(ValueError, match="month-end"):
        D.visible({"available_at": D.clock("2021-01-15T00:00:00Z")}, dt.date(2021, 1, 19), sessions=sessions, monthly=True)
    with pytest.raises(ValueError, match="legacy"):
        B.phase_parse()
