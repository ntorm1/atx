from atx_db.alpha_panel.thirteenf_source import audit_roots
import io
import json


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
