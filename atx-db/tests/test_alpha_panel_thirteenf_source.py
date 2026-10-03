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
