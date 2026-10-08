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


def prepared_fixture(tmp_path, monkeypatch):
    from atx_db.alpha_panel import fundamentals as fu
    root = tmp_path / "source"
    for table in ("sub", "num", "pre"):
        (root / table).mkdir(parents=True)
        for quarter in ("2020q1", "2020q2"):
            pq.write_table(pa.table({"token": [1]}), root / table / (quarter + ".parquet"))
    snapshot(root)
    monkeypatch.setattr(cache.common, "FSDS_DIR", root)
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    monkeypatch.setenv("ATX_FUND_STAGE", "fundamentals_v11")
    links = fu.links_path()
    links.parent.mkdir(parents=True)
    pq.write_table(pa.table({"cik": [1], "end_incl": [__import__('datetime').date(2020, 1, 1)]}), links)
    # Small synthetic SQL isolates authentication; scope really reads linkage bytes.
    monkeypatch.setattr(fu, "_sub_clock_sql", lambda: "SELECT 'a' AS adsh, TIMESTAMP '2020-01-01' AS accepted_utc")
    for name in ("class_sum_sql", "pos_sum_sql", "pre_flags_sql"):
        monkeypatch.setattr(fu, name, lambda *args: "SELECT 1 AS value")
    monkeypatch.setattr(fu, "label_lines_sql", lambda *args: "SELECT 'sale' AS concept, 1 AS value")
    return fu, root, links


def test_prepublication_rechecks_exact_source_and_prepared_output(tmp_path, monkeypatch):
    fu, root, links = prepared_fixture(tmp_path, monkeypatch)
    receipt = fu.prepare()
    assert fu._verify_preparation() == receipt["preparation_binding"]
    source = root / "num/2020q1.parquet"
    original = source.read_bytes(); stat = source.stat()
    source.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
    os.utime(source, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    with pytest.raises(ValueError, match="source byte drift"):
        fu._verify_preparation()
    source.write_bytes(original)
    pq.write_table(pa.table({"value": [7]}), fu.pos_sums_path())
    with pytest.raises(ValueError, match="output drift"):
        fu._verify_preparation()


def test_after_prepare_linkage_drift_refused_with_unchanged_control(tmp_path, monkeypatch):
    fu, root, links = prepared_fixture(tmp_path, monkeypatch)
    receipt = fu.prepare()
    assert pq.read_table(fu.scope_path())["cik"].to_pylist() == [1]
    assert fu._verify_preparation() == receipt["preparation_binding"]
    stat = links.stat()
    pq.write_table(pa.table({"cik": [2], "end_incl": [__import__('datetime').date(2020, 1, 1)]}), links)
    os.utime(links, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    with pytest.raises(ValueError, match="dependency drift"):
        fu._verify_preparation()
    assert pq.read_table(fu.scope_path())["cik"].to_pylist() == [1]


@pytest.mark.parametrize("damage", ["empty", "missing_global", "missing_quarter", "extra"])
def test_final_required_inventory_refuses_omitted_or_extra_units(tmp_path, monkeypatch, damage):
    fu, root, links = prepared_fixture(tmp_path, monkeypatch)
    receipt = fu.prepare()
    units = receipt["preparation_units"]
    assert len(units) == 10  # six fixed products plus two products for each of two quarters
    assert len([p for p in units if '/pre_flags/' in p or '/label_lines/' in p]) == 4
    if damage == "empty": units.clear()
    elif damage == "missing_global": units.pop(fu.scope_path().as_posix())
    elif damage == "missing_quarter": units.pop((fu.work_dir() / 'pre_flags/2020q2.parquet').as_posix())
    else: units[(fu.work_dir() / 'unapproved.parquet').as_posix()] = next(iter(units.values()))
    cache.common.write_json_atomic(fu.work_dir() / 'prepare.json', receipt)
    with pytest.raises(ValueError, match="inventory"):
        fu._verify_preparation()


@pytest.mark.parametrize("damage", ["incomplete", "missing", "sql", "sources", "dependencies", "options", "rows", "malformed"])
def test_final_unit_receipt_corruption_refused(tmp_path, monkeypatch, damage):
    fu, root, links = prepared_fixture(tmp_path, monkeypatch)
    receipt = fu.prepare()
    path = fu.scope_path() if damage == 'dependencies' else fu.pos_sums_path()
    side = path.with_name(path.name + '.prepare.json')
    unit = json.loads(side.read_text())
    if damage == 'missing': side.unlink()
    elif damage == 'malformed': side.write_text('{')
    else:
        if damage == 'incomplete': unit['complete'] = False
        elif damage == 'rows': unit['rows'] += 1
        else: unit['binding'][damage] = {'unexpected': True} if damage in ('dependencies', 'options') else 'wrong'
        # Corrupt summary and sidecar together, so equality alone cannot certify it.
        side.write_text(json.dumps(unit))
        receipt['preparation_units'][path.as_posix()] = unit
        cache.common.write_json_atomic(fu.work_dir() / 'prepare.json', receipt)
    with pytest.raises((ValueError, OSError)):
        fu._verify_preparation()


def test_preparation_completion_rechecks_linkage_before_summary(tmp_path, monkeypatch):
    fu, root, links = prepared_fixture(tmp_path, monkeypatch)
    real = cache.PreparationCache.verify_units
    def drift(self, receipt, required):
        pq.write_table(pa.table({"cik": [2], "end_incl": [__import__('datetime').date(2020, 1, 1)]}), links)
        return real(self, receipt, required)
    monkeypatch.setattr(cache.PreparationCache, 'verify_units', drift)
    with pytest.raises(ValueError, match='dependency drift'):
        fu.prepare()
    assert not (fu.work_dir() / 'prepare.json').exists()


def test_required_quarters_refuse_missing_configured_table(tmp_path, monkeypatch):
    fu, root, links = prepared_fixture(tmp_path, monkeypatch)
    (root / 'sub/2020q2.parquet').unlink()
    s = snapshot(root)
    with pytest.raises(ValueError, match='configured FSDS quarter'):
        fu._required_preparation_units(s)


def test_cached_return_rechecks_live_dependency(tmp_path, monkeypatch):
    c = cache.PreparationCache(snapshot(tmp_path / 'source'), {})
    con = duckdb.connect()
    dependency = tmp_path / 'linkage.parquet'
    pq.write_table(pa.table({'cik': [1]}), dependency)
    dest = tmp_path / 'scope.parquet'
    sql = f"SELECT cik FROM read_parquet('{dependency.as_posix()}')"
    c.copy(con, sql, dest, {}, 'scope', dependencies=(dependency,))
    real = cache.identity
    def mutate_during_admission(path):
        pin = real(path)
        if path == dest:
            pq.write_table(pa.table({'cik': [2]}), dependency)
        return pin
    monkeypatch.setattr(cache, 'identity', mutate_during_admission)
    receipt = {}
    with pytest.raises(ValueError, match='dependency drift'):
        c.copy(con, sql, dest, receipt, 'scope', dependencies=(dependency,))
    assert receipt['cached'] == [] and receipt['preparation_units'] == {}
    con.close()


@pytest.mark.parametrize('order', [('2020q2', '2020q1'), ('2020q2', '2020q3', '2020q1')])
def test_permuted_manifest_quarters_certify_identical_outputs_and_resume(tmp_path, monkeypatch, order):
    fu, root, links = prepared_fixture(tmp_path, monkeypatch)
    if '2020q3' in order:
        for table in ('sub', 'num', 'pre'):
            pq.write_table(pa.table({'token': [1]}), root / table / '2020q3.parquet')
        snapshot(root)
    baseline = fu.prepare()
    manifest = root / 'fsds-staging-manifest.json'
    metadata = json.loads(manifest.read_text())
    metadata['quarters'] = {q: {'tables': dict(reversed(list(metadata['quarters'][q]['tables'].items())))}
                            for q in order}
    manifest.write_text(json.dumps(metadata))
    prepared = fu.prepare()
    assert fu._verify_preparation() == prepared['preparation_binding']
    assert {p: u['output'] for p, u in prepared['preparation_units'].items()} == {
        p: u['output'] for p, u in baseline['preparation_units'].items()}
    for path, unit in prepared['preparation_units'].items():
        old = baseline['preparation_units'][path]['binding']
        for field in ('sql', 'key', 'sources', 'dependencies', 'options', 'code'):
            assert unit['binding'][field] == old[field]
        assert unit['binding']['sources'] == sorted(unit['binding']['sources'])
    # Raw manifest bytes remain pinned: formatting/order changes invalidate old
    # source bindings; an unchanged reordered snapshot resumes with the same binding.
    assert prepared['preparation_binding']['source']['manifest'] != baseline['preparation_binding']['source']['manifest']
    resumed = fu.prepare()
    assert resumed['preparation_binding'] == prepared['preparation_binding']
    assert len(resumed['cached']) == 6 + 2 * len(order)
    assert fu._verify_preparation() == resumed['preparation_binding']


def test_empty_quarter_inventory_and_duplicate_quarter_keys_fail_closed(tmp_path, monkeypatch):
    from atx_db.alpha_panel import fundamentals as fu
    s = snapshot(tmp_path / 'empty')
    with pytest.raises(ValueError, match='configured FSDS quarter'):
        fu._required_preparation_units(s)
    # JSON duplicates must be refused before a parser can silently discard a quarter.
    s.manifest.write_text('{"quarters":{"2020q1":{"tables":{}},"2020q1":{"tables":{}}}}')
    with pytest.raises(ValueError, match='duplicate FSDS manifest key'):
        cache.SourceSnapshot(s.root)
    with pytest.raises(ValueError, match='duplicate preparation source'):
        cache.ordered_sources(['num/2020q1.parquet', 'num/2020q1.parquet'])
