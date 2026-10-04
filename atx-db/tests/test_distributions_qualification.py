"""Offline original-format semantic fixtures, not actual source certification."""
import datetime as dt
import pytest
from atx_db.alpha_panel import distributions as D, distributions_qualification as Q

DECL = "0000000900-21-000001"
COVER = "0000000900-20-000002"

CLASS = b'''<html><xbrli:context id="c"><xbrli:entity><xbrli:identifier>900</xbrli:identifier></xbrli:entity></xbrli:context><ix:nonNumeric name="dei:EntityCentralIndexKey" contextRef="c">900</ix:nonNumeric><ix:nonNumeric name="dei:TradingSymbol" contextRef="c">ABC</ix:nonNumeric><ix:nonNumeric name="dei:Security12bTitle" contextRef="c">Class A common stock, par value 0.01</ix:nonNumeric></html>'''


def uri(acc, name):
    return f"https://www.sec.gov/Archives/edgar/data/900/{acc.replace('-', '')}/{name}"


def index(acc, stamp, name=None):
    name = name or ("cover.htm" if acc == COVER else "release.htm")
    path = uri(acc, name).replace("https://www.sec.gov", "")
    return f'''<html><div id="secNum">SEC Accession No. {acc}</div>
      <div class="companyInfo"><a href="/cgi-bin/browse-edgar?CIK=0000000900">900</a></div>
      <div>Accepted</div><div class="info">{stamp}</div>
      <table class="tableFile"><tr><td><a href="{path}">{name}</a></td></tr></table></html>'''.encode()


def inputs(validity=""):
    text = f"The board declared on January 4, 2021 a regular quarterly cash dividend of USD0.60 per share of Class A common stock{validity}."
    event, = D.extract_declarations(text)
    event["cik"] = 900
    return event, text, dict(declaration_url=uri(DECL,"release.htm"), declaration_document=("<html><p>"+text+"</p></html>").encode(), class_url=uri(COVER,"cover.htm"), declaration_accession=DECL, declaration_clock=index(DECL, "2021-01-04 10:00:00"),
        class_document=CLASS, class_accession=COVER, class_clock=index(COVER, "2020-12-15 10:00:00"),
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
    assert Q.accepted_clock(index(DECL, "2021-07-01 10:00:00"), DECL, issuer=900, member_url=uri(DECL,"release.htm"), member_blob=b"release") == D.clock("2021-07-01T14:00:00Z")
    for value in ("2021-11-07 01:30:00", "2021-03-14 02:30:00"):
        with pytest.raises(ValueError, match="ambiguous/nonexistent"):
            Q.accepted_clock(index(DECL, value), DECL, issuer=900, member_url=uri(DECL,"release.htm"), member_blob=b"release")
    with pytest.raises(ValueError, match="accession"):
        Q.accepted_clock(index(DECL, "2021-01-01 10:00:00"), COVER, issuer=900, member_url=uri(COVER,"release.htm"), member_blob=b"release")


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


@pytest.mark.parametrize("case", ["positive", "annual_meeting", "conditional", "earnings", "conflicting_cadence",
    "only_when", "as_long_as", "false_declaration", "quoted_declaration", "separate_denial", "unknown_condition",
    "future_date", "wrong_header", "wrong_issuer", "multiple_clocks", "swapped_member", "tzif_drift", "normalizer_drift",
    "same_function_code_drift", "same_function_public_code_drift", "producer_after_replay_code_drift",
    "public_after_replay_code_drift", "dom_deleted", "dom_quoted", "dom_hidden", "dom_unknown",
    "dom_transparent"])
def test_production_ancillary_admission_replay_qualifies_and_missing_remains_null(tmp_path, monkeypatch, case):
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
    if case == "annual_meeting": text = "At the annual meeting, " + text
    if case == "conditional": text = text[:-1] + ", subject to regulatory approval."
    if case == "earnings": text = text[:-1] + ", if earnings targets are achieved."
    if case == "only_when": text = text[:-1] + ", only when earnings targets are achieved."
    if case == "as_long_as": text = text[:-1] + ", as long as regulatory approval is granted."
    if case == "false_declaration": text = "It is false that " + text
    if case == "quoted_declaration": text = 'The disputed statement reads: "' + text + '"'
    if case == "separate_denial": text += " This declaration is false."
    if case == "unknown_condition": text = text[:-1] + ", assuming the lender consents."
    if case == "conflicting_cadence": text = text.replace("regular quarterly", "regular annual quarterly")
    if case == "future_date": text = text.replace("January 4, 2021", "January 8, 2021")
    if case == "tzif_drift":
        import tzdata
        package = tmp_path / "tzdata"
        zone = package / "zoneinfo/America/New_York"
        zone.parent.mkdir(parents=True)
        zone.write_bytes(Q.timezone_paths()[0].read_bytes())
        monkeypatch.setattr(tzdata, "__file__", str(package / "__init__.py"))
    def original(name, blob, accession):
        path = tmp_path / name
        path.write_bytes(blob)
        receipt = tmp_path / (name + ".receipt.jsonl")
        raw = (json.dumps({"url": uri(accession, name), "status": 200,
            "sha256": C.sha256_file(path), "fetched_at": "2026-01-01T00:00:00Z"}) + "\n").encode()
        receipt.write_bytes(raw)
        return {"accession": accession, "cik": 900, "source_path": str(path), "source_doc": name,
            "source_url": uri(accession, name), "source_sha256": C.sha256_file(path),
            "stored_sha256": C.sha256_file(path), "storage_encoding": "plain", "fetched_at": "2026-01-01T00:00:00Z",
            "link_basis": "unknown", "receipt": {"path": str(receipt), "sha256": C.sha256_file(receipt), "line": 1,
            "line_sha256": __import__("hashlib").sha256(raw).hexdigest()}}
    original_html = "<html><p>" + text + "</p></html>"
    if case == "dom_deleted": original_html = "<html><body><del>" + text + "</del></body></html>"
    if case == "dom_quoted": original_html = "<html><body><blockquote>" + text + "</blockquote></body></html>"
    if case == "dom_hidden": original_html = '<html><p style="display:none">' + text + "</p></html>"
    if case == "dom_unknown": original_html = "<html><body><aside>" + text + "</aside></body></html>"
    if case == "dom_transparent": original_html = "<html><body><div><p><span>" + text + "</span></p></div></body></html>"
    doc = original("release.htm", original_html.encode(), DECL)
    clock_body = index(DECL, "2021-01-04 10:00:00")
    if case == "wrong_header":
        clock_body = ("<SEC-DOCUMENT><SEC-HEADER><ACCESSION-NUMBER>0000000900-21-999999\n"
            "<ACCEPTANCE-DATETIME>20210104100000\nCENTRAL INDEX KEY: 900\n</SEC-HEADER>"
            "<DOCUMENT><FILENAME>release.htm\n<TEXT>Related accession " + DECL + "</TEXT></DOCUMENT>").encode()
    if case == "wrong_issuer": clock_body = clock_body.replace(b"CIK=0000000900", b"CIK=0000000999")
    if case == "multiple_clocks": clock_body = clock_body.replace(b"</html>", b'<div>Accepted</div><div>2021-01-01 10:00:00</div></html>')
    if case == "swapped_member": clock_body = index(DECL, "2021-01-04 10:00:00", "different.htm")
    clock_doc = original(DECL + "-index.html", clock_body, DECL)
    class_doc = original("cover.htm", CLASS, COVER)
    class_clock_doc = original(COVER + "-index.html", index(COVER, "2020-12-15 10:00:00"), COVER)
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
        path.write_text(json.dumps({"schema": f"atx.native-{role}-evidence/v1", "records": [{"declaration_accession":DECL,
            "declaration_sha256":doc["source_sha256"], **data}]}))
    expected = json.loads(admission.read_text())
    expected["schema"] = "atx.native-admission/v2"
    expected["inputs"]["prices"] = {"path":str(prices),"sha256":C.sha256_file(prices)}
    for role in ("clock","class"):
        expected["inputs"][role]["sha256"] = C.sha256_file(tmp_path / (role + ".json"))
    admission.write_text(json.dumps(expected))
    sha = C.sha256_file(admission)
    if case == "tzif_drift":
        zone.write_bytes(b"changed TZif bytes")
    if case == "normalizer_drift":
        monkeypatch.setattr(Q, "ZoneInfo", lambda *args: dt.timezone.utc)
    if case in {"same_function_code_drift", "same_function_public_code_drift",
                "producer_after_replay_code_drift", "public_after_replay_code_drift"}:
        # Freeze expected originals/code first. Change code on the same loaded
        # function; all disk files, pointer identities and TZif remain intact.
        if case in {"same_function_public_code_drift", "public_after_replay_code_drift"}:
            D.build(catalog, [], admission_path=admission, expected_sha256=sha)
            assert D.validate_product(target, admission, sha)["conditionally_qualified_events"] == 1
        loader = Q._zoneinfo_common.load_data
        def utc_loader(stream):
            return (), (), (0,), (0,), ("UTC",), b"UTC0"
        if case in {"producer_after_replay_code_drift", "public_after_replay_code_drift"}:
            original_replay = D.replay_rows
            def drift_after_replay(*args, **kwargs):
                result = original_replay(*args, **kwargs)
                monkeypatch.setattr(loader, "__code__", utc_loader.__code__)
                return result
            monkeypatch.setattr(D, "replay_rows", drift_after_replay)
            with pytest.raises(ValueError, match="normalizer entrypoint"):
                if case == "producer_after_replay_code_drift":
                    D.build(catalog, [], admission_path=admission, expected_sha256=sha)
                else:
                    D.validate_product(target, admission, sha)
            return
        monkeypatch.setattr(loader, "__code__", utc_loader.__code__)
        assert Q._zoneinfo_common.load_data is loader
        with pytest.raises(ValueError, match="normalizer entrypoint"):
            D.build(catalog, [], admission_path=admission, expected_sha256=sha)
        with pytest.raises(ValueError, match="normalizer entrypoint"):
            D.validate_product(target, admission, sha)
        return
    if case in {"tzif_drift", "normalizer_drift"}:
        with pytest.raises(ValueError, match="code mismatch|normalizer entrypoint"):
            D.build(catalog, [], admission_path=admission, expected_sha256=sha)
        with pytest.raises(ValueError, match="code mismatch|normalizer entrypoint"):
            D.validate_product(target, admission, sha)
        return
    result = D.build(catalog, [], admission_path=admission, expected_sha256=sha)
    row, = pq.read_table(target).to_pylist()
    if case not in {"positive", "annual_meeting", "dom_transparent"}:
        assert row["indicated_annual_dividend_rate"] is None
        if case.startswith("dom_"):
            assert "original declaration DOM scope unsupported" in row["source_qualification"]
        assert D.validate_product(target, admission, sha)["conditionally_qualified_events"] == 0
        row["indicated_annual_dividend_rate"] = 2.4
        if case.startswith("dom_"):
            row["source_qualification"] = "native_fixed_regular_event"
            row["security_class_currency_basis"] = "Class A common stock/USD"
        pq.write_table(pa.Table.from_pylist([row], schema=D.output_schema()), target)
        with pytest.raises(ValueError, match="replay"):
            D.validate_product(target, admission, sha)
        return
    assert row["indicated_annual_dividend_rate"] == pytest.approx(2.4)
    assert row["available_at"] == D.clock("2021-01-04T15:00:00Z")
    assert row["security_id"] == 10
    assert not row["quote_currency_qualified"]
    assert D.validate_product(target, admission, sha)["conditionally_qualified_events"] == 1
    assert all(gate["status"] == "FAIL" for gate in result["gate_status"].values())
    if case in {"annual_meeting", "dom_transparent"}: return
    # Independently admitted original ADS evidence cannot qualify common-stock cash.
    negative_class = original("cover.htm", CLASS.replace(b"Class A common stock", b"American depositary shares"), COVER)
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


def test_ambient_timezone_path_does_not_change_admitted_normalization(tmp_path):
    import zoneinfo
    from atx_db.alpha_panel import distributions_native as N
    old = zoneinfo.TZPATH
    pins = N.code_pins()
    try:
        zoneinfo.reset_tzpath([str(tmp_path)])
        zoneinfo.ZoneInfo.clear_cache()
        event, text, args = inputs()
        assert Q.qualify(event, text, **args)["available_at"] == D.clock("2021-01-04T15:00:00Z")
        assert N.code_pins() == pins
    finally:
        zoneinfo.reset_tzpath(old)
        zoneinfo.ZoneInfo.clear_cache()


@pytest.mark.parametrize("wrapper", [
    "<del>{}</del>", "<blockquote>{}</blockquote>", "<q>{}</q>", "<s>{}</s>",
    "<span hidden='hidden'>{}</span>", "<p class='disputed'>{}</p>",
    "<script>{}</script>", "<ins>{}</ins>", "<!-- quoted -->{}",
    "<p>{}</span>", "<span xmlns='urn:quoted'>{}</span>"])
def test_original_dom_unsupported_scope_refuses(wrapper):
    event, text, args = inputs()
    args["declaration_document"] = ("<html><body>" + wrapper.format(text) + "</body></html>").encode()
    with pytest.raises(ValueError, match="original declaration DOM"):
        Q.qualify(event, text, **args)


def test_original_dom_complete_text_binds_normalized_evidence():
    event, text, args = inputs()
    args["declaration_document"] = ("<html><p>" + text + "</p><p>This declaration is disputed.</p></html>").encode()
    with pytest.raises(ValueError, match="differs from normalized"):
        Q.qualify(event, text, **args)


def test_original_sgml_membership_positive_and_wrong_member_body():
    event, text, args = inputs()
    blob = (b"<SEC-DOCUMENT><SEC-HEADER><ACCESSION-NUMBER>" + DECL.encode()
        + b"\n<ACCEPTANCE-DATETIME>20210104100000\nCENTRAL INDEX KEY: 900\n</SEC-HEADER>"
        + b"<DOCUMENT><FILENAME>release.htm\n<TEXT>" + args["declaration_document"] + b"</TEXT></DOCUMENT>")
    kwargs = dict(issuer=900,member_url=args["declaration_url"],member_blob=args["declaration_document"])
    assert Q.accepted_clock(blob, DECL, **kwargs) == D.clock("2021-01-04T15:00:00Z")
    with pytest.raises(ValueError, match="membership"):
        Q.accepted_clock(blob, DECL, **{**kwargs,"member_blob":b"unrelated"})


def test_same_loaded_parser_code_is_independently_pinned_and_rechecked(monkeypatch):
    from atx_db.alpha_panel import distributions_native as N
    pins = N.code_pins()
    assert "runtime:zoneinfo.load_data" in pins
    assert "runtime:zoneinfo.TZifHeader.from_file" in pins
    N.recheck(pins)
    loader = Q._zoneinfo_common.load_data
    def utc_loader(stream):
        return (), (), (0,), (0,), ("UTC",), b"UTC0"
    monkeypatch.setattr(loader, "__code__", utc_loader.__code__)
    assert Q._zoneinfo_common.load_data is loader
    for operation in (N.code_pins, lambda: N.recheck(pins), lambda: Q.qualify(*inputs()[:2], **inputs()[2])):
        with pytest.raises(ValueError, match="normalizer entrypoint"):
            operation()


def test_same_loaded_tzif_header_code_drift_is_rejected(monkeypatch):
    from atx_db.alpha_panel import distributions_native as N
    pins = N.code_pins()
    header = Q._zoneinfo_common._TZifHeader.from_file.__func__
    def changed_header(cls, stream):
        return None
    monkeypatch.setattr(header, "__code__", changed_header.__code__)
    with pytest.raises(ValueError, match="normalizer entrypoint"):
        N.recheck(pins)
