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
