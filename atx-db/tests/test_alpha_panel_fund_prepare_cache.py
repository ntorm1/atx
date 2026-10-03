import json
import os

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import fund_prepare_cache as cache


def snapshot(root):
    quarters = {}
    for p in root.glob("*/*.parquet"):
        quarters.setdefault(p.stem, {"tables": {}})["tables"][p.parent.name] = {
            "path": p.relative_to(root).as_posix(),
            "parquet_sha256": cache.digest(p), "parquet_bytes": p.stat().st_size}
    root.mkdir(exist_ok=True)
    (root / "fsds-staging-manifest.json").write_text(json.dumps({"quarters": quarters}))
    return cache.SourceSnapshot(root)


def test_interruption_resume_fresh_equivalence_and_legacy(tmp_path, monkeypatch):
    s = snapshot(tmp_path / "source")
    c = cache.PreparationCache(s, {"code": "a"})
    con = duckdb.connect()
    dest = tmp_path / "unit.parquet"
    pq.write_table(pa.table({"a": [99]}), dest)
    legacy = dest.with_name(dest.name + ".sha256")
    legacy.write_text("legacy")
    real = cache.common.copy_to_parquet
    def interrupted(con, sql, out, **kw):
        real(con, sql, out, **kw)
        raise KeyboardInterrupt()
    monkeypatch.setattr(cache.common, "copy_to_parquet", interrupted)
    with pytest.raises(KeyboardInterrupt):
        c.copy(con, "SELECT 1 AS a", dest, {}, "one")
    assert pq.read_table(dest).to_pylist() == [{"a": 99}]
    assert not dest.with_name(dest.name + ".prepare.json").exists()
    assert list(tmp_path.glob("*.attempt-*"))
    monkeypatch.setattr(cache.common, "copy_to_parquet", real)
    c.copy(con, "SELECT 1 AS a", dest, {}, "one")
    assert legacy.read_text() == "legacy"
    assert list(tmp_path.glob("*.previous-*"))
    r = {}
    resumed = cache.PreparationCache(cache.SourceSnapshot(s.root), {"code": "a"})
    resumed.copy(con, "SELECT 1 AS a", dest, r, "one")
    assert r["cached"] == ["one"]
    fresh = tmp_path / "fresh.parquet"
    resumed.copy(con, "SELECT 1 AS a", fresh, {}, "one")
    assert pq.read_table(dest).equals(pq.read_table(fresh))
    assert cache.digest(dest) == cache.digest(fresh)
    con.close()


@pytest.mark.parametrize("damage", ["payload", "missing", "incomplete", "malformed", "code", "sql"])
def test_corrupt_or_stale_units_recompute(tmp_path, damage):
    c = cache.PreparationCache(snapshot(tmp_path / "source"), {"code": "a"})
    con = duckdb.connect()
    dest = tmp_path / "unit.parquet"
    c.copy(con, "SELECT 1 AS a", dest, {}, "one")
    side = dest.with_name(dest.name + ".prepare.json")
    if damage == "payload":
        pq.write_table(pa.table({"a": [7]}), dest)  # readable footer is insufficient
    elif damage == "missing":
        side.unlink()
    elif damage == "incomplete":
        receipt = json.loads(side.read_text()); receipt["complete"] = False
        side.write_text(json.dumps(receipt))
    elif damage == "malformed":
        side.write_text("{")
    elif damage == "code":
        c = cache.PreparationCache(c.source, {"code": "b"})
    r = {}
    c.copy(con, "SELECT 2 AS a" if damage == "sql" else "SELECT 1 AS a", dest, r, "one")
    assert r["cached"] == []
    assert pq.read_table(dest)["a"].to_pylist() == [2 if damage == "sql" else 1]
    con.close()


def test_same_size_mtime_source_drift_before_and_during_unit(tmp_path, monkeypatch):
    root = tmp_path / "source"
    (root / "num").mkdir(parents=True)
    source = root / "num" / "2020q1.parquet"
    source.write_bytes(b"abcd")
    s = snapshot(root)
    stat = source.stat()
    def drift():
        source.write_bytes(b"wxyz")
        os.utime(source, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    drift()
    with pytest.raises(ValueError, match="source byte drift"):
        cache.SourceSnapshot(root)
    source.write_bytes(b"abcd")
    c = cache.PreparationCache(s, {})
    real = cache.common.copy_to_parquet
    def changed(con, sql, out, **kw):
        rows = real(con, sql, out, **kw); drift(); return rows
    monkeypatch.setattr(cache.common, "copy_to_parquet", changed)
    con = duckdb.connect()
    dest = tmp_path / "unit.parquet"
    with pytest.raises(ValueError, match="source byte drift"):
        c.copy(con, "SELECT 1 AS a", dest, {}, "one", sources=("num/2020q1.parquet",))
    assert not dest.exists()
    assert not dest.with_name(dest.name + ".prepare.json").exists()
    assert list(tmp_path.glob("*.attempt-*"))
    with pytest.raises(ValueError):
        s.verify()  # same prepublication verification
    con.close()


def test_quarter_verification_does_not_rehash_unrelated_corpus(tmp_path, monkeypatch):
    root = tmp_path / "source"
    (root / "num").mkdir(parents=True)
    for q in ("2020q1", "2020q2"):
        (root / "num" / (q + ".parquet")).write_bytes(q.encode())
    s = snapshot(root)
    calls = []
    real = cache.digest
    def tracked(p):
        calls.append(p); return real(p)
    monkeypatch.setattr(cache, "digest", tracked)
    s.verify(("num/2020q1.parquet",))
    assert root / "num" / "2020q1.parquet" in calls
    assert root / "num" / "2020q2.parquet" not in calls


def test_interruption_after_output_before_receipt_refuses_reuse(tmp_path, monkeypatch):
    c = cache.PreparationCache(snapshot(tmp_path / "source"), {})
    con = duckdb.connect()
    dest = tmp_path / "unit.parquet"
    real = cache.common.write_json_atomic
    def interrupted(*args, **kwargs):
        raise KeyboardInterrupt()
    monkeypatch.setattr(cache.common, "write_json_atomic", interrupted)
    with pytest.raises(KeyboardInterrupt):
        c.copy(con, "SELECT 1 AS a", dest, {}, "one")
    assert dest.exists()
    assert not dest.with_name(dest.name + ".prepare.json").exists()
    monkeypatch.setattr(cache.common, "write_json_atomic", real)
    r = {}
    c.copy(con, "SELECT 1 AS a", dest, r, "one")
    assert r["cached"] == []
    assert list(tmp_path.glob("*.previous-*"))
    con.close()


def test_inventory_and_manifest_drift_fail_closed(tmp_path):
    s = snapshot(tmp_path / "source")
    (s.root / "num").mkdir()
    extra = s.root / "num" / "2020q1.parquet"
    extra.write_bytes(b"unapproved")
    with pytest.raises(ValueError, match="inventory drift"):
        s.verify()
    extra.unlink()
    s.manifest.write_text('{"quarters":{},"changed":true}')
    with pytest.raises(ValueError, match="manifest drift"):
        s.verify()


def test_prepublication_rechecks_exact_source_and_prepared_output(tmp_path, monkeypatch):
    from atx_db.alpha_panel import fundamentals as fu
    root = tmp_path / "source"
    (root / "num").mkdir(parents=True)
    source = root / "num" / "2020q1.parquet"
    source.write_bytes(b"abcd")
    s = snapshot(root)
    code = fu._prepare_code_identity()
    c = cache.PreparationCache(s, code)
    con = duckdb.connect()
    receipt = {}
    output = tmp_path / "unit.parquet"
    c.copy(con, "SELECT 1 AS a", output, receipt, "one")
    receipt["preparation_binding"] = c.binding
    cache.common.write_json_atomic(tmp_path / "prepare.json", receipt)
    monkeypatch.setattr(fu, "work_dir", lambda: tmp_path)
    monkeypatch.setattr(cache.common, "FSDS_DIR", root)
    assert fu._verify_preparation() == c.binding
    stat = source.stat()
    source.write_bytes(b"wxyz")
    os.utime(source, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    with pytest.raises(ValueError, match="source byte drift"):
        fu._verify_preparation()
    source.write_bytes(b"abcd")
    pq.write_table(pa.table({"a": [7]}), output)
    with pytest.raises(ValueError, match="output drift"):
        fu._verify_preparation()
    con.close()
