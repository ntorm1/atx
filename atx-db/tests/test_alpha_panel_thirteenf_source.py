from atx_db.alpha_panel.thirteenf_source import audit_roots
import io
import json
import sqlite3
import pytest


def test_metadata_does_not_certify_receipts(tmp_path):
    raw = tmp_path / "sec-13f"
    raw.mkdir()
    (raw / "receipts.jsonl").write_text("{}", encoding="utf-8")
    (tmp_path / "opaque.zip").write_bytes(b"archive")
    result = audit_roots([tmp_path, tmp_path])
    assert result["files"] == 2 and result["bytes"] == 9
    assert result["metadata_complete"]
    assert result["source_completeness"] == "UNMEASURED"
    assert result["candidate_files"][0]["kind"] == "receipt"


def test_bounds_and_missing_roots_are_explicit(tmp_path):
    for name in ("a", "b"):
        (tmp_path / name).write_bytes(b"x")
    result = audit_roots([tmp_path], max_files=1)
    assert not result["metadata_complete"]
    assert result["exceptions"][0]["reason"] == "file_bound"
    assert audit_roots([tmp_path / "missing"])["exceptions"][0]["reason"] == "missing_root"


def test_all_file_inventory_preserves_opaque_source_candidates(tmp_path):
    (tmp_path / "master.idx").write_bytes(b"index")
    sink = io.StringIO()
    result = audit_roots([tmp_path], metadata_sink=sink)
    assert result["candidate_files"] == []
    assert json.loads(sink.getvalue())["path"].endswith("master.idx")


def test_readonly_submissions_cache_inspection_pins_receipt_without_certifying_archive(tmp_path):
    from atx_db.alpha_panel.thirteenf_source import inspect_submissions_cache
    from atx_db.alpha_panel.common import sha256_file
    archive,index,receipt = [tmp_path / p for p in ("submissions.zip","directory.sqlite3","directory.sha256")]
    archive.write_bytes(b"fixture")
    stat=archive.stat()
    con=sqlite3.connect(index)
    con.execute("CREATE TABLE metadata(key TEXT,value TEXT)")
    identity=dict(source_sha256=sha256_file(archive),source_stat=[stat.st_dev,stat.st_ino,stat.st_size,stat.st_mtime_ns])
    con.execute("INSERT INTO metadata VALUES('identity',?)",(json.dumps(identity),))
    con.execute("CREATE TABLE members(name TEXT,kind INTEGER)")
    con.executemany("INSERT INTO members VALUES(?,?)",[("CIK0000000001.json",1),("CIK0000000001-submissions-001.json",2)])
    con.commit()
    con.close()
    receipt.write_text(sha256_file(index))
    result=inspect_submissions_cache(archive,index,receipt)
    assert result["member_counts_by_kind"] == {1:1,2:1}
    assert result["archive_content_verified"] is False
    receipt.write_text("0"*64)
    with pytest.raises(ValueError,match="digest mismatch"):
        inspect_submissions_cache(archive,index,receipt)


def test_bounded_ledger_leads_keep_opaque_records_unknown_without_reading_objects(tmp_path):
    from atx_db.alpha_panel.thirteenf_source import inspect_ledgers
    ledger=tmp_path / "receipts.jsonl"
    rows=[dict(form="13F-HR",url="https://www.sec.gov/Archives/opaque",path="missing-original.gz"),
          dict(url="https://www.sec.gov/Archives/other-opaque",sha256="a"*64),dict(form="10-K")]
    ledger.write_text("".join(json.dumps(row)+"\n" for row in rows))
    result=inspect_ledgers([ledger],max_candidates=1)
    report=result["files"][0]
    assert report["scan_complete"] and report["records_read"]==3
    assert report["13f_lead_records"]==1 and report["unknown_records"]==1
    assert report["candidate_bound_reached"] and result["objects_read"]==0
    assert result["source_completeness"]=="UNMEASURED"
    bounded=inspect_ledgers([ledger],max_records=1)["files"][0]
    assert not bounded["scan_complete"] and bounded["exceptions"][0]["reason"]=="record_or_byte_bound"
    ledger.write_text('{bad}\n')
    assert inspect_ledgers([ledger])["files"][0]["exceptions"][0]["reason"]=="malformed_record"


def test_ledger_unknown_forms_paths_and_truncation_are_explicit(tmp_path):
    from atx_db.alpha_panel.thirteenf_source import inspect_ledgers
    ledger=tmp_path / "ledger.jsonl"
    ledger.write_text(json.dumps(dict(form="UNKNOWN_FORM",path="../relative.gz"))+"\n"+
                      json.dumps(dict(path="C:/outside/opaque.gz"))+"\n")
    report=inspect_ledgers([ledger])["files"][0]
    assert report["unknown_records"]==2
    assert {row["metadata"]["path"] for row in report["candidates"]}=={"../relative.gz","C:/outside/opaque.gz"}
    assert all(row["referenced_paths_status"]=="UNVALIDATED_CONFIGURED_ROOTS" for row in report["candidates"])
    report=inspect_ledgers([ledger],max_record_bytes=8)["files"][0]
    assert not report["scan_complete"] and report["exceptions"][0]["reason"]=="record_or_byte_bound"
    report=inspect_ledgers([ledger],max_bytes=12)["files"][0]
    assert not report["scan_complete"] and report["bytes_read"]==13


def test_known_13f_leads_survive_unknown_sample_saturation(tmp_path):
    from atx_db.alpha_panel.thirteenf_source import inspect_ledgers
    ledger=tmp_path / "ledger.jsonl"
    rows=[dict(url="https://www.sec.gov/Archives/opaque") for _ in range(3)]
    rows+= [dict(form="13F-HR",path="original-1.gz"),dict(form="13F-NT",path="original-2.gz")]
    ledger.write_text("".join(json.dumps(row)+"\n" for row in rows))
    report=inspect_ledgers([ledger],max_candidates=1)["files"][0]
    assert report["candidate_bound_reached"] and report["candidates"][0]["classification"]=="UNKNOWN"
    assert report["unknown_records"]==3 and report["13f_lead_records"]==2
    assert [row["record"] for row in report["known_13f_leads"]]==[4,5]
    assert not report["known_13f_lead_bound_reached"] and report["scan_complete"]
    ledger.write_text("".join(json.dumps(dict(form="13F-HR"))+"\n" for _ in range(257)))
    report=inspect_ledgers([ledger],max_candidates=1)["files"][0]
    assert len(report["known_13f_leads"])==256 and report["known_13f_lead_bound_reached"]
    assert report["13f_lead_records"]==257 and report["scan_complete"]


ACTUAL_DIGEST_SUFFIX_FALSE_LEADS = [{'accession': '0001089063-20-000014', 'fetched_at': '2026-09-30T11:14:14.273+00:00', 'http_status': 200, 'sha256': 'd7518906e3d3a4a9f870378df64494b4482b0340b666250aa46842d8c915413f', 'url': 'https://www.sec.gov/Archives/edgar/data/1089063/000108906320000014/dks-20200201.htm'}, {'accession': '0001654954-18-010445', 'fetched_at': '2026-09-30T11:41:42.490+00:00', 'http_status': 200, 'sha256': 'eb0630ec65094eaefc84e2f9e4258d2d5ac16596e31c64ae9d2a44a97364713f', 'url': 'https://www.sec.gov/Archives/edgar/data/732026/000165495418010445/tti_10k.htm'}, {'fetched_at': '2026-09-30T00:16:15.107+00:00', 'sha256': '54a2a1c6ac1bb14a45a09a423b0b68b02b5313d97141b4601597e8c0d2b0113f', 'status': 200, 'url': 'https://www.sec.gov/Archives/edgar/data/851968/000085196826000031/a2026q2pressreleaseandtabl.htm'}, {'fetched_at': '2026-09-30T00:18:37.105+00:00', 'sha256': '94f113f2920e3a6773e5a19c188254c09a55b3061947e0f378c335498f84613f', 'status': 200, 'url': 'https://efts.sec.gov/LATEST/search-index?q=%22financial+results%22+OR+%22results+for+the%22+OR+%22earnings+release%22&forms=6-K&dateRange=custom&startdt=2020-08-01&enddt=2020-08-31&from=1000'}, {'fetched_at': '2026-09-30T00:56:41.809+00:00', 'sha256': 'cd588a5455bf884ec8c380f2b655ba793e40101cf487fb5789671960b3c7713f', 'status': 200, 'url': 'https://www.sec.gov/Archives/edgar/data/1754068/000119312523268875/d559627dex991.htm'}, {'fetched_at': '2026-09-30T01:12:41.539+00:00', 'sha256': 'b938a2a41319563d8d9f3b982440b13e90744f2298cfc3488168bca12695913f', 'status': 200, 'url': 'https://www.sec.gov/Archives/edgar/data/1420811/000142081120000122/ondkearningsrelease202.htm'}]


def test_actual_metadata_digest_suffixes_never_become_13f_leads():
    from atx_db.alpha_panel.thirteenf_source import classify_ledger_metadata
    assert len(ACTUAL_DIGEST_SUFFIX_FALSE_LEADS)==6
    assert all(classify_ledger_metadata(row)=="UNKNOWN" for row in ACTUAL_DIGEST_SUFFIX_FALSE_LEADS)


@pytest.mark.parametrize("metadata,classification",[
    ({"form":"13F-HR"},"13F_LEAD"),
    ({"source.form_type":"13F-NT/A"},"13F_LEAD"),
    ({"url":"https://www.sec.gov/Archives/edgar/data/1/000000000120000001/13f-hr.xml"},"13F_LEAD"),
    ({"filename":"manager_13f.xml"},"13F_LEAD"),
    ({"local_path":"C:/raw/sec_13f/opaque.gz"},"13F_LEAD"),
    ({"filename":"prefix13f.xml"},"UNKNOWN"),
    ({"filename":"13foo.xml"},"UNKNOWN"),
    ({"sha256":"0"*61+"13f","fetched_at":"13f","digest_path":"/13f/x"},"UNKNOWN"),
    ({"url":"https://13f.example/opaque?form=13f"},"UNKNOWN"),
    ({"form":"10-K","sha256":"0"*61+"13f"},"OTHER_FORM"),
])
def test_semantic_fields_and_token_boundaries_only(metadata,classification):
    from atx_db.alpha_panel.thirteenf_source import classify_ledger_metadata
    assert classify_ledger_metadata(metadata)==classification
