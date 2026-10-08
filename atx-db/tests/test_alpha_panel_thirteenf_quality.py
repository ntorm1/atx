from atx_db.alpha_panel.thirteenf_quality import master_inventory, reconcile
import json
import pytest


def test_index_keeps_amendments_notices_without_invented_clocks():
    rows = list(master_inventory([
        "1|Manager|13F-HR/A|2020-05-15|edgar/data/1/0000000001-20-000001.txt",
        "1|Manager|13F-NT|2020-05-16|edgar/data/1/0000000001-20-000002.txt"],
        inventory_url="fixture", inventory_sha256="digest"))
    assert len(rows) == 2
    assert rows[0]["filing_quarter"] == "2020q2"
    assert rows[0]["report_quarter"] is None and rows[0]["available_at"] is None


def test_receipt_and_confidentiality_do_not_exempt_missing_public_bronze():
    expected = {"accession": "0000000001-20-000001", "form": "13F-HR", "inventory_sha256": "x"}
    parsed = {"parse_ok": True, "available_at": "2020-05-15T12:00:00Z", "report_quarter": "2020q1",
              "value_unit": "USD_THOUSANDS", "cover_reconciled": True,
              "manager_chain_resolved": True, "holdings_rows": 0, "confidential": True}
    result = reconcile(expected, {"url": "receipt"}, parsed)
    assert result["gate_status"] == "FAIL"
    assert "retained_original_missing" in result["exceptions"]
    assert "zero_holdings_unexplained" in result["exceptions"]


def test_notice_admission_requires_parsed_clock_and_retained_original():
    expected = {"accession": "0000000001-20-000001", "form": "13F-NT", "inventory_sha256": "x"}
    retained = dict(path="fixture", sha256="x", url="fixture", fetched_at="clock", verified_original=True)
    parsed = dict(parse_ok=True, available_at="2020-05-15T12:00:00Z", report_quarter="2020q1")
    assert reconcile(expected, retained, parsed)["gate_status"] == "PASS"


def test_wrong_accession_and_timezone_are_not_admitted():
    expected = {"accession": "0000000001-20-000001", "form": "13F-NT", "inventory_sha256": "x"}
    parsed = dict(accession="0000000001-20-000002", parse_ok=True,
                  available_at="2020-05-15T12:00:00", report_quarter="2020q1")
    result = reconcile(expected, None, parsed)
    assert "parsed_accession_mismatch" in result["exceptions"]
    assert "clock_timezone_missing" in result["exceptions"]


def test_native_quality_build_preserves_missing_and_unexpected_accessions(tmp_path, monkeypatch):
    from atx_db.alpha_panel.thirteenf_quality import build
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    expected, retained, parsed = [tmp_path / (name + ".jsonl") for name in ("expected", "retained", "parsed")]
    expected.write_text(json.dumps(dict(accession="0000000001-20-000001", form="13F-HR", inventory_sha256="x")) + "\n")
    retained.write_text("")
    parsed.write_text(json.dumps(dict(accession="0000000001-20-000002", parse_ok=False)) + "\n")
    receipt = build(expected, retained, parsed)
    assert receipt["failed_accessions"] == 1 and receipt["unexpected_parsed"] == 1
    assert receipt["gate_status"] == "FAIL"
    assert (tmp_path / "lake/thirteenf_quality/manifest.json").exists()


def test_duplicate_inventory_accessions_fail_closed(tmp_path, monkeypatch):
    from atx_db.alpha_panel.thirteenf_quality import build
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    path = tmp_path / "expected.jsonl"
    row = json.dumps(dict(accession="0000000001-20-000001", form="13F-NT")) + "\n"
    path.write_text(row + row)
    empty = tmp_path / "empty.jsonl"
    empty.write_text("")
    with pytest.raises(Exception, match="Duplicate key"):
        build(path, empty, empty)
    assert not (tmp_path / "lake/thirteenf_quality/manifest.json").exists()


def test_parsed_adapter_reconciles_cover_and_uses_actual_acceptance(tmp_path):
    import duckdb
    from atx_db.alpha_panel.thirteenf_quality import parsed_adapter_sql
    con = duckdb.connect()
    con.execute("SET TimeZone='UTC'")
    files = [tmp_path / (name + ".parquet") for name in ("filings", "holdings", "sec")]
    con.execute("""CREATE TABLE f AS SELECT '0000000001-20-000001' AS accession,
        '13F-HR' AS submission_type,'1' AS filer_cik,DATE '2020-03-31' AS period_of_report,
        DATE '2020-03-31' AS period_q,TIMESTAMPTZ '2020-05-17T22:00:00Z' AS available_at,
        1 AS table_entry_total,100.0 AS table_value_total_raw,0 AS other_included_managers_count,
        'N' AS is_amendment,NULL::VARCHAR AS amendment_type,'HOLDINGS REPORT' AS report_type,
        'N' AS is_confidential_omitted""")
    con.execute("""CREATE TABLE h AS SELECT '0000000001-20-000001' AS accession,
        100.0 AS value_raw,'thousands' AS value_unit,'SH' AS sshprnamt_type,
        NULL::VARCHAR AS put_call,NULL::VARCHAR AS other_manager""")
    con.execute("""CREATE TABLE s AS SELECT '0000000001-20-000001' AS accession,'13F-HR' AS form,
        TIMESTAMP '2020-05-15 20:00:00' AS acceptance_utc,NULL::VARCHAR AS vintage_risk""")
    for table, path in zip(("f", "h", "s"), files):
        con.execute(f"COPY {table} TO '{path.as_posix()}' (FORMAT parquet)")
    row = con.execute(parsed_adapter_sql(*(p.as_posix() for p in files))).fetchone()
    result = dict(zip([r[0] for r in con.description], row))
    assert result["cover_reconciled"] and result["manager_chain_resolved"]
    assert result["clock_basis"] == "public_acceptance"
    assert result["available_at"].day == 15
    assert result["value_unit"] == "USD_THOUSANDS"
    con.close()


def test_native_submissions_partition_reads_original_history_and_missing_clock(tmp_path):
    import zipfile
    from atx_db._submissions_archive import SubmissionsArchive
    from atx_db.alpha_panel.thirteenf_quality import inventory_submissions
    archive = tmp_path / "submissions.zip"
    history = "CIK0000000001-submissions-001.json"
    main = dict(filings=dict(recent=dict(form=["13F-HR/A"],accessionNumber=["0000000001-20-000001"],
        filingDate=["2020-05-15"],reportDate=["2020-03-31"],acceptanceDateTime=["2020-05-15T20:00:00Z"]),
        files=[dict(name=history),dict(name="CIK0000000001-submissions-002.json")]))
    historical = dict(form=["13F-NT"],accessionNumber=["0000000001-19-000001"],filingDate=["2019-05-15"])
    with zipfile.ZipFile(archive,"w",zipfile.ZIP_DEFLATED) as source:
        source.writestr("CIK0000000001.json",json.dumps(main))
        source.writestr(history,json.dumps(historical))
    # Fixture-only cache creation; native production reader forbids rebuilding it.
    with SubmissionsArchive(archive):
        pass
    output=tmp_path / "inventory.jsonl"
    receipt=inventory_submissions(archive,output,after=None,through="CIK0000000009.json",max_main=10,
                                  first=__import__('datetime').date(2019,1,1))
    rows=[json.loads(line) for line in output.read_text().splitlines()]
    assert len(rows)==2 and rows[0]["form"]=="13F-HR/A"
    assert rows[1]["report_quarter"] is None and rows[1]["available_at"] is None
    assert receipt["exceptions"][0]["reason"]=="history_member_missing"
    assert receipt["source_completeness"]=="UNQUALIFIED_PARTITION"


def test_effective_amendment_chain_uses_strict_public_clock():
    import datetime as dt
    from atx_db.alpha_panel.thirteenf_quality import effective_chain
    rows=[]
    for number,(day,form,kind) in enumerate([(15,"13F-HR",None),(20,"13F-HR/A","NEW HOLDINGS"),(25,"13F-HR/A","RESTATEMENT")]):
        rows.append(dict(accession=f"0000000001-20-{number+1:06d}",filer_cik=1,report_date="2020-03-31",form=form,
            amendment_type=kind,available_at=f"2020-05-{day}T20:00:00Z"))
    cutoff=lambda day:dt.datetime(2020,5,day,20,tzinfo=dt.UTC)
    assert effective_chain(rows,cutoff(20))["effective_accessions"]==["000000000120000001"]
    assert effective_chain(rows,cutoff(21))["effective_accessions"]==["000000000120000001","000000000120000002"]
    assert effective_chain(rows,cutoff(26))["effective_accessions"]==["000000000120000003"]


def test_actual_original_cover_positions_and_claims_stay_unqualified(tmp_path):
    import hashlib
    from atx_db.alpha_panel.thirteenf_quality import original_submission_evidence,bridge_originals
    raw=b"""<SEC-DOCUMENT>
ACCESSION NUMBER: 0000000001-20-000001
CONFORMED SUBMISSION TYPE: 13F-HR
<ACCEPTANCE-DATETIME>20200515120000
<XML><edgarSubmission><reportCalendarOrQuarter>03-31-2020</reportCalendarOrQuarter><tableEntryTotal>1</tableEntryTotal><tableValueTotal>10</tableValueTotal></edgarSubmission></XML>
<XML><informationTable><infoTable><cusip>123456789</cusip><value>10</value><shrsOrPrnAmt><sshPrnamt>100</sshPrnamt><sshPrnamtType>SH</sshPrnamtType></shrsOrPrnAmt><investmentDiscretion>SOLE</investmentDiscretion></infoTable></informationTable></XML>
</SEC-DOCUMENT>"""
    path=tmp_path / "original.txt"
    path.write_bytes(raw)
    expected=dict(accession="0000000001-20-000001",form="13F-HR",report_date="2020-03-31")
    retained=dict(path=str(path),sha256=hashlib.sha256(raw).hexdigest(),url="https://www.sec.gov/Archives/edgar/data/1/000000000120000001/complete.txt",fetched_at="2026-01-01T00:00:00Z",verified_original=True)
    result=original_submission_evidence(raw,expected,retained)
    assert result["cover_reconciled"] and result["positions"][0]["shares"]==100
    assert result["available_at"] is None and result["acceptance_header_raw"]=="20200515120000"
    assert not result["source_qualified"] and not result["table_absence_qualified"]
    assert "documentary_value_unit_unverified" in result["exceptions"]
    bad=original_submission_evidence(raw,expected,dict(retained,sha256="0"*64,url="https://other.example/complete.txt"))
    assert "original_digest_mismatch" in bad["exceptions"] and "original_sec_url_accession_unproven" in bad["exceptions"]
    evidence=tmp_path / "evidence.jsonl"
    evidence.write_text(json.dumps(dict(expected=expected,retained=retained))+"\n"+
                        json.dumps(dict(expected=expected,retained=dict(retained,path="../outside.txt")))+"\n")
    output=tmp_path / "bridge.jsonl"
    receipt=bridge_originals(evidence,[tmp_path],output)
    rows=[json.loads(line) for line in output.read_text().splitlines()]
    assert receipt["records"]==2 and rows[1]["exceptions"]==["original_path_must_be_absolute"]
    assert receipt["source_completeness"]=="UNQUALIFIED_ORIGINAL_DIAGNOSTICS"
    with pytest.raises(ValueError,match="output/receipt exists"):
        bridge_originals(evidence,[tmp_path],output)
    assert bridge_originals(evidence,[tmp_path],tmp_path / "bounded.jsonl",max_records=1)["bound_reached"]


def test_original_xml_entities_wrong_counts_and_missing_clock_are_keyed_diagnostics():
    from atx_db.alpha_panel.thirteenf_quality import original_submission_evidence
    raw=b'<SEC-DOCUMENT>\nACCESSION NUMBER: invalid\nCONFORMED SUBMISSION TYPE: 13F-HR\n<XML><!DOCTYPE a [<!ENTITY x SYSTEM "file:///secret">]><a>&x;</a></XML></SEC-DOCUMENT>'
    result=original_submission_evidence(raw,dict(accession="000000000120000001",form="13F-HR"),{})
    assert "original_header_accession_invalid" in result["exceptions"]
    assert "xml_external_entity_or_doctype_unsupported" in result["exceptions"]
    assert "original_acceptance_missing" in result["exceptions"]
    assert "original_cover_table_count_or_value_unreconciled" in result["exceptions"]
    assert result["source_qualified"] is False


@pytest.mark.parametrize("suffix,valid",[
    ("000000000120000001/complete.txt",True),
    ("0000000001-20-000001.txt",True),
    ("000000000120000001.txt",True),
    ("000000000120000001999/complete.txt",False),
    ("prefix000000000120000001/complete.txt",False),
    ("prefix0000000001-20-000001.txt",False)])
def test_original_url_accession_is_exact_component_or_submission_filename(suffix,valid):
    from atx_db.alpha_panel.thirteenf_quality import original_submission_evidence
    result=original_submission_evidence(b"",dict(accession="000000000120000001",form="13F-HR"),
        dict(url="https://www.sec.gov/Archives/edgar/data/1/"+suffix))
    assert ("original_sec_url_accession_unproven" not in result["exceptions"])==valid


@pytest.mark.parametrize("collision",["output","receipt","interruption"])
def test_original_publication_atomically_refuses_collisions_and_preserves_forensics(tmp_path,monkeypatch,collision):
    import os
    from atx_db.alpha_panel.thirteenf_quality import _publish_original_bridge
    output=tmp_path / "bridge.jsonl"
    partial=tmp_path / "bridge.jsonl.partial"
    receipt=tmp_path / "bridge.jsonl.receipt.json"
    receipt_partial=tmp_path / "bridge.jsonl.receipt.json.partial"
    partial.write_bytes(b"new evidence")
    real_link=os.link
    def competing_link(source,target):
        if target==output and collision=="output":
            output.write_bytes(b"other output")
        if target==receipt:
            if collision=="receipt":
                receipt.write_bytes(b"other receipt")
            elif collision=="interruption":
                raise InterruptedError("fixture publication interrupted")
        return real_link(source,target)
    monkeypatch.setattr(os,"link",competing_link)
    with pytest.raises((FileExistsError,InterruptedError)):
        _publish_original_bridge(partial,output,{"gate_status":"UNQUALIFIED"})
    assert partial.read_bytes()==b"new evidence" and receipt_partial.exists()
    if collision=="output":
        assert output.read_bytes()==b"other output" and not receipt.exists()
    elif collision=="receipt":
        assert output.read_bytes()==b"new evidence" and receipt.read_bytes()==b"other receipt"
    else:
        assert output.read_bytes()==b"new evidence" and not receipt.exists()
