"""Offline original-format semantic fixtures, not actual source certification."""
import datetime as dt
import pytest
from atx_db.alpha_panel import distributions as D, distributions_qualification as Q

CLASS = b'''<html><xbrli:context id="c"><xbrli:entity><xbrli:identifier>900</xbrli:identifier></xbrli:entity></xbrli:context><ix:nonNumeric name="dei:EntityCentralIndexKey" contextRef="c">900</ix:nonNumeric><ix:nonNumeric name="dei:TradingSymbol" contextRef="c">ABC</ix:nonNumeric><ix:nonNumeric name="dei:Security12bTitle" contextRef="c">Class A common stock, par value 0.01</ix:nonNumeric></html>'''


def index(acc, stamp):
    return f'<html>{acc}<div>Accepted</div><div class="info">{stamp}</div></html>'.encode()


def inputs(validity=""):
    text = f"The board declared on January 4, 2021 a regular quarterly cash dividend of USD0.60 per share of Class A common stock{validity}."
    event, = D.extract_declarations(text)
    return event, text, dict(declaration_accession="decl", declaration_clock=index("decl", "2021-01-04 10:00:00"),
        class_document=CLASS, class_accession="cover", class_clock=index("cover", "2020-12-15 10:00:00"),
        line_rows=[{"cik": 900, "security_id": 10, "ticker": "ABC", "valid_from": dt.date(2020, 1, 1),
                    "valid_to": dt.date(2022, 1, 1), "available_at": D.clock("2020-12-15T16:00:00Z")}])


def test_positive_native_clock_class_fixed_rate_without_indefinite_carry():
    event, text, args = inputs()
    result = Q.qualify(event, text, **args)
    assert result["indicated_annual_dividend_rate"] == pytest.approx(2.4)
    assert result["available_at"] == D.clock("2021-01-04T15:00:00Z")
    assert result["security_id"] == 10
    assert not result["quote_currency_qualified"]
    assert Q.carry(result, D.clock("2021-02-01T22:00:00Z")) is None


def test_explicit_source_validity_clock_equality_and_expiry():
    event, text, args = inputs(", effective through March 31, 2021")
    result = Q.qualify(event, text, **args)
    assert Q.carry(result, result["available_at"]) is None
    assert Q.carry(result, D.clock("2021-03-31T22:00:00Z")) == pytest.approx(2.4)
    assert Q.carry(result, D.clock("2021-04-01T00:00:00Z")) is None


def test_native_clock_different_dst_seasons_and_ambiguous_hours():
    assert Q.accepted_clock(index("a", "2021-07-01 10:00:00"), "a") == D.clock("2021-07-01T14:00:00Z")
    for value in ("2021-11-07 01:30:00", "2021-03-14 02:30:00"):
        with pytest.raises(ValueError, match="ambiguous/nonexistent"):
            Q.accepted_clock(index("a", value), "a")
    with pytest.raises(ValueError, match="accession"):
        Q.accepted_clock(index("a", "2021-01-01 10:00:00"), "missing")


def test_no_class_expansion_future_link_or_variable_policy():
    event, text, args = inputs()
    args["line_rows"] *= 2
    with pytest.raises(ValueError, match="ambiguous"):
        Q.qualify(event, text, **args)
    event, text, args = inputs()
    args["line_rows"][0]["available_at"] = D.clock("2021-02-01T00:00:00Z")
    with pytest.raises(ValueError, match="future"):
        Q.qualify(event, text, **args)
    event, text, args = inputs()
    with pytest.raises(ValueError, match="variable"):
        Q.qualify(event, text + " This variable dividend policy pays one-third of net income.", **args)
    assert Q.native_classes(CLASS.replace(b"Class A common stock", b"American depositary shares")) == []


def test_policy_tombstones_and_future_declarations_do_not_rewrite_earlier_rate():
    event, text, args = inputs(", effective through March 31, 2021")
    result = {**Q.qualify(event, text, **args), "policy_state": Q.policy_state(text)}
    tombstone = {"security_id": 10, "available_at": D.clock("2021-02-01T15:00:00Z"),
                 "policy_state": Q.policy_state("The company will not pay a dividend.")}
    assert Q.rate_asof([result, tombstone], 10, D.clock("2021-01-31T22:00:00Z")) == pytest.approx(2.4)
    assert Q.rate_asof([result, tombstone], 10, D.clock("2021-02-02T22:00:00Z")) is None
    assert Q.policy_state("Pursuant to its variable dividend policy, it pays one-third of net income.") == "variable"


def test_production_ancillary_admission_replay_qualifies_and_missing_remains_null(tmp_path, monkeypatch):
    import json
    import pyarrow as pa
    import pyarrow.parquet as pq
    from atx_db.alpha_panel import common as C, distributions_native as N
    import importlib.util
    from pathlib import Path
    spec = importlib.util.spec_from_file_location("distribution_fixture_helpers", Path(__file__).with_name("test_alpha_panel_distributions.py"))
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    admitted_fixture = helpers.admitted_fixture
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    event, text, _ = inputs(", effective through March 31, 2021")
    def original(name, blob, accession):
        path = tmp_path / name
        path.write_bytes(blob)
        receipt = tmp_path / (name + ".receipt.jsonl")
        raw = (json.dumps({"url": "https://www.sec.gov/fixture/" + name, "status": 200,
            "sha256": C.sha256_file(path), "fetched_at": "2026-01-01T00:00:00Z"}) + "\n").encode()
        receipt.write_bytes(raw)
        return {"accession": accession, "cik": 900, "source_path": str(path), "source_doc": name,
            "source_url": "https://www.sec.gov/fixture/" + name, "source_sha256": C.sha256_file(path),
            "stored_sha256": C.sha256_file(path), "storage_encoding": "plain", "fetched_at": "2026-01-01T00:00:00Z",
            "link_basis": "unknown", "receipt": {"path": str(receipt), "sha256": C.sha256_file(receipt), "line": 1,
            "line_sha256": __import__("hashlib").sha256(raw).hexdigest()}}
    doc = original("release.htm", ("<html><p>" + text + "</p></html>").encode(), "decl")
    clock_doc = original("decl-index.htm", index("decl", "2021-01-04 10:00:00"), "decl")
    class_doc = original("cover.htm", CLASS, "cover")
    class_clock_doc = original("cover-index.htm", index("cover", "2020-12-15 10:00:00"), "cover")
    catalog = tmp_path / "documents.jsonl"
    catalog.write_text(json.dumps(doc) + "\n")
    identity = tmp_path / "identity"
    identity.mkdir()
    pq.write_table(pa.Table.from_pylist([{"security_id": 10, "cik": 900, "valid_from": dt.date(2020,1,1),
        "valid_to": dt.date(2022,1,1), "available_at": dt.datetime(2020,12,15,16), "link_tier": "strict"}]), identity / "link_table.parquet")
    admission, sha = admitted_fixture(tmp_path, catalog)
    D.build(catalog, [], admission_path=admission, expected_sha256=sha)
    target = tmp_path / "corporate_actions/distributions.parquet"
    assert pq.read_table(target).to_pylist()[0]["indicated_annual_dividend_rate"] is None
    prices = tmp_path / "prices.parquet"
    pq.write_table(pa.Table.from_pylist([{"security_id":10,"ticker":"ABC","session_date":dt.date(2021,1,4)}]), prices)
    for role, data in (("clock", {"clock_document": clock_doc}), ("class", {"class_document": class_doc, "class_clock_document": class_clock_doc})):
        path = tmp_path / (role + ".json")
        path.write_text(json.dumps({"schema": f"atx.native-{role}-evidence/v1", "records": [{"declaration_accession":"decl",
            "declaration_sha256":doc["source_sha256"], **data}]}))
    expected = json.loads(admission.read_text())
    expected["schema"] = "atx.native-admission/v2"
    expected["inputs"]["prices"] = {"path":str(prices),"sha256":C.sha256_file(prices)}
    for role in ("clock","class"):
        expected["inputs"][role]["sha256"] = C.sha256_file(tmp_path / (role + ".json"))
    admission.write_text(json.dumps(expected))
    sha = C.sha256_file(admission)
    result = D.build(catalog, [], admission_path=admission, expected_sha256=sha)
    row, = pq.read_table(target).to_pylist()
    assert row["indicated_annual_dividend_rate"] == pytest.approx(2.4)
    assert row["available_at"] == D.clock("2021-01-04T15:00:00Z")
    assert row["security_id"] == 10
    assert not row["quote_currency_qualified"]
    assert D.validate_product(target, admission, sha)["conditionally_qualified_events"] == 1
    assert all(gate["status"] == "FAIL" for gate in result["gate_status"].values())
    # Independently admitted original ADS evidence cannot qualify common-stock cash.
    negative_class = original("cover.htm", CLASS.replace(b"Class A common stock", b"American depositary shares"), "cover")
    class_path = tmp_path / "class.json"
    class_value = json.loads(class_path.read_text())
    class_value["records"][0]["class_document"] = negative_class
    class_path.write_text(json.dumps(class_value))
    expected["inputs"]["class"]["sha256"] = C.sha256_file(class_path)
    admission.write_text(json.dumps(expected))
    sha = C.sha256_file(admission)
    D.build(catalog, [], admission_path=admission, expected_sha256=sha)
    negative, = pq.read_table(target).to_pylist()
    assert negative["indicated_annual_dividend_rate"] is None
    assert negative["source_qualification"].startswith("native_chain_unqualified:")
    assert D.validate_product(target, admission, sha)["conditionally_qualified_events"] == 0
    (tmp_path / "cover.htm").write_bytes(b"tampered")
    with pytest.raises(ValueError, match="hash mismatch"):
        D.validate_product(target, admission, sha)
